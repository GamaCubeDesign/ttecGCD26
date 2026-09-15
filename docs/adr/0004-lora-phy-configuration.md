# ADR-0004: LoRa PHY configuration and rate profiles

- **Status:** Accepted
- **Date:** 2026-09-15
- **Requirements:** HLR-COMM-01, HLR-COMM-03, HLR-ADS-05, HLR-ADS-08
- **Deciders:** TT&C team (LoRa sub-team)

## Context

The radio is an Ai-Thinker RA-02 (Semtech SX1278) at **433 MHz**. The module
and the band are fixed: the RA-02's matching network is tuned for 433 MHz, and
both units are already procured.

Spreading factor, bandwidth, coding rate and output power are **not** fixed.
They are values written to `REG_MODEM_CONFIG_1`, `REG_MODEM_CONFIG_2` and
`REG_PA_CONFIG`, and the driver inherited from the previous mission already
exposes the full range as enumerations
(`ultima_missao/satellite/LoRa.h:70-97`: `BW7_8` through `BW500`, `SF7`
through `SF12`, `CR5` through `CR8`). Changing them costs a register write.

The previous mission ran **SF12 / BW 62.5 kHz / CR 4:8**
(`ultima_missao/satellite/Moden.cpp:49-62`). No record exists of why. That
configuration yields:

- **23.3 seconds** of air time for a single 208-byte frame;
- **71 bps** of goodput;
- **2.6 KB** total downlink capacity across the whole 10-minute mission.

Against the mission's 5 MB of ADS-B data (ADR-0003), 2.6 KB is not a tight
budget — it is three orders of magnitude short. This one inherited setting, not
the payload and not the processing, was the binding constraint on the entire
mission.

The reason it is so costly is that it is buying sensitivity. SF12 at 62.5 kHz
reaches about -140 dBm. The link budget says what that is worth here:

| Distance | Rx power | Margin at SF7 | Margin at SF9 | Margin at SF12 |
|---|---|---|---|---|
| 10 m | -23.2 dBm | 99.8 dB | 105.8 dB | 113.8 dB |
| 100 m | -43.2 dBm | 79.8 dB | 85.8 dB | 93.8 dB |
| 1 km | -63.2 dBm | 59.8 dB | 65.8 dB | 73.8 dB |

(20 dBm PA_BOOST, 2 dBi at each end, 2 dB of feedline and connector loss, free
space.) The competition ground station sits tens of metres from the CubeSat,
and teams bring their own (§9 of the rules). At 100 m, SF9 already has 86 dB of
margin. The previous configuration was paying a 22x data-rate penalty for 8 dB
of sensitivity it had eighty-five decibels of surplus in.

## Decision

Three fixed profiles, selectable from the ground by telecommand
(`GAMA_TC_SET_RATE`). All at **BW 125 kHz, 433 MHz, 20 dBm PA_BOOST, explicit
header, PHY CRC enabled, sync word 0x12, preamble 8**:

| Profile | SF | CR | ToA (208 B) | Goodput | Capacity in 600 s @50% | Sensitivity |
|---|---|---|---|---|---|---|
| `GAMA_RATE_SAFE` | 12 | 4:8 | 11.67 s | 143 bps | 5.2 KB | -137 dBm |
| `GAMA_RATE_NOMINAL` | 9 | 4:5 | 1.05 s | 1592 bps | 58.3 KB | -129 dBm |
| `GAMA_RATE_FAST` | 7 | 4:5 | 0.33 s | 5074 bps | 185.8 KB | -123 dBm |

- **NOMINAL** is the default and carries the mission. It is what the 58 KB data
  budget in ADR-0003 is built on.
- **FAST** is commanded for the post-mission bulk download of the raw log,
  where the ground station is at a known-good range and throughput dominates.
- **SAFE** is the recovery and beacon mode. `ttcd` falls back to it
  automatically after a contact timeout.

Two constraints on the mechanism:

1. **Only these three combinations are reachable.** `GAMA_TC_SET_RATE` takes a
   profile index, not SF/BW/CR fields.
2. **Every rate change is ACKed and reverts on timeout.** After switching, if
   nothing is heard at the new rate within the revert window, `ttcd` returns
   to the previous profile unprompted.

**Duty cycle.** Budgets assume 50% of mission time transmitting. This is not a
regulatory figure — it is a consequence of the radio being half-duplex: every
second spent transmitting is a second in which a telecommand cannot be heard
(HLR-COMM-01). The default streaming period is therefore **5 s**, not the 4.2 s
the budget would allow: 120 snapshots consume 251 s of the 300 s allocation,
leaving 49 s for housekeeping, statistics and acknowledgements.

## Rationale

The selecting criterion is: **take the lowest spreading factor whose link
margin remains large after every pessimistic assumption we can justify, and
spend the rest on data rate.**

Applied to SF9 at 100 m, the 85.8 dB of margin absorbs, simultaneously:

- 20 dB for indoor multipath and obstruction;
- 20 dB for antenna mismatch and polarisation loss inside a metal-framed 1U;
- 10 dB of 433 MHz ISM interference, which is a crowded band;
- 10 dB for a CubeSat antenna detuned by its own structure;
- 10 dB of arbitrary margin for what we have not thought of.

That is 70 dB of accumulated pessimism, and SF9 still closes with 16 dB to
spare — at ten times the distance the competition actually requires. SF12 would
add 8 dB to an already unusable surplus and cost 11x the data rate.

**Why BW 125 kHz and not 250 or 500.** 125 kHz is the best-characterised
SX127x setting, it sits comfortably inside the 433.05-434.79 MHz allocation,
and the datasheet's sensitivity figures are specified for it. Wider bandwidth
would buy more rate, but 500 kHz occupies almost a third of the band, and
SX1276-family errata carry bandwidth-dependent register workarounds in the
lower band that we have no time to characterise before the on-site test. The
data budget closes at 125 kHz; there is no reason to spend schedule risk on
rate we do not need.

**Why CR 4:5 and not 4:8.** Forward error correction is the wrong tool at this
margin. At 86 dB of surplus, errors are not thermal — they will come from
interference bursts, which overwhelm 4:8 as readily as 4:5. The PHY CRC turns
those into detected losses, and the application counts them (HLR-ADS-08). SAFE
retains 4:8 because it is the profile that must work when our assumptions are
wrong.

**Why fixed profiles rather than free parameters.** The failure mode is
asymmetric and unrecoverable: a malformed or partially-received rate command
that leaves the two ends on settings that cannot hear each other ends the
mission, because the only channel for the correction is the one that just
broke. Three validated profiles plus a revert timer bounds this. The revert
timer is the part that actually saves us; the restricted parameter space just
makes the state space small enough to test exhaustively.

## Alternatives considered

### Keep SF12 / BW 62.5 kHz / CR 4:8

The inherited configuration. Rejected: 2.6 KB over the mission makes the
primary objective unreachable regardless of how good the payload is. It is
kept in `tools/analysis/lora_budget.py` as the comparison baseline.

### A single fixed rate, SF9

Simpler, and removes the rate-change failure mode entirely. Rejected because
the bulk download of a multi-megabyte raw log at 1592 bps would take hours, and
because losing SAFE means losing the only graceful response to a link that
turns out worse than modelled. The revert timer is what makes multiple profiles
affordable.

### Automatic rate adaptation from RSSI/SNR

Standard practice in LoRaWAN, and tempting. Rejected on schedule: an adaptation
loop is a control system with its own stability and oscillation failure modes,
it needs far more link-condition data than we will have gathered by the test,
and a wrong adaptation is indistinguishable from a link failure. Ground-
commanded selection gives the same capability with a human in the loop.

### Moving to 915 MHz

Would sidestep the crowded 433 MHz band and suit ITU Region 2, where the test
takes place. Rejected: the RA-02 is a 433 MHz part with a matched front end,
both units are procured, and the band was fixed by the team before this
decision. Recorded here because it is the first question a reviewer will ask,
and because a future mission should revisit it.

## Consequences

### Positive

- The downlink budget rises **22x** over the inherited configuration, from
  2.6 KB to 58.3 KB, and **71x** in FAST mode, at zero hardware cost.
- The mission's binding constraint moves off the radio and onto onboard
  processing, where we control it.
- SAFE preserves a recovery path that is more robust than the previous
  mission's nominal setting.

### Negative

- **Three profiles is three times the link states to test**, plus the
  transitions between them. The revert timer must be exercised deliberately,
  including the case where the ACK is lost but the switch happened.
- **Reduced sensitivity is a real reduction.** If the ground station turns out
  to be far more distant or more obstructed than assumed, NOMINAL may not
  close where the inherited configuration would have. SAFE covers this, but
  recovery costs an operator action and time.
- **The 50% duty assumption is a design constraint, not a measurement.** If
  telecommand traffic is heavier than expected, snapshot cadence must give way.
- **433 MHz remains a crowded band** and we have not characterised the noise
  floor at the venue. The 10 dB allowance above is an estimate.

### Follow-up required

- Bench-measure actual time on air for all three profiles against the
  calculated values, and packet error rate over 1000 frames at each.
- Exercise every rate transition including the lost-ACK case, and measure the
  revert time.
- Measure the 433 MHz noise floor on arrival at the venue, before the test.
- Replace `LoRa_send()`'s `sleep(Tpkt/1000 + 1)` busy wait
  (`ultima_missao/satellite/Moden.cpp:91`) with a DIO0 TxDone interrupt; at
  SF12 that sleep rounds away up to a second of air time per packet.

## References

- `tools/analysis/lora_budget.py` — all figures; `--check` verifies them
- `docs/budgets/link-budget.md`, `docs/budgets/data-budget.md`
- `common/gama_tc.h` — `gama_rate_profile_t`, the three reachable profiles
- SX1276/77/78/79 datasheet (Semtech rev. 7), §4.1.1.7 time on air, §5.5.4 PA
- `ultima_missao/satellite/LoRa.h:70-97` — the driver's existing enumerations
