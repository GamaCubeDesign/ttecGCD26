#!/usr/bin/env python3
import argparse, json, sys
from collections import Counter, defaultdict
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from ground.aircraft_manager import AircraftManager
from ground.airport_estimator import AirportEstimator
from ground.config import Config
from ground.database import Database
from ground.parser import parse_line

ICAOS = ("F10001", "F10002", "F10003", "F10004")


def code_of(c):
    if not c:
        return None
    return c.get("icao_code") or c.get("ident")


def rank_of(cands, expected):
    if not expected:
        return None
    for i, c in enumerate(cands, 1):
        if code_of(c) == expected or c.get("ident") == expected:
            return i
    return None


def fusion_info(manager, icao, kind, candidate):
    pair = manager.fusions.get(icao)
    if not pair or not candidate:
        return {}
    fusion = pair[0 if kind == "origin" else 1]
    entry = fusion.memory.get(candidate.get("airport_id"))
    if not entry:
        return {}
    return {
        "count": entry.get("count"),
        "age_s": (
            (fusion.last_ns - entry["first_seen_ns"]) / 1e9
            if fusion.last_ns is not None and entry.get("first_seen_ns") is not None
            else None
        ),
        "strength": entry.get("strength"),
    }


def diagnose(manager, estimator, result, icao, kind, expected):
    origin = kind == "origin"
    snap = result.get("snapshot")
    phase = result.get("flight_state")
    quality = result.get("data_quality") or {}
    decision = (result.get("airport_method") or {}).get(kind) or {}
    first = manager.first_position_ns.get(icao)
    elapsed = (result["event_ns"] - first) / 1e9 if first is not None else None
    early = (not origin) or elapsed is None or elapsed <= 180
    terminal = (
        not origin
        and bool((snap or {}).get("approach_context", {}).get("descent_context"))
        and phase != "GROUND"
    )
    if snap is None:
        cands = []
    elif origin and phase != "CLIMB":
        cands = []
    else:
        cands = estimator.estimate(snap, phase, origin=origin)
    top = cands[0] if cands else None
    best = decision.get("best_candidate") or decision.get("candidate") or top
    info = fusion_info(manager, icao, kind, best)
    comp = (best or {}).get("components", {})
    ev = (best or {}).get("evidence", {})
    coverage = (best or {}).get("evidence_coverage")
    trend = comp.get("trend")
    conv = comp.get("cumulative_convergence")
    height = (
        ev.get("initial_height_ft") if origin else ev.get("height_above_airport_ft")
    )
    strength = decision.get("compatibility", info.get("strength"))
    margin = decision.get("margin")
    blockers = []
    if snap is None:
        blockers.append("SEM_POSICAO_RECENTE")
    if not quality.get("usable", False):
        blockers.append("QUALIDADE_NAO_UTILIZAVEL")
    if origin and not early:
        blockers.append("JANELA_ORIGEM_180S_ENCERRADA")
    phase_ok = (
        phase == "CLIMB" if origin else (phase in ("DESCENT", "APPROACH") or terminal)
    )
    if not phase_ok:
        blockers.append("FASE_NAO_PERMITE_INFERENCIA")
    if phase_ok and quality.get("usable", False) and snap is not None:
        if not cands:
            blockers.append("SEM_CANDIDATOS")
        else:
            if info.get("count") is not None and info["count"] < 3:
                blockers.append("PERSISTENCIA_CONTAGEM_<3")
            if info.get("age_s") is not None and info["age_s"] < 20:
                blockers.append("PERSISTENCIA_TEMPO_<20S")
            if quality.get("score") is not None and quality["score"] < 0.35:
                blockers.append("QUALITY_SCORE_<0.35")
            if coverage is not None and coverage < 0.55:
                blockers.append("EVIDENCE_COVERAGE_<0.55")
            if terminal:
                if conv is None or conv < 0.35:
                    blockers.append("CONVERGENCIA_<0.35")
            else:
                if trend is None or trend < 0.65:
                    blockers.append("TREND_<0.65")
            max_h = 6000 if origin else 10000
            if height is None:
                blockers.append("ALTURA_RELATIVA_AUSENTE")
            elif not (-300 <= height <= max_h):
                blockers.append(f"ALTURA_FORA_FAIXA_-300_{max_h}FT")
            if strength is not None and strength < 0.65:
                blockers.append("COMPATIBILIDADE_<0.65")
            if margin is not None and margin < 0.08:
                blockers.append("MARGEM_<0.08")
            if not origin and terminal:
                if strength is not None and strength < 0.55:
                    blockers.append("TENTATIVE_COMPATIBILIDADE_<0.55")
                if ev.get("distance_km") is not None and ev["distance_km"] > 50:
                    blockers.append("TENTATIVE_DISTANCIA_>50KM")
    return {
        "icao": icao,
        "kind": kind,
        "event_ns": result["event_ns"],
        "phase": phase,
        "expected": expected,
        "decision_status": decision.get("status", "UNKNOWN"),
        "decision_reason": decision.get("reason"),
        "best_candidate": code_of(best),
        "raw_top_candidate": code_of(top),
        "expected_rank": rank_of(cands, expected),
        "quality_score": quality.get("score"),
        "elapsed_first_position_s": elapsed,
        "terminal_context": terminal,
        "compatibility": strength,
        "margin": margin,
        "evidence_coverage": coverage,
        "trend": trend,
        "cumulative_convergence": conv,
        "height_ft": height,
        "distance_km": ev.get("distance_km"),
        "memory_count": info.get("count"),
        "memory_age_s": info.get("age_s"),
        "blockers": blockers,
    }


def closeness(r):
    hard = {
        "SEM_POSICAO_RECENTE",
        "QUALIDADE_NAO_UTILIZAVEL",
        "JANELA_ORIGEM_180S_ENCERRADA",
        "FASE_NAO_PERMITE_INFERENCIA",
        "SEM_CANDIDATOS",
    }
    return (
        sum(b in hard for b in r["blockers"]),
        len(r["blockers"]),
        -(r["compatibility"] if r["compatibility"] is not None else -1),
    )


def summarize(rows, truth):
    by = defaultdict(list)
    for r in rows:
        by[(r["icao"], r["kind"])].append(r)
    out = {}
    for (icao, kind), g in sorted(by.items()):
        relevant = [
            r
            for r in g
            if "FASE_NAO_PERMITE_INFERENCIA" not in r["blockers"]
            and "JANELA_ORIGEM_180S_ENCERRADA" not in r["blockers"]
        ]
        accepted = [r for r in g if r["decision_status"] in ("TENTATIVE", "ESTIMATED")]
        cnt = Counter()
        for r in relevant:
            cnt.update(r["blockers"])
        ranks = [r["expected_rank"] for r in relevant if r["expected_rank"] is not None]
        out[f"{icao}:{kind}"] = {
            "expected": (truth.get("aircraft", {}).get(icao) or {}).get(kind),
            "ever_accepted": bool(accepted),
            "best_expected_rank_seen": min(ranks) if ranks else None,
            "most_common_blockers": cnt.most_common(8),
            "closest_moment": min(relevant or g, key=closeness) if g else None,
        }
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument(
        "--input", type=Path, default=ROOT / "validation" / "casos_avaliacao.ndjson"
    )
    p.add_argument(
        "--truth", type=Path, default=ROOT / "validation" / "ground_truth.json"
    )
    p.add_argument(
        "--json-out",
        type=Path,
        default=ROOT / "validation" / "diagnostico_estimador.json",
    )
    args = p.parse_args()

    defaults = Config()
    db = ROOT / "validation" / "telemetria_diagnostico.db"
    state = ROOT / "validation" / "estado_diagnostico.json"
    for x in (db, Path(str(db) + "-wal"), Path(str(db) + "-shm"), state):
        if x.exists():
            x.unlink()
    config = replace(defaults, telemetry_db=db, output=state, top=100)
    estimator = AirportEstimator(config)
    manager = AircraftManager(Database(db), estimator, config)
    truth = json.loads(args.truth.read_text(encoding="utf-8"))
    rows = []
    with args.input.open(encoding="utf-8") as f:
        for raw in f:
            if not raw.strip():
                continue
            obs = parse_line(raw, 0)
            if obs.icao not in ICAOS:
                continue
            result = manager.process(obs)
            t = truth.get("aircraft", {}).get(obs.icao, {})
            rows.append(
                diagnose(
                    manager, estimator, result, obs.icao, "origin", t.get("origin")
                )
            )
            rows.append(
                diagnose(
                    manager,
                    estimator,
                    result,
                    obs.icao,
                    "destination",
                    t.get("destination"),
                )
            )
    summary = summarize(rows, truth)
    args.json_out.write_text(
        json.dumps({"summary": summary, "rows": rows}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print("\nDIAGNOSTICO F10001-F10004")
    print("=" * 90)
    for key, s in summary.items():
        print(
            f"\n{key} | esperado={s['expected']} | aceitou={s['ever_accepted']} | melhor_rank_correto={s['best_expected_rank_seen']}"
        )
        print(
            "bloqueios principais:",
            ", ".join(f"{b}({n})" for b, n in s["most_common_blockers"]) or "nenhum",
        )
        c = s["closest_moment"]
        if c:
            print("mais perto da decisão:")
            print(
                f"  fase={c['phase']} candidato={c['best_candidate']} rank_correto={c['expected_rank']} compat={c['compatibility']} margem={c['margin']}"
            )
            print(
                f"  coverage={c['evidence_coverage']} trend={c['trend']} convergencia={c['cumulative_convergence']} altura={c['height_ft']} distancia={c['distance_km']}"
            )
            print("  bloqueios=", ", ".join(c["blockers"]) or "nenhum")
    print("\nIMPORTANTE:")
    print(
        "  Origem não pode virar TENTATIVE no código atual; origem só vira ESTIMATED ou UNKNOWN."
    )
    print("  TENTATIVE existe apenas para destino em contexto terminal.")
    print(f"\nRelatório completo: {args.json_out}")


if __name__ == "__main__":
    main()
