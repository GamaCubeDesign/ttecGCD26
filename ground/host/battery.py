#!/usr/bin/env python3
"""battery.py — the ground's estimate of the battery's charge (ADR-0024).

From the housekeeping alone (battery_mv and current_ma, every 10 s): the
state of charge in percent, the autonomy at the present load, and how the
charge is evolving, charging included. Pure: no I/O, no clock; the time of
each sample is given, the satellite's uptime_s when there is one.

The pack is 2S1P Panasonic NCR18650B (EPS: BMS 2S, one cell on each battery
board). Two limits shape the method, both in ADR-0024:

- The INA219 sits on the satellite's 8 V input, after the battery and the
  charger (hardware/README.md, A9): it measures the load, never the charge
  current. Counting charge works only while discharging; a charge shows only
  as the voltage rising against what the load predicts.
- The INA219 reads the voltage after the EPS's D1 (1N5822 Schottky, from the
  BMS's P+ to the 8 V line) and its own shunt: below the open-circuit voltage
  by D1's drop plus I·R, with R the cells, the BMS, the switch and the shunt.
  Both are corrected for and both are parameters the bench should measure
  (V at P+ against the INA219; a load step: ΔV/ΔI).

    python3 battery.py GROUND.jsonl [--capacity-mah 3200] [--r-ohm 0.25] [--diode-v 0.30]
        one line per HK of a recorded ground log, the estimate after it
"""

import argparse
import collections
import json
import sys
from dataclasses import dataclass

# Open-circuit voltage of one NCR18650B cell against its state of charge, %
# and V, at rest and 20-25 degC. Approximate: the shape of the NCA chemistry's
# curve, 0 % placed at 3.00 V (above the 2.50 V cut-off, the usable floor
# under the satellite's load). Calibrate against a full discharge on the
# bench before trusting the plateau to better than ~10 %.
CELL_OCV = ((0, 3.00), (5, 3.30), (10, 3.45), (20, 3.56), (30, 3.63), (40, 3.70),
            (50, 3.77), (60, 3.85), (70, 3.94), (80, 4.02), (90, 4.09), (100, 4.18))

NO_DATA_MV = 65535
NO_DATA_MA = -32768


@dataclass(frozen=True)
class BatteryModel:
    # NCR18650B datasheet: 3200 mAh minimum, 3350 typical (0.2 C, 20 degC);
    # the minimum, so that the autonomy errs short.
    capacity_mah: float = 3200.0
    cells_series: int = 2
    # Cells (~2 x 60 mOhm DC), BMS switches and wiring (~30), the INA219's
    # 100 mOhm shunt: ~0.25 Ohm from the cells to where the INA219 measures.
    r_ohm: float = 0.25
    # D1 on the EPS (hardware/eps-potencia, P+ -> D1 -> SW1 -> 8V Bruto): a
    # 1N5822 Schottky drops ~0.3 V at 0.5 A. Its rise with the current is
    # small next to I*R and lands in the measured R.
    diode_v: float = 0.30
    # A cell voltage outside this is a broken reading, not a battery.
    cell_v_min: float = 2.50
    cell_v_max: float = 4.35


def soc_from_cell_v(v):
    """State of charge, %, of one cell at open-circuit voltage v."""
    if v <= CELL_OCV[0][1]:
        return 0.0
    for (s0, v0), (s1, v1) in zip(CELL_OCV, CELL_OCV[1:]):
        if v <= v1:
            return s0 + (s1 - s0) * (v - v0) / (v1 - v0)
    return 100.0


def ocv_slope(soc):
    """dV/dSoC of one cell at soc, V per %: the curve's segment there."""
    for (s0, v0), (s1, v1) in zip(CELL_OCV, CELL_OCV[1:]):
        if soc <= s1:
            return (v1 - v0) / (s1 - s0)
    s0, v0 = CELL_OCV[-2]
    s1, v1 = CELL_OCV[-1]
    return (v1 - v0) / (s1 - s0)


def _slope(points):
    """Least-squares slope of [(t, y)], per second; None with fewer than 3."""
    if len(points) < 3:
        return None
    n = len(points)
    mt = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    sxx = sum((p[0] - mt) ** 2 for p in points)
    if sxx <= 0:
        return None
    return sum((p[0] - mt) * (p[1] - my) for p in points) / sxx


class BatteryEstimator:
    """Feed it every HK; ask it for the estimate.

    Discharging, the load is counted (coulomb counting) and the count is
    pulled slowly towards the voltage's figure. Charging, the voltage's level
    reads high by the unknown charge current times R, but its slope does not:
    the estimate follows the slope. A charger switched on or off shows as a
    jump of the voltage with the load steady; a jump with the load changing is
    R mis-set, and measures R instead (ADR-0024)."""

    WINDOW_S = 300.0        # the trend and the mean load: the last 5 min
    TREND_MIN_S = 120.0     # no trend from less than 2 min of samples
    HOLD_GAP_S = 900.0      # a gap up to this keeps counting at the last load
    STEP_GAP_S = 60.0       # two samples further apart are not compared
    K_DISCHARGE = 0.02      # per sample, how far the count is pulled to the voltage
    # Charging by the trend: the voltage rising faster than the load predicts
    # by more than this, %/h. 1 A into 3.2 Ah is +31 %/h.
    CHARGE_PCT_H = 10.0
    # A charger edge: the voltage's figure jumping this much between two HK
    # with the load within STEADY_MA. 0.5 A of charge through 0.25 Ohm is
    # ~0.06 V a cell, ~8 % in the plateau.
    JUMP_PCT = 3.0
    STEADY_MA = 50
    STEP_MA = 100           # a load step this large measures R
    R_SAMPLES = 15

    def __init__(self, model=None):
        self.model = model or BatteryModel()
        self.r_steps = collections.deque(maxlen=self.R_SAMPLES)
        self.reset()

    def reset(self):
        self.soc = None          # the estimate, %
        self.soc_v = None        # from the last voltage, %
        self.used_mah = 0.0      # the load counted since the anchor
        self.anchor_t = None
        self.last_t = None       # the last sample, any
        self.last_uptime = None
        self.last_ma = None
        self.prev = None         # the last valid voltage sample: (t, mv, ma, soc_v)
        self.last_reason = "no_data"
        self.window = collections.deque()   # (t, ocv V a cell, ma or None)
        self.charging = False
        self.edges = 0           # charger edges seen

    def feed(self, t, battery_mv, current_ma, uptime_s=None):
        """One HK. t is the ground's time, s; uptime_s, when present, is the
        time base (the satellite's, unaffected by retries or replay speed)."""
        mv = None if battery_mv in (None, NO_DATA_MV) else battery_mv
        ma = None if current_ma in (None, NO_DATA_MA) else current_ma
        if isinstance(uptime_s, (int, float)):
            if self.last_uptime is not None and uptime_s < self.last_uptime:
                self.reset()                    # the satellite rebooted
            self.last_uptime = uptime_s
            t = float(uptime_s)
        m = self.model
        if mv is None or not m.cell_v_min <= (mv / 1000.0 + m.diode_v) / m.cells_series <= m.cell_v_max:
            self.last_reason = "no_data" if mv is None else "out_of_range"
            self._advance(t, ma)
            return
        self.last_reason = None
        # The OCV, assuming the battery feeds the load: wrong by the charge
        # current times R while charging, which is why the level is then unused.
        cell_v = (mv / 1000.0 + m.diode_v) / m.cells_series
        ocv = cell_v + (ma / 1000.0 * m.r_ohm / m.cells_series if ma is not None else 0.0)
        soc_v = soc_from_cell_v(ocv)
        self._edges(t, mv, ma, soc_v)
        self.soc_v = soc_v
        self.prev = (t, mv, ma, soc_v)
        self._advance(t, ma)                    # may drop the anchor after a long gap
        if self.soc is None:
            self.soc, self.anchor_t, self.last_t, self.last_ma = soc_v, t, t, ma
            self.used_mah = 0.0
        elif not self.charging:
            self.soc += self.K_DISCHARGE * (soc_v - self.soc)
        self.soc = min(100.0, max(0.0, self.soc))
        self.window.append((t, ocv, ma))
        while self.window and t - self.window[0][0] > self.WINDOW_S:
            self.window.popleft()
        self._detect_charge()

    def _edges(self, t, mv, ma, soc_v):
        """Against the previous valid sample: a load step measures R and
        restarts the trend; a jump with the load steady is a charger edge."""
        if self.prev is None or t - self.prev[0] > self.STEP_GAP_S:
            return
        _, pmv, pma, psoc = self.prev
        if ma is None or pma is None:
            return
        di = ma - pma
        if abs(di) >= self.STEP_MA:
            r = -(mv - pmv) / di                # mV / mA = Ohm, for the whole pack
            if 0.0 < r < 2.0:
                self.r_steps.append(r)
            self.window.clear()
        elif abs(di) <= self.STEADY_MA and abs(soc_v - psoc) >= self.JUMP_PCT:
            self.charging = soc_v > psoc
            self.edges += 1
            self.window.clear()

    def _advance(self, t, ma):
        """Moves the estimate to time t: the load counted (trapezoid, the
        known end held when one is missing); while charging, the trend."""
        if self.last_t is None or self.soc is None:
            return
        dt = t - self.last_t
        if dt <= 0:
            return
        if dt > self.HOLD_GAP_S:
            # Too long unseen: whatever happened, the voltage says it.
            self.soc, self.anchor_t, self.last_t, self.last_ma = None, None, None, None
            self.window.clear()
            return
        ends = [x for x in (self.last_ma, ma) if x is not None]
        mah = sum(ends) / len(ends) * dt / 3600.0 if ends else 0.0
        self.used_mah += mah
        if self.charging:
            trend = self._trend_pct_h()
            if trend is not None:
                self.soc += trend * dt / 3600.0
        else:
            self.soc -= mah / self.model.capacity_mah * 100.0
        self.soc = min(100.0, max(0.0, self.soc))
        self.last_t = t
        if ma is not None:
            self.last_ma = ma

    def _span(self):
        return self.window[-1][0] - self.window[0][0] if len(self.window) > 1 else 0.0

    def _mean_ma(self):
        ma = [w[2] for w in self.window if w[2] is not None]
        if not ma and self.last_ma is not None:
            return float(self.last_ma)
        return sum(ma) / len(ma) if ma else None

    def _trend_pct_h(self):
        """The voltage's slope as %/h, through the curve's slope where the
        estimate is: not where the level reads, which charging lifts."""
        if self._span() < self.TREND_MIN_S or self.soc is None:
            return None
        s = _slope([(w[0], w[1]) for w in self.window])
        return None if s is None else s * 3600.0 / ocv_slope(self.soc)

    def _detect_charge(self):
        trend, load = self._trend_pct_h(), self._mean_ma()
        if trend is None or load is None:
            return
        excess = trend + load / self.model.capacity_mah * 100.0
        # Hysteresis: in at the threshold, out at half of it.
        self.charging = excess > (self.CHARGE_PCT_H / 2 if self.charging else self.CHARGE_PCT_H)

    def r_measured(self):
        """The median of R from the load steps seen, Ohm; None before 3."""
        if len(self.r_steps) < 3:
            return None
        r = sorted(self.r_steps)
        return r[len(r) // 2]

    def estimate(self):
        """What the page shows. None for what cannot be said."""
        cap = self.model.capacity_mah
        r = self.r_measured()
        out = {"state": self.last_reason or ("charging" if self.charging else "discharging"),
               "soc_pct": None, "soc_v_pct": None, "trend_pct_h": None, "load_ma": None,
               "autonomy_s": None, "full_in_s": None, "charge_ma": None, "used_mah": None,
               "counted_s": None, "capacity_mah": cap, "r_ohm": self.model.r_ohm,
               "diode_v": self.model.diode_v,
               "r_measured_ohm": None if r is None else round(r, 3)}
        if self.soc is None:
            return out
        trend, load = self._trend_pct_h(), self._mean_ma()
        if self.charging:
            if trend is not None and trend > 0:
                out["full_in_s"] = round((100.0 - self.soc) / trend * 3600.0)
                # Into the cells, from the slope, plus the load the charger
                # also feeds: rough, see ADR-0024.
                out["charge_ma"] = round(trend / 100.0 * cap + (load or 0.0))
        elif load is not None and load > 0:
            out["autonomy_s"] = round(self.soc / 100.0 * cap / load * 3600.0)
        out.update({
            "soc_pct": round(self.soc, 1),
            "soc_v_pct": None if self.soc_v is None else round(self.soc_v, 1),
            "trend_pct_h": None if trend is None else round(trend, 1),
            "load_ma": None if load is None else round(load),
            "used_mah": round(self.used_mah, 1),
            "counted_s": None if self.anchor_t is None or self.last_t is None
            else round(self.last_t - self.anchor_t),
        })
        return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="the battery estimate over a recorded ground log")
    ap.add_argument("log", help="ground JSON lines (gs_serial.py, gs_cli, a session's ground.jsonl)")
    ap.add_argument("--capacity-mah", type=float, default=BatteryModel.capacity_mah)
    ap.add_argument("--r-ohm", type=float, default=BatteryModel.r_ohm)
    ap.add_argument("--diode-v", type=float, default=BatteryModel.diode_v)
    args = ap.parse_args(argv)
    est = BatteryEstimator(BatteryModel(capacity_mah=args.capacity_mah, r_ohm=args.r_ohm,
                                        diode_v=args.diode_v))
    print("uptime_s  mV     mA    carga%  tensão%  tend%/h  autonomia_min  estado")
    with open(args.log, encoding="utf-8") as f:
        for line in f:
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if obj.get("ev") != "hk":
                continue
            est.feed(obj.get("mono_ms", 0) / 1000.0, obj.get("battery_mv"), obj.get("current_ma"),
                     obj.get("uptime_s"))
            e = est.estimate()
            fmt = lambda v, f: "-" if v is None else f % v
            print("%8s  %5s  %5s  %6s  %7s  %7s  %13s  %s" % (
                obj.get("uptime_s", "-"), obj.get("battery_mv"), obj.get("current_ma"),
                fmt(e["soc_pct"], "%.1f"), fmt(e["soc_v_pct"], "%.1f"), fmt(e["trend_pct_h"], "%+.1f"),
                fmt(None if e["autonomy_s"] is None else e["autonomy_s"] / 60, "%.0f"), e["state"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
