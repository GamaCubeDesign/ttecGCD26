# Architecture Decision Records

Every architectural decision in this repository is recorded here, in English,
as a numbered immutable document.

## Why these exist

Three reasons, in order of importance to the project:

1. **The competition scores it.** CubeDesign 2026 requires design decisions to
   be justified against performance, cost and complexity trade-offs
   (HLR-COST-02), requires the communication architecture to be "defined,
   justified and consistent" (HLR-COMM-03), requires the processing
   architecture to be justified (HLR-ADS-04), and awards a prize for technical
   documentation judged on "clarity, traceability and professional standard".
2. **The Design Package is assembled from them.** An ADR written at the moment
   the code materialises the decision needs no archaeology later. An ADR
   written the week before submission is a reconstruction, and reads like one.
3. **Two teams, two repositories.** The OBC is developed separately. A written
   contract with a stated rationale is what keeps the two halves from drifting.

## Process

- One decision per record. If a record needs "and", it is probably two.
- Copy `0000-template.md`, take the next free number, never reuse a number.
- Records are **immutable once Accepted**. To change a decision, write a new
  ADR and set the old one to `Superseded by ADR-NNNN`. The superseded record
  stays in the repository: the reasoning that was once correct is evidence,
  and reviewers ask why something changed.
- Cite the HLR identifiers the decision serves. The traceability matrix in
  `docs/vv/` is generated against these.
- Numbers, not adjectives. If the decision came from a calculation, the script
  that produced it belongs in `tools/analysis/` so a reviewer can rerun it.

## Index

| # | Title | Status | Requirements |
|---|-------|--------|--------------|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted | HLR-SYS-01, HLR-COST-02 |
| [0002](0002-flight-software-language.md) | C11 for flight and ground software | Accepted | HLR-SW-01, HLR-SYS-01 |
| [0003](0003-processing-split-onboard-vs-ground.md) | Hybrid processing split at the track-state boundary | Accepted | HLR-ADS-04, HLR-ADS-05, HLR-ADS-08 |
| [0004](0004-lora-phy-configuration.md) | LoRa PHY configuration and rate profiles | Accepted | HLR-COMM-03, HLR-ADS-08 |
