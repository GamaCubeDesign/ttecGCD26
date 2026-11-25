"""battery.py: the charge, the autonomy and the charging estimated from the
HK's voltage and load current, against a synthetic 2S pack that follows the
same open-circuit curve (ADR-0024)."""

import os
import random
import sys
import time
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
    """A 2S pack: the INA219 sees the load and the voltage after D1 and R; a
    charger feeds the cells (and so the load) without the INA219 seeing it.

    r is the cells' and the BMS's resistance, which both the load and the
    charge current cross; r_extra is D1's and the shunt's, which only the load
    crosses. The 25/11 logs: r + r_extra ~0.55 Ohm, noise ~6 mV, the load
    wandering 150-260 mA."""

    def __init__(self, soc=70.0, cap=3300.0, r=0.25, seed=1, diode=0.30, r_extra=0.0,
                 jitter=10, noise_mv=0.0):
        self.soc, self.cap, self.r, self.diode, self.t = soc, cap, r, diode, 0
        self.r_extra, self.jitter, self.noise_mv = r_extra, jitter, noise_mv
        self.rng = random.Random(seed)

    def run(self, est, seconds, load, charge=0, every=10, watch=None):
        for _ in range(int(seconds / every)):
            ma = load + self.rng.randint(-self.jitter, self.jitter)
            ibat = ma - charge
            self.soc -= ibat * every / 3600.0 / self.cap * 100.0
            self.t += every
            v = (2 * cell_ocv(self.soc) - ibat / 1000.0 * self.r - ma / 1000.0 * self.r_extra
                 - self.diode + self.rng.gauss(0, self.noise_mv) / 1000.0)
            mv = int(round(v * 1000 / 4)) * 4 + self.rng.choice((-4, 0, 4))   # the INA219's 4 mV
            est.feed(self.t, mv, ma, uptime_s=self.t)
            if watch is not None:
                watch.append((self.t, est.estimate()["state"]))
        return est.estimate()


def like_25_11(**kw):
    """The pack as the 25/11 logs show it."""
    return Pack(**dict(dict(soc=45.0, r=0.25, r_extra=0.30, jitter=55, noise_mv=5.0), **kw))


class TheCurve(unittest.TestCase):
    def test_ends_and_middle(self):
        self.assertEqual(battery.soc_from_cell_v(2.9), 0.0)
        self.assertEqual(battery.soc_from_cell_v(4.25), 100.0)
        self.assertAlmostEqual(battery.soc_from_cell_v(3.77), 50.0)
        vs = [battery.soc_from_cell_v(3.0 + k * 0.01) for k in range(120)]
        self.assertEqual(vs, sorted(vs))


class Discharging(unittest.TestCase):
    def test_follows_a_constant_load(self):
        est, pack = battery.BatteryEstimator(), Pack(soc=70.0, r=0.55)
        e = pack.run(est, 600, load=600)
        self.assertEqual(e["state"], "discharging")
        self.assertLess(abs(e["soc_pct"] - pack.soc), 1.5)
        self.assertAlmostEqual(e["load_ma"], 600, delta=10)
        # 600 mA for 600 s is 100 mAh, counted
        self.assertAlmostEqual(e["used_mah"], 100, delta=3)
        self.assertAlmostEqual(e["autonomy_s"], e["soc_pct"] / 100 * 3200 / e["load_ma"] * 3600, delta=60)
        self.assertLess(e["trend_pct_h"], 0)
        self.assertIsNone(e["full_in_s"])

    def test_the_load_wandering_measures_r(self):
        est, pack = battery.BatteryEstimator(), like_25_11()
        e = pack.run(est, 600, load=200)
        self.assertAlmostEqual(e["r_measured_ohm"], 0.55, delta=0.08)
        self.assertLess(e["noise_mv"], 10)

    def test_a_mode_change_with_r_off_is_not_a_charge(self):
        # The default R (0.55) off by 0.15 Ohm and a 700 mA load step: the
        # baseline starts again instead of reading 105 mV as a charge. One
        # step alone does not make R either: it could be a charger switched
        # on in the same HK.
        est, pack, seen = battery.BatteryEstimator(), Pack(r=0.40), []
        e = pack.run(est, 300, load=300)
        self.assertIsNone(e["r_measured_ohm"])
        e = pack.run(est, 300, load=1000, watch=seen)
        self.assertIsNone(e["r_measured_ohm"])
        self.assertEqual({st for _, st in seen}, {"discharging"})

    def test_r_from_the_bench_detects_with_a_steady_load(self):
        # The load steady (no R from the HK): R measured on the bench is
        # trusted from the first HK, and the charger's step shows.
        model = battery.model_from(3200, 0.30, 0.30)
        est, pack, seen = battery.BatteryEstimator(model), Pack(r=0.30), []
        pack.run(est, 300, load=500)
        on = pack.t
        pack.run(est, 120, load=500, charge=600, watch=seen)
        self.assertLessEqual(first(seen, "charging", on), 20)

    def test_a_charger_in_the_same_hk_as_a_load_step_does_not_make_r(self):
        est, pack = battery.BatteryEstimator(), Pack(r=0.30)
        pack.run(est, 600, load=900)
        e = pack.run(est, 300, load=500, charge=1200)
        self.assertIsNone(e["r_measured_ohm"])

    def test_no_false_charge_over_half_an_hour(self):
        for seed in range(5):
            est, pack, seen = battery.BatteryEstimator(), like_25_11(seed=seed), []
            pack.run(est, 1800, load=200, watch=seen)
            self.assertEqual({st for _, st in seen}, {"discharging"}, seed)

    def test_a_load_step_with_r_known_is_not_a_charge(self):
        est, pack, seen = battery.BatteryEstimator(), like_25_11(), []
        pack.run(est, 300, load=200)
        pack.run(est, 300, load=450, watch=seen)          # the SDR on
        pack.run(est, 300, load=200, watch=seen)
        self.assertEqual({st for _, st in seen}, {"discharging"})


def first(seen, state, after):
    return next(t for t, st in seen if st == state and t > after) - after


class Charging(unittest.TestCase):
    def test_a_charger_switched_on_and_off(self):
        est, pack = battery.BatteryEstimator(), like_25_11(soc=60.0)
        pack.run(est, 300, load=500)
        e = pack.run(est, 20, load=500, charge=1200)
        self.assertEqual(e["state"], "charging")           # within two HK
        before = e["soc_pct"]
        e = pack.run(est, 600, load=500, charge=1200)
        self.assertEqual(e["state"], "charging")
        self.assertAlmostEqual(e["charge_ma"], 1200, delta=180)
        self.assertGreater(e["soc_pct"], before)
        self.assertLess(abs(e["soc_pct"] - pack.soc), 3.0)  # the level is not lifted by I*R
        self.assertIsNone(e["autonomy_s"])
        self.assertGreater(e["full_in_s"], 0)
        e = pack.run(est, 20, load=500)
        self.assertEqual(e["state"], "discharging")        # within two HK
        e = pack.run(est, 300, load=500)
        self.assertLess(abs(e["soc_pct"] - pack.soc), 3.0)

    def test_a_weak_charge_shows_in_seconds_both_ways(self):
        # 100 mA into the cells: 25 mV through their 0.25 Ohm, 4-5 noise units.
        for seed in range(5):
            est, pack, seen = battery.BatteryEstimator(), like_25_11(seed=seed), []
            pack.run(est, 300, load=200)
            on = pack.t
            pack.run(est, 300, load=200, charge=300, watch=seen)
            off = pack.t
            pack.run(est, 300, load=200, watch=seen)
            self.assertLessEqual(first(seen, "charging", on), 30, seed)
            self.assertLessEqual(first(seen, "discharging", off), 30, seed)

    def test_a_light_soon_after_a_mode_change(self):
        # 30 s after the load stepped down, the new baseline is there.
        est, pack, seen = battery.BatteryEstimator(), like_25_11(soc=60.0), []
        pack.run(est, 300, load=450)
        pack.run(est, 30, load=200)
        on = pack.t
        pack.run(est, 120, load=200, charge=500, watch=seen)
        self.assertLessEqual(first(seen, "charging", on), 30)

    def test_right_from_the_first_hk(self):
        # No wait for R: the light switched on a minute after the page started.
        est, pack, seen = battery.BatteryEstimator(), like_25_11(), []
        pack.run(est, 60, load=200)
        on = pack.t
        pack.run(est, 60, load=200, charge=300, watch=seen)
        self.assertLessEqual(first(seen, "charging", on), 30)

    def test_hk_every_2_s(self):
        # The page's fast HK: the same detector, five times the samples.
        for seed in range(5):
            est, pack, seen = battery.BatteryEstimator(), like_25_11(seed=seed), []
            pack.run(est, 300, load=200, every=2)
            on = pack.t
            pack.run(est, 120, load=200, charge=300, every=2, watch=seen)
            off = pack.t
            pack.run(est, 120, load=200, every=2, watch=seen)
            self.assertLessEqual(first(seen, "charging", on), 8, seed)
            self.assertLessEqual(first(seen, "discharging", off), 8, seed)
            self.assertEqual([st for t, st in seen if t <= on + 8 or on + 20 < t <= off][-1], "charging")

    def test_no_false_charge_with_hk_every_2_s(self):
        for seed in range(5):
            est, pack, seen = battery.BatteryEstimator(), like_25_11(seed=seed), []
            pack.run(est, 900, load=200, every=2, watch=seen)
            self.assertEqual({st for _, st in seen}, {"discharging"}, seed)


class TheDiode(unittest.TestCase):
    def test_d1_drop_ignored_reads_low(self):
        # The INA219 is after D1: without its 0.3 V the charge reads ~15 % low
        # in the plateau.
        right = Pack(soc=60.0, r=0.55).run(battery.BatteryEstimator(), 120, load=500)
        wrong = Pack(soc=60.0, r=0.55).run(battery.BatteryEstimator(battery.BatteryModel(diode_v=0.0)),
                                   120, load=500)
        self.assertLess(abs(right["soc_pct"] - 60.0), 1.5)
        self.assertGreater(60.0 - wrong["soc_pct"], 10.0)


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
        est.feed(0, 7300, 600, uptime_s=5)
        e = est.estimate()
        self.assertEqual((e["used_mah"], e["counted_s"]), (0.0, 0))


class InThePage(unittest.TestCase):
    def test_the_snapshot_carries_the_estimate(self):
        clock = [1000.0]
        s = gs_web.GsState(now=lambda: clock[0])
        for k in range(3):
            s.feed({"ev": "hk", "battery_mv": 7260, "current_ma": 600, "uptime_s": 10 * k,
                    "obc_mode": "PRE_TEST"})
            clock[0] += 10
        snap = s.snapshot()
        self.assertEqual(snap["battery"]["state"], "discharging")
        self.assertEqual(snap["hk_hist"][-1][6], snap["battery"]["soc_pct"])
        s.feed({"ev": "hk", "battery_mv": 65535, "current_ma": -32768, "uptime_s": 40})
        self.assertEqual(s.snapshot()["battery"]["state"], "no_data")



class FastHk(unittest.TestCase):
    def test_paced_by_the_ack_and_off_by_itself(self):
        f = gs_web.FastHk()
        self.assertFalse(f.due(0.0))
        f.start(0.0)
        self.assertTrue(f.due(0.0))
        f.sent(0.0)
        self.assertFalse(f.due(5.0))                      # no ACK yet
        f.acked()
        self.assertTrue(f.due(5.0))
        f.sent(5.0)
        f.acked()
        self.assertFalse(f.due(6.0))                      # 2 s apart
        self.assertTrue(f.due(7.0))
        f.sent(7.0)
        self.assertTrue(f.due(7.0 + f.ACK_TIMEOUT_S))     # an ACK lost: go on
        self.assertFalse(f.due(f.MAX_S + 1))
        self.assertEqual(f.snapshot(f.MAX_S + 1)["on"], False)

    def test_the_hub_sends_hk_and_waits_for_its_ack(self):
        class Source:
            def __init__(self):
                self.sent = []

            def send(self, cmd):
                self.sent.append(cmd)
                return True

        clock = [100.0]
        src = Source()
        hub = gs_web.Hub(gs_web.GsState(now=lambda: clock[0]), src)
        hub.mono = lambda: clock[0]
        try:
            def wait_for(n):
                for _ in range(40):
                    if len(src.sent) >= n:
                        return
                    time.sleep(0.05)

            self.assertTrue(hub.set_fast_hk(True)["ok"])
            wait_for(1)
            self.assertEqual(src.sent, ["hk"])
            clock[0] += 3
            time.sleep(0.4)
            self.assertEqual(len(src.sent), 1)                 # waiting for the ACK
            hub.feed({"ev": "ack", "cmd": "REQ_HK", "status": "OK", "latency_ms": 300, "attempts": 1})
            wait_for(2)
            self.assertEqual(len(src.sent), 2)
            snap = hub.snapshot()
            self.assertTrue(snap["fast_hk"]["on"])
            self.assertNotIn("REQ_HK", " ".join(e["text"] for e in snap["log"]))
            hub.set_fast_hk(False)
            hub.feed({"ev": "ack", "cmd": "REQ_HK", "status": "OK", "latency_ms": 300, "attempts": 1})
            clock[0] += 3
            time.sleep(0.4)
            self.assertEqual(len(src.sent), 2)
            self.assertFalse(hub.snapshot()["fast_hk"]["on"])
        finally:
            hub.close()

    def test_one_chart_point_and_one_log_line_in_10_s(self):
        clock = [1000.0]
        s = gs_web.GsState(now=lambda: clock[0])
        s.quiet_hk = True
        for k in range(10):                                # 20 s of HK every 2 s
            s.feed({"ev": "hk", "battery_mv": 7100, "current_ma": 180, "uptime_s": 2 * k,
                    "obc_mode": "PRE_TEST"})
            clock[0] += 2
        snap = s.snapshot()
        self.assertEqual(len(snap["hk_hist"]), 2)
        self.assertEqual(sum(1 for e in snap["log"] if e["text"].startswith("hk:")), 2)


if __name__ == "__main__":
    unittest.main()
