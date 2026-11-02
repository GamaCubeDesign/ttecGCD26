# ADR-0015: 433.92 MHz carrier and 14 dBm flight power

- **Status:** Proposed
- **Date:** 2026-10-24
- **Requirements:** HLR-COMM-03, HLR-ADS-01
- **Deciders:** TT&C team
- **Supersedes:** the carrier frequency (433 MHz) and transmit power (20 dBm)
  of ADR-0004. Its rate profiles, bandwidth and the rest of that record stand.

## Context

ADR-0004 fixed the link at 433.000 MHz and 20 dBm, the SX1278's PA_BOOST
maximum. The Critical Design Review (10 October 2026, `docs/cdr/`) set
different values, which the code had not followed:

- TTC-01: "The link operates at 433.92 MHz, with its 125 kHz channel inside
  433.05-434.79 MHz."
- TTC-03: "The flight transmit power is 14 dBm, commandable from 2 to
  20 dBm."
- The CDR's link budget (Table "Link budget of the nominal profile"): 2.15 dBi
  per end, the half-wave dipoles the team is building, and 2.3 dB of losses in
  baluns, cables and connectors at both ends.

433.000 MHz was not only a different number. A 125 kHz channel centred on it
spans 432.9375-433.0625 MHz, so its lower 12.5 kHz lie **below** the
433.05 MHz edge of the allocation ADR-0004 itself cites. 433.92 MHz is the
centre of the allocation: the channel spans 433.8575-433.9825 MHz, 807.5 kHz
from either edge.

The power has two costs that 20 dBm ignored. The radio and the ADS-B receiver
share one board, with no filter between them, and the CDR expects the
receiver to saturate while the LoRa radio transmits (its risk R6). And above
17 dBm the SX1278 needs its high-power PA DAC setting, for which the driver
raises the current limit from 100 to 140 mA (`flight/radio/sx1278.c`,
`sx1278_set_power`): the peak of the power budget. 14 dBm stays on the
100 mA setting.

## Decision

- `GAMA_LORA_FREQ_HZ` is **433 920 000** (`common/gama_lora.h`). The SX1278's
  Frf register is 0x6C7AE1, 433.919983 MHz.
- The default transmit power of `ttcd` is **14 dBm** (`ttcd_params_default`,
  `ttcd.conf.example`). `SET_TX_POWER` still accepts 2 to 20 dBm; the bench
  keeps using 2 dBm with the radios a metre apart.
- `tools/analysis/lora_budget.py` and `docs/budgets/link-budget.md` use the
  CDR's flight parameters: 433.92 MHz, 14 dBm, 2.15 dBi per end, 2.3 dB.
  ADR-0004's own figures are still checked, with ADR-0004's parameters.

## Rationale

1. **Inside the allocation, by the widest margin there is.** Centring the
   channel puts 807.5 kHz between it and either edge, far beyond the
   crystal's ±10 ppm (±4.3 kHz) and any drift with temperature.
2. **14 dBm still closes the link with room to spare.** Free-space margin at
   SF9 (`lora_budget.py`):

   | Distance | Rx power | Margin SF7 | Margin SF9 | Margin SF12 |
   |---|---|---|---|---|
   | 10 m | -29.2 dBm | 93.8 dB | 99.8 dB | 107.8 dB |
   | 100 m | -49.2 dBm | 73.8 dB | 79.8 dB | 87.8 dB |
   | 1 km | -69.2 dBm | 53.8 dB | 59.8 dB | 67.8 dB |

   After the 70 dB of pessimistic allowances of `link-budget.md` §4, applied
   at once at 100 m — ten times the distance the test requires — SF9 keeps
   9.8 dB and SF7 3.8 dB. The 6 dB given up against ADR-0004's 20 dBm buys
   less self-interference into the ADS-B receiver and a lower current peak.
3. **The power stays commandable.** If the link turns out worse on site than
   the allowances assume, `SET_TX_POWER 20` restores ADR-0004's 6 dB from the
   ground without a software change.

## Alternatives considered

### Keep 433.000 MHz

Lost: part of its channel is outside the allocation (Context).

### Keep 20 dBm as the flight default

The 6 dB are not needed to close the link (Rationale 2), and they are paid
for in current and in desensitisation of the ADS-B receiver on the same
board. The CDR promised 14 dBm.

## Consequences

### Positive

- The link meets TTC-01 and TTC-03 as the CDR states them.
- The ADS-B receiver sees 6 dB less of our own transmitter.

### Negative

- 6 dB less margin than ADR-0004 quoted: 9.8 dB instead of 15.8 dB at SF9
  after the 70 dB of allowances.
- Bench logs from before this change (`docs/vv/bancada/2026-10-07`, `-09`)
  were taken at 433.000 MHz. Both RA-02 changed together, so their results
  stand, but they are not at the flight frequency.
- Any ground built from an older `common/` hears nothing. The ESP32 ground
  (PLANO etapa 3) is written against this one.

### Follow-up required

- On the bench: confirm both RA-02 at 433.92 MHz (PER at 1 m, step 3 of the
  radio bench) and measure the current at 14 dBm for the power budget.
- On site: the 433 MHz noise floor before the test (ADR-0004's follow-up
  still stands).

## Verification

| What | Where | Result |
|---|---|---|
| Frf registers 0x6C 0x7A 0xE1 after init | `test_sx1278` | pass |
| `ttcd` boots at 14 dBm; the configuration default is 14 | `test_ttcd_core`, `test_config` | pass |
| Margins 99.8 / 79.8 / 59.8 dB at SF9 and 9.8 dB after the allowance, as the CDR quotes; ADR-0004's figures with its parameters | `lora_budget.py --check` (in CTest) | pass |
| Both RA-02 exchange frames at 433.92 MHz | — | to do |

## References

- ADR-0004 — the profiles this keeps; the frequency and power this replaces.
- CDR v4 (`docs/cdr/`): TTC-01, TTC-03, "Link budget of the nominal profile".
- `docs/budgets/link-budget.md`, `tools/analysis/lora_budget.py`.
