import json

from .flight_state import FlightStateEstimator
from .temporal import vertical_features, quality, approach_context
from .decision_fusion import DecisionFusion
from .trajectory import build_snapshot


class AircraftManager:
    def __init__(self, database, estimator, config):
        self.database, self.estimator, self.config = database, estimator, config
        self.states = {}
        self.phase_models = {}
        self.fusions = {}
        self.first_position_ns = {}
        self.approach_since = {}
        self.segment_start = {}
        self.grounded = set()
        self.takeoff_anchor = {}

    def process(self, observation):
        observation_id = self.database.save(observation)
        history = self.database.recent(
            observation.icao, self.config.window_s, self.config.max_observations
        )
        previous = self.states.get(observation.icao, {})
        # Separate tracking episodes after a long reception gap; never reuse an
        # origin across disconnected episodes of the same aircraft identifier.
        if (
            previous
            and observation.event_ns - previous["event_ns"] > self.config.window_s * 1e9
        ):
            self.segment_start[observation.icao] = observation.event_ns
            self.phase_models.pop(observation.icao, None)
            self.fusions.pop(observation.icao, None)
            self.first_position_ns.pop(observation.icao, None)
            self.takeoff_anchor.pop(observation.icao, None)
            self.approach_since.pop(observation.icao, None)
            previous = {}
            self.grounded.discard(observation.icao)
        start = self.segment_start.get(observation.icao, history[0].event_ns)
        history = [o for o in history if o.event_ns >= start]
        snapshot = build_snapshot(history, self.config.field_ttl_s)
        vertical = vertical_features(history, history[-1].event_ns)
        data_quality = quality(snapshot)
        if snapshot:
            snapshot["vertical"] = vertical
            snapshot["approach_context"] = approach_context(
                history, history[-1].event_ns
            )
            self.first_position_ns.setdefault(
                observation.icao, snapshot["trajectory"][0]["event_ns"]
            )
        # Use an explicitly received ground position, never a propagated flag
        # paired with a newer airborne position or an out-of-order older anchor.
        if (
            snapshot
            and observation.has_position
            and observation.fields.get("on_ground") == 1
        ):
            anchor = self.takeoff_anchor.get(observation.icao)
            if anchor is None or observation.event_ns >= anchor["event_ns"]:
                self.takeoff_anchor[observation.icao] = {
                    "lat": observation.fields["lat"],
                    "lon": observation.fields["lon"],
                    "altitude_ft": observation.fields.get("altitude_ft"),
                    "event_ns": observation.event_ns,
                }
        phase_model = self.phase_models.setdefault(
            observation.icao, FlightStateEstimator()
        )
        flight_state = phase_model.update(
            snapshot if not data_quality.get("position_jumps") else None, vertical
        )
        if flight_state == "GROUND":
            self.grounded.add(observation.icao)
        elif flight_state == "CLIMB" and observation.icao in self.grounded:
            self.grounded.discard(observation.icao)
            self.fusions.pop(observation.icao, None)
            self.approach_since.pop(observation.icao, None)
            self.first_position_ns[observation.icao] = history[-1].event_ns
            self.segment_start[observation.icao] = history[-1].event_ns
            previous = {}
        if snapshot and observation.icao in self.takeoff_anchor:
            snapshot["takeoff_anchor"] = self.takeoff_anchor[observation.icao]
        destination = self.estimator.estimate(snapshot, flight_state)
        # APPROACH requires persistent runway geometry plus descent, never a
        # low absolute altitude alone (airports can be at high elevations).
        aligned = next(
            (
                c
                for c in destination
                if c["evidence"].get("runway", {}).get("approach_compatible")
                and c["components"].get("trend", 0) >= 0.65
            ),
            None,
        )
        if flight_state == "DESCENT" and aligned and data_quality["usable"]:
            stamp = snapshot["trajectory"][-1]["event_ns"]
            key = (aligned["airport_id"], aligned["evidence"]["runway"]["runway"])
            old_key, since = self.approach_since.get(observation.icao, (key, stamp))
            if old_key != key:
                since = stamp
            self.approach_since[observation.icao] = (key, since)
            if stamp - since >= 10e9:
                flight_state = "APPROACH"
                destination = self.estimator.estimate(snapshot, flight_state)
        else:
            self.approach_since.pop(observation.icao, None)
        early = (
            history[-1].event_ns
            - self.first_position_ns.get(observation.icao, history[-1].event_ns)
            <= 180e9
        )
        origin = (
            self.estimator.estimate(snapshot, flight_state, origin=True)
            if early
            else []
        )
        positions = [o for o in history if o.has_position]
        position_age = (
            (history[-1].event_ns - positions[-1].event_ns) / 1e9 if positions else None
        )
        if not positions:
            reason = "SEM_POSICAO_NA_JANELA"
        elif snapshot is None:
            reason = "POSICAO_EXPIRADA"
        elif flight_state == "GROUND":
            reason = "AERONAVE_NO_SOLO"
        elif not data_quality["usable"]:
            reason = data_quality["reason"]
        elif not destination:
            reason = "SEM_AEROPORTOS_NO_RAIO_E_TIPOS_SELECIONADOS"
        else:
            reason = "CANDIDATOS_DISPONIVEIS"
        result = {
            "icao": observation.icao,
            "event_ns": history[-1].event_ns,
            "last_received_ns": max(o.received_ns for o in history),
            "flight_state": flight_state,
            "snapshot": snapshot,
            "vertical_estimate": vertical,
            "data_quality": data_quality,
            "destination_candidates": destination[: self.config.top],
            "origin_candidates": origin[: self.config.top],
            "estimation_status": "CANDIDATES"
            if destination
            else "INSUFFICIENT_EVIDENCE_OR_NO_CANDIDATES",
            "reason": reason,
            "last_position_age_s": position_age,
            "score_kind": "heuristic_compatibility_not_probability",
            "candidate_radius_km": self.config.radius_km,
        }
        result["session_id"] = self.database.session_id
        origin_fusion, destination_fusion = self.fusions.setdefault(
            observation.icao, (DecisionFusion(), DecisionFusion())
        )
        origin_decision = (
            origin_fusion.update(
                origin, snapshot, data_quality, flight_state, origin=True
            )
            if early
            else {
                "status": "UNKNOWN",
                "reason": "JANELA_INICIAL_ENCERRADA",
                "candidate": None,
            }
        )
        destination_decision = destination_fusion.update(
            destination, snapshot, data_quality, flight_state
        )
        if vertical["conflict"]:
            for decision in (origin_decision, destination_decision):
                decision.update(
                    status="UNKNOWN",
                    candidate=None,
                    reason="SENTIDOS_VERTICAIS_DIVERGENTES",
                )
        diagnostics = dict(previous.get("airport_last_evidence", {}))
        for kind, decision in [
            ("origin", origin_decision),
            ("destination", destination_decision),
        ]:
            if decision.get("ranking"):
                diagnostics[kind] = dict(decision)
        result["airport_last_evidence"] = diagnostics
        result["airport_method"] = {
            "origin": origin_decision,
            "destination": destination_decision,
        }
        statuses = {d["status"] for d in (origin_decision, destination_decision)}
        result["estimation_status"] = (
            "ESTIMATED"
            if "ESTIMATED" in statuses
            else "TENTATIVE"
            if "TENTATIVE" in statuses
            else "UNKNOWN"
        )
        summary = dict(
            previous.get("airport_summary") or {"takeoff": None, "landing": None}
        )
        # Preserve only accepted past hypotheses, explicitly historical in UI.
        for key, decision in [
            ("takeoff", origin_decision),
            ("landing", destination_decision),
        ]:
            if decision["status"] in ("ESTIMATED", "TENTATIVE"):
                summary[key] = {
                    "airport": decision["candidate"],
                    "event_ns": decision["evidence_ns"],
                    "confidence": "BAIXA",
                    "status": decision["status"],
                    "basis": "temporal_compatibility_not_probability",
                }
        result["airport_summary"] = summary
        self.database.save_estimate(observation_id, result)
        self.states[observation.icao] = result
        self.config.output.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.config.output.with_suffix(".tmp")
        tmp.write_text(
            json.dumps(
                {"aircraft": self.states}, ensure_ascii=False, allow_nan=False, indent=2
            )
        )
        tmp.replace(self.config.output)
        return result
