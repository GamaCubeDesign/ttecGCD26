# Link Budget

**Requirements:** HLR-COMM-03
**Status:** calculated; bench and on-site measurements pending
**Reproduce:** `python3 tools/analysis/lora_budget.py`

## 1. Parameters

| Parameter | Value | Note |
|---|---|---|
| Frequency | 433.92 MHz | TTC-01: the centre of the 433.05-434.79 MHz allocation (ADR-0015) |
| Transmit power | 14 dBm | TTC-03: flight power, commandable 2..20 dBm (ADR-0015) |
| Antenna gain | 2.15 dBi each end | half-wave dipole |
| Losses | 2.3 dB total | baluns, cables and connectors, both ends combined |
| Bandwidth | 125 kHz | ADR-0004 |
| Propagation model | free space | see §4 for why this is optimistic and why it does not matter |

## 2. Receiver sensitivity

SX1276/78 datasheet typical values at BW 125 kHz:

| SF | Sensitivity | Profile |
|---|---|---|
| 7 | -123 dBm | `GAMA_RATE_FAST` |
| 9 | -129 dBm | `GAMA_RATE_NOMINAL` |
| 12 | -137 dBm | `GAMA_RATE_SAFE` |

## 3. Margin

| Distance | FSPL | Rx power | Margin SF7 | Margin SF9 | Margin SF12 |
|---|---|---|---|---|---|
| 10 m | 45.2 dB | -29.2 dBm | 93.8 dB | 99.8 dB | 107.8 dB |
| 50 m | 59.2 dB | -43.2 dBm | 79.8 dB | 85.8 dB | 93.8 dB |
| 100 m | 65.2 dB | -49.2 dBm | 73.8 dB | 79.8 dB | 87.8 dB |
| 500 m | 79.2 dB | -63.2 dBm | 59.8 dB | 65.8 dB | 73.8 dB |
| 1 km | 85.2 dB | -69.2 dBm | 53.8 dB | 59.8 dB | 67.8 dB |
| 5 km | 99.2 dB | -83.2 dBm | 39.8 dB | 45.8 dB | 53.8 dB |

Until 2026-10-24 this table was at 433 MHz and 20 dBm with 2 dBi and 2 dB
(ADR-0004): 6 dB more margin everywhere. ADR-0015 explains the change.

## 4. Degradation allowance

Free space is optimistic for an indoor test. The chosen profile is defended by
showing the margin survives every degradation we can justify, applied at once
and at 100 m — ten times the distance the competition requires:

| Source | Allowance |
|---|---|
| Indoor multipath and obstruction | 20 dB |
| Antenna mismatch and polarisation loss inside a metal-framed 1U | 20 dB |
| 433 MHz ISM interference | 10 dB |
| CubeSat antenna detuned by its own structure | 10 dB |
| Unmodelled | 10 dB |
| **Total** | **70 dB** |

SF9 at 100 m: 79.8 - 70 = **9.8 dB remaining**. SF7: 3.8 dB. Both close; if
the site turns out worse than this, `SET_TX_POWER 20` buys back 6 dB from the
ground.

This is why the design does not need SF12's extra 8 dB, and why paying an 11x
data-rate penalty for it was the previous mission's binding constraint
(ADR-0004).

## 5. Open items

- Bench-measure packet error rate over 1000 frames per profile at 1 m and at
  the expected operating distance.
- Confirm both RA-02 at 433.92 MHz on the bench, and the current drawn at
  14 dBm for the power budget.
- Measure the 433 MHz noise floor at the venue before the test. The 10 dB
  interference allowance above is an estimate, and 433 MHz is a crowded band.
- Confirm the actual antenna gain and pattern once the 1U structure is closed;
  the 20 dB mismatch allowance is deliberately pessimistic but unverified.
