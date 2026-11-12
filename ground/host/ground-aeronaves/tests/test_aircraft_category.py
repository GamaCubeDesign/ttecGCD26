"""Category filtering through NDJSON, SQLite, temporal state and airport ranking."""
import json
import sqlite3
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from ground.aircraft_category import describe_category
from ground.aircraft_manager import AircraftManager
from ground.airport_estimator import AirportEstimator
from ground.config import Config
from ground.database import Database
from ground.decision_fusion import DecisionFusion
from ground.temporal import quality
from ground.trajectory import build_snapshot
from ground.visualize import build_data
from simular_trajetorias import Flight, scenarios
from test_ground import observation
from test_temporal import candidate, snapshot


class CategoryTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        ref = root/'reference.db'
        with sqlite3.connect(ref) as db:
            db.execute('CREATE TABLE airports(id INTEGER PRIMARY KEY, ident TEXT, icao_code TEXT, name TEXT, type TEXT, latitude_deg REAL, longitude_deg REAL, elevation_ft REAL)')
            db.executemany('INSERT INTO airports VALUES(?,?,?,?,?,?,?,?)', [
                (i, ident, ident, ident, kind, 0, .2, 100)
                for i, ident, kind in [(1,'SMALL','small_airport'),(2,'MEDIUM','medium_airport'),
                                      (3,'LARGE','large_airport'),(4,'HELI','heliport'),(5,'CLOSED','closed')]
            ])
        self.config = Config(reference_db=ref, telemetry_db=root/'telemetry.db', output=root/'state.json')
        self.estimator = AirportEstimator(self.config)

    def data(self, **extra):
        return build_snapshot([observation(t, lat=0, lon=.02+t*.001,
            altitude_ft=3000-10*t, vertical_rate_fpm=-600, track_deg=90,
            ground_speed_kt=150, **extra) for t in (0,10,20,30,40)], 60)

    def test_fixed_wing_excludes_only_heliport_for_both_endpoints(self):
        before = self.config.reference_db.read_bytes()
        for code in ('A1','A2','A3','A5'):
            for origin in (False, True):
                with self.subTest(code=code, origin=origin):
                    data = self.data(category=code)
                    results = self.estimator.estimate(data, 'CLIMB' if origin else 'DESCENT', origin)
                    self.assertEqual({r['ident'] for r in results}, {'SMALL','MEDIUM','LARGE'})
                    diagnostic = data['airport_filters']['origin' if origin else 'destination']
                    self.assertEqual(diagnostic['considered'], 4)
                    self.assertEqual(diagnostic['excluded'], 1)
                    self.assertEqual(diagnostic['excluded_by_reason'], {'ASA_FIXA_EM_HELIPORTO':1})
        self.assertEqual(self.config.reference_db.read_bytes(), before)

    def test_helicopter_keeps_airports_and_heliports_without_runway_prior(self):
        with patch('ground.airport_estimator.runway_evidence') as runway:
            results = self.estimator.estimate(self.data(category='A7'), 'APPROACH')
            runway.assert_not_called()
        self.assertEqual({r['ident'] for r in results}, {'SMALL','MEDIUM','LARGE','HELI'})
        self.assertTrue(all(r['evidence']['aircraft_compatibility']['factor']==1 for r in results))
        unknown = self.estimator.estimate(self.data(), 'APPROACH')
        heli = next(r for r in results if r['ident']=='HELI')
        unknown_heli = next(r for r in unknown if r['ident']=='HELI')
        self.assertGreater(heli['score'],unknown_heli['score'])

    def test_unknown_invalid_reserved_and_unsupported_are_neutral(self):
        baseline = self.estimator.estimate(self.data(), 'DESCENT')
        for code in ('A0','A4','A6','B1','B6','C2','D7',None,'',7,True,{},[], 'A9'):
            with self.subTest(code=code):
                results = self.estimator.estimate(self.data(category=code), 'DESCENT')
                self.assertEqual([(r['ident'],r['score']) for r in results],
                                 [(r['ident'],r['score']) for r in baseline])

    def test_large_category_prefers_size_without_fabricating_coverage(self):
        baseline = {r['ident']:r for r in self.estimator.estimate(self.data(), 'DESCENT')}
        results = self.estimator.estimate(self.data(category='A3'), 'DESCENT')
        self.assertEqual([r['ident'] for r in results], ['LARGE','MEDIUM','SMALL'])
        for item in results:
            self.assertEqual(item['evidence_coverage'], baseline[item['ident']]['evidence_coverage'])
            self.assertLessEqual(item['score'], baseline[item['ident']]['score'])
        a1 = self.estimator.estimate(self.data(category='A1'), 'DESCENT')
        self.assertEqual({r['score'] for r in a1}, {baseline['SMALL']['score']})

    def test_stronger_trajectory_can_still_favor_a_small_airport_for_a3(self):
        with sqlite3.connect(self.config.reference_db) as db:
            db.execute("UPDATE airports SET longitude_deg=-3 WHERE type IN ('medium_airport','large_airport')")
        estimator = AirportEstimator(self.config)
        results = estimator.estimate(self.data(category='A3'), 'DESCENT')
        self.assertEqual(results[0]['ident'], 'SMALL')
        self.assertEqual(len(results), 3)

    def test_helicopter_destination_can_be_estimated_without_runway_alignment(self):
        with sqlite3.connect(self.config.reference_db) as db:
            db.execute("UPDATE airports SET longitude_deg=-3 WHERE type!='heliport'")
        manager = AircraftManager(Database(self.config.telemetry_db),AirportEstimator(self.config),self.config)
        for t in range(0,81,10):
            result = manager.process(observation(t,lat=0,lon=.02+t*.001,
                altitude_ft=3000-10*t,vertical_rate_fpm=-600,category='A7'))
        self.assertEqual(result['airport_method']['destination']['status'],'ESTIMATED')
        self.assertEqual(result['airport_method']['destination']['candidate']['ident'],'HELI')
        self.assertNotIn('runway',result['airport_method']['destination']['candidate']['evidence'])

    def test_category_in_identification_message_survives_sqlite_and_expires(self):
        db = Database(self.config.telemetry_db)
        identification = observation(0, category=' a7 ')
        for obs in [identification, observation(120,lat=0,lon=0)]:
            db.save(obs)
        history = db.recent('ABC123',900,100)
        data = build_snapshot(history,60)
        category = describe_category(data)
        self.assertEqual(category['code'], 'A7')
        self.assertEqual(category['source'], 'last_known')
        self.assertEqual(category['age_s'], 120)
        self.assertEqual(history[0].raw, identification.raw)
        self.assertEqual(history[0].fields['category'], ' a7 ')
        expired = build_snapshot(history+[observation(301,lat=0,lon=0)],60)
        self.assertIsNone(describe_category(expired)['code'])
        self.assertIsNone(describe_category(build_snapshot(history,60,category_ttl_s=100))['code'])

    def test_explicit_unknown_does_not_resurrect_an_older_category(self):
        for value in (None, 'A0', 'invalid', 7):
            data = build_snapshot([observation(0,category='A3'),
                observation(10,category=value),observation(20,lat=0,lon=0)],60)
            self.assertEqual(describe_category(data)['profile'], 'unknown')
            self.assertIn('HELI', [r['ident'] for r in self.estimator.estimate(data,'DESCENT')])

    def test_interleaved_aircraft_and_out_of_order_categories(self):
        manager = AircraftManager(Database(self.config.telemetry_db),self.estimator,self.config)
        manager.process(observation(20,lat=0,lon=0,category='A7'))
        other = manager.process(observation(21,icao='DEF456',lat=0,lon=0,category='A3'))
        late = manager.process(observation(10,category='A1'))
        self.assertEqual(late['aircraft_category']['code'], 'A7')
        self.assertEqual(other['aircraft_category']['code'], 'A3')
        self.assertIn('HELI',[r['ident'] for r in late['destination_candidates']])
        self.assertNotIn('HELI',[r['ident'] for r in other['destination_candidates']])

    def test_category_update_invalidates_cached_decision_before_next_position(self):
        fusion = DecisionFusion()
        heli = candidate()
        heli['airport_type'] = 'heliport'
        for t in (60,70,80):
            data = snapshot(t)
            data['fields']['category'] = {'value':'A7'}
            result = fusion.update([heli],data,quality(data),'DESCENT')
        self.assertEqual(result['status'], 'ESTIMATED')
        data['fields']['category'] = {'value':'A3'}
        result = fusion.update([candidate(2)],data,quality(data),'DESCENT')
        self.assertEqual(result['status'], 'UNKNOWN')
        self.assertIsNone(result['candidate'])
        self.assertNotIn(heli['airport_id'],fusion.memory)

    def test_manager_removes_incompatible_history_and_exports_diagnostics(self):
        manager = AircraftManager(Database(self.config.telemetry_db),self.estimator,self.config)
        for t in range(0,81,10):
            result = manager.process(observation(t,lat=0,lon=.02+t*.001,
                altitude_ft=3000-10*t,vertical_rate_fpm=-600,category='A7'))
        heli = next(r for r in result['destination_candidates'] if r['ident']=='HELI')
        # A hypothesis previously accepted and now shown as historical.
        result['airport_summary']['landing'] = {'airport':heli,'event_ns':result['event_ns'],
                                               'status':'ESTIMATED','confidence':'BAIXA'}
        result['airport_last_evidence']['destination'] = {'candidate':heli,'best_candidate':heli}
        updated = manager.process(observation(81,category='A3'))
        self.assertNotIn('HELI',[r['ident'] for r in updated['destination_candidates']])
        self.assertFalse(updated['airport_summary']['landing'] and
                         updated['airport_summary']['landing']['airport']['ident']=='HELI')
        json.dumps(updated, allow_nan=False)
        frame = build_data(self.config.telemetry_db,self.config.reference_db)['aircraft']['ABC123']['frames'][-1]
        self.assertEqual(frame['category']['code'],'A3')
        self.assertEqual(frame['airport_filter']['excluded'],1)
        self.assertTrue(all(c['airport_type']!='heliport' for c in frame['decisions']['destination']['candidates']))

    def test_user_airport_type_selection_is_respected(self):
        estimator = AirportEstimator(replace(self.config,airport_types=('large_airport',)))
        self.assertEqual([r['ident'] for r in estimator.estimate(self.data(category='A7'),'DESCENT')],['LARGE'])

    def test_category_does_not_allow_level_destination_confirmation_or_invent_origin(self):
        fusion = DecisionFusion()
        for t in (60,70,80):
            data = snapshot(t)
            data['fields']['category'] = {'value':'A3'}
            result = fusion.update([candidate()],data,quality(data),'LEVEL')
        self.assertEqual(result['status'],'UNKNOWN')
        self.assertEqual(self.estimator.estimate(self.data(category='A3'),'DESCENT',origin=True),[])

    def test_simulator_emits_explicit_categories(self):
        flight = Flight('F00001','SIM001','arrival',0,480,0,0,100,90,60,category='A3')
        self.assertEqual(flight.message(20,1790000000000000000)['category'],'A3')
        with sqlite3.connect(self.config.reference_db) as db:
            db.execute("UPDATE airports SET ident='SBBR' WHERE id=3")
        self.assertTrue(all(f.category in ('A2','A3') for f in scenarios(self.config.reference_db)))
