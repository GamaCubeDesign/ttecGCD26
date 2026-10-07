import json
import tempfile
import time
import unittest
from pathlib import Path

import test_ground
from ground.live_input import LiveInput
from ground.visualize import LiveData


class LiveInputTests(unittest.TestCase):
    def test_existing_messages_and_new_partial_line_are_processed(self):
        fixture = test_ground.GroundTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        source = fixture.config.output.parent / "eventos.ndjson"
        stamp = time.time_ns()

        def message(n):
            return json.dumps(
                {
                    "icao": "F00001",
                    "rx_epoch_ns": stamp + n,
                    "lat": 0,
                    "lon": 0.01,
                    "altitude_ft": 2000,
                }
            )

        # Simulator started before the web interface.
        source.write_text(message(0) + "\n")
        worker = LiveInput(source, fixture.config)
        worker.start()
        self.addCleanup(worker.close)
        live = LiveData(
            fixture.config.telemetry_db, fixture.config.reference_db, worker.session
        )

        def wait_count(expected):
            limit = time.monotonic() + 5
            while time.monotonic() < limit:
                data = json.loads(live.read()[1])
                if (
                    len(data["aircraft"].get("F00001", {}).get("frames", []))
                    == expected
                ):
                    return
                time.sleep(0.02)
            self.fail("O processamento automático não recebeu as linhas esperadas")

        wait_count(1)
        with source.open("a") as f:
            f.write(message(1))
            f.flush()
        time.sleep(0.3)
        self.assertEqual(
            len(json.loads(live.read()[1])["aircraft"]["F00001"]["frames"]), 1
        )
        with source.open("a") as f:
            f.write("\n")
        wait_count(2)
        worker.close()
