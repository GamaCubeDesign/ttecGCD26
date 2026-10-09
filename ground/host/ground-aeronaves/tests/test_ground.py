import io
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

from ground.__main__ import run
from ground.airport_estimator import AirportEstimator
from ground.aircraft_manager import AircraftManager
from ground.config import Config
from ground.database import Database
from ground.geo import distance_km, angle_delta
from ground.parser import parse_line
from ground.trajectory import build_snapshot


def observation(seconds=0, **fields):
    data = {
        "icao": "abc123",
        "rx_epoch_ns": 1_790_000_000_000_000_000 + int(seconds * 1e9),
        **fields,
    }
    return parse_line(json.dumps(data), data["rx_epoch_ns"] + 10)


class GroundTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        reference = root / "references.db"
        with sqlite3.connect(reference) as c:
            c.execute(
                "CREATE TABLE airports(id INTEGER PRIMARY KEY, ident TEXT, icao_code TEXT, name TEXT, type TEXT, latitude_deg REAL, longitude_deg REAL, elevation_ft REAL)"
            )
            c.executemany(
                "INSERT INTO airports VALUES (?,?,?,?,?,?,?,?)",
                [
                    (1, "EAST", "EAST", "East", "small_airport", 0, 0.2, 100),
                    (2, "WEST", "WEST", "West", "small_airport", 0, -0.2, 100),
                    (3, "DATE", "DATE", "Date line", "small_airport", 0, -179.9, None),
                    (4, "CLOSED", None, "Closed", "closed", 0, 0.1, 100),
                ],
            )
        self.config = Config(
            reference_db=reference,
            telemetry_db=root / "telemetry.db",
            output=root / "state.json",
        )
        self.db = Database(self.config.telemetry_db)
        self.estimator = AirportEstimator(self.config)

    def test_raw_missing_fields_preserved(self):
        obs = observation(altitude_ft=0)
        self.db.save(obs)
        stored = self.db.recent(obs.icao, 900, 100)[0]
        self.assertEqual(stored.raw, obs.raw)
        self.assertNotIn("lat", stored.fields)
        self.assertEqual(stored.fields["altitude_ft"], 0)

    def test_invalid_messages_and_valid_partial(self):
        for raw in [
            "{}",
            "[]",
            '{"icao":"abc123","lat":NaN}',
            '{"icao":"abc123","lat":true}',
            '{"icao":"abc123","rx_epoch_ns":1.5}',
            '{"icao":"abc123","lat":91}',
            '{"icao":"abc123","icao":"123456"}',
        ]:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                parse_line(raw, 1)
        self.assertEqual(parse_line('{"icao":"abc123"}', 123).event_ns, 123)

    def test_out_of_order_and_aircraft_isolation(self):
        manager = AircraftManager(self.db, self.estimator, self.config)
        manager.process(observation(20, lat=0, lon=0.1, track_deg=90))
        result = manager.process(observation(10, lat=0, lon=0, track_deg=90))
        self.assertEqual(result["snapshot"]["lon"], 0.1)
        self.assertEqual([p["lon"] for p in result["snapshot"]["trajectory"]], [0, 0.1])
        other = parse_line('{"icao":"123456","lat":20,"lon":20}', 1)
        manager.process(other)
        self.assertEqual(len(self.db.recent("ABC123", 900, 100)), 2)

    def test_stale_position_not_used(self):
        history = [
            observation(0, lat=0, lon=0, track_deg=90),
            observation(61, altitude_ft=1000),
        ]
        self.assertIsNone(build_snapshot(history, 60))

    def test_unknown_ground_flag_is_preserved(self):
        history = [
            observation(0, lat=0, lon=0, on_ground=1),
            observation(1, on_ground=-1),
        ]
        self.db.save(history[-1])
        snapshot = build_snapshot(history, 60)
        self.assertNotIn("on_ground", snapshot["fields"])
        self.assertEqual(self.db.recent("ABC123", 900, 10)[0].fields["on_ground"], -1)

    def test_zero_track_and_field_provenance(self):
        history = [
            observation(0, lat=0, lon=0, track_deg=0),
            observation(10, altitude_ft=1000),
        ]
        snapshot = build_snapshot(history, 60)
        self.assertEqual(snapshot["fields"]["track_deg"]["value"], 0)
        self.assertEqual(snapshot["fields"]["track_deg"]["source"], "last_known")
        self.assertNotIn("track_deg", history[-1].fields)

    def test_airport_direction_and_origin_gate(self):
        snapshot = build_snapshot([observation(lat=0, lon=0, track_deg=90)], 60)
        result = self.estimator.estimate(snapshot, "UNKNOWN")
        self.assertEqual(result[0]["ident"], "EAST")
        self.assertNotIn("CLOSED", [r["ident"] for r in result])
        self.assertEqual(self.estimator.estimate(snapshot, "DESCENT", origin=True), [])
        self.assertEqual(
            self.estimator.estimate(snapshot, "CLIMB", origin=True)[0]["ident"], "WEST"
        )

    def test_dateline_and_missing_elevation(self):
        snapshot = build_snapshot(
            [
                observation(
                    lat=0,
                    lon=179.9,
                    track_deg=90,
                    altitude_ft=3000,
                    ground_speed_kt=150,
                    vertical_rate_fpm=-800,
                )
            ],
            60,
        )
        result = self.estimator.estimate(snapshot, "DESCENT")
        self.assertEqual(result[0]["ident"], "DATE")
        self.assertNotIn("vertical_profile", result[0]["components"])
        self.assertLess(distance_km(0, 179.9, 0, -179.9), 23)
        self.assertEqual(angle_delta(359, 1), 2)

    def test_restart_reloads_history(self):
        self.db.save(observation(0, lat=0, lon=0))
        manager = AircraftManager(self.db, self.estimator, self.config)
        result = manager.process(observation(10, lat=0, lon=0.01))
        self.assertEqual(len(result["snapshot"]["trajectory"]), 2)
        self.assertEqual(
            result["snapshot"]["fields"]["track_deg"]["source"], "derived_positions"
        )

    def test_new_execution_does_not_read_previous_execution(self):
        self.db.save(observation(120, lat=0, lon=0.1))
        new_db = Database(self.config.telemetry_db)
        self.assertEqual(new_db.recent("ABC123", 900, 100), [])
        manager = AircraftManager(new_db, self.estimator, self.config)
        result = manager.process(observation(0, lat=0, lon=0, track_deg=90))
        self.assertEqual(result["event_ns"], observation(0).event_ns)
        self.assertEqual(result["snapshot"]["lon"], 0)
        with sqlite3.connect(self.config.telemetry_db) as c:
            self.assertEqual(
                c.execute("SELECT COUNT(*) FROM observations").fetchone()[0], 2
            )

    def test_report_does_not_claim_landing_during_climb(self):
        from ground.reporting import table_row

        manager = AircraftManager(self.db, self.estimator, self.config)
        result = manager.process(
            observation(lat=0, lon=0, track_deg=90, vertical_rate_fpm=1000)
        )
        self.assertIsNone(result["airport_summary"]["landing"])
        self.assertIsNone(result["airport_summary"]["takeoff"])
        row = table_row(result, 60)
        self.assertEqual(result["airport_method"]["origin"]["status"], "UNKNOWN")
        self.assertIn("indeterminado", row)
        self.assertNotIn("%", row)

    def test_temporal_destination_without_received_vertical_rate(self):
        manager = AircraftManager(self.db, self.estimator, self.config)
        for t in range(0, 81, 10):
            result = manager.process(
                observation(
                    t,
                    lat=0,
                    lon=0.02 + t * 0.001,
                    altitude_ft=4000 - 10 * t,
                    track_deg=270,
                )
            )
        # Direction temporarily pointing away must not dominate convergence.
        self.assertEqual(result["flight_state"], "DESCENT")
        self.assertEqual(result["vertical_estimate"]["source"], "altitude_regression")
        self.assertEqual(result["airport_method"]["destination"]["status"], "ESTIMATED")
        self.assertEqual(
            result["airport_summary"]["landing"]["airport"]["ident"], "EAST"
        )
        self.assertIsNone(result["airport_summary"]["takeoff"])

    def test_vertical_conflict_does_not_invalidate_horizontal_trajectory(self):
        manager = AircraftManager(self.db, self.estimator, self.config)
        for t in range(0, 61, 10):
            result = manager.process(
                observation(
                    t,
                    lat=0,
                    lon=t * 0.001,
                    altitude_ft=1000 + 10 * t,
                    vertical_rate_fpm=-900,
                )
            )
        self.assertTrue(result["vertical_estimate"]["conflict"])
        self.assertTrue(result["data_quality"]["usable"])
        self.assertIsNotNone(result["snapshot"])
        self.assertEqual(result["airport_method"]["destination"]["status"], "UNKNOWN")

    def test_pipeline_quarantines_and_continues(self):
        source = io.StringIO(
            observation(lat=0, lon=0, track_deg=90).raw
            + "\nINVALID\n"
            + observation(1, altitude_ft=1000).raw
            + "\n"
        )
        from contextlib import redirect_stdout, redirect_stderr

        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            counts = run(self.config, source, 0)
        self.assertEqual(counts, {"accepted": 2, "rejected": 1})
        with sqlite3.connect(self.config.telemetry_db) as c:
            self.assertEqual(c.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            self.assertEqual(
                c.execute("SELECT COUNT(*) FROM estimates").fetchone()[0], 2
            )
            self.assertEqual(
                c.execute("SELECT COUNT(*) FROM rejected_messages").fetchone()[0], 1
            )
        state = json.loads(self.config.output.read_text())["aircraft"]["ABC123"]
        self.assertIn("airport_method", state)
        self.assertNotIn("route_method", state)
        self.assertNotIn("route_summary", state)


if __name__ == "__main__":
    unittest.main()
