"""gs_web.py: the state from the ground's lines, the commands the page may
send, and the server end to end against a fake ground process."""

import json
import os
import subprocess
import sys
import textwrap
import unittest
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gs_web  # noqa: E402


def hk(mode="PRE_TEST", battery=7400, current=80, temp=2588, t=0):
    return {"mono_ms": t, "wall_ms": 1793000000000 + t, "ev": "hk", "seq": 1, "rssi": -50,
            "snr": 10, "battery_mv": battery, "current_ma": current, "temp_bat_ccel": temp,
            "temp_ext_ccel": -32768, "temp_soc_ccel": 4293, "roll_cdeg": -32768,
            "pitch_cdeg": -32768, "yaw_cdeg": -32768, "adsb_msgs": 4294967295,
            "obc_mode": mode, "link": "IDLE", "uptime_s": 16, "flags": 1}


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class TestCommands(unittest.TestCase):
    def test_allowed(self):
        for cmd in ("ping 3", "hk", "stat", "roster", "time", "stop", "stream 5", "stream 2.5",
                    "rate fast", "mode mission_adsb", "mode downlink", "mode survival",
                    "mode task_done"):
            self.assertIsNone(gs_web.check_command(cmd, False), cmd)

    def test_refused(self):
        for cmd in ("shutdown confirm", "deafen 10", "quit", "set power 20", "ping", "ping 3; hk",
                    "mode 1", "rate turbo", "hk\nquit", "", None, 5):
            self.assertIsNotNone(gs_web.check_command(cmd, True), repr(cmd))

    def test_mechanism_and_aocs_need_arming(self):
        for cmd in ("mode basic_inter", "mode aocs"):
            self.assertEqual(gs_web.check_command(cmd, False), "arm first")
            self.assertIsNone(gs_web.check_command(cmd, True))


class TestState(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.s = gs_web.GsState("test", now=self.clock)

    def test_no_data_becomes_none(self):
        self.s.feed(hk(battery=65535, current=-32768, temp=-32768))
        snap = self.s.snapshot()["hk"]
        self.assertIsNone(snap["battery_mv"])
        self.assertIsNone(snap["current_ma"])
        self.assertIsNone(snap["temp_bat_ccel"])
        self.assertIsNone(snap["adsb_msgs"])
        self.assertEqual(snap["obc_mode"], "PRE_TEST")

    def test_values_pass_through(self):
        self.s.feed(hk(battery=7400, current=84, temp=2588))
        snap = self.s.snapshot()["hk"]
        self.assertEqual((snap["battery_mv"], snap["current_ma"], snap["temp_bat_ccel"]), (7400, 84, 2588))

    def test_mode_change_is_logged_once(self):
        self.s.feed(hk("PRE_TEST"))
        self.s.feed(hk("PRE_TEST"))
        self.clock.t += 5
        self.s.feed(hk("MISSION_ADSB"))
        modes = [e for e in self.s.snapshot()["log"] if e["kind"] == "mode"]
        self.assertEqual(len(modes), 1)
        self.assertIn("PRE_TEST → MISSION_ADSB", modes[0]["text"])
        self.assertEqual(self.s.snapshot()["mode"], "MISSION_ADSB")

    def test_ages_follow_the_clock(self):
        self.s.feed(hk())
        self.clock.t += 12.34
        snap = self.s.snapshot()
        self.assertEqual(snap["hk_age"], 12.3)
        self.assertEqual(snap["frame_age"], 12.3)

    def test_contact_acks_and_failures(self):
        self.s.feed({"ev": "contact", "up": True})
        self.s.feed({"ev": "ack", "cmd": "PING", "status": "OK", "latency_ms": 268, "attempts": 1})
        self.s.feed({"ev": "ack", "cmd": "SET_MODE", "status": "OK", "latency_ms": 290, "attempts": 1})
        self.s.feed({"ev": "tc_failed", "cmd": "SET_MODE", "attempts": 27})
        snap = self.s.snapshot()
        self.assertTrue(snap["contact"])
        self.assertEqual(snap["acks"], {"ok": 2, "bad": 0, "tc_failed": 1, "rx_bad": 0, "latency_ms": 279})
        self.s.feed({"ev": "contact", "up": False})
        self.assertFalse(self.s.snapshot()["contact"])

    def test_radio_error_is_shown_and_cleared_by_start(self):
        self.s.feed({"ev": "error", "what": "ground station radio: unexpected RegVersion"})
        self.assertIn("RegVersion", self.s.snapshot()["error"])
        self.s.feed({"ev": "start", "radio": "esp32", "profile": "NOMINAL"})
        self.assertIsNone(self.s.snapshot()["error"])
        self.assertEqual(self.s.snapshot()["ground"], {"radio": "esp32", "profile": "NOMINAL"})

    def test_link_series_are_bounded(self):
        for i in range(200):
            self.s.feed(hk(t=i))
        snap = self.s.snapshot()
        self.assertEqual(len(snap["rssi"]), gs_web.GsState.SPARK_MAX)
        self.assertLessEqual(len(snap["log"]), 120)

    def test_unknown_lines_are_ignored(self):
        self.s.feed({"x": 1})
        self.s.feed({"ev": "something_new", "a": 1})
        self.assertIsNone(self.s.snapshot()["hk"])


FAKE_GROUND = textwrap.dedent("""
    import json, sys
    def out(**o):
        print(json.dumps(o), flush=True)
    out(ev="start", radio="udp", profile="NOMINAL")
    out(ev="contact", up=True)
    out(ev="hk", seq=1, rssi=-50, snr=10, battery_mv=7400, current_ma=80, temp_bat_ccel=2588,
        temp_ext_ccel=-32768, temp_soc_ccel=4293, roll_cdeg=-32768, pitch_cdeg=-32768,
        yaw_cdeg=-32768, adsb_msgs=4294967295, obc_mode="PRE_TEST", link="IDLE", uptime_s=5, flags=1)
    for line in sys.stdin:
        out(ev="command", text=line.strip())
        if line.strip() == "quit":
            break
""")


class TestServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "gs_web.py"), "--port", "0", "--exec",
             sys.executable, "-c", FAKE_GROUND],
            stdout=subprocess.PIPE, text=True)
        first = cls.proc.stdout.readline()
        cls.base = first.split(" on ")[1].strip()

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=5)
        cls.proc.stdout.close()

    def get(self, path):
        return urllib.request.urlopen(self.base + path, timeout=5)

    def post(self, cmd, armed=False):
        req = urllib.request.Request(
            self.base + "/api/cmd", data=json.dumps({"cmd": cmd, "armed": armed}).encode(),
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def wait_state(self, ready):
        import time
        for _ in range(50):
            snap = json.loads(self.get("/api/state").read())
            if ready(snap):
                return snap
            time.sleep(0.1)
        self.fail("the state never got there")

    def test_page_and_state(self):
        page = self.get("/").read().decode()
        self.assertIn("EventSource('/events')", page)
        snap = self.wait_state(lambda s: s["hk"] is not None)
        self.assertEqual(snap["mode"], "PRE_TEST")
        self.assertIsNone(snap["hk"]["adsb_msgs"])
        self.assertTrue(snap["contact"])

    def test_command_reaches_the_ground(self):
        self.assertEqual(self.post("ping 3")[0], 200)
        self.wait_state(lambda s: any(e["text"] == "> ping 3" for e in s["log"]))

    def test_refused_commands_do_not_reach_the_ground(self):
        self.assertEqual(self.post("shutdown confirm", armed=True)[0], 403)
        self.assertEqual(self.post("mode basic_inter")[0], 403)
        code, body = self.post("mode basic_inter", armed=True)
        self.assertEqual((code, body["ok"]), (200, True))
        snap = self.wait_state(lambda s: any(e["text"] == "> mode basic_inter" for e in s["log"]))
        self.assertFalse(any("shutdown" in e["text"] for e in snap["log"]))

    def test_event_stream_starts_with_the_state(self):
        with self.get("/events") as r:
            self.assertEqual(r.headers["Content-Type"], "text/event-stream")
            line = r.readline().decode()
            self.assertTrue(line.startswith("data: "))
            json.loads(line[6:])


if __name__ == "__main__":
    unittest.main()
