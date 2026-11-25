# ADR-0024: The ground estimates the battery's charge, autonomy and charging from the housekeeping

- **Status:** Proposed
- **Date:** 2026-11-25
- **Requirements:** HLR-EPS-04 (safe battery conditions, by telemetry
  analysis), HLR-EPS-02 / REG-09 (charging from a light source: current and
  voltage evaluated), HLR-EPS-01 (battery for the whole mission)
- **Deciders:** TT&C/OBC team

## Context

The OBC's INA219 (the new one fitted on 2026-11-25 reads; the previous one gave
no ACK, `docs/relatorios/2026-11-24-sensores-obc.html`) reports voltage and
current in every `TM_HK`, every 10 s: `battery_mv` (u16, mV) and `current_ma`
(i16, mA), "no data" as `0xFFFF` / `0x8000`. The team wants three figures from
them: the battery's state of charge in percent, the autonomy at the present
load, and how the charge evolves, charging included.

The pack is 2S1P Panasonic NCR18650B: the EPS's BMS is 2S and each battery
board carries one cell (`hardware/README.md`). Datasheet: 3200 mAh minimum,
3350 mAh typical (0.2 C, 20 °C), 2.50 V discharge cut-off, 4.20 V charge.

Two facts of the hardware decide what can be estimated:

1. **The INA219 measures the load, not the battery** (finding A9). It sits on
   the 8 V line from the EPS to the stack, after the BMS and the charger. Its
   current is what the satellite draws and is never negative; the charge
   current from the light source never passes through it.
2. **The voltage is measured under load,** after the cells' internal
   resistance, the BMS switches, the wiring and the 100 mΩ shunt: about
   0.25 Ω for the pack. At 600 mA that is 0.15 V, ~20 % of state of charge in
   the curve's plateau if not corrected.

The radio budget is fixed (58 KB per mission, `docs/budgets/data-budget.md`).

## Decision

The estimate is computed **at the ground**, from the HK already received, in
`ground/host/battery.py` (pure, stdlib), fed by `GsState` and shown in the
page's OBC tab. Nothing changes on board or on the wire.

- **Voltage to charge:** the open-circuit voltage of a cell is estimated as
  `V/2 + I·R/2` and mapped to percent through a piecewise-linear NCR18650B
  curve (`CELL_OCV`, 0 % at 3.00 V, 100 % at 4.18 V).
- **Discharging:** coulomb counting of the INA219's current (trapezoid,
  timed by the satellite's `uptime_s`), anchored on the first valid voltage and
  pulled towards the voltage's figure by 2 % of the difference per sample.
- **Charging:** the level from the voltage reads high by the unknown charge
  current times R, but its slope does not. The estimate then follows the
  slope of the open-circuit voltage over the last 5 min, converted to %/h
  through the curve's slope at the estimate. The charge current is estimated
  as `slope × capacity + load`.
- **Charging is detected** by a jump of the voltage's figure of 3 % or more
  between two HK with the load steady (±50 mA), the charger switched on or
  off; and by the trend over 5 min exceeding what the load predicts by
  10 %/h, in at 10 and out at 5, for a charge that starts with a load change.
- **A load step of 100 mA or more** restarts the trend window, so that an
  R mis-set cannot read as a charge, and measures R as `−ΔV/ΔI`. The median of
  the last 15 is shown next to the R in use. R is not adapted automatically.
- **Autonomy** is `charge × capacity / mean load` over the last 5 min;
  charging, the time to full is `(100 − charge) / trend`.
- **Gaps** (no data, the AOCS lease of ADR-0017) keep counting at the last
  load for up to 900 s; a longer gap or a reboot (`uptime_s` going back)
  re-anchors on the voltage.
- Defaults: 3200 mAh (the minimum, so the autonomy errs short) and 0.25 Ω.
  `gs_web.py --battery-mah`, `--battery-r-ohm` override them.

## Rationale

1. **Zero bytes over the radio, no protocol change.** The inputs are already in
   `TM_HK`; adding the percent on board would change the 27 B record, the test
   vectors and the budget for a figure the ground can compute.
2. **The OBC does not need it to act.** No mode transition of Figure 44 depends
   on the state of charge; if one ever does, the estimate moves on board with
   its own ADR.
3. **Measured against a synthetic pack** (`ground/host/tests/host/test_battery.py`,
   the same curve, the INA219's 4 mV step and ±10 mA noise):

| Case | Result |
|---|---|
| 600 mA for 10 min, R right | charge within 1.5 %, 100 mAh counted within 3 |
| R mis-set (true 0.30, model 0.25) | worst charge error 2.4 % over 1 h with two charges |
| R mis-set (true 0.40) | worst error 7.1 %: why R is measured and shown |
| charger 1200 mA, load 500 mA | estimated charge current 1150–1250 mA; level within 3 % |
| charger 600 mA, load 500 mA | 567 mA |
| load steps of 500 mA, R 0.32 Ω | R measured 0.32 ± 0.03 |

## Alternatives considered

### The percent on board, in `TM_HK`

Costs a protocol bump (vectors, budget, ADR-0013's record) days before
the on-site tests, for a figure that the ground computes from bytes it already
receives. Rejected while no onboard decision needs it.

### Voltage only

Stateless, survives any reboot, but under load it depends entirely on R and
cannot show the 1–5 % that a 10-min mission consumes against the INA219's and
the curve's noise. Kept as the anchor and the slow correction.

### Coulomb counting only

Drifts with no correction, needs an exact starting charge, and is blind to
charging, which the INA219 cannot see. Kept for discharge only.

## Consequences

### Positive

- The page shows charge, autonomy, trend, estimated charge current and state
  in the OBC tab, in every replay of a recorded log too (`python3 battery.py
  LOG.jsonl` prints it per HK).
- R is measured from the mode changes' load steps, so the bench calibrates the
  estimate without extra hardware.

### Negative

- The curve is the chemistry's typical shape, not measured on our cells:
  ~10 % in the plateau until calibrated.
- During charging the figure rests on the slope alone: the first 2 min after a
  charge starting with a load change pull the level towards the voltage's
  high reading (~4 % in the simulation).
- The charge current is inferred, not measured; REG-09 evaluates current and
  voltage, and the jury may want a meter on the charge line.
- In the CV phase of a charge the voltage stops rising while the charge still
  increases: the estimate then understates the charge rate.

### Follow-up required

- Bench, with the satellite assembled: a known load step (`mode` changes) to
  read R from the page, then `--battery-r-ohm` with it; a discharge from full
  against the counted mAh to check the capacity and the curve.
- If the EPS adds a current sensor on the charge line, replace the inferred
  charge current with it.

## References

- `ground/host/battery.py`, `ground/host/tests/host/test_battery.py`
- `hardware/README.md` (A9), `docs/relatorios/2026-11-24-sensores-obc.html`
- Panasonic NCR18650B datasheet (capacity, cut-off)
- ADR-0013 (`TM_HK`), ADR-0017 (the AOCS lease)
