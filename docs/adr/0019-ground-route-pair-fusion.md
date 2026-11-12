# ADR-0019: Ground route-pair analysis and phase-aware fusion

- Status: Proposed
- Date: 2026-11-11
- Requirements: HLR-ADS-04 (processing split); this change does not establish flight validation.

## Context

Short observed tracks can fit multiple airport pairs. The existing local airport
estimator has useful terminal-phase evidence but cannot reliably distinguish
cruise destinations. The radio and onboard track format remain unchanged.

## Decision

Run directed finite great-circle candidate evaluation in bounded ground worker
processes, retaining the existing local estimator. Keep both rankings visible.
Combine endpoint support only from the same observation version. Local weights
are 0.8 near the relevant departure/arrival endpoint, 0.6 during other vertical
phases, and 0.3 during straight level flight. Missing evidence uses the available
method. Agreement adds at most 0.05; these are heuristic supports, not calibrated
probabilities. Require support 0.6, separation 0.08, three distinct position
updates spanning 20 seconds; replacement needs a 0.12 advantage and persistence.
Abstain on ambiguous endpoints. Report a complete route only when the chosen
endpoints also form a geometric candidate.

Generate the pair database locally from the versioned public reference snapshot.
Regional priority orders evaluation without increasing scores or excluding other
regions. Do not store the generated 710 MB database in Git.

## Consequences and verification

The ground requires NumPy and additional CPU/memory; generation also requires
Shapely. The 3,291-airport selection excludes smaller scheduled airports found
by the audit. Great circles do not model airway restrictions or actual services.

77 Python tests and the viewer logic tests cover geometry, queue isolation,
versioned persistence, phase weights, agreement, ambiguity, replacement and
partial decisions. This is software verification, not measured operational
accuracy or hardware validation. No over-the-air bytes are added.
