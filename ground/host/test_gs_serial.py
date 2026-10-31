"""gs_serial.py against a fake ESP32 on a pseudo-terminal.

The fake prints what the firmware prints — ROM boot noise, the `boot` line,
answers to `set` and `begin`, an echo per command — and records what it was
sent, so the bridge's protocol is checked without a board: the clock and the
options before `begin`, one command per echo, `quit` at the end of input,
only JSON on stdout, and a reboot in the middle reported as an error.
"""

import json
import os
import subprocess
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "gs_serial.py")


def line(ev, **fields):
    obj = {"mono_ms": 100, "wall_ms": 1793000000000, "ev": ev}
    obj.update(fields)
    return json.dumps(obj, separators=(",", ":")) + "\n"


class FakeEsp32(threading.Thread):
    def __init__(self, master, reboot_on=None):
        super().__init__(daemon=True)
        self.master = master
        self.received = []
        self.reboot_on = reboot_on

    def write(self, text):
        os.write(self.master, text.encode())

    def run(self):
        time.sleep(0.5)                       # after the bridge has opened the port
        self.write("ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\r\n")
        self.write(line("boot", fw="test", radio="ok", why=""))
        buf = b""
        while True:
            try:
                chunk = os.read(self.master, 1024)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode()
                self.received.append(text)
                self.answer(text)

    def answer(self, text):
        words = text.split()
        if words[0] == "set":
            self.write(line("set", key=words[1], value=words[2]))
        elif words[0] == "begin":
            self.write(line("start", radio="esp32", profile="NOMINAL"))
        else:
            self.write(line("command", text=text))
            if text == self.reboot_on:
                self.write(line("boot", fw="test", radio="ok", why=""))
            elif words[0] == "hk":
                self.write("a stray log line, not JSON\r\n")
                self.write(line("ack", id=1, cmd="REQ_HK", status="OK"))
            elif words[0] == "quit":
                self.write(line("summary", dl_frames=1))


def run_bridge(commands, reboot_on=None, args=()):
    master, slave = os.openpty()
    fake = FakeEsp32(master, reboot_on)
    fake.start()
    proc = subprocess.run([sys.executable, BRIDGE, "--port", os.ttyname(slave), *args],
                          input=commands, capture_output=True, text=True, timeout=30)
    os.close(slave)
    return proc, fake


class TestGsSerial(unittest.TestCase):
    def test_a_session_looks_like_gs_cli(self):
        proc, fake = run_bridge("# comment\n\nhk\nwait 1\n",
                                args=("--power", "2", "--lbt", "preamble"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        out = proc.stdout.splitlines()
        events = [json.loads(l)["ev"] for l in out]       # every line is JSON
        self.assertEqual(events, ["start", "command", "ack", "command", "command", "summary"])
        self.assertIn("stray log line", proc.stderr)
        self.assertNotIn("ets Jun", proc.stdout)
        # The clock, then the options, then begin, then the commands and quit.
        sent = fake.received
        self.assertTrue(sent[0].startswith("set clock "))
        self.assertGreater(int(sent[0].split()[2]), 1767225600000)
        self.assertEqual(sent[1:4], ["set power 2", "set lbt preamble", "begin"])
        self.assertEqual(sent[4:], ["hk", "wait 1", "quit"])

    def test_a_reboot_mid_session_is_an_error(self):
        proc, _ = run_bridge("hk\nping\nhk\n", reboot_on="ping")
        self.assertEqual(proc.returncode, 1)
        last = json.loads(proc.stdout.splitlines()[-1])
        self.assertEqual(last["ev"], "error")
        self.assertEqual(last["what"], "ground station rebooted")


if __name__ == "__main__":
    unittest.main()
