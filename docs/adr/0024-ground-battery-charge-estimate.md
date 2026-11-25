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
2. **The voltage is measured after a diode and under load.** On the EPS the
   BMS's P+ feeds the 8 V line through D1 (1N5822 Schottky) and the switch
   SW1; the solar boost (U2, from the panels on J3) joins P+ through D3, and
   the external input (J2) the 8 V line through D2 (netlist of
   `hardware/eps-potencia`). The INA219 module then reads after its own shunt.
   So it sees the pack's voltage minus D1's ~0.3 V, minus I·R with R the
   cells, the BMS, the switch and the shunt, about 0.25 Ω. Uncorrected, D1
   alone is ~20 % of state of charge in the curve's plateau, and I·R at
   600 mA another ~20 %.

The radio budget is fixed (58 KB per mission, `docs/budgets/data-budget.md`).

## Decision

The estimate is computed **at the ground**, from the HK already received, in
`ground/host/battery.py` (pure, stdlib), fed by `GsState` and shown in the
page's OBC tab. Nothing changes on board or on the wire.

- **Voltage to charge:** the open-circuit voltage of a cell is estimated as
  `(V + V_D1 + I·R)/2` and mapped to percent through a piecewise-linear NCR18650B
  curve (`CELL_OCV`, 0 % at 3.00 V, 100 % at 4.18 V).
- **Discharging:** coulomb counting of the INA219's current (trapezoid,
  timed by the satellite's `uptime_s`), anchored on the first valid voltage and
  pulled towards the voltage's figure by 2 % of the difference per sample.
- **Charging:** the level from the voltage reads high by the unknown charge
  current times R, but its slope does not. The estimate then follows the
  slope of the open-circuit voltage over the last 5 min, converted to %/h
  through the curve's slope at the estimate. The charge current is estimated
  as `slope × capacity + load`.
- **R is measured from the HK themselves:** `−ΔV/ΔI` by least squares over
  the differences between consecutive HK of the last 10 min, refitted without
  the pairs far off the line. It needs 18 pairs, a load moving at least
  15 mA rms between HK, and no single pair holding more than half of the ΔI²,
  so that a charger switched on in the same HK as a load step cannot make R
  alone; it must lie within 0.05–1.0 Ω. The noise σ is the MAD of the same
  differences, compensated, divided by √2. Until then the default, 0.55 Ω
  (this board's, from the HK of 25/11) or `--battery-r-ohm`, is trusted from
  the first HK: waiting for R left the state frozen for the first ~3 min. The
  first R measured, if 0.05 Ω or more off, re-anchors the charge on the
  voltage.
- **The state changes only on a step** of the compensated voltage `V + R·I`:
  the light on lifts it by the charge current times the cells' resistance
  with the load unchanged, and off drops it back. A two-sided CUSUM against
  the median of the last 6 HK since the previous step, with slack 1σ,
  threshold 3.5σ and each HK clipped at 3σ, so that one HK alone never
  changes the state and two in a row do (single HK reach +28/−37 mV in the
  25/11 logs, two in a row ±6). With HK closer than 5 s the threshold is 5σ,
  three HK: five times the HK are five times the chances of two noisy ones in
  a row (14 false charges in 10 simulated hours at 3.5σ, 1 at 5σ). The load
  moving 100 mA or more from the baseline's (a mode change) restarts the
  baseline instead of deciding, so that an R off by 0.15 Ω cannot read a
  200 mA step as 30 mV of charge. Only a change of state restarts the trend
  window.
- **Fast HK for the EPS test:** the OBC tab can ask for one HK every 2 s
  (`gs_web.py` `FastHk`, the console's `hk`), each after the previous one's
  ACK, off by itself after 15 min. `obcd` sends its readings to `ttcd` every
  second, so each HK is a new reading. The state then follows the light in
  ~6 s instead of 10–20 s. Over LoRa a cycle is ~0.5 s on air (REQ_HK, its
  ACK, the 0.247 s HK): a quarter of the channel, for the test only. The log
  and the chart keep one HK in ~10 s; the estimate sees them all.
- **The trend does not decide the state.** On the curve's plateau 1 % is
  14 mV of the pack, and the 25/11 logs wander 10 mV in 4 min: with no light
  at all, a 5-min trend read +38 %/h. A light already on at the first HK
  shows only when it goes off.
- **Autonomy** is `charge × capacity / mean load` over the last 5 min;
  charging, the time to full is `(100 − charge) / trend`.
- **Gaps** (no data, the AOCS lease of ADR-0017) keep counting at the last
  load for up to 900 s; a longer gap or a reboot (`uptime_s` going back)
  re-anchors on the voltage.
- Defaults: 3200 mAh (the minimum, so the autonomy errs short), 0.25 Ω until
  R is measured, and 0.30 V for D1. `gs_web.py --battery-mah`,
  `--battery-r-ohm`, `--battery-diode-v` override them.

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
| the load wandering ±55 mA, R 0.55 Ω, noise 5 mV | R measured 0.55 ± 0.08 in 10 min |
| charger of 300 mA (25 mV step), 5 seeds | charging and back within 30 s each way |
| 30 min with no light, 5 seeds | no false charging |

4. **Against the two real logs of 25/11** (`ground/host/sessions/ssh-mission-*`,
   158 HK, no light, the new INA219): R 0.53 and 0.61 Ω, noise 5.6 and
   5.0 mV, 0 HK read as charging (the first version of this estimator, with
   a 3 % jump and the trend deciding: 1 false charge that lasted to the end of
   the log). A charge step added to the real voltage between two times:

| Step (≈ charge current at 0.25 Ω) | On detected after | Off detected after |
|---|---|---|
| 10 mV (40 mA) | never | — |
| 15 mV (60 mA) | 20 s, in one of two cases | never: stays charging |
| 20 mV (80 mA) | 20 s | 10–80 s |
| 25 mV (100 mA) | 10 s | 10–30 s |
| 40 mV (160 mA) | 10 s | 10–20 s |

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
- R and the noise are measured from the load's own wander between HK, so
  the estimate calibrates itself in ~3 min, and the state follows the light
  within one or two HK.

### Negative

- The curve is the chemistry's typical shape, not measured on our cells:
  ~10 % in the plateau until calibrated.
- During charging the figure rests on the slope alone, which needs 2 min of
  HK after the step.
- A charge below ~20 mV of step (~80 mA into the cells) is not reliably
  detected, and one of ~15 mV can be seen on and never off. A steadier load
  than 25/11's (below 15 mA rms between HK) leaves R at the default.
- A light switched within one HK of a mode change goes unseen until it is
  switched again.
- The default R is this board's; another board, or a battery swapped, needs
  `--battery-r-ohm` or ~3 min of HK before the compensation is right.
- The charge current is inferred, not measured; REG-09 evaluates current and
  voltage, and the jury may want a meter on the charge line.
- In the CV phase of a charge the voltage stops rising while the charge still
  increases: the estimate then understates the charge rate.

### Follow-up required

- Bench, with the satellite assembled (`docs/relatorios/2026-11-25-roteiro-teste-eps.html`):
  V at P+ against the INA219 at a light load for `--battery-diode-v`; the R
  the page measures, then `--battery-r-ohm` with it; the step a real light
  gives, against the table above; a discharge from full
  against the counted mAh to check the capacity and the curve.
- If the EPS adds a current sensor on the charge line, replace the inferred
  charge current with it.

## References

- `ground/host/battery.py`, `ground/host/tests/host/test_battery.py`
- `hardware/README.md` (A9), `docs/relatorios/2026-11-24-sensores-obc.html`
- Panasonic NCR18650B datasheet (capacity, cut-off)
- ADR-0013 (`TM_HK`), ADR-0017 (the AOCS lease)
