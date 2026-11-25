"""gs_web.py: the state from the ground's lines, the commands the page may
send, and the server end to end against a fake ground process."""

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import unittest
from unittest import mock
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # ground/host
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

    def test_hk_flags_are_decoded_and_its_line_has_no_sentinels(self):
        self.s.feed(dict(hk(), flags=4 | 8))
        h = self.s.snapshot()["hk"]
        self.assertEqual((h["obc_linked"], h["adsb_linked"], h["time_synced"], h["dump1090_up"]),
                         (False, False, True, True))
        self.s.feed(dict(hk(), battery_mv=65535, current_ma=-32768))
        text = self.s.snapshot()["log"][-1]["text"]
        self.assertEqual(text, "hk: PRE_TEST · 25.9 °C · 42.9 °C SoC")

    def test_the_log_comes_whole_or_as_what_is_new(self):
        for i in range(5):
            self.s.note("info", "line %d" % i)
        full = self.s.snapshot()
        self.assertTrue(full["full"])
        self.assertEqual(full["log_n"], 5)
        delta = self.s.snapshot(since=3)
        self.assertFalse(delta["full"])
        self.assertEqual([e["text"] for e in delta["log"]], ["line 3", "line 4"])

    def ack(self, cmd, status="OK", ms=300):
        self.s.feed({"ev": "ack", "id": 1, "cmd": cmd, "status": status, "latency_ms": ms, "attempts": 1})

    def test_a_mission_is_followed_from_the_command_to_the_downlink(self):
        self.clock.t = 0.0
        self.s.feed(hk())
        self.s.feed({"ev": "command", "text": "mode mission_adsb"})
        self.clock.t = 1.0
        self.ack("SET_TIME", ms=310)
        self.ack("SET_MODE", ms=289)
        self.clock.t = 4.0
        self.s.feed(dict(hk(mode="MISSION_ADSB"), flags=4))
        self.clock.t = 9.0
        self.s.feed(dict(hk(mode="MISSION_ADSB"), flags=4 | 8))
        self.s.feed({"ev": "tracks", "n": 8})
        self.clock.t = 10.0
        self.s.feed({"ev": "tracks", "n": 8})
        self.clock.t = 604.0
        self.s.feed(hk(mode="MISSION_DOWNLINK"))
        self.s.feed({"ev": "tracks", "n": 8})            # after the mission: not counted
        run = self.s.snapshot()["runs"][-1]
        self.assertEqual(run["target"], "MISSION_ADSB")
        self.assertEqual([(a["cmd"], a["status"], a["latency_ms"]) for a in run["acks"]],
                         [("SET_TIME", "OK", 310), ("SET_MODE", "OK", 289)])
        self.assertEqual((run["sent"], run["entered"], run["dump1090"], run["first_tracks"]), (0, 4.0, 9.0, 9.0))
        self.assertEqual((run["tracks"], run["left"], run["left_to"], run["failed"]), (2, 604.0, "MISSION_DOWNLINK", None))

    def test_a_refused_time_fails_the_run(self):
        self.s.feed({"ev": "command", "text": "mode mission_adsb"})
        self.ack("SET_TIME", status="BAD_ARGS")
        run = self.s.snapshot()["runs"][-1]
        self.assertEqual(run["failed"], "SET_TIME BAD_ARGS")
        self.s.feed({"ev": "command", "text": "mode mission_adsb"})
        self.s.feed({"ev": "tc_failed", "id": 2, "cmd": "SET_TIME", "attempts": 8})
        self.assertEqual(self.s.snapshot()["runs"][-1]["failed"], "SET_TIME NO_ACK")

    def test_a_pulse_shorter_than_the_hk_period_completes_the_run(self):
        # burnwire_ms = 750: no HK ever shows BASIC_INTERMEDIATE.
        self.clock.t = 0.0
        self.s.feed(hk())
        self.s.feed({"ev": "command", "text": "mode basic_inter"})
        self.clock.t = 0.3
        self.ack("SET_MODE", ms=290)
        self.s.feed(hk())                                # too soon after the ACK: proves nothing
        self.assertIsNone(self.s.snapshot()["runs"][-1]["entered"])
        self.clock.t = 8.0
        self.s.feed(hk())
        run = self.s.snapshot()["runs"][-1]
        self.assertEqual((run["entered"], run["left"], run["left_to"], run["inferred"]),
                         (0.3, 8.0, "PRE_TEST", True))

    def test_a_refused_pulse_is_not_inferred(self):
        self.s.feed(hk())
        self.s.feed({"ev": "command", "text": "mode basic_inter"})
        self.ack("SET_MODE", status="REJECTED_STATE")
        self.clock.t += 10
        self.s.feed(hk())
        run = self.s.snapshot()["runs"][-1]
        self.assertEqual((run["entered"], run["left"]), (None, None))

    def test_the_hk_history_keeps_the_power_series(self):
        self.s.feed(hk(battery=7400, current=80))
        self.clock.t += 10
        self.s.feed(hk(battery=65535, current=95))
        h = self.s.snapshot()["hk_hist"]
        self.assertEqual([r[1:3] for r in h], [[7400, 80], [None, 95]])
        self.assertEqual(h[0][4], None)                  # temp_ext: no data

    def test_other_commands_are_not_runs(self):
        for text in ("ping 3", "hk", "rate fast", "mode"):
            self.s.feed({"ev": "command", "text": text})
        self.assertEqual(self.s.snapshot()["runs"], [])


class TestHub(unittest.TestCase):
    def test_a_burst_reaches_the_pages_as_few_messages_with_every_entry(self):
        hub = gs_web.Hub(gs_web.GsState(), source=None)
        self.addCleanup(hub.close)
        q = hub.subscribe()
        first = json.loads(q.get(timeout=1))
        self.assertTrue(first["full"])
        for i in range(200):
            hub.note("info", "line %d" % i)
        import time
        time.sleep(1.3)
        msgs = []
        while not q.empty():
            msgs.append(json.loads(q.get_nowait()))
        self.assertLessEqual(len(msgs), 4)
        texts = [e["text"] for m in msgs for e in m["log"]]
        self.assertEqual(texts, ["line %d" % i for i in range(200)])

    def test_the_page_is_told_how_long_the_mission_lasts(self):
        hub = gs_web.Hub(gs_web.GsState(), source=None)
        self.addCleanup(hub.close)
        self.assertEqual(hub.snapshot()["mission_s"], 600)       # the flight's adsb_timeout
        short = gs_web.Hub(gs_web.GsState(), source=None, mission_s=120)
        self.addCleanup(short.close)
        self.assertEqual(short.snapshot()["mission_s"], 120)


# gama-deploy's output against a real obcd (tests/test_gama_deploy.sh), with
# the GPIO driven as on the board.
DEPLOY_DONE = textwrap.dedent("""\
    {"mono_ms":6940621,"event":"local_command","signal":10,"uid":0,"pid":40327}
    {"mono_ms":6940621,"event":"mode","from":"PRE_TEST","to":"BASIC_INTERMEDIATE","event":"TC_BASIC_INTER","by":"signal","after_ms":845}
    {"mono_ms":6940621,"event":"burnwire","on":true,"why":"deploy","driven":true}
    {"mono_ms":6945621,"event":"burnwire","on":false,"why":"pulse done","driven":true}
    {"mono_ms":6945621,"event":"mode","from":"BASIC_INTERMEDIATE","to":"PRE_TEST","event":"TASK_DONE","by":"burnwire","after_ms":5000}
    RESULT done pid=40322 log=/var/lib/gama/obcd.jsonl
""")
DEPLOY_REFUSED = textwrap.dedent("""\
    {"mono_ms":10,"event":"local_command","signal":10,"uid":0,"pid":1}
    {"mono_ms":10,"event":"event_refused","event":"TC_BASIC_INTER","mode":"MISSION_ADSB","by":"signal"}
    RESULT refused pid=2 log=/var/lib/gama/obcd.jsonl
""")


class Ran:
    def __init__(self, code, out, err=""):
        self.returncode, self.stdout, self.stderr = code, out, err


class TestDeploySsh(unittest.TestCase):
    def test_a_done_pulse_is_read_step_by_step(self):
        r = gs_web.parse_deploy(0, DEPLOY_DONE, "")
        self.assertTrue(r["ok"])
        self.assertEqual(r["result"], "done")
        self.assertEqual(r["burn_ms"], 5000)
        self.assertTrue(all(r["steps"].values()))

    def test_an_undriven_gpio_is_not_success(self):
        r = gs_web.parse_deploy(0, DEPLOY_DONE.replace('"driven":true', '"driven":false'), "")
        self.assertFalse(r["ok"])
        self.assertFalse(r["steps"]["driven"])
        self.assertIn("not driven", r["why"])

    def test_a_refusal_names_the_mode(self):
        r = gs_web.parse_deploy(1, DEPLOY_REFUSED, "")
        self.assertFalse(r["ok"])
        self.assertEqual(r["result"], "refused")
        self.assertIn("MISSION_ADSB", r["why"])
        self.assertFalse(r["steps"]["entered"])

    def test_sudo_without_its_rule_says_what_to_do(self):
        r = gs_web.parse_deploy(1, "", "sudo: a password is required\n")
        self.assertIsNone(r["result"])
        self.assertIn("make install", r["why"])

    def test_the_hub_needs_a_host_and_arming(self):
        calls = []
        hub = gs_web.Hub(gs_web.GsState(), source=None, runner=lambda *a, **k: calls.append(a))
        self.addCleanup(hub.close)
        self.assertIn("--pi-ssh", hub.deploy_ssh(True)["why"])
        hub.pi_ssh = "pi@gamapi.local"
        self.assertEqual(hub.deploy_ssh(False)["why"], "arm first")
        self.assertEqual(calls, [])

    def test_the_hub_runs_ssh_and_logs_the_outcome(self):
        calls = []
        def runner(argv, **kw):
            calls.append(argv)
            return Ran(0, DEPLOY_DONE)
        hub = gs_web.Hub(gs_web.GsState(), source=None, pi_ssh="pi@gamapi.local", runner=runner)
        self.addCleanup(hub.close)
        self.assertTrue(hub.deploy_ssh(True)["ok"])
        self.assertEqual(calls, [["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                                  "pi@gamapi.local", "sudo -n /usr/local/sbin/gama-deploy"]])
        texts = [e["text"] for e in hub.snapshot()["log"]]
        self.assertIn("deploy over ssh: burn-wire pulse done, back in PRE_TEST (pulse 5000 ms)", texts)


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


class TestSources(unittest.TestCase):
    def test_udp_runs_the_repository_gs_cli(self):
        # --udp once looked one directory above the repository (16/11).
        source, name = gs_web.build_source(gs_web.argparse.Namespace(
            replay=None, serial=None, udp=True, exec=None))
        self.assertEqual(name, "udp")
        self.assertEqual(source.argv[0], gs_web.GS_CLI)
        self.assertTrue(os.path.isfile(os.path.join(gs_web.REPO, "flight", "ttcd", "link.c")))
        self.assertTrue(gs_web.GS_CLI.startswith(gs_web.REPO + os.sep))


    def test_ssh_runs_gs_cli_on_the_pi(self):
        source, name = gs_web.build_source(gs_web.argparse.Namespace(
            replay=None, serial=None, udp=False, exec=None, ssh="pi@test", ssh_dir="ttec"))
        self.assertEqual(name, "ssh")
        with mock.patch.object(gs_web.subprocess, "Popen") as popen:
            source._spawn()
        argv = popen.call_args[0][0]
        self.assertEqual(argv[:2], ["ssh", "-T"])
        self.assertIn("BatchMode=yes", argv)
        self.assertEqual(argv[-2], "pi@test")
        self.assertEqual(argv[-1], "cd ttec && { pkill -x gs_cli && sleep 0.5; true; } && "
                                   "echo '@clock?' && read -r clock && exec "
                                   "build/tools/gs_cli/gs_cli --radio udp --auto-time on --clock \"$clock\"")

    def test_the_pi_gets_the_clock_when_it_asks(self):
        # 25/11: read when ssh started, the clock was 0.5 s to 4.4 s old by
        # the time gs_cli took it, and the satellite's anchor as far behind.
        # A shell plays the Pi (no pkill: it would reach a real gs_cli), a
        # script gs_cli prints the clock it was given and its first command.
        with tempfile.TemporaryDirectory() as d:
            gs = os.path.join(d, "build", "tools", "gs_cli")
            os.makedirs(gs)
            with open(os.path.join(gs, "gs_cli"), "w") as f:
                f.write("#!/bin/sh\n"
                        "printf '{\"ev\": \"argv\", \"argv\": \"%s\"}\\n' \"$*\"\n"
                        "read -r line\n"
                        "printf '{\"ev\": \"line\", \"line\": \"%s\"}\\n' \"$line\"\n")
            os.chmod(os.path.join(gs, "gs_cli"), 0o755)
            remote = gs_web.ssh_ground_argv("pi@test", d)[-1].replace("pkill -x gs_cli", "false")
            now = [1795614460.549]
            source = gs_web.SshSource("pi@test", d, clock=lambda: now[0])
            fed = []
            with mock.patch.object(gs_web, "ssh_ground_argv", lambda h, dd: ["sh", "-c", remote]):
                source._spawn()
            self.assertFalse(source.send("hk"))   # the Pi's shell would take it for the clock
            now[0] += 10.0                          # ssh connecting
            pump = threading.Thread(target=source._pump, args=(mock.Mock(feed=fed.append),))
            pump.start()
            deadline = time.time() + 5
            while not any(o.get("ev") == "argv" for o in fed) and time.time() < deadline:
                time.sleep(0.01)
            self.assertTrue(source.send("hk"))
            pump.join(5)
            source.proc.stdin.close()
            source.proc.stdout.close()
        self.assertEqual([o["argv"] for o in fed if o["ev"] == "argv"],
                         ["--radio udp --auto-time on --clock 1795614470549"])
        self.assertEqual([o["line"] for o in fed if o["ev"] == "line"], ["hk"])


    def test_a_dropped_ssh_is_started_again(self):
        # 25/11: the Pi's Wi-Fi went away for a minute and the page stayed
        # without a ground. Each run here prints one line and ends, as an
        # ssh that drops.
        line = '{"ev": "start", "radio": "udp", "profile": "NOMINAL"}'
        fed = []
        hub = mock.Mock(feed=fed.append)
        source = gs_web.SshSource("pi@test", "ttec", retry_s=0.05)
        with mock.patch.object(gs_web, "ssh_ground_argv",
                               lambda h, d: [sys.executable, "-c", "print(%r)" % line]):
            source.start(hub)
            deadline = time.time() + 5
            while sum(1 for o in fed if o.get("ev") == "start") < 3 and time.time() < deadline:
                time.sleep(0.02)
            source.stop()
        self.assertGreaterEqual(sum(1 for o in fed if o.get("ev") == "start"), 3)
        drops = [o for o in fed if o.get("ev") == "error"]
        self.assertTrue(drops and "dropped" in drops[0]["what"])
        # Each drop after lines is one error: never one per failed attempt.
        self.assertLessEqual(len(drops), sum(1 for o in fed if o.get("ev") == "start"))
        n = len(fed)
        time.sleep(0.2)
        self.assertEqual(len(fed), n)                 # stopped: no more attempts

    def test_both_links_start_where_the_satellite_is(self):
        args = gs_web.argparse.Namespace(replay=None, udp=False, exec=None, serial="/dev/ttyUSB0",
                                         power=14, ssh="pi@test", ssh_dir="ttec", link=None)
        on_ssh = lambda argv, **kw: mock.Mock(returncode=0, stdout="ssh\n")
        self.assertEqual(gs_web.initial_link(args, gs_web.build_links(args), on_ssh), "ssh")
        unreachable = lambda argv, **kw: mock.Mock(returncode=255, stdout="")
        self.assertEqual(gs_web.initial_link(args, gs_web.build_links(args), unreachable), "lora")
        args.link = "ssh"
        self.assertEqual(gs_web.initial_link(args, gs_web.build_links(args), unreachable), "ssh")
        source, name = gs_web.build_source(gs_web.argparse.Namespace(**dict(vars(args), link="lora")))
        self.assertEqual((name, source.argv[-2:]), ("serial", ["--power", "14"]))

    def test_one_source_kind(self):
        for argv in (["--serial", "/dev/x", "--udp"], ["--ssh", "pi@test", "--replay", "x.jsonl"], []):
            with self.assertRaises(SystemExit), mock.patch("sys.stderr"):
                gs_web.main(argv)


class FakeSource:
    def __init__(self, name):
        self.name, self.started, self.stopped = name, False, False

    def start(self, hub):
        self.started = True

    def stop(self):
        self.stopped = True


class TestLinkSwitch(unittest.TestCase):
    def hub(self, stdout, returncode=0, stderr=""):
        self.calls, self.made = [], []
        def runner(argv, **kw):
            self.calls.append(argv)
            return mock.Mock(returncode=returncode, stdout=stdout, stderr=stderr)
        def make(name):
            def f():
                s = FakeSource(name)
                self.made.append(s)
                return s
            return f
        self.old = FakeSource("lora")
        return gs_web.Hub(gs_web.GsState(source="serial"), self.old, runner=runner,
                          links={"lora": make("lora"), "ssh": make("ssh")}, link="lora", link_host="pi@test")

    def test_the_satellite_moves_then_the_page(self):
        hub = self.hub("RESULT ssh active\n")
        out = hub.switch_link("ssh")
        self.assertTrue(out["ok"])
        self.assertEqual(self.calls, [["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "pi@test",
                                       "sudo -n /usr/local/sbin/gama-link ssh"]])
        self.assertTrue(self.old.stopped)
        self.assertEqual([(s.name, s.started) for s in self.made], [("ssh", True)])
        snap = hub.snapshot()
        self.assertEqual((snap["source"], snap["link"]["current"]), ("ssh", "ssh"))
        self.assertIn("link: now ssh (satellite on ssh, ttcd active)", [e["text"] for e in snap["log"]])
        self.assertEqual(hub.switch_link("ssh")["why"], "already on ssh")
        self.assertEqual(len(self.calls), 1)

    def test_a_satellite_that_cannot_move_keeps_the_page(self):
        hub = self.hub("", returncode=1, stderr="sudo: a password is required\n")
        out = hub.switch_link("ssh")
        self.assertFalse(out["ok"])
        self.assertIn("make install on the Pi", out["why"])
        self.assertFalse(self.old.stopped)
        self.assertEqual(self.made, [])
        self.assertEqual(hub.snapshot()["link"]["current"], "lora")

    def test_back_to_a_radio_that_does_not_answer(self):
        hub = self.hub("RESULT lora activating\n", returncode=1)
        hub.link = "ssh"
        out = hub.switch_link("lora")
        self.assertFalse(out["ok"])                   # said, and the page follows anyway
        self.assertIn("the RA-02 does not answer?", out["why"])
        self.assertEqual(hub.snapshot()["link"]["current"], "lora")

    def test_without_both_links_there_is_no_switch(self):
        hub = gs_web.Hub(gs_web.GsState(), FakeSource("lora"), runner=None)
        self.assertIn("--serial and --ssh", hub.switch_link("ssh")["why"])
        self.assertEqual(hub.snapshot()["link"]["options"], [])


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
        self.assertIn('src="/js/main.js"', page)
        with self.get("/js/state.js") as r:
            self.assertEqual(r.headers["Content-Type"], "text/javascript; charset=utf-8")
            self.assertIn("EventSource('/events')", r.read().decode())
        with self.get("/fonts/OpenSans.woff2") as r:
            self.assertEqual(r.headers["Content-Type"], "font/woff2")
        for path in ("/../gs_web.py", "/%2e%2e/gs_web.py", "/js/../../gs_web.py", "/aocs/README.md", "/nothing.js"):
            with self.assertRaises(urllib.error.HTTPError, msg=path) as cm:
                self.get(path)
            self.assertEqual(cm.exception.code, 404, path)
            cm.exception.close()
        with self.get("/api/adsb") as r:
            self.assertEqual(json.loads(r.read())["aircraft"], [])
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
