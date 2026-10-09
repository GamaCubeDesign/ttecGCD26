# ADR-0001: Record architecture decisions

- **Status:** Accepted
- **Date:** 2026-09-15
- **Requirements:** HLR-SYS-01, HLR-COST-02, HLR-VV-01
- **Deciders:** TT&C team

## Context

The previous mission's telemetry software (`ultima_missao/`) works, but none of
its design choices are written down anywhere. Reconstructing why the radio was
configured at SF12 / BW 62.5 kHz / CR 4:8 required reading the driver
initialisation and re-deriving the resulting data rate from the SX1276
datasheet. That calculation turned out to be the single most consequential
number in the mission, and nobody on the team could have stated it from memory.

CubeDesign 2026 makes this an explicit scoring matter rather than a matter of
taste:

- **HLR-COST-02** — design decisions shall be justified on performance, cost
  and complexity trade-offs.
- **HLR-COMM-03** — the communication architecture shall be defined, justified
  and consistent with mission requirements.
- **HLR-ADS-04** — the processing architecture shall be implemented *and
  justified*.
- **HLR-SYS-01** — mission definition, architecture, requirements, verification
  plan and results shall be consistent and defensible from a systems
  engineering perspective.
- §8.3 of the rules awards a prize for technical documentation judged on
  "clarity, traceability and professional standard".

The Design Package is due 2026-09-27. The work it documents is being written
now. Those two facts have to be connected by something more durable than
memory.

## Decision

We record every architectural decision as a numbered Architecture Decision
Record in `docs/adr/`, written in English at the moment the decision is made,
and we treat accepted records as immutable.

A decision qualifies as architectural if changing it later would require
changing code in more than one component, would change a wire format, or would
invalidate a budget.

## Rationale

The alternative that actually competes with this is "write the Design Package
at the end", which is what most teams do. It loses on three concrete counts:

1. **Information decays.** The SF12 example above is not hypothetical — it is
   this repository, one mission ago, with the same people.
2. **The DP deadline precedes the competition by two months.** Documentation
   written at the end is written under the worst time pressure of the project,
   about work whose details are freshest at the start.
3. **The OBC lives in a separate repository, maintained by a different team.**
   `obc/docs/log_schema.md` already contains the line "depends on the interface
   contract with the payload, which is also pending" — a decision that was
   never recorded and therefore never made. ADRs plus the ICDs in `docs/icd/`
   are how that stops happening.

Immutability is what separates an ADR from a wiki page. A superseded record
that stays in the repository shows a reviewer that a decision was reconsidered
and on what grounds; an edited page shows nothing, and quietly destroys the
evidence that the earlier reasoning ever existed.

## Alternatives considered

### A single living architecture document

One `architecture.md`, updated in place. Rejected: it records the current state
but not the reasoning or the rejected options, which is exactly the content
HLR-COST-02 and HLR-ADS-04 ask for. It also produces merge conflicts between
two sub-teams editing the same file, where ADRs produce independent new files.

### Decisions in commit messages and code comments

Rejected as the primary record. Comments are the right place for a local "why",
and this codebase uses them heavily, but they cannot hold the alternatives that
were rejected, and a reviewer assembling a Design Package cannot find them.
They remain a complement, not a substitute.

### Writing the records in Portuguese and translating before submission

Rejected. See ADR-0011.

## Consequences

### Positive

- The Design Package's justification sections are assembled from records that
  already exist, rather than written from scratch under deadline.
- A decision has one location, reachable from the traceability matrix.
- New members read the ADR index and learn the system's constraints in order of
  importance.

### Negative

- Writing a record costs 20-40 minutes at the moment one would rather keep
  coding. This is real and is the reason most teams do not do it.
- Over-application is a failure mode. A record for a decision that is not
  architectural dilutes the index and makes the important ones harder to find.
  The qualifying test above exists to bound this.

### Follow-up required

- Keep the index table in `docs/adr/README.md` current; it is the entry point.
- Generate `docs/vv/traceability-matrix.md` against the requirement IDs cited
  in these records.

## References

- CubeDesign 2026 rules, §8.1-§8.3 and the HLR catalogue (`docs/cubedesign2026.pdf`)
- Michael Nygard, "Documenting Architecture Decisions" (2011), the original
  formulation of this practice
