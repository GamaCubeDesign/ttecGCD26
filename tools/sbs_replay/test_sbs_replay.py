"""sbs_replay's AVR port (ADR-0023): the identification messages it plays
must be ones adsbd accepts, at a transponder's pace.

Run by CTest (sbs_replay); also: python3 -m unittest test_sbs_replay
"""

import json
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sbs_replay as r  # noqa: E402


def decode(line):
    """(icao, set, category, crc ok) of an AVR identification line."""
    m = bytes.fromhex(line.strip()[1:-1])
    tc, ca = m[4] >> 3, m[4] & 7
    return m[1:4].hex().upper(), "ABCD"[4 - tc], ca, r.crc24(m) == 0


class Ident(unittest.TestCase):
    def test_the_classic_message_byte_for_byte(self):
        # KLM1023, 4840D6, category A0: the textbook example, which pins the
        # callsign's 6-bit coding and the CRC at once.
        self.assertEqual(r.avr_ident("4840D6", "A0", "KLM1023"),
                         "*8D4840D6202CC371C32CE0576098;\n")

    def test_every_category_round_trips_with_a_good_crc(self):
        for cat in ("A1", "A2", "A3", "A5", "A7", "B1", "C3", "D0"):
            icao, s, ca, ok = decode(r.avr_ident("e0b244", cat, "ARG1842"))
            self.assertEqual((icao, s + str(ca), ok), ("E0B244", cat, True))

    def test_what_is_not_a_category_gives_nothing(self):
        for bad in (None, "", "A", "A8", "E1", "A33", 3):
            self.assertIsNone(r.avr_ident("E0B244", bad))

    def test_pace_first_after_a_second_then_every_five(self):
        pacer = r.IdentPacer()
        rec = {"icao": "E0B244", "category": "A3"}
        sent = [t for t in [x * 0.5 for x in range(30)] if pacer.lines(rec, 100 + t)]
        self.assertEqual(sent, [1.0, 6.0, 11.0])
        self.assertEqual(pacer.lines({"icao": "E0B244"}, 200), "")      # no category


class Ports(unittest.TestCase):
    def test_both_ports_from_one_capture(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "c.ndjson"
            now = time.time_ns()
            with open(path, "w") as f:
                for k in range(8):
                    f.write(json.dumps({"icao": "E0B244", "category": "A3", "callsign": "ARG1842",
                                        "rx_epoch_ns": now + k * 500_000_000, "lat": -34.8,
                                        "lon": -58.5, "altitude_ft": 6000}) + "\n")
            sbs_port, avr_port = free_port(), free_port()
            p = subprocess.Popen([sys.executable, str(HERE / "sbs_replay.py"), "--ndjson", str(path),
                                  "--port", str(sbs_port), "--avr-port", str(avr_port),
                                  "--exit-at-end", "--speed", "4"],
                                 stderr=subprocess.PIPE, text=True)
            try:
                avr = connect(avr_port)
                sbs = connect(sbs_port)
                sbs_text, avr_text = read_all(sbs), read_all(avr, 1.5)
            finally:
                p.wait(timeout=10)
                p.stderr.close()
            self.assertEqual(sbs_text.count("MSG,3,"), 8)
            idents = [decode(x) for x in avr_text.splitlines()]
            self.assertTrue(idents)
            self.assertTrue(all(i == ("E0B244", "A", 3, True) for i in idents))

    def test_avr_needs_an_ndjson_input(self):
        p = subprocess.run([sys.executable, str(HERE / "sbs_replay.py"), "--sbs", "x.sbs",
                            "--avr-port", "31999"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 2)
        self.assertIn("--avr-port needs --ndjson", p.stderr)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def connect(port):
    for _ in range(100):
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(0.05)
    raise AssertionError(f"nothing listening on {port}")


def read_all(s, idle=None):
    s.settimeout(idle or 5)
    out = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    s.close()
    return out.decode("ascii")


if __name__ == "__main__":
    unittest.main()
