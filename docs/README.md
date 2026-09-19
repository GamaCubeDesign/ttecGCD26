# Documentation

Written in English: the competition requires the Design Package in English
(rules §2), and these documents feed it almost verbatim. See ADR-0011.

| Directory | Contents |
|---|---|
| [`adr/`](adr/README.md) | Architecture Decision Records — every decision, with its rationale and the alternatives rejected |
| `icd/` | Interface Control Documents — the OBC↔TT&C contract and the over-the-air protocol |
| `budgets/` | Data, link and power budgets |
| `vv/` | Verification and Validation plan and the requirements traceability matrix |
| `conops.md` | Concept of Operations — mission phases and operational modes |
| `relatorios/` | Team status reports, in Portuguese — working notes for the team, not part of the Design Package |

Source material from the organisers, kept as received:

- `cubedesign2026.pdf` — competition rules and the HLR catalogue
- `anotacoes_cubedesign2026.pdf` — the team's working notes on those rules
- `arquitetura.png` — the agreed physical topology

## Reading order

Start with [`adr/README.md`](adr/README.md). The records are numbered in the
order the decisions were made, which is also roughly their order of
consequence: ADR-0003 and ADR-0004 between them determine what the mission can
and cannot do.
