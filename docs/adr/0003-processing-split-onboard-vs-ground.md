# ADR-0003: Hybrid processing, split at the track-state boundary

- **Status:** Accepted
- **Date:** 2026-09-15
- **Requirements:** HLR-ADS-04, HLR-ADS-02, HLR-ADS-05, HLR-ADS-07, HLR-ADS-08, HLR-SW-02
- **Deciders:** TT&C team (ADS-B and LoRa sub-teams)

## Context

HLR-ADS-04 requires us to implement **and justify** a processing architecture,
choosing between onboard, ground, and hybrid. The rules present these as three
comparable options. The arithmetic does not.

**What has to be moved.** HLR-ADS-05 fixes the mission at 10 continuous minutes
with up to 20 aircraft. ADS-B airborne position and airborne velocity messages
are each nominally broadcast at 2 Hz, with identification every 5 seconds, so a
tracked aircraft produces roughly 4 messages per second:

| Message rate | Messages in 600 s | As SBS-1 / NDJSON lines |
|---|---|---|
| 2 msg/s/aircraft | 24 000 | 2.52 MB |
| 4 msg/s/aircraft | 48 000 | 5.04 MB |
| 6 msg/s/aircraft | 72 000 | 7.55 MB |

**What the link can move.** The radio is fixed at an SX1278 in the 433 MHz
band (ADR-0004). Its best usable configuration delivers **58 KB** over the
mission, and that figure already assumes half the mission is spent
transmitting.

The gap is between **44:1 and 133:1**, centred on **88:1** at the nominal
message rate. Reproduce with `python3 tools/analysis/lora_budget.py`.

There is a second, subtler point that decides the shape of the answer. The
phrase "process on the ground" is ambiguous between two very different things:

- *decoding and aggregating* the ADS-B stream, and
- *analysing* the resulting tracks.

Only the second can move. Moving decoding to the ground would require the raw
stream to cross the link first, which is the one thing the link cannot do.
Ground-side processing does not relieve a downlink bottleneck; it sits behind
it.

## Decision

The processing architecture is **hybrid**, with the split placed at the
**track-state boundary**:

**Onboard** (Raspberry Pi Zero 2 W, `adsbd`):
1. Demodulate and decode 1090ES with `dump1090-fa` over the RTL-SDR.
2. Parse SBS-1 into per-message records and timestamp each on arrival
   (HLR-ADS-07).
3. Append every message to an NDJSON log held on the SD card — the complete,
   unreduced record (HLR-SW-02).
4. Maintain a table of current state per ICAO address, and emit a 20-byte
   track record per aircraft on demand.

**Downlinked** (`ttcd`, every 5 s during the mission):
- One snapshot of all tracked aircraft: 2 frames, 414 bytes, 2.09 s of air time.
- Housekeeping, mission statistics and telecommand acknowledgements in the
  remaining air time.

**On the ground** (ESP32 plus host):
- Trajectory reconstruction from the received track points.
- Origin and destination estimation.
- Dashboard, plots and the post-test report.

The unreduced NDJSON log is retrieved over USB or SSH after the mission, not
over the radio. The rules require mission data to be "structured, documented,
reproducible and available for post-analysis"; they do not require it to
arrive by RF.

## Rationale

**Why the split is at track state, and not somewhere else.** Each candidate
boundary was measured against the 58 KB budget:

| Downlink the... | Volume | Fits? |
|---|---|---|
| Raw IQ samples | ~2.9 GB (2.4 MS/s, 16 bit) | 50 000x over |
| Raw 1090ES frames | ~0.7 MB (14 bytes each) | 12x over |
| Decoded SBS-1 lines | 5.04 MB | 88x over |
| **Track state, 5 s cadence** | **58.0 KB** | **exactly fills the allocation** |
| One final summary per aircraft | 0.4 KB | fits, but no trajectory |

Track state is the first boundary that fits, and the last one that still
carries a trajectory. Anything coarser — a single summary per aircraft at the
end of the mission — would satisfy the letter of "transmitting processed data
as telemetry" while making trajectory reconstruction impossible, which is a
named primary objective in §4.4.1 of the rules.

**What the chosen cadence buys.** 120 snapshots over the mission is 120
trajectory points per aircraft: one every 5 seconds for ten minutes. At a
typical 450 kt cruise that is a fix roughly every 1.2 km, which reconstructs a
trajectory with no meaningful loss of shape.

**The reduction is 89:1**, achieved entirely onboard, and it is what makes the
mission possible on this radio.

## Alternatives considered

### Fully onboard

Decode, aggregate, *and* analyse on the Pi; downlink only conclusions such as
estimated origin and destination. Rejected on two grounds. It puts trajectory
fitting on a 512 MB single-board computer that is already sharing four cores
with `dump1090`, for no gain — the analysis inputs are the same track points
we are downlinking anyway. And it makes the result unauditable: a judge can
check a trajectory we reconstruct from downlinked points, but can only take on
faith a conclusion computed out of sight.

### Fully on the ground

Downlink the ADS-B stream and do everything on the ground. Rejected
arithmetically: 88x over budget. This is the option the rules list first and
the one the numbers eliminate most decisively, which is why the calculation is
reproduced above rather than asserted.

### Store everything, downlink nothing during the mission

Run the mission silently, then bulk-download afterwards. Rejected: HLR-ADS-08
requires a *maximum acceptable latency between reception and telemetry
transmission* as a defined success criterion, and this option's latency is the
mission duration. It also forfeits the live demonstration the on-site test is
scored on. We keep the bulk-download path (`GAMA_FRAME_BULK_DATA`) as a
post-mission capability, not as the primary mechanism.

### Adaptive downlink: send only aircraft whose state changed

An obvious optimisation, and a real one — at cruise, most of a track record is
unchanged between snapshots. Rejected **for this mission** because it makes
data loss self-concealing: with no change-detection, a missing aircraft in a
snapshot is a lost frame; with it, a missing aircraft is indistinguishable
from an aircraft that did not move. HLR-ADS-08 requires us to *measure* the
loss rate, and we are not willing to trade that for bandwidth we do not need.
Worth revisiting if aircraft count ever exceeds the budget.

## Consequences

### Positive

- The mission fits the link with the correct order of magnitude of margin,
  rather than by a few percent.
- Trajectory reconstruction, origin and destination estimation — the
  judged deliverables — happen on a laptop with real tooling.
- The complete unreduced dataset survives onboard, so post-analysis is not
  limited by what the radio carried (HLR-SW-02).
- Quantisation is explicit and documented (ADR-0007), so the ground can state
  the error bars on every reconstructed position.

### Negative

- **Position is quantised** to about 1.2 m in latitude and 2.4 m in longitude,
  and time to 100 ms. Below ADS-B's own accuracy, but it is a real loss of
  information relative to the onboard log, and any comparison between
  downlinked and stored data must account for it.
- **A lost frame loses up to 12 aircraft** for one snapshot. We accept this
  rather than adding retransmission to the live stream, because the next
  snapshot is 5 seconds away and a retransmit would cost more air time than
  the data is worth. Sequence numbers make the loss countable.
- **Onboard CPU becomes mission-critical.** `dump1090` at 2.4 MS/s on a Pi
  Zero 2 W is the largest unmeasured risk in this architecture. Measurement is
  scheduled before the design is frozen; the fallback is 2.0 MS/s.
- **The bulk download path is needed but not on the critical path**, which
  means it is the component most likely to be under-tested.

### Follow-up required

- Measure `dump1090` CPU and decode rate on the target hardware; record the
  result in `docs/budgets/data-budget.md`. If a core is saturated, drop to
  2.0 MS/s and re-measure before freezing.
- Define and measure the HLR-ADS-08 success metrics against this design:
  decode rate >= 95%, downlink loss <= 5%, and p95 latency from message
  reception to the start of the transmission carrying it <= 5 s (the 5 s
  snapshot period bounds this at 4.75 s by construction; end-to-end receipt on
  the ground adds the 2.09 s snapshot air time).

## References

- `tools/analysis/lora_budget.py` — every figure above; `--check` verifies them
- `docs/budgets/data-budget.md`
- ADR-0004 — the radio configuration that sets the 58 KB budget
- ADR-0008 — which process owns the SDR
- CubeDesign 2026 rules §4.4, HLR-ADS-01 through HLR-ADS-08

## Erratum — 2026-09-19

The decision in this record is unchanged, and every correction below makes the
case for onboard processing stronger. The record stays as written; these are
the figures that were wrong.

**Cause.** The raw volumes assumed 110 bytes per NDJSON line. The line format
of `adsb_capture.c`, reproduced by `tools/analysis/lora_budget.py` for a
representative aircraft and weighted by the ADS-B broadcast mix, is
**160.4 bytes** — 156 B for position, 170 B for velocity, 109 B for
identification. Separately, two figures were taken from the 4.2 s snapshot
cadence first considered, not the 5 s cadence the design actually flies.

| Where | As written | Corrected |
|---|---|---|
| Context, volume at 2 / 4 / 6 msg/s | 2.52 / 5.04 / 7.55 MB | 3.67 / 7.34 / 11.02 MiB |
| Context, gap over the NOMINAL capacity | 44:1 to 133:1, centred on 88:1 | 65:1 to 194:1, centred on 129:1 |
| Rationale table, "Decoded SBS-1 lines" | 5.04 MB, 88x over | 7.34 MiB, 129x over |
| Rationale table, "Raw 1090ES frames" | 12x over | 11x over |
| Rationale table, "Track state, 5 s cadence" | 58.0 KB, exactly fills the allocation | 48.5 KB, 83% of the 58.3 KB allocation; the rest carries housekeeping, statistics and acknowledgements (`data-budget.md` §5) |
| Rationale, "The reduction is 89:1" | 89:1 | 155:1 — raw volume over the 48.5 KB actually downlinked at the 5 s cadence |
| Alternatives, "Fully on the ground" | 88x over budget | 129x over budget |

`lora_budget.py --check` verifies the corrected figures. ADR-0004 quotes the
same understated volume and carries its own erratum.
