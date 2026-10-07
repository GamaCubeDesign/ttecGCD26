import unittest

from ground.temporal import regression, vertical_features, quality
from ground.flight_state import FlightStateEstimator
from ground.decision_fusion import DecisionFusion
from ground.runway import evidence
from test_ground import observation


def snapshot(seconds, latitude=0, longitude=-0.1):
    return {
        "lat": latitude,
        "lon": longitude,
        "position_age_s": 0,
        "fields": {"track_deg": {"value": 90}, "altitude_ft": {"value": 2000}},
        "trajectory": [
            {
                "event_ns": int((seconds - 40 + i * 10) * 1e9),
                "lat": latitude,
                "lon": longitude - 0.04 + i * 0.01,
                "altitude_ft": 2400 - i * 100,
            }
            for i in range(5)
        ],
    }


def candidate(ident=1, score=0.9):
    return {
        "airport_id": ident,
        "ident": str(ident),
        "score": score,
        "evidence_coverage": 0.85,
        "components": {"trend": 0.9},
        "evidence": {"height_above_airport_ft": 2000, "initial_height_ft": 1000},
    }


class TemporalTests(unittest.TestCase):
    def test_regression_tolerates_isolated_altitude_error(self):
        points = [
            (t, 1000 + 10 * t + (8000 if t == 30 else 0)) for t in range(0, 70, 10)
        ]
        self.assertAlmostEqual(regression(points)["slope"], 10)

    def test_vertical_from_altitude_only(self):
        history = [observation(t, altitude_ft=1000 + 10 * t) for t in range(0, 61, 10)]
        result = vertical_features(history, history[-1].event_ns)
        self.assertAlmostEqual(result["value_fpm"], 600)
        self.assertEqual(result["source"], "altitude_regression")
        self.assertNotIn("vertical_rate_fpm", history[-1].fields)

    def test_vertical_disagreement_abstains(self):
        history = [
            observation(t, altitude_ft=1000 + 10 * t, vertical_rate_fpm=-900)
            for t in range(0, 61, 10)
        ]
        result = vertical_features(history, history[-1].event_ns)
        self.assertTrue(result["conflict"])
        self.assertIsNone(result["value_fpm"])

    def test_phase_persistence_hysteresis_and_missing_evidence(self):
        model = FlightStateEstimator()

        def update(t, rate):
            return model.update(
                snapshot(t),
                {"value_fpm": rate, "evidence_ns": int(t * 1e9), "conflict": False},
            )

        self.assertEqual(update(0, 600), "UNKNOWN")
        self.assertEqual(update(0, 600), "UNKNOWN")
        self.assertEqual(update(10, 600), "CLIMB")
        self.assertEqual(update(20, 200), "CLIMB")
        self.assertEqual(update(30, 0), "CLIMB")
        self.assertEqual(update(40, 0), "LEVEL")
        self.assertEqual(update(50, None), "UNKNOWN")

    def test_quality_rejects_jump(self):
        data = snapshot(60)
        data["trajectory"][-1]["lon"] = 40
        self.assertFalse(quality(data)["usable"])
        self.assertTrue(quality(snapshot(60))["usable"])

    def test_runway_approach_and_wrong_side(self):
        runway = {
            "le_latitude_deg": 0,
            "le_longitude_deg": 0,
            "le_heading_degT": 90,
            "le_ident": "09",
            "le_elevation_ft": 0,
        }
        self.assertTrue(evidence(snapshot(60), [runway], 0)["approach_compatible"])
        self.assertIsNone(evidence(snapshot(60, longitude=0.1), [runway], 0))

    def test_missing_elevation_does_not_claim_approach(self):
        runway = {"le_latitude_deg": 0, "le_longitude_deg": 0, "le_heading_degT": 90}
        self.assertFalse(evidence(snapshot(60), [runway], None)["approach_compatible"])

    def test_temporal_fusion_no_accumulation_from_duplicate_positions(self):
        fusion = DecisionFusion()
        data = snapshot(60)
        for _ in range(20):
            result = fusion.update([candidate()], data, quality(data), "DESCENT")
        self.assertEqual(result["status"], "UNKNOWN")
        self.assertEqual(fusion.updates, 1)
        for t in [70, 80]:
            data = snapshot(t)
            result = fusion.update([candidate()], data, quality(data), "DESCENT")
        self.assertEqual(result["status"], "ESTIMATED")
        self.assertLessEqual(result["compatibility"], 1)

    def test_ambiguity_level_and_high_altitude_abstain(self):
        for phase, competitors, height in [
            ("DESCENT", True, 2000),
            ("LEVEL", False, 2000),
            ("DESCENT", False, 35000),
        ]:
            fusion = DecisionFusion()
            for t in [60, 70, 80]:
                data = snapshot(t)
                c = candidate()
                c["evidence"]["height_above_airport_ft"] = height
                result = fusion.update(
                    [c, candidate(2, 0.88)] if competitors else [c],
                    data,
                    quality(data),
                    phase,
                )
            self.assertEqual(result["status"], "UNKNOWN")

    def test_new_winner_requires_its_own_persistence(self):
        fusion = DecisionFusion()
        for t in [60, 70, 80]:
            data = snapshot(t)
            fusion.update([candidate()], data, quality(data), "DESCENT")
        data = snapshot(90)
        result = fusion.update([candidate(2, 1)], data, quality(data), "DESCENT")
        self.assertEqual(result["status"], "UNKNOWN")

    def test_same_direction_different_magnitude_is_not_conflict(self):
        history = [
            observation(t, altitude_ft=6000 + 30 * t, vertical_rate_fpm=2752)
            for t in range(0, 61, 10)
        ]
        result = vertical_features(history, history[-1].event_ns)
        self.assertFalse(result["conflict"])
        self.assertTrue(result["magnitude_warning"])
        self.assertEqual(result["value_fpm"], 2752)

    def test_recent_level_segment_overrides_old_descent_average(self):
        history = [
            observation(t, altitude_ft=10000 - 20 * min(t, 40), vertical_rate_fpm=0)
            for t in range(0, 81, 5)
        ]
        result = vertical_features(history, history[-1].event_ns)
        self.assertEqual(result["derived_fpm"], 0)
        self.assertFalse(result["conflict"])
