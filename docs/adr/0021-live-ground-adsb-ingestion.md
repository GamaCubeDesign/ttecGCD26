# ADR-0021: Feed the aircraft estimators from the ground's existing event source

- Status: Proposed
- Date: 2026-11-12
- Requirements: HLR-ADS-04, HLR-ADS-07, HLR-ADS-08

## Context

The browser console owns the serial bridge and mission commands, while the
aircraft estimators previously required a separately prepared NDJSON input.
Opening the same ESP32 serial port twice cannot provide a reliable live chain.

## Decision

An explicit ADS-B mode of gs_web journals the existing source's JSON events to
a unique per-run directory. A consumer thread reconstructs observations, counts
invalid records, suppresses repeated snapshots and invokes AircraftManager.
The existing bounded process pool computes route pairs and fusion; idle polling
publishes completed jobs even when the radio is silent. Both dashboards share
one HTTP server and one serial owner. The launcher enables this mode.

Reference databases remain read-only and are checked before connecting. Each
run has its own telemetry SQLite database and state export. Startup errors
prevent command acceptance; fatal consumer errors block subsequent mission
starts and remain visible. Raw reception continues while analysis is behind.
Normal shutdown stops the producer before draining the journal and closing the
estimator. Replay uses historical timestamps and cannot send commands.

The existing console retains SET_TIME/ACK/SET_MODE ordering (ADR-0020). This
change adds no radio fields or traffic. Roster callsigns are available for five
minutes; the wire roster carries no emitter category, so it remains unknown.

## Consequences

Disk usage grows with the received log. Journaling is line-flushed but not
fsynced per frame; abrupt power loss can lose buffered writes. A failed consumer
needs operator intervention and replay of its log. Full pair-database geometry
validation remains in the pair worker, and worker errors are shown by the
existing aircraft panel. Hardware end-to-end validation remains outstanding.

## Verification

Tests exercise the HTTP mission command through a fake ground subprocess into
the live aircraft view, real pair calculations on a small database, combined
output after idle polling, malformed records and missing database startup.
