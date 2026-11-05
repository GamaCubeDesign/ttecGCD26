# Documentation

Written in English: the competition requires the Design Package in English
(rules §2), and these documents feed it almost verbatim. See ADR-0011.

| Directory | Contents |
|---|---|
| [`adr/`](adr/README.md) | Architecture Decision Records — every decision, with its rationale and the alternatives rejected |
| `icd/` | Interface Control Documents. Written: the OBC↔AOCS I²C contract (`obc-aocs-i2c.md`, a draft for the AOCS team). Not written yet: the OBC↔TT&C contract and the over-the-air protocol, whose normative description today is the code and ADR-0007 |
| `budgets/` | Data and link budgets (`data-budget.md`, `link-budget.md`), every figure produced by `tools/analysis/lora_budget.py`. The power budget is the CDR's (Table 13) |
| `vv/bancada/` | Hardware bench evidence: one directory per day with the logs `make bench` writes and a `notas.md`, the progress per stage (`README.md`) and the November bench script. The V&V plan and the traceability matrix are in the CDR (Tables 66 and 67) |
| `cdr/` | The delivered Critical Design Review (Design Package, v4, 2026-10-11): what the project promised |
| `relatorios/` | Team reports and bench scripts, dated, in Portuguese — working notes for the team, not part of the Design Package |
| [`requisitos.md`](requisitos.md) | Every mission requirement quoted verbatim from the rules, with its status in this repository and what is missing — in Portuguese, working notes for the team |

Source material from the organisers, kept as received:

- `cubedesign2026.pdf` — competition rules and the HLR catalogue; transcribed
  and checked verbatim in [`requisitos.md`](requisitos.md)
- `anotacoes_cubedesign2026.pdf` — the team's working notes on those rules
- `arquitetura.png` — the agreed physical topology

## Reading order

Start with [`adr/README.md`](adr/README.md). The records are numbered in the
order the decisions were made, which is also roughly their order of
consequence: ADR-0003 and ADR-0004 between them determine what the mission can
and cannot do.
