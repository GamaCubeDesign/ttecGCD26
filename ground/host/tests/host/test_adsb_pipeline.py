"""Exercise live downlink ingestion, real pair workers and HTTP integration."""
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import time
import unittest
import urllib.error
from dataclasses import replace

HERE = Path(__file__).resolve().parents[2]  # ground/host
from adsb.estimator.config import Config
from tests.support import pair_db
from adsb.pipeline import AdsbPipeline, summarize, TRACK_POINTS_MAX
from gs_web import Hub, GsState


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        reference = root/'reference.db'
        with sqlite3.connect(reference) as db:
            db.execute('CREATE TABLE airports(id INTEGER PRIMARY KEY,ident TEXT,icao_code TEXT,name TEXT,type TEXT,latitude_deg REAL,longitude_deg REAL,elevation_ft REAL)')
            db.executemany('INSERT INTO airports VALUES (?,?,?,?,?,?,?,?)', [(1,'P1','P1','West','large_airport',0,-2,0),(2,'P2','P2','East','large_airport',0,2,0)])
        pairs = root/'pairs.db'
        pair_db(pairs)
        self.config = replace(Config(), reference_db=reference,pairs_db=pairs,pairs_enabled=True,pair_interval_s=.1,
                              publish_interval_s=.2)
        self.root = root

    def test_live_fusion_and_rejections(self):
        pipeline = AdsbPipeline(self.root/'sessions', self.config)
        self.addCleanup(pipeline.close)
        hub = Hub(GsState(), None, pipeline)
        base = int(time.time()*1000)-60000
        for i in range(4):
            hub.feed(dict(ev='tracks',wall_ms=base+i*20000,records=[dict(icao='ABC123',flags=15,lat=0,lon=-.2+i*.04,alt_ft=30000,gs_kt=400,trk=90,vr_fpm=0,age_ds=0)]))
        # One bad record must not stop the next valid records or idle polling.
        hub.feed(dict(ev='tracks',wall_ms=base,records=[{'flags':1},None]))
        hub.feed(dict(ev='tracks',wall_ms=base,records=None))
        deadline = time.monotonic()+15
        state = {}
        while time.monotonic()<deadline:
            path = pipeline.config.output
            if path.exists():
                state = json.loads(path.read_text())['aircraft'].get('ABC123',{})
            if state.get('pair_analysis',{}).get('source_version') == 4 and 'combined_analysis' in state.get('pair_analysis',{}):
                break
            time.sleep(.05)
        self.assertIsNone(pipeline.snapshot()['error'])
        self.assertEqual(pipeline.snapshot()['processed'],4)
        self.assertEqual(pipeline.snapshot()['rejected'],3)
        self.assertIn('combined_analysis',state['pair_analysis'])
        self.assertNotEqual(state['pair_analysis']['status'],'ERROR')
        self.assertIn('ABC123',json.loads(pipeline.page('/adsb/data')[0])['aircraft'])
        self.assertIn("'/adsb/data",pipeline.page('/adsb/')[0])
        self.assertEqual(hub.snapshot()['adsb']['processed'],4)
        self.assertTrue((pipeline.directory/'ground.jsonl').exists())
        # The page's aircraft view, rebuilt by the estimator's thread.
        deadline = time.monotonic()+5
        while time.monotonic()<deadline and b'ABC123' not in pipeline.summary()[1]:
            time.sleep(.05)
        revision, body = pipeline.summary()
        view = json.loads(body)
        self.assertGreaterEqual(revision, 1)
        self.assertEqual([a['icao'] for a in view['aircraft']], ['ABC123'])
        self.assertGreaterEqual(len(view['aircraft'][0]['track']), 1)
        self.assertEqual(pipeline.config.publish_interval_s, .2)
        default = AdsbPipeline(self.root/'sessions', replace(self.config, publish_interval_s=0.0, pairs_enabled=False))
        self.addCleanup(default.close)
        self.assertEqual(default.config.publish_interval_s, 10.0)

    def test_http_mission_command_starts_ingestion(self):
        import subprocess
        import urllib.request
        fake = """import sys,json,time
for line in sys.stdin:
    if line.strip() == 'quit': break
    if line.strip() == 'mode mission_adsb':
        print(json.dumps(dict(ev='tracks',wall_ms=int(time.time()*1000),records=[dict(icao='ABC123',flags=3,lat=0,lon=0,alt_ft=30000)])),flush=True)
"""
        proc = subprocess.Popen([sys.executable,str(HERE/'gs_web.py'),'--adsb','--reference-db',str(self.config.reference_db),'--pairs-db',str(self.config.pairs_db),'--session-dir',str(self.root/'sessions'),'--port','0','--exec',sys.executable,'-c',fake],stdout=subprocess.PIPE,text=True)
        try:
            first = proc.stdout.readline()
            self.assertIn(' on ',first)
            base = first.split(' on ')[1].strip()
            request = urllib.request.Request(base+'/api/cmd',data=json.dumps(dict(cmd='mode mission_adsb')).encode(),headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(request,timeout=5) as response:
                self.assertTrue(json.load(response)['ok'])
            deadline = time.monotonic()+10
            while time.monotonic()<deadline:
                with urllib.request.urlopen(base+'/api/state',timeout=5) as response:
                    state = json.load(response)
                if state['adsb']['processed'] == 1: break
                time.sleep(.05)
            self.assertEqual(state['adsb']['processed'],1)
            with urllib.request.urlopen(base+'/adsb/data',timeout=5) as response:
                self.assertIn('ABC123',json.load(response)['aircraft'])
            deadline = time.monotonic()+5
            while time.monotonic()<deadline:
                with urllib.request.urlopen(base+'/api/adsb',timeout=5) as response:
                    tag, view = response.headers['ETag'], json.load(response)
                if view['aircraft']: break
                time.sleep(.1)
            self.assertEqual(view['aircraft'][0]['icao'],'ABC123')
            again = urllib.request.Request(base+'/api/adsb',headers={'If-None-Match':tag})
            with self.assertRaises(urllib.error.HTTPError) as cm:
                urllib.request.urlopen(again,timeout=5)
            self.assertEqual(cm.exception.code,304)
            cm.exception.close()
            with urllib.request.urlopen(base+'/api/airports',timeout=5) as response:
                self.assertEqual(json.load(response)[0][0] in ('P1','P2'),True)
        finally:
            proc.terminate()
            proc.wait(timeout=35)
            proc.stdout.close()
        self.assertEqual(proc.returncode,0)

    def test_shutdown_drains_and_dedup_preserves_velocity_changes(self):
        pipeline = AdsbPipeline(self.root/'sessions',replace(self.config,pairs_enabled=False))
        event = dict(ev='tracks',wall_ms=int(time.time()*1000),records=[dict(icao='ABC123',flags=7,lat=0,lon=0,alt_ft=30000,gs_kt=400,trk=90)])
        pipeline.feed(event)
        pipeline.feed(event)
        event['records'][0]['gs_kt'] = 410
        pipeline.feed(event)
        pipeline.close()
        self.assertEqual(pipeline.snapshot()['processed'],2)
        self.assertEqual(pipeline.snapshot()['repeats'],1)
        self.assertIsNone(pipeline.snapshot()['error'])

    def test_the_session_folder_carries_the_prefix(self):
        default = AdsbPipeline(self.root/'sessions', replace(self.config, pairs_enabled=False))
        self.addCleanup(default.close)
        sim = AdsbPipeline(self.root/'sessions', replace(self.config, pairs_enabled=False), prefix='sim-mission-')
        self.addCleanup(sim.close)
        self.assertTrue(default.directory.name.startswith('mission-'))
        self.assertTrue(sim.directory.name.startswith('sim-mission-'))

    def test_missing_pairs_prevents_start(self):
        with self.assertRaises(sqlite3.Error):
            AdsbPipeline(self.root/'sessions',replace(self.config,pairs_db=self.root/'absent.db'))
        self.assertFalse((self.root/'sessions').exists())


class SummaryTests(unittest.TestCase):
    def result(self, n_points):
        t0 = 1_794_000_000_000_000_000
        traj = [{'event_ns': t0 + i * 1_000_000_000, 'lat': -34.5 + i * 1e-4, 'lon': -58.4, 'altitude_ft': 1000 + i}
                for i in range(n_points)]
        return {
            'icao': 'E0B110', 'event_ns': traj[-1]['event_ns'], 'flight_state': 'CLIMB', 'last_position_age_s': 0.2,
            'aircraft_category': {'code': 'A3', 'label': 'Aeronave de grande porte'},
            'snapshot': {'lat': -34.4, 'lon': -58.4, 'trajectory': traj,
                         'fields': {'altitude_ft': {'value': 3000}, 'ground_speed_kt': {'value': 180},
                                    'track_deg': {'value': 305}, 'callsign': {'value': 'JES3104'}}},
            'airport_summary': {'takeoff': {'airport': {'ident': 'SABE'}, 'status': 'ESTIMATED'}, 'landing': None},
            'airport_method': {'origin': {'status': 'UNKNOWN'},
                               'destination': {'status': 'TENTATIVE', 'candidate': {'ident': 'SAEZ'}, 'reason': 'X'}},
            'destination_candidates': [{'ident': 'SAEZ', 'score': .7}, {'ident': 'SADF', 'score': .5}],
            'combined_analysis': {'origin': {'ident': 'SABE', 'status': 'ESTIMATED'}, 'destination': {'ident': None, 'status': 'UNKNOWN'}},
            'pair_analysis': {'status': 'CANDIDATES', 'candidates': [
                {'origin': {'ident': 'SABE'}, 'destination': {'ident': 'SAME'}, 'score': .6}]},
        }

    def test_the_view_is_compact_and_names_its_airports(self):
        airports = {i: [0, 0, i, 'large_airport'] for i in ('SABE', 'SAEZ', 'SADF', 'SAME', 'SBBR')}
        view = summarize({'E0B110': self.result(1000)}, airports)
        a = view['aircraft'][0]
        self.assertLessEqual(len(a['track']), TRACK_POINTS_MAX + 1)
        self.assertEqual(a['track'][-1][0], self.result(1000)['event_ns'] // 1_000_000)   # the last point kept
        self.assertEqual((a['callsign'], a['category'], a['phase'], a['alt'], a['trk']), ('JES3104', 'A3', 'CLIMB', 3000, 305))
        self.assertIsNone(a['category_source'])
        self.assertEqual((a['origin']['ident'], a['origin']['status'], a['origin']['source']), ('SABE', 'ESTIMATED', 'history'))
        self.assertEqual((a['destination']['ident'], a['destination']['status']), ('SAEZ', 'TENTATIVE'))
        self.assertEqual([c['ident'] for c in a['destination']['candidates']], ['SAEZ', 'SADF'])
        self.assertEqual(a['pairs']['top'], [{'origin': 'SABE', 'destination': 'SAME', 'score': .6}])
        self.assertEqual(sorted(view['airports']), ['SABE', 'SADF', 'SAEZ', 'SAME'])
        json.dumps(view, allow_nan=False)

    def test_an_aircraft_without_position_is_still_listed(self):
        view = summarize({'E0844F': {'icao': 'E0844F', 'snapshot': None}}, {})
        a = view['aircraft'][0]
        self.assertEqual((a['icao'], a['lat'], a['track'], a['pairs'], a['combined']), ('E0844F', None, [], None, None))


if __name__ == '__main__':
    unittest.main()
