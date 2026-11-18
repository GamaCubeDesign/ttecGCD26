"""ADR-0022: how far the take-off should be, from the climb so far."""
import unittest
from dataclasses import replace

import test_ground
from ground.aircraft_manager import AircraftManager
from ground.airport_estimator import AirportEstimator, climb_back
from ground.geo import distance_km


class ClimbBack(unittest.TestCase):
    def test_the_expected_distance(self):
        # 5000 ft above the field at 2500 ft/min: 2 min after take-off; at
        # 180 kt that is 180 × 1.852 × 2 / 60 = 11.112 km.
        c = climb_back({'altitude_ft': 5100, 'ground_speed_kt': 180}, 2500, 100, 11.112)
        self.assertEqual(c['expected_km'], 11.112)
        self.assertEqual(c['score'], 1.0)
        self.assertEqual(c['sigma_km'], 3.334)                     # 30 % of 11.112
        far = climb_back({'altitude_ft': 5100, 'ground_speed_kt': 180}, 2500, 100, 30)
        self.assertLess(far['score'], 0.01)

    def test_the_tolerance_has_a_floor(self):
        c = climb_back({'altitude_ft': 600, 'ground_speed_kt': 150}, 2000, 100, 1.0)
        self.assertEqual(c['sigma_km'], 3.0)

    def test_no_evidence_is_none_not_a_bad_score(self):
        fields = {'altitude_ft': 5000, 'ground_speed_kt': 180}
        self.assertIsNone(climb_back(fields, 200, 100, 10))               # barely climbing
        self.assertIsNone(climb_back(fields, None, 100, 10))
        self.assertIsNone(climb_back(fields, 2000, None, 10))             # field elevation unknown
        self.assertIsNone(climb_back({'altitude_ft': 5000}, 2000, 100, 10))
        self.assertIsNone(climb_back(fields, 2000, 6000, 10))             # below the field


class InEstimator(unittest.TestCase):
    def setUp(self):
        self.fixture = test_ground.GroundTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def climb_from_west(self, config):
        # Seen first already airborne east of WEST (lon -0.2), climbing east:
        # no ground position, so no take-off anchor.
        f = self.fixture
        manager = AircraftManager(f.db, AirportEstimator(config), config)
        result = None
        for t in (0, 10, 20, 30):
            result = manager.process(test_ground.observation(
                t, lat=0, lon=-0.17 + t * 0.0008, altitude_ft=1500 + t * 30, vertical_rate_fpm=1800,
                ground_speed_kt=160, track_deg=90, on_ground=0))
        return result

    def test_the_origin_carries_the_climb_back_evidence_without_an_anchor(self):
        result = self.climb_from_west(self.fixture.config)
        self.assertEqual(result['flight_state'], 'CLIMB')
        west = next(c for c in result['origin_candidates'] if c['ident'] == 'WEST')
        self.assertIn('climb_back', west['evidence'])
        self.assertIn('climb_back', west['components'])
        self.assertNotIn('takeoff_anchor', result['snapshot'])
        # The destination never uses it.
        for c in result['destination_candidates']:
            self.assertNotIn('climb_back', c['evidence'])

    def test_weight_zero_turns_it_off(self):
        result = self.climb_from_west(replace(self.fixture.config, climb_back_weight=0))
        for c in result['origin_candidates']:
            self.assertNotIn('climb_back', c['components'])

    def test_the_closer_airport_on_the_climb_wins(self):
        # WEST is ~3 km behind and fits a climb that started ~1 min ago; EAST
        # is ahead. The origin must be WEST.
        result = self.climb_from_west(self.fixture.config)
        self.assertEqual(result['origin_candidates'][0]['ident'], 'WEST')
        lon = result['snapshot']['lon']
        self.assertLess(distance_km(0, -0.2, 0, lon), distance_km(0, 0.2, 0, lon))


if __name__ == '__main__':
    unittest.main()
