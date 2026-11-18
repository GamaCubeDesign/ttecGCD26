"""estado.json at most once per publish_interval_s, and never left stale."""
import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

from adsb.estimator import aircraft_manager
from adsb.estimator.aircraft_manager import AircraftManager
from adsb.estimator.config import Config


class PublishInterval(unittest.TestCase):
    def manager(self, interval, out):
        m = AircraftManager.__new__(AircraftManager)
        m.config = replace(Config(), output=out, publish_interval_s=interval)
        m.states, m.pairs = {}, None
        m._published_at, m._publish_pending = float('-inf'), False
        return m

    def test_held_back_then_flushed_then_closed(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'estado.json'
            m = self.manager(1.0, out)
            clock = [100.0]
            with mock.patch.object(aircraft_manager.time, 'monotonic', lambda: clock[0]):
                m.states['A'] = {'n': 1}
                m.publish()                                  # the first one goes out
                self.assertEqual(json.loads(out.read_text())['aircraft']['A']['n'], 1)
                for n in range(2, 6):
                    clock[0] += 0.1
                    m.states['A'] = {'n': n}
                    m.publish()                              # held back
                self.assertEqual(json.loads(out.read_text())['aircraft']['A']['n'], 1)
                m.flush()                                    # still inside the second
                self.assertEqual(json.loads(out.read_text())['aircraft']['A']['n'], 1)
                clock[0] += 1.0
                m.flush()
                self.assertEqual(json.loads(out.read_text())['aircraft']['A']['n'], 5)
                m.states['A'] = {'n': 6}
                m.publish()
                m.close()                                    # nothing pending is lost
                self.assertEqual(json.loads(out.read_text())['aircraft']['A']['n'], 6)

    def test_zero_writes_every_change(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'estado.json'
            m = self.manager(0.0, out)
            for n in range(3):
                m.states['A'] = {'n': n}
                m.publish()
                self.assertEqual(json.loads(out.read_text())['aircraft']['A']['n'], n)


if __name__ == '__main__':
    unittest.main()
