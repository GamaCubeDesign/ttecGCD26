"""ADR-0022: the "large" class from kinematics when no category arrives."""
import unittest

import test_ground
from adsb.estimator.aircraft_category import infer_class
from adsb.estimator.aircraft_manager import AircraftManager


def obs(t, **fields):
    return test_ground.observation(t, **fields)


class InferClass(unittest.TestCase):
    def test_sustained_speed_altitude_or_climb_make_it_large(self):
        self.assertEqual(infer_class([obs(0, ground_speed_kt=420), obs(5, ground_speed_kt=421)]), 'A3')
        self.assertEqual(infer_class([obs(0, altitude_ft=35000), obs(5, altitude_ft=35025)]), 'A3')
        self.assertEqual(infer_class([obs(0, vertical_rate_fpm=2500, ground_speed_kt=180),
                                      obs(5, vertical_rate_fpm=2400, ground_speed_kt=185)]), 'A3')

    def test_one_value_is_never_enough(self):
        # A single corrupt altitude or speed must not decide the class.
        self.assertIsNone(infer_class([obs(0, altitude_ft=35000), obs(5, altitude_ft=3000)]))
        self.assertIsNone(infer_class([obs(0, ground_speed_kt=600)]))

    def test_light_aircraft_and_helicopters_stay_unknown(self):
        light = [obs(t, ground_speed_kt=120, altitude_ft=3000, vertical_rate_fpm=700) for t in range(0, 300, 5)]
        heli = [obs(t, ground_speed_kt=60, altitude_ft=700, vertical_rate_fpm=-192) for t in range(0, 300, 5)]
        self.assertIsNone(infer_class(light))
        self.assertIsNone(infer_class(heli))
        # A fast climb, but at a light aircraft's speed: not evidence of size.
        self.assertIsNone(infer_class([obs(t, vertical_rate_fpm=2000, ground_speed_kt=120) for t in (0, 5, 10)]))

    def test_positions_on_the_ground_do_not_count(self):
        self.assertIsNone(infer_class([obs(0, ground_speed_kt=300, on_ground=1),
                                       obs(5, ground_speed_kt=300, on_ground=1)]))


class InManager(unittest.TestCase):
    def setUp(self):
        self.fixture = test_ground.GroundTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        f = self.fixture
        self.manager = AircraftManager(f.db, f.estimator, f.config)

    def run_track(self, points, **extra):
        result = None
        for t, gs, alt in points:
            result = self.manager.process(obs(t, lat=0, lon=-0.1 + t * 1e-4, ground_speed_kt=gs,
                                              altitude_ft=alt, track_deg=90, **extra))
        return result

    def test_inferred_and_kept_for_the_episode(self):
        # Cruise, then a slow approach: the jet must stay large.
        result = self.run_track([(0, 430, 33000), (5, 430, 33000), (10, 140, 2000), (15, 135, 1800)])
        category = result['aircraft_category']
        self.assertEqual((category['code'], category['source']), ('A3', 'kinematics'))

    def test_a_received_category_always_wins(self):
        result = self.run_track([(0, 430, 33000), (5, 430, 33000)], category='A1')
        self.assertEqual(result['aircraft_category']['code'], 'A1')
        self.assertNotEqual(result['aircraft_category']['source'], 'kinematics')

    def test_a0_is_no_category(self):
        result = self.run_track([(0, 430, 33000), (5, 430, 33000)], category='A0')
        self.assertEqual((result['aircraft_category']['code'], result['aircraft_category']['source']),
                         ('A3', 'kinematics'))

    def test_off_keeps_the_estimator_as_it_was(self):
        from dataclasses import replace
        f = self.fixture
        self.manager = AircraftManager(f.db, f.estimator, replace(f.config, kinematic_category=False))
        result = self.run_track([(0, 430, 33000), (5, 430, 33000)])
        self.assertIsNone(result['aircraft_category']['code'])


if __name__ == '__main__':
    unittest.main()
