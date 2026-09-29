import json
import unittest

import test_ground
from ground.geo import distance_km
from ground.trajectory import build_snapshot


class DescentFeasibilityTests(unittest.TestCase):
    def setUp(self):
        self.fixture=test_ground.GroundTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.estimator=self.fixture.estimator

    def candidate(self,phase='LEVEL',origin=False,**fields):
        snapshot=build_snapshot([test_ground.observation(lat=0,lon=0,**fields)],60)
        return next(c for c in self.estimator.estimate(snapshot,phase,origin=origin) if c['ident']=='EAST')

    def test_high_cruise_near_airport_is_penalized(self):
        result=self.candidate(altitude_ft=35000,ground_speed_kt=450,track_deg=90)
        self.assertGreater(result['evidence']['required_descent_fpm'],5000)
        self.assertEqual(result['evidence']['descent_feasibility'],0)
        self.assertEqual(result['score'],0)

    def test_thresholds_and_linear_transition(self):
        minutes=distance_km(0,0,0,.2)/1.852/180*60
        for rate,factor in [(2000,1),(2500,1),(3750,.5),(5000,0),(6000,0)]:
            with self.subTest(rate=rate):
                altitude=100+rate*minutes
                base=self.candidate(altitude_ft=altitude)
                result=self.candidate(altitude_ft=altitude,ground_speed_kt=180)
                self.assertAlmostEqual(result['evidence']['required_descent_fpm'],rate,places=1)
                self.assertEqual(result['evidence']['descent_feasibility'],factor)
                self.assertAlmostEqual(result['score'],base['score']*factor,delta=.0001)

    def test_missing_or_zero_speed_does_not_invent_feasibility(self):
        for fields in ({'altitude_ft':35000},{'ground_speed_kt':450},{'altitude_ft':35000,'ground_speed_kt':0}):
            with self.subTest(fields=fields):
                self.assertNotIn('descent_feasibility',self.candidate(**fields)['evidence'])
        snapshot=build_snapshot([test_ground.observation(lat=0,lon=179.9,altitude_ft=35000,ground_speed_kt=450)],60)
        result=next(c for c in self.estimator.estimate(snapshot,'LEVEL') if c['ident']=='DATE')
        self.assertNotIn('descent_feasibility',result['evidence'])

    def test_zero_distance_produces_finite_json(self):
        for altitude,factor in [(35000,0),(100,1)]:
            snapshot=build_snapshot([test_ground.observation(lat=0,lon=.2,altitude_ft=altitude,ground_speed_kt=450)],60)
            result=next(c for c in self.estimator.estimate(snapshot,'LEVEL') if c['ident']=='EAST')
            self.assertEqual(result['evidence']['descent_feasibility'],factor)
            json.dumps(result,allow_nan=False)

    def test_penalty_does_not_change_descent_or_origin(self):
        for phase,origin in [('DESCENT',False),('CLIMB',True),('UNKNOWN',False)]:
            with self.subTest(phase=phase):
                a=self.candidate(phase,origin,altitude_ft=35000)
                b=self.candidate(phase,origin,altitude_ft=35000,ground_speed_kt=450)
                self.assertEqual(a['score'],b['score'])
                if origin:
                    self.assertNotIn('descent_feasibility',b['evidence'])

    def test_level_trend_has_less_influence_than_previous_unknown_weight(self):
        history=[test_ground.observation(t,lat=0,lon=t*.001,track_deg=270) for t in range(0,41,10)]
        snapshot=build_snapshot(history,60)
        level=next(c for c in self.estimator.estimate(snapshot,'LEVEL') if c['ident']=='EAST')
        unknown=next(c for c in self.estimator.estimate(snapshot,'UNKNOWN') if c['ident']=='EAST')
        self.assertLess(level['score'],unknown['score'])
