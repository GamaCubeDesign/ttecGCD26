"""Exercise live downlink ingestion, real pair workers and HTTP integration."""
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import time
import unittest
from dataclasses import replace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE/'ground-aeronaves'))
sys.path.insert(0, str(HERE/'ground-aeronaves/tests'))
from ground.config import Config
from test_pair_processing import pair_db
from adsb_pipeline import AdsbPipeline
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
        self.config = replace(Config(), reference_db=reference,pairs_db=pairs,pairs_enabled=True,pair_interval_s=.1)
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

    def test_missing_pairs_prevents_start(self):
        with self.assertRaises(sqlite3.Error):
            AdsbPipeline(self.root/'sessions',replace(self.config,pairs_db=self.root/'absent.db'))
        self.assertFalse((self.root/'sessions').exists())


if __name__ == '__main__':
    unittest.main()
