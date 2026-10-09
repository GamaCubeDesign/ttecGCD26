"""tracks_to_ndjson: the downlink in the ground estimator's contract.

Run by CTest; also: python3 -m unittest discover -s ground/host -p 'test_*.py'
"""

import io
import json
import sys
import unittest
from pathlib import Path

from tracks_to_ndjson import convert, records

# The estimator itself, next to this file: its parser is the contract.
sys.path.insert(0, str(Path(__file__).resolve().parent / "ground-aeronaves"))
from ground.parser import parse_line  # noqa: E402

# A TM_TRACKS event exactly as tools/gs_cli prints it.
EVENT = {
    "mono_ms": 1813903, "wall_ms": 1790607292702, "ev": "tracks", "seq": 41,
    "rssi": -50, "snr": 10, "toa_ms": 700, "n": 2,
    "records": [
        {"icao": "E48DF5", "lat": -15.70000, "lon": -47.90000, "alt_ft": 20000,
         "gs_kt": 420.0, "trk": 180.00, "vr_fpm": -640, "age_ds": 12, "flags": 15},
        {"icao": "E49608", "lat": -15.87000, "lon": -47.92000, "alt_ft": 0,
         "gs_kt": 12.0, "trk": 275.30, "vr_fpm": 0, "age_ds": 3, "flags": 21},
    ],
    "payload": "00",
}


def lines(*events):
    return [json.dumps(e) + "\n" for e in events]


def run(*events):
    out = io.StringIO()
    written, repeats = convert(lines(*events), out.write)
    return [json.loads(x) for x in out.getvalue().splitlines()], written, repeats


class Adapter(unittest.TestCase):
    def test_time_is_arrival_minus_air_minus_age(self):
        rows, _, _ = run(EVENT)
        self.assertEqual(rows[0]["rx_epoch_ns"],
                         1790607292702_000_000 - 700_000_000 - 12 * 100_000_000)
        self.assertEqual(rows[1]["rx_epoch_ns"],
                         1790607292702_000_000 - 700_000_000 - 3 * 100_000_000)

    def test_only_flagged_fields_are_written(self):
        rows, _, _ = run(EVENT)
        a, b = rows
        self.assertEqual(a["altitude_ft"], 20000)          # 15: pos, alt, vel, vrate
        self.assertEqual(a["vertical_rate_fpm"], -640)
        self.assertNotIn("on_ground", a)
        self.assertNotIn("altitude_ft", b)                 # 21: pos, vel, on ground
        self.assertNotIn("vertical_rate_fpm", b)
        self.assertEqual(b["on_ground"], 1)
        self.assertEqual(b["ground_speed_kt"], 12.0)

    def test_every_line_passes_the_estimator_parser(self):
        # ground-aeronaves/ground/parser.py raises on anything outside its
        # contract, and must take the rebuilt time, not its own arrival.
        out = io.StringIO()
        convert(lines(EVENT), out.write)
        rows = out.getvalue().splitlines()
        self.assertEqual(len(rows), 2)
        for raw in rows:
            obs = parse_line(raw, 1)
            self.assertEqual(obs.timestamp_source, "rx_epoch_ns")
            self.assertEqual(obs.event_ns, json.loads(raw)["rx_epoch_ns"])

    def test_a_repeated_update_is_written_once(self):
        # Next snapshot, 5 s later: the same update, 5 s older.
        again = dict(EVENT, wall_ms=EVENT["wall_ms"] + 5000, seq=42,
                     records=[dict(EVENT["records"][0], age_ds=62)])
        rows, written, repeats = run(EVENT, again)
        self.assertEqual(written, 2)
        self.assertEqual(repeats, 1)

    def test_a_new_update_is_written(self):
        moved = dict(EVENT, wall_ms=EVENT["wall_ms"] + 5000, seq=42,
                     records=[dict(EVENT["records"][0], lat=-15.71, age_ds=10)])
        rows, written, repeats = run(EVENT, moved)
        self.assertEqual(written, 3)
        self.assertEqual(repeats, 0)

    def test_other_events_and_noise_are_ignored(self):
        out = io.StringIO()
        written, _ = convert(['{"mono_ms":1,"wall_ms":2,"ev":"hk","seq":1}\n',
                              "not json\n", "\n", '{"ev":"tracks","wall_ms":5,"records":[]}\n'],
                             out.write)
        self.assertEqual(written, 0)
        self.assertEqual(out.getvalue(), "")

    def test_a_record_with_nothing_valid_is_skipped(self):
        empty = dict(EVENT, records=[dict(EVENT["records"][0], flags=0)])
        self.assertEqual(records(empty), [])


if __name__ == "__main__":
    unittest.main()
