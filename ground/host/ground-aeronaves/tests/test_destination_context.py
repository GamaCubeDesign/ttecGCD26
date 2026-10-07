"""Ground truth fornecido pelo usuário: E480F9 chegou a BSB; não é prior do modelo."""

import unittest
from pathlib import Path

from ground.config import Config
from ground.airport_estimator import AirportEstimator
from ground.decision_fusion import DecisionFusion
from ground.parser import parse_line
from ground.trajectory import build_snapshot
from ground.temporal import approach_context, vertical_features, quality
from test_ground import observation


class DestinationContextTests(unittest.TestCase):
    def test_level_after_descent_preserves_context(self):
        history = [
            observation(
                t,
                lat=0,
                lon=t * 0.001,
                altitude_ft=8000 - 20 * min(t, 100),
                ground_speed_kt=300 - 0.5 * min(t, 100),
            )
            for t in range(0, 161, 10)
        ]
        context = approach_context(history, history[-1].event_ns)
        self.assertTrue(context["descent_context"])
        self.assertGreater(context["speed_drop_kt"], 30)

    def test_sustained_climb_rebounds_from_descent(self):
        history = [
            observation(
                t, altitude_ft=8000 - 20 * t if t <= 100 else 6000 + 40 * (t - 100)
            )
            for t in range(0, 161, 10)
        ]
        self.assertFalse(
            approach_context(history, history[-1].event_ns)["descent_context"]
        )

    def test_e480f9_destination_candidate_without_origin_hint(self):
        config = Config()
        if not config.reference_db.exists():
            self.skipTest("Referência geográfica local não instalada")
        fixture_path = Path(__file__).parent / "fixtures/e480f9.ndjson"
        if not fixture_path.exists():
            self.skipTest("Captura real local não distribuída no Git")
        lines = fixture_path.read_text().splitlines()
        history = [parse_line(line, 0) for line in lines if line.strip()]
        final = history[-1].event_ns
        estimator = AirportEstimator(config)
        fusion = DecisionFusion()
        for seconds_before in (30, 15, 0):
            prefix = [o for o in history if o.event_ns <= final - seconds_before * 1e9]
            now = prefix[-1].event_ns
            snapshot = build_snapshot(prefix, 60)
            snapshot["vertical"] = vertical_features(prefix, now)
            snapshot["approach_context"] = approach_context(prefix, now)
            # Explicitly test a brief LEVEL segment, not just DESCENT.
            candidates = estimator.estimate(snapshot, "LEVEL")
            result = fusion.update(candidates, snapshot, quality(snapshot), "LEVEL")
        self.assertEqual(candidates[0]["ident"], "SBBR")
        self.assertIn(result["status"], ("TENTATIVE", "ESTIMATED"))
        self.assertEqual(result["candidate"]["ident"], "SBBR")
        self.assertEqual(result["confidence"], "BAIXA")
        # This sample does not observe takeoff and must not fabricate Cuiabá.
        self.assertEqual(estimator.estimate(snapshot, "LEVEL", origin=True), [])

    def test_top_is_display_limit_not_inference_limit(self):
        # Uses the synthetic airport database from the existing fixture.
        from test_ground import GroundTests

        fixture = GroundTests()
        fixture.setUp()
        try:
            from dataclasses import replace

            estimator = AirportEstimator(replace(fixture.config, top=1))
            snapshot = build_snapshot([observation(lat=0, lon=0, track_deg=90)], 60)
            self.assertGreater(len(estimator.estimate(snapshot, "UNKNOWN")), 1)
        finally:
            fixture.doCleanups()
