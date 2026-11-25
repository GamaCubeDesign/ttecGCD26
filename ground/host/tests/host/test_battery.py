"""battery.py: the charge, the autonomy and the charging estimated from the
HK's voltage and load current, against a synthetic 2S pack that follows the
same open-circuit curve (ADR-0024)."""

import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # ground/host
sys.path.insert(0, HERE)
import battery  # noqa: E402
import gs_web  # noqa: E402


def cell_ocv(soc):
    for (s0, v0), (s1, v1) in zip(battery.CELL_OCV, battery.CELL_OCV[1:]):
        if soc <= s1:
            return v0 + (v1 - v0) * (soc - s0) / (s1 - s0)
    return battery.CELL_OCV[-1][1]


class Pack:
    """A 2S pack: the INA219 sees the load and the voltage after R; a charger
    feeds the cells (and so the load) without the INA219 seeing it."""

    def __init__(self, soc=70.0, cap=3300.0, r=0.25, seed=1):
        self.soc, self.cap, self.r, self.t = soc, cap, r, 0
        self.rng = random.Random(seed)

    def run(self, est, seconds, load, charge=0, every=10):
        for _ in range(int(seconds / every)):
            ibat = load - charge
            self.soc -= ibat * every / 3600.0 / self.cap * 100.0
            self.t += every
            v = 2 * cell_ocv(self.soc) - ibat / 1000.0 * self.r
            mv = int(round(v * 1000 / 4)) * 4 + self.rng.choice((-4, 0, 4))   # the INA219's 4 mV
            est.feed(self.t, mv, load + self.rng.randint(-10, 10), uptime_s=self.t)
        return est.estimate()


class TheCurve(unittest.TestCase):
    def test_ends_and_middle(self):
        self.assertEqual(battery.soc_from_cell_v(2.9), 0.0)
        self.assertEqual(battery.soc_from_cell_v(4.25), 100.0)
        self.assertAlmostEqual(battery.soc_from_cell_v(3.77), 50.0)
        vs = [battery.soc_from_cell_v(3.0 + k * 0.01) for k in range(120)]
        self.assertEqual(vs, sorted(vs))


class Discharging(unittest.TestCase):
    def test_follows_a_constant_load(self):
        est, pack = battery.BatteryEstimator(), Pack(soc=70.0)
        e = pack.run(est, 600, load=600)
        self.assertEqual(e["state"], "discharging")
        self.assertLess(abs(e["soc_pct"] - pack.soc), 1.5)
        self.assertAlmostEqual(e["load_ma"], 600, delta=10)
        # 600 mA for 600 s is 100 mAh, counted
        self.assertAlmostEqual(e["used_mah"], 100, delta=3)
        self.assertAlmostEqual(e["autonomy_s"], e["soc_pct"] / 100 * 3200 / e["load_ma"] * 3600, delta=60)
        self.assertLess(e["trend_pct_h"], 0)
        self.assertIsNone(e["full_in_s"])

    def test_a_load_step_measures_r(self):
        est, pack = battery.BatteryEstimator(), Pack(r=0.32)
        for k in range(6):
            pack.run(est, 30, load=400 if k % 2 == 0 else 900)
        self.assertAlmostEqual(est.estimate()["r_measured_ohm"], 0.32, delta=0.03)

    def test_a_load_step_is_not_a_charge(self):
        # R mis-set by 0.15 Ohm: the step moves the voltage's figure, and must
        # neither start a charge nor bend the trend.
        est, pack = battery.BatteryEstimator(), Pack(r=0.40)
        pack.run(est, 300, load=300)
        e = pack.run(est, 300, load=1000)
        self.assertEqual(e["state"], "discharging")
        self.assertLess(e["trend_pct_h"], 0)


class Charging(unittest.TestCase):
    def test_a_charger_switched_on_and_off(self):
        est, pack = battery.BatteryEstimator(), Pack(soc=60.0)
        pack.run(est, 300, load=500)
        e = pack.run(est, 20, load=500, charge=1200)
        self.assertEqual(e["state"], "charging")           # the edge, at once
        before = e["soc_pct"]
        e = pack.run(est, 600, load=500, charge=1200)
        self.assertEqual(e["state"], "charging")
        self.assertAlmostEqual(e["charge_ma"], 1200, delta=180)
        self.assertGreater(e["soc_pct"], before)
        self.assertLess(abs(e["soc_pct"] - pack.soc), 3.0)  # the level is not lifted by I*R
        self.assertIsNone(e["autonomy_s"])
        self.assertGreater(e["full_in_s"], 0)
        e = pack.run(est, 300, load=500)
        self.assertEqual(e["state"], "discharging")
        self.assertLess(abs(e["soc_pct"] - pack.soc), 3.0)

    def test_a_charge_starting_with_a_load_step_is_found_by_the_trend(self):
        est, pack = battery.BatteryEstimator(), Pack(soc=60.0)
        pack.run(est, 300, load=900)
        e = pack.run(est, 300, load=500, charge=1200)
        self.assertEqual(e["state"], "charging")


class NoReading(unittest.TestCase):
    def test_no_data_and_impossible_voltages(self):
        est = battery.BatteryEstimator()
        est.feed(0, 65535, -32768, uptime_s=10)
        self.assertEqual(est.estimate()["state"], "no_data")
        est.feed(10, 880, 0, uptime_s=20)                 # the old INA219's readings
        e = est.estimate()
        self.assertEqual((e["state"], e["soc_pct"], e["autonomy_s"]), ("out_of_range", None, None))

    def test_an_aocs_gap_keeps_counting_the_last_load(self):
        est, pack = battery.BatteryEstimator(), Pack()
        pack.run(est, 120, load=720)
        used = est.estimate()["used_mah"]
        for k in range(30):                              # 300 s without the INA219 (ADR-0017)
            pack.t += 10
            est.feed(pack.t, 65535, -32768, uptime_s=pack.t)
        e = est.estimate()
        self.assertEqual(e["state"], "no_data")
        self.assertAlmostEqual(e["used_mah"] - used, 720 * 300 / 3600, delta=2)
        self.assertIsNotNone(e["soc_pct"])

    def test_a_reboot_starts_again_from_the_voltage(self):
        est, pack = battery.BatteryEstimator(), Pack()
        pack.run(est, 300, load=600)
        est.feed(0, 7600, 600, uptime_s=5)
        e = est.estimate()
        self.assertEqual((e["used_mah"], e["counted_s"]), (0.0, 0))


class InThePage(unittest.TestCase):
    def test_the_snapshot_carries_the_estimate(self):
        clock = [1000.0]
        s = gs_web.GsState(now=lambda: clock[0])
        for k in range(3):
            s.feed({"ev": "hk", "battery_mv": 7560, "current_ma": 600, "uptime_s": 10 * k,
                    "obc_mode": "PRE_TEST"})
            clock[0] += 10
        snap = s.snapshot()
        self.assertEqual(snap["battery"]["state"], "discharging")
        self.assertEqual(snap["hk_hist"][-1][6], snap["battery"]["soc_pct"])
        s.feed({"ev": "hk", "battery_mv": 65535, "current_ma": -32768, "uptime_s": 40})
        self.assertEqual(s.snapshot()["battery"]["state"], "no_data")


if __name__ == "__main__":
    unittest.main()
