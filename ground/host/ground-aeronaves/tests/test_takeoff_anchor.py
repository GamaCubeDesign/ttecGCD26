import unittest

import test_ground
from ground.aircraft_manager import AircraftManager
from ground.decision_fusion import DecisionFusion
from ground.trajectory import build_snapshot
from test_temporal import candidate, snapshot
from ground.temporal import quality


class AnchorTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_ground.GroundTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def test_anchor_survives_takeoff_and_expires_between_episodes(self):
        f = self.fixture
        manager = AircraftManager(f.db, f.estimator, f.config)
        for t in (0, 10):
            manager.process(
                test_ground.observation(
                    t, lat=0, lon=-0.2, on_ground=1, altitude_ft=100
                )
            )
        for t in (20, 30, 40):
            result = manager.process(
                test_ground.observation(
                    t,
                    lat=0,
                    lon=-0.2 + (t - 10) * 0.001,
                    on_ground=0,
                    altitude_ft=100 + (t - 10) * 10,
                    vertical_rate_fpm=600,
                )
            )
        self.assertEqual(result["flight_state"], "CLIMB")
        self.assertEqual(result["snapshot"]["takeoff_anchor"]["lon"], -0.2)
        self.assertEqual(
            result["snapshot"]["takeoff_anchor"]["event_ns"],
            test_ground.observation(10).event_ns,
        )
        result = manager.process(
            test_ground.observation(1000, lat=0, lon=0, on_ground=0)
        )
        self.assertNotIn("takeoff_anchor", result["snapshot"])

    def test_propagated_flag_and_old_messages_do_not_move_anchor(self):
        f = self.fixture
        manager = AircraftManager(f.db, f.estimator, f.config)
        manager.process(test_ground.observation(10, lat=0, lon=-0.2, on_ground=1))
        manager.process(test_ground.observation(20, lat=0, lon=-0.19))
        manager.process(test_ground.observation(5, lat=0, lon=-0.21, on_ground=1))
        self.assertEqual(manager.takeoff_anchor["ABC123"]["lon"], -0.2)

    def test_anchor_improves_origin_ranking_without_changing_destination(self):
        estimator = self.fixture.estimator
        data = build_snapshot([test_ground.observation(lat=0, lon=0, track_deg=90)], 60)
        before = estimator.estimate(data, "CLIMB")
        self.assertEqual(
            estimator.estimate(data, "CLIMB", origin=True)[0]["ident"], "WEST"
        )
        data["takeoff_anchor"] = {"lat": 0, "lon": 0.2}
        result = estimator.estimate(data, "CLIMB", origin=True)
        self.assertEqual(result[0]["ident"], "EAST")
        self.assertEqual(result[0]["evidence"]["takeoff_anchor_distance_km"], 0)
        self.assertEqual(estimator.estimate(data, "CLIMB"), before)


class TentativeTests(unittest.TestCase):
    def test_tentative_keeps_evidence_phase_distance_and_persistence_gates(self):
        cases = [
            ("DESCENT", False, 0.9, 50, 2000, 0.85, "TENTATIVE"),
            ("APPROACH", False, 0.9, 30, 2000, 0.85, "TENTATIVE"),
            ("DESCENT", False, 0.64, 30, 2000, 0.85, "UNKNOWN"),
            ("DESCENT", False, 0.9, 51, 2000, 0.85, "UNKNOWN"),
            ("DESCENT", False, 0.9, 30, 35000, 0.85, "UNKNOWN"),
            ("DESCENT", False, 0.9, 30, 2000, 0.54, "UNKNOWN"),
            ("LEVEL", False, 0.9, 30, 2000, 0.85, "UNKNOWN"),
            ("CLIMB", True, 0.9, 30, 2000, 0.85, "UNKNOWN"),
        ]
        for phase, origin, score, distance, height, coverage, expected in cases:
            with self.subTest(
                phase=phase,
                origin=origin,
                score=score,
                distance=distance,
                height=height,
                coverage=coverage,
            ):
                fusion = DecisionFusion()
                for t in (60, 70, 80):
                    data = snapshot(t)
                    c = candidate(score=score)
                    c["evidence"].update(
                        distance_km=distance, height_above_airport_ft=height
                    )
                    c["evidence_coverage"] = coverage
                    result = fusion.update(
                        [c, candidate(2, score - 0.02)],
                        data,
                        quality(data),
                        phase,
                        origin,
                    )
                    if t < 80:
                        self.assertEqual(result["status"], "UNKNOWN")
                self.assertEqual(result["status"], expected)
                if expected == "TENTATIVE":
                    self.assertLess(result["margin"], 0.08)
                    self.assertEqual(
                        result["reason"], "CANDIDATO_SEM_SEPARACAO_SUFICIENTE"
                    )
