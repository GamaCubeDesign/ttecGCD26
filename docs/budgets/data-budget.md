# Data Budget

**Requirements:** HLR-ADS-05, HLR-ADS-08, HLR-COMM-03, HLR-SW-02
**Status:** calculated; onboard CPU figures pending measurement on target
**Reproduce:** `python3 tools/analysis/lora_budget.py`

## 1. Mission parameters

| Parameter | Value | Source |
|---|---|---|
| Mission duration | 600 s | HLR-ADS-05 |
| Maximum aircraft | 20 | HLR-ADS-05 |
| ADS-B message rate | ~4 msg/s/aircraft | position 2 Hz, velocity 2 Hz, ident every 5 s |
| Downlink duty cycle | 50% | half-duplex radio; see §4 |

## 2. Source volume

| Rate assumption | Messages | Raw NDJSON (160.4 B/line) |
|---|---|---|
| 2 msg/s/aircraft | 24 000 | 3.67 MiB |
| **4 msg/s/aircraft** | **48 000** | **7.34 MiB** |
| 6 msg/s/aircraft | 72 000 | 11.02 MiB |

The line size is computed, not assumed: `lora_budget.py` reproduces the
`printf` format of `adsb_capture.c` for a representative aircraft and weights
it by the broadcast mix — 156 B for a position message, 170 B for velocity,
109 B for identification. An earlier revision of this budget assumed 110 B per
line and understated every volume by 31%.

## 3. Downlink capacity

At the NOMINAL profile (SF9 / BW 125 kHz / CR 4:5, ADR-0004), a 208-byte frame
takes 1.046 s of air time and yields 1592 bps of goodput. Over 600 s at 50%
duty: **58.3 KB**.

The source is therefore **129x** the capacity at the nominal message rate.
This is the finding that forces onboard processing (ADR-0003).

## 4. Why 50% duty

Not a regulatory limit. The SX1278 is half-duplex: while transmitting, the
receiver is off and a telecommand cannot be heard. HLR-COMM-01 requires
telecommands to be executed within a bounded response time, which bounds how
long the radio may spend transmitting. 50% is the allocation; the remaining
300 s of listening time bounds telecommand latency at roughly one snapshot
period.

## 5. Allocation

Record sizes are pinned by `tests/test_vectors.c`; changing one fails that test
and this budget must be revised.

| Item | Size | Cadence | Air time | Mission total |
|---|---|---|---|---|
| Track snapshot (20 aircraft) | 2 frames, 414 B | 5 s | 2.09 s | 120 x = 251 s, 48.5 KB |
| Housekeeping (`TM_HK`) | 32 B | 10 s | 0.247 s | 60 x = 15 s, 1.9 KB |
| Statistics (`TM_STAT`) | 33 B | 30 s | 0.247 s | 20 x = 5 s, 0.6 KB |
| Telecommand ACKs | 11 B | on demand | 0.144 s | budgeted 20 x = 3 s |
| **Total** | | | | **274 s of 300 s** |

26 s of margin, about 9% of the transmit allocation.

### Why the snapshot period is 5 s and not 4.2 s

4.2 s is the fastest cadence the air-time budget permits, and it consumes 299 s
of the 300 s allocation — leaving room for one housekeeping frame across the
entire mission. 5 s costs 23 trajectory points per aircraft (143 down to 120)
and buys back the entire housekeeping, statistics and acknowledgement budget.

## 6. Reduction achieved onboard

| Stage | Volume | Ratio to source |
|---|---|---|
| Raw IQ at 2.4 MS/s | ~2.9 GB | — |
| Decoded SBS-1 (source) | 7.34 MiB | 1:1 |
| Stored onboard as NDJSON | 7.34 MiB | 1:1 (complete record, HLR-SW-02) |
| **Downlinked as track state** | **48.5 KB** | **155:1** |

## 7. Onboard storage

The NDJSON log grows at ~12.5 KiB/s, so 7.34 MiB for the mission. A 16 GB SD
card is three orders of magnitude clear of that; no rotation is needed for the
nominal mission.

`ENV_SURVIVAL` may run indefinitely, which the OBC's `docs/log_schema.md`
already flags as unbounded. `adsbd` caps its log at a configured size and stops
appending rather than filling the filesystem, because a full root filesystem
would take down `ttcd` and the radio with it.

## 8. HLR-ADS-08 success metrics

| Metric | Target | Measurement |
|---|---|---|
| Decode rate | >= 95% | decoded / injected, against the organisers' scenario |
| Downlink loss | <= 5% | sequence-number gaps in the ground log |
| Latency, reception to start of transmission (p95) | <= 5 s | bounded at 4.75 s by the 5 s period |
| Latency, reception to receipt on the ground (p95) | <= 7 s | adds the 2.09 s snapshot air time |
| Simultaneous aircraft | 20 | peak size of the track table |

## 9. Open items

- **`dump1090` CPU on the Pi Zero 2 W at 2.4 MS/s is not yet measured.** This
  is the largest unquantified risk in the architecture. Fallback: 2.0 MS/s.
- Message rate per aircraft is an assumption from the ADS-B specification, not
  a measurement of the organisers' scenario. To be confirmed on site; the
  budget holds up to 6 msg/s/aircraft.
