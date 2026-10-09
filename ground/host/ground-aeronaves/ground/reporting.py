"""Resumo de hipóteses de aeroportos, sem transformar score em probabilidade."""


def update_summary(previous, result):
    summary = dict(previous or {"takeoff": None, "landing": None})
    for key, phase, candidates_key in (
        ("takeoff", "CLIMB", "origin_candidates"),
        ("landing", "DESCENT", "destination_candidates"),
    ):
        candidates = result[candidates_key]
        if result["flight_state"] == phase and candidates:
            summary[key] = {
                "airport": candidates[0],
                "event_ns": result["event_ns"],
                "confidence": "BAIXA",
                "basis": "heuristica_nao_calibrada",
            }
    return summary


def table_row(result, ttl_s, include_method=False):
    cells, confidence = [], []
    for key, label in (("takeoff", "decolagem"), ("landing", "pouso")):
        hypothesis = result["airport_summary"][key]
        decision = result.get("airport_method", {}).get(
            "origin" if key == "takeoff" else "destination"
        )
        if decision and decision.get("reason") == "AMBIGUO":
            cells.append("indeterminado")
            confidence.append(f"{label}: insuficiente (ambíguo)")
            continue
        if hypothesis is None:
            cells.append("indeterminado")
            reason = f" ({decision['reason']})" if decision else ""
            past = result.get("airport_last_evidence", {}).get(
                "origin" if key == "takeoff" else "destination"
            )
            if (
                decision
                and past
                and decision.get("reason") == "JANELA_INICIAL_ENCERRADA"
            ):
                reason += f"; última análise: {past['reason']}"
            confidence.append(f"{label}: insuficiente{reason}")
            continue
        airport = hypothesis["airport"]
        code = airport.get("icao_code") or airport["ident"]
        # A hipótese é uma inferência registrada, nunca uma confirmação do evento.
        historical = (
            (
                decision is not None
                and decision["status"] not in ("ESTIMATED", "TENTATIVE")
            )
            or (result["event_ns"] - hypothesis["event_ns"]) / 1e9 > ttl_s
            or result["reason"] == "POSICAO_EXPIRADA"
        )
        suffix = (
            "histórico"
            if historical
            else "candidato"
            if hypothesis.get("status") == "TENTATIVE"
            else "estimado"
        )
        cells.append(f"{code} ({suffix})")
        confidence.append(
            f"{label}: baixa"
            + (
                "; decisão inconclusiva"
                if hypothesis.get("status") == "TENTATIVE"
                else ""
            )
        )
    prefix = f"{result['icao']} | AEROPORTOS" if include_method else result["icao"]
    return f"{prefix} | {cells[0]} | {cells[1]} | {'; '.join(confidence)}"
