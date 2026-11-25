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
  Both are corrected for: the drop is a parameter the bench measures (V at
  P+ against the INA219); R is measured here, from how the voltage follows
  the load between consecutive HK (0.53-0.61 Ohm on 25/11), the parameter
  (0.55 Ohm) standing in until then.

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
    # From the cells to where the INA219 measures (cells, BMS, D1's slope,
    # switch, shunt): 0.53-0.61 Ohm from the HK of 25/11 on this board.
    # Trusted from the first HK; the HK's own R replaces it once measured.
    r_ohm: float = 0.55
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


def _median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


class BatteryEstimator:
    """Feed it every HK; ask it for the estimate.

    The state (charging or not) comes from steps of the compensated voltage,
    V + R·I: the light switched on lifts it by the charge current times the
    cells' resistance, with the load unchanged, and switched off drops it back.
    A two-sided CUSUM against the median of the last HK finds the step in one
    or two HK, with thresholds set by the measured noise. R and the noise are
    measured continuously from the differences between consecutive HK
    (ΔV against ΔI), which neither a step nor the slow discharge disturbs.

    The charge: discharging, the load is counted (coulomb counting) and
    pulled slowly towards the voltage's figure; charging, the voltage's level
    reads high by the unknown charge current times R, but its slope does not,
    and the estimate follows the slope (ADR-0024).

    Only a step changes the state, with the load steady. A light already on
    at the first HK shows only when it goes off."""

    WINDOW_S = 300.0        # the trend and the mean load: the last 5 min
    TREND_MIN_S = 120.0     # no trend from less than 2 min of samples
    HOLD_GAP_S = 900.0      # a gap up to this keeps counting at the last load
    STEP_GAP_S = 60.0       # two samples further apart are not compared
    K_DISCHARGE = 0.02      # per 10 s (one HK at NOMINAL), how far the count is pulled to the voltage
    # R and the noise, from the differences of the last 10 min; R is trusted
    # once the load has moved enough (rms of ΔI) to show it.
    R_WINDOW_S = 600.0
    R_MIN_PAIRS = 18        # 3 min of HK: with 12, the first R of 25/11 swung 0.41-0.74
    R_MIN_DI_MA = 15.0
    R_MIN, R_MAX = 0.05, 1.0     # 25/11: 0.47-0.77 Ohm
    # No single pair may carry more than this share of the ΔI²: a charger
    # switched on in the same HK as a 400 mA load step made R 1.17 Ohm alone.
    R_MAX_SHARE = 0.5
    SIGMA_DEFAULT_MV = 8.0  # before it is measured: the 25/11 logs gave 5-6
    SIGMA_MIN_MV = 3.0      # below the INA219's 4 mV step there is no noise to trust
    # The CUSUM, in noise units: slack k, threshold h, and each HK's deviation
    # clipped, so that one HK alone never changes the state (the 25/11 log has
    # single HK 40 mV off with the load unchanged) and two in a row do. With
    # the 25/11 noise (~6 mV), a step of 25 mV (100 mA of charge through
    # ~0.25 Ohm of cells) shows at the second HK.
    # HK every 2 s (the page's fast HK) are five times the chances per hour
    # for two noisy HK in a row: 14 false charges in 10 simulated hours at
    # 3.5, 1 at 5.0 (three HK, 6 s). The threshold follows the HK spacing.
    CUSUM_K = 1.0
    CUSUM_H = 3.5
    CUSUM_H_FAST = 5.0
    FAST_HK_S = 5.0         # HK closer than this (median) use CUSUM_H_FAST
    CUSUM_CLIP = 3.0
    BASE_N = 6              # the baseline: median of the last HK since the last step
    BASE_MIN = 3
    # The load this far from the baseline's: a mode change (the SDR, ~200 mA).
    # An R off by 0.15 Ohm would make it a 30 mV step, so the baseline starts
    # again instead; a light switched in the same HK goes unseen.
    STEP_MA = 100

    def __init__(self, model=None):
        self.model = model or BatteryModel()
        self.reset()

    def reset(self):
        self.soc = None          # the estimate, %
        self.soc_v = None        # from the last voltage, %
        self.used_mah = 0.0      # the load counted since the anchor
        self.anchor_t = None
        self.last_t = None       # the last sample, any
        self.last_uptime = None
        self.last_ma = None
        self.prev = None         # the last valid voltage sample: (t, mv, ma)
        self.last_reason = "no_data"
        self.window = collections.deque()   # (t, mv, ma or None): compensated with the R of the moment
        self.pairs = collections.deque()    # (t, ΔmV, ΔmA) of consecutive HK
        self.base = collections.deque(maxlen=self.BASE_N)   # (mv, ma) since the last step
        self.gp = self.gn = 0.0  # the CUSUM's two sums, mV
        self.charging = False
        self.edges = 0           # steps of the charger seen
        self.state_t = None      # when the present state began
        self.r_now, self.sigma_now = None, self.SIGMA_DEFAULT_MV
        self.reanchor = False    # R just measured: the level from the voltage again
        self.pull_t = None

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
        self._measure(t, mv, ma)                # may ask for a new anchor: R first known
        r = self.r_used()
        self._step(t, mv, ma, r)
        self.prev = (t, mv, ma)
        self.soc_v = soc_from_cell_v(self._ocv(mv, ma, r))
        self._advance(t, ma)                    # may drop the anchor after a long gap
        if self.soc is None:
            self.soc, self.anchor_t, self.last_t, self.last_ma = self.soc_v, t, t, ma
            self.used_mah = 0.0
            self.state_t = t
        elif self.reanchor:
            self.soc = self.soc_v
        elif not self.charging:
            self.soc += self._pull(t) * (self.soc_v - self.soc)
        self.reanchor = False
        self.soc = min(100.0, max(0.0, self.soc))
        self.window.append((t, mv, ma))
        while self.window and t - self.window[0][0] > self.WINDOW_S:
            self.window.popleft()

    def _measure(self, t, mv, ma):
        """R and the noise from the differences between consecutive HK."""
        p = self.prev
        if p is not None and ma is not None and p[2] is not None and t - p[0] <= self.STEP_GAP_S:
            self.pairs.append((t, mv - p[1], ma - p[2]))
        while self.pairs and t - self.pairs[0][0] > self.R_WINDOW_S:
            self.pairs.popleft()
        if len(self.pairs) < self.R_MIN_PAIRS:
            return
        r = self._fit_r(self.pairs)
        if r is not None:
            # The first R measured: the anchor taken with the default R is
            # off by the difference times the load (8 % at 0.3 Ohm, 0.5 A).
            if self.r_now is None and not self.charging and abs(r - self.model.r_ohm) > 0.05:
                self.reanchor = True
            self.r_now = r
        r = self.r_used()
        e = [d[1] + r * d[2] for d in self.pairs]
        med = _median(e)
        # MAD of the differences: one step is one outlier; /sqrt(2) for a
        # difference of two samples.
        self.sigma_now = max(self.SIGMA_MIN_MV, _median([abs(x - med) for x in e]) * 1.4826 / 2 ** 0.5)

    def _fit_r(self, pairs):
        """-ΔV/ΔI by least squares, refitted without the pairs far off the
        line: a charger's step is a ΔV with no ΔI, and alone it pulled R from
        0.47 to 0.23 Ohm in the simulation."""
        def fit(ps):
            sdi = sum(d[2] * d[2] for d in ps)
            if len(ps) < self.R_MIN_PAIRS or sdi / len(ps) < self.R_MIN_DI_MA ** 2:
                return None
            if max(d[2] * d[2] for d in ps) > self.R_MAX_SHARE * sdi:
                return None
            return -sum(d[1] * d[2] for d in ps) / sdi              # mV / mA = Ohm
        r = fit(pairs)
        if r is None:
            return None
        e = [d[1] + r * d[2] for d in pairs]
        med = _median(e)
        mad = max(self.SIGMA_MIN_MV, _median([abs(x - med) for x in e]) * 1.4826)
        r = fit([d for d, x in zip(pairs, e) if abs(x - med) <= 4.0 * mad])
        return r if r is not None and self.R_MIN < r < self.R_MAX else None

    def _ocv(self, mv, ma, r):
        """One cell's open-circuit voltage, V, assuming the battery feeds the
        load: wrong by the charge current times R while charging, which is why
        the level is then unused."""
        m = self.model
        return (mv / 1000.0 + m.diode_v + (ma / 1000.0 * r if ma is not None else 0.0)) / m.cells_series

    def r_used(self):
        return self.r_now if self.r_now is not None else self.model.r_ohm

    def _step(self, t, mv, ma, r):
        """The CUSUM on V + R·I against the baseline: a step up is the
        charger switched on, down switched off."""
        if ma is None:
            return
        if self.base and abs(ma - _median([b[1] for b in self.base])) >= self.STEP_MA:
            self.base.clear()
            self.gp = self.gn = 0.0
        if len(self.base) < self.BASE_MIN:
            self.base.append((mv, ma))
            return
        sig = self.sigma_now
        d = mv + r * ma - _median([b[0] + r * b[1] for b in self.base])
        d = max(-self.CUSUM_CLIP * sig, min(self.CUSUM_CLIP * sig, d))
        k = self.CUSUM_K * sig
        h = (self.CUSUM_H_FAST if self._spacing() < self.FAST_HK_S else self.CUSUM_H) * sig
        self.gp = max(0.0, self.gp + d - k)
        self.gn = max(0.0, self.gn - d - k)
        if self.gp > h or self.gn > h:
            up = self.gp > h
            if up != self.charging:
                self.charging = up
                self.edges += 1
                self.state_t = t
                self.window.clear()             # the trend starts again from the step
            self.base.clear()
            self.gp = self.gn = 0.0
        self.base.append((mv, ma))

    def _spacing(self):
        """The median time between consecutive HK lately, s."""
        p = list(self.pairs)[-10:]
        if len(p) < 3:
            return 10.0
        return _median([b[0] - a[0] for a, b in zip(p, p[1:])])

    def _pull(self, t):
        """K_DISCHARGE for the time since the last sample, so that HK every
        2 s pull no harder than every 10 s."""
        dt = t - self.pull_t if self.pull_t is not None else 10.0
        self.pull_t = t
        return 1.0 - (1.0 - self.K_DISCHARGE) ** (max(0.0, min(dt, 60.0)) / 10.0)

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
            self.base.clear()
            self.gp = self.gn = 0.0
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
        r = self.r_used()
        s = _slope([(w[0], self._ocv(w[1], w[2], r)) for w in self.window])
        return None if s is None else s * 3600.0 / ocv_slope(self.soc)

    def estimate(self):
        """What the page shows. None for what cannot be said."""
        cap = self.model.capacity_mah
        out = {"state": self.last_reason or ("charging" if self.charging else "discharging"),
               "soc_pct": None, "soc_v_pct": None, "trend_pct_h": None, "load_ma": None,
               "autonomy_s": None, "full_in_s": None, "charge_ma": None, "used_mah": None,
               "counted_s": None, "capacity_mah": cap, "r_ohm": self.model.r_ohm,
               "diode_v": self.model.diode_v,
               "r_measured_ohm": None if self.r_now is None else round(self.r_now, 3),
               "noise_mv": round(self.sigma_now, 1),
               "state_s": None if self.state_t is None or self.last_t is None
               else round(self.last_t - self.state_t)}
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


def model_from(capacity_mah, r_ohm, diode_v):
    """The model from the command line: r_ohm None is the default."""
    if r_ohm is None:
        return BatteryModel(capacity_mah=capacity_mah, diode_v=diode_v)
    return BatteryModel(capacity_mah=capacity_mah, r_ohm=r_ohm, diode_v=diode_v)


def main(argv=None):
    ap = argparse.ArgumentParser(description="the battery estimate over a recorded ground log")
    ap.add_argument("log", help="ground JSON lines (gs_serial.py, gs_cli, a session's ground.jsonl)")
    ap.add_argument("--capacity-mah", type=float, default=BatteryModel.capacity_mah)
    ap.add_argument("--r-ohm", type=float, help="R until the HK measure it, Ohm (default 0.55)")
    ap.add_argument("--diode-v", type=float, default=BatteryModel.diode_v)
    args = ap.parse_args(argv)
    est = BatteryEstimator(model_from(args.capacity_mah, args.r_ohm, args.diode_v))
    print("uptime_s  mV     mA    carga%  tensão%  tend%/h  autonomia_min  R_ohm  ruído_mV  estado")
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
            print("%8s  %5s  %5s  %6s  %7s  %7s  %13s  %5s  %8s  %s" % (
                obj.get("uptime_s", "-"), obj.get("battery_mv"), obj.get("current_ma"),
                fmt(e["soc_pct"], "%.1f"), fmt(e["soc_v_pct"], "%.1f"), fmt(e["trend_pct_h"], "%+.1f"),
                fmt(None if e["autonomy_s"] is None else e["autonomy_s"] / 60, "%.0f"),
                fmt(e["r_measured_ohm"], "%.2f"), fmt(e["noise_mv"], "%.1f"), e["state"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
