"""Memória limitada de compatibilidade, não posterior Bayesiano calibrado.

Uma atualização por nova posição, intervalo mínimo de 10 s. Média exponencial
limitada evita multiplicar repetidamente evidências de janelas sobrepostas.
"""

import math


class DecisionFusion:
    def __init__(self):
        self.memory = {}
        self.last_ns = None
        self.started_ns = None
        self.updates = 0
        self.last_result = {
            "status": "UNKNOWN",
            "reason": "EVIDENCIA_INSUFICIENTE",
            "candidate": None,
        }

    def update(self, candidates, snapshot, data_quality, phase, origin=False):
        empty = {
            "status": "UNKNOWN",
            "candidate": None,
            "confidence": "INSUFICIENTE",
            "score_kind": "temporal_compatibility_not_probability",
        }
        if not data_quality["usable"] or snapshot is None:
            return {**empty, "reason": data_quality["reason"]}
        terminal = (
            not origin
            and snapshot.get("approach_context", {}).get("descent_context", False)
            and phase != "GROUND"
        )
        if not terminal and phase not in (
            ("CLIMB",) if origin else ("DESCENT", "APPROACH")
        ):
            return {
                **empty,
                "reason": "FASE_SEM_EVIDENCIA_DE_ORIGEM"
                if origin
                else "FASE_SEM_EVIDENCIA_DE_DESTINO",
            }
        now = snapshot["trajectory"][-1]["event_ns"]
        if self.last_ns is not None and now - self.last_ns < 10e9:
            return self.last_result
        dt = (now - self.last_ns) / 1e9 if self.last_ns is not None else 10
        if dt > 120:
            self.memory.clear()
            self.started_ns, self.updates = None, 0
        self.started_ns = now if self.started_ns is None else self.started_ns
        self.previous_update_ns = self.last_ns
        self.last_ns = now
        self.updates += 1
        alpha = 1 - math.exp(-min(dt, 120) / 30)
        seen = set()
        for candidate in candidates:
            key = candidate["airport_id"]
            seen.add(key)
            old = self.memory.get(key)
            strength = (
                candidate["score"]
                if old is None
                else (1 - alpha) * old["strength"] + alpha * candidate["score"]
            )
            continuous = (
                old is not None and old["last_seen_ns"] == self.previous_update_ns
            )
            self.memory[key] = {
                "strength": strength,
                "candidate": candidate,
                "count": old["count"] + 1 if continuous else 1,
                "first_seen_ns": old["first_seen_ns"] if continuous else now,
                "last_seen_ns": now,
            }
        for key in list(self.memory):
            if key not in seen:
                self.memory[key]["strength"] *= math.exp(-dt / 30)
                if self.memory[key]["strength"] < 0.05:
                    del self.memory[key]
        ranked = sorted(self.memory.values(), key=lambda m: -m["strength"])
        result = {**empty, "reason": "SEM_CANDIDATOS", "updates": self.updates}
        if ranked:
            first = ranked[0]
            candidate = first["candidate"]
            strength = first["strength"]
            margin = strength - ranked[1]["strength"] if len(ranked) > 1 else strength
            ev = candidate["evidence"]
            height = (
                ev.get("initial_height_ft")
                if origin
                else ev.get("height_above_airport_ft")
            )
            enough = (
                first["count"] >= 3
                and now - first["first_seen_ns"] >= 20e9
                and data_quality["score"] >= 0.35
                and candidate["airport_id"] in seen
                and candidate["evidence_coverage"] >= 0.55
                and (
                    candidate["components"].get("cumulative_convergence", 0) >= 0.35
                    if terminal
                    else candidate["components"].get("trend", 0) >= 0.65
                )
                and height is not None
                and -300 <= height <= (6000 if origin else 10000)
            )
            result.update(
                {
                    "compatibility": round(strength, 4),
                    "margin": round(margin, 4),
                    "best_candidate": candidate,
                    "ranking": [
                        {
                            "airport": m["candidate"]["ident"],
                            "compatibility": round(m["strength"], 4),
                        }
                        for m in ranked[:5]
                    ],
                    "reason": "EVIDENCIA_INSUFICIENTE"
                    if not enough or strength < 0.65
                    else "AMBIGUO"
                    if margin < 0.08
                    else "HIPOTESE_TEMPORAL",
                }
            )
            if enough and strength >= 0.65 and margin >= 0.08:
                result.update(
                    {
                        "status": "ESTIMATED",
                        "candidate": candidate,
                        "confidence": "BAIXA",
                        "evidence_ns": now,
                    }
                )
            # A useful hypothesis and a sufficiently separated decision are
            # different outputs; do not hide all candidates behind UNKNOWN.
            tentative_destination = not origin and (
                terminal or phase in ("DESCENT", "APPROACH")
            )
            tentative_strength = 0.55 if terminal else 0.65
            if (
                enough
                and tentative_destination
                and strength >= tentative_strength
                and ev.get("distance_km", float("inf")) <= 50
            ):
                result.update(candidate=candidate, evidence_ns=now, confidence="BAIXA")
                if result["status"] != "ESTIMATED":
                    result.update(
                        status="TENTATIVE",
                        reason="CANDIDATO_SEM_SEPARACAO_SUFICIENTE"
                        if margin < 0.08
                        else "CANDIDATO_COM_BAIXA_CONFIANCA",
                    )
        self.last_result = result
        return result
