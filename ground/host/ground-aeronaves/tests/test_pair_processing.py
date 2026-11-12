import io,json,sqlite3,tempfile,unittest
from concurrent.futures import Future
from contextlib import redirect_stdout,redirect_stderr
from dataclasses import replace
from pathlib import Path
from ground.pair_estimator import PairEstimator
from ground.pair_processing import PairScheduler
from ground.config import Config
from ground.__main__ import run
from ground.aircraft_manager import AircraftManager
from ground.visualize import build_data,LiveData
import test_ground

observation=test_ground.observation

def pair_db(path,coords=((0,-2),(0,2)),priorities=None):
    with sqlite3.connect(path) as db:
        db.executescript('''CREATE TABLE airports(id INTEGER PRIMARY KEY,ident TEXT,name TEXT,type TEXT,latitude_deg REAL,longitude_deg REAL);
          CREATE TABLE metadata(key TEXT,value TEXT); CREATE TABLE regional_metadata(key TEXT,value TEXT);
          CREATE TABLE regional_pairs(airport_a_id INTEGER,airport_b_id INTEGER,analysis_priority INTEGER);''')
        db.executemany('INSERT INTO airports VALUES (?,?,?,?,?,?)',[(i+1,'P'+str(i+1),'Airport','large_airport',lat,lon) for i,(lat,lon) in enumerate(coords)])
        pairs=[(a+1,b+1,(priorities or {}).get((a+1,b+1),0)) for a in range(len(coords)) for b in range(a+1,len(coords))]
        db.executemany('INSERT INTO regional_pairs VALUES (?,?,?)',pairs)
        db.executemany('INSERT INTO metadata VALUES (?,?)',[('generation_status','complete'),('pair_count',str(len(pairs)*2))])
        db.executemany('INSERT INTO regional_metadata VALUES (?,?)',[('status','complete'),('bonus_enabled','false'),('unordered_pair_count',str(len(pairs)))])

def job(points):
    return {'snapshot':{'trajectory':[{'event_ns':int(t*1e9),'lat':lat,'lon':lon} for t,lat,lon in points]},'category':'A3','corridor_km':80,'top':5}

class GeometryTests(unittest.TestCase):
    def estimator(self,coords=((0,-2),(0,2)),priorities=None):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup)
        path=Path(tmp.name)/'pairs.db';pair_db(path,coords,priorities)
        return PairEstimator(path)
    def test_direction_and_finite_arc(self):
        e=self.estimator()
        r=e.estimate(job([(0,0,-.2),(30,0,-.1),(60,0,0)]))
        self.assertEqual((r['candidates'][0]['origin']['id'],r['candidates'][0]['destination']['id']),(1,2))
        r=e.estimate(job([(0,0,0),(30,0,-.1),(60,0,-.2)]))
        self.assertEqual(r['candidates'][0]['origin']['id'],2)
        self.assertEqual(e.estimate(job([(0,0,10),(30,0,10.1),(60,0,10.2)]))['status'],'NO_MATCH')
    def test_dateline(self):
        r=self.estimator(((0,179),(0,-179))).estimate(job([(0,0,179.8),(30,0,179.9),(60,0,-180),(90,0,-179.9)]))
        self.assertEqual(r['status'],'CANDIDATES')
        self.assertEqual(r['candidates'][0]['origin']['id'],1)
        self.assertLess(r['candidates'][0]['evidence']['rms_distance_km'],.01)
    def test_insufficient_jump_and_no_region_bonus(self):
        e=self.estimator(((0,-2),(0,2),(1,-2),(1,2)),{(3,4):2})
        self.assertEqual(e.estimate(job([(0,0,0)]))['status'],'INSUFFICIENT')
        self.assertEqual(e.estimate(job([(0,0,0),(30,0,.1),(60,30,40)]))['status'],'INSUFFICIENT')
        r=e.estimate(job([(0,0,-.2),(30,0,-.1),(60,0,0)]))
        self.assertEqual(r['directed_pairs_examined'],12)
        self.assertEqual(r['candidates'][0]['regional_priority'],0)
        self.assertEqual((r['candidates'][0]['origin']['id'],r['candidates'][0]['destination']['id']),(1,2))

class FakeExecutor:
    def __init__(self): self.calls=[]
    def submit(self,fn,job):
        future=Future();self.calls.append((job,future));return future

def state(version,icao='ABC123'):
    return {'icao':icao,'analysis_version':version,'tracking_episode':0,'event_ns':version*1000000000,
        'snapshot':{'trajectory':[]},'aircraft_category':{'code':'A3'},'airport_method':{'origin':{'status':'UNKNOWN','candidate':None}}}

class SchedulingTests(unittest.TestCase):
    def test_coalescing(self):
        ex=FakeExecutor();s=PairScheduler(replace(Config(),pair_workers=2),executor=ex,clock=lambda:5)
        s.offer(state(1));s.poll();s.offer(state(2));s.offer(state(3));s.offer(state(1,'DEF456'));s.poll()
        self.assertEqual(len(ex.calls),2);self.assertEqual(s.pending['ABC123']['version'],3)
        ex.calls[0][1].set_result({'status':'CANDIDATES','candidates':[]})
        self.assertEqual(s.poll(force=True)[0]['source_version'],1)
        self.assertEqual(ex.calls[-1][0]['version'],3);self.assertEqual(s.coalesced,1)
    def test_error_recovery_interval_and_bound(self):
        ex=FakeExecutor();clock=[0];s=PairScheduler(replace(Config(),pair_max_pending=1),executor=ex,clock=lambda:clock[0])
        s.offer(state(1));s.poll();ex.calls[0][1].set_exception(ValueError('bad db'))
        self.assertEqual(s.poll()[0]['status'],'ERROR')
        s.offer(state(2));s.poll();self.assertEqual(len(ex.calls),1)
        self.assertEqual(s.offer(state(1,'DEF456'))['status'],'DEFERRED')
        clock[0]=1;s.poll();self.assertEqual(ex.calls[-1][0]['version'],2)
    def test_versions_persistence_and_idle_viewer(self):
        f=test_ground.GroundTests();f.setUp();self.addCleanup(f.doCleanups)
        config=replace(f.config,pairs_enabled=True);m=AircraftManager(f.db,f.estimator,config)
        ex=FakeExecutor();m.pairs=PairScheduler(config,executor=ex)
        for t in (0,10,20): m.process(observation(t,lat=0,lon=t*.001))
        version=m.states['ABC123']['analysis_version']
        live=LiveData(config.telemetry_db,config.reference_db,f.db.session_id)
        m.process(observation(30,lat=0,lon=.03))
        ex.calls[0][1].set_result({'status':'CANDIDATES','candidates':[]});m.poll_pairs()
        latest=m.states['ABC123'];self.assertEqual(latest['pair_analysis']['source_version'],version)
        self.assertFalse(latest['pair_analysis']['matches_current_version'])
        self.assertEqual(latest['pair_analysis']['combined_analysis']['source_version'],version)
        self.assertNotIn('fusion_context',latest['pair_analysis'])
        data=build_data(config.telemetry_db,config.reference_db,f.db.session_id)
        self.assertNotEqual(data['aircraft']['ABC123']['frames'][2]['pair_analysis']['status'],'CANDIDATES')
        self.assertEqual(data['aircraft']['ABC123']['frames'][3]['pair_analysis']['source_version'],version)
        self.assertEqual(data['aircraft']['ABC123']['frames'][3]['pair_analysis']['combined_analysis']['source_version'],version)
        revision=live.read()[0];m.pairs.poll(force=True)
        ex.calls[-1][1].set_result({'status':'CANDIDATES','candidates':[]});m.poll_pairs()
        self.assertNotEqual(live.read()[0],revision)
        m.process(observation(40,lat=0,lon=.04));m.pairs.poll(force=True)
        m.process(observation(1000,lat=0,lon=.1))
        ex.calls[-1][1].set_result({'status':'CANDIDATES','candidates':[]});m.poll_pairs()
        self.assertNotEqual(m.states['ABC123']['pair_analysis']['status'],'CANDIDATES')
        m.pairs.active.clear();m.pairs.pending.clear();m.close()

class ProcessIntegrationTests(unittest.TestCase):
    def test_real_process_and_missing_database(self):
        f=test_ground.GroundTests();f.setUp();self.addCleanup(f.doCleanups)
        pairs=f.config.output.parent/'pairs.db';pair_db(pairs)
        config=replace(f.config,pairs_enabled=True,pairs_db=pairs,pair_interval_s=.1)
        source='\n'.join(observation(t,lat=0,lon=t*.001).raw for t in range(0,61,10))+'\n'
        with redirect_stdout(io.StringIO()),redirect_stderr(io.StringIO()): counts=run(config,io.StringIO(source),0)
        result=json.loads(config.output.read_text())['aircraft']['ABC123']
        self.assertEqual(counts['accepted'],7)
        self.assertEqual(result['pair_analysis']['status'],'CANDIDATES',result['pair_analysis'])
        self.assertTrue(result['pair_analysis']['matches_current_version'])
        with sqlite3.connect(config.telemetry_db) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM observations').fetchone()[0],7)
            self.assertEqual(db.execute('SELECT count(*) FROM estimates').fetchone()[0],7)
        config=replace(config,pairs_db=pairs.parent/'missing.db')
        with redirect_stdout(io.StringIO()),redirect_stderr(io.StringIO()): run(config,io.StringIO(source),0)
        result=json.loads(config.output.read_text())['aircraft']['ABC123']
        self.assertEqual(result['pair_analysis']['status'],'ERROR');self.assertIn('airport_method',result)

class LivePairTests(unittest.TestCase):
    def test_idle_live_receiver_publishes_finished_analysis(self):
        import time
        from ground.live_input import LiveInput
        f=test_ground.GroundTests();f.setUp();self.addCleanup(f.doCleanups)
        pairs=f.config.output.parent/'pairs.db';pair_db(pairs)
        config=replace(f.config,pairs_enabled=True,pairs_db=pairs)
        source=config.output.parent/'live.ndjson';now=time.time_ns()
        source.write_text(''.join(json.dumps({'icao':'F00001','rx_epoch_ns':now-(60-i*30)*1000000000,'lat':0,'lon':i*.02})+'\n' for i in range(3)))
        live=LiveInput(source,config);live.start();self.addCleanup(live.close)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            if config.output.exists():
                state=json.loads(config.output.read_text())['aircraft'].get('F00001',{})
                if state.get('pair_analysis',{}).get('status')=='CANDIDATES':
                    self.assertTrue(state['pair_analysis']['matches_current_version']);return
            time.sleep(.05)
        self.fail('Resultado de pares não foi publicado durante recepção ociosa')

if __name__=='__main__': unittest.main()
