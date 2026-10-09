# ADR-0011: English for Design Package documents, Portuguese for working notes

- **Status:** Accepted
- **Date:** 2026-09-19 (decision agreed 2026-09-15)
- **Requirements:** REG-04 in `docs/requisitos.md`; HLR-SYS-01
- **Deciders:** TT&C team

## Context

The competition rules require it without an HLR identifier: "Documentation
must be submitted in English." (rules §2, *Reglamentación General*; REG-04 in
`docs/requisitos.md`).

The team works in Portuguese. The OBC repository had already recorded the
debt: `obc/docs/log_schema.md` opens by noting that "O Design Package da
competição precisa desta seção em inglês".

The Design Package is assembled from documents in this repository (ADR-0001),
and it is due on 2026-09-27.

## Decision

| Artifact | Language |
|---|---|
| Code, identifiers, comments | English |
| ADRs, ICDs, budgets, V&V plan, traceability matrix | English |
| `README.md` files, `PLANO.md`, team reports in `docs/relatorios/`, `docs/requisitos.md` | Portuguese |
| Quotations of normative text | the source's own language, verbatim |

The deciding test is whether the document enters the Design Package. If it
does, it is written in English from its first draft.

## Rationale

**Translation is interpretation.** A document written in Portuguese and
translated at the end exists in two versions, and the version the jury reads
is the one nobody reviewed while the design was being made. Writing once, in
the language of the deliverable, means the reviewed text is the submitted
text.

**The alternative concentrates risk at the worst moment.** Translating every
ADR, ICD and budget would land in the last days before 2026-09-27, when the
same people are finishing the firmware the Design Package must describe.

**Working notes are for the team.** Planning, status reports and the
requirements catalogue are read by the team daily and never submitted. Writing
them in the team's language costs nothing and makes them faster to use.

**Quotations stay in the original** because a translated requirement is a
paraphrase, and the jury evaluates against the original text.
`tools/analysis/check_requirements.py` enforces this for `docs/requisitos.md`.

## Alternatives considered

### Everything in Portuguese, translated before submission

Rejected for the two reasons above: two diverging versions, and the
translation effort landing in the final week.

### Bilingual Design Package documents

Rejected. Twice the maintenance, and the two halves drift the first time
someone updates one and forgets the other — exactly the incoherence
HLR-SYS-01 is written against.

## Consequences

### Positive

- The Design Package documents need no translation pass.
- The reviewed text is the submitted text.

### Negative

- Team members less fluent in English write ADRs and ICDs more slowly.
- Content that moves from a working note into a DP document must be translated
  once, by hand, at that moment.
- The repository is visibly mixed-language; newcomers need this record to know
  which is intended.

## References

- CubeDesign 2026 rules, §2 (`docs/cubedesign2026.pdf`)
- `docs/requisitos.md`, REG-04
- `AGENTS.md`, section "Language"
