import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

from ground.visualize import build_data, export, LiveData


class VisualizationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.db=self.root/'telemetry.db'
        self.ref=self.root/'reference.db'
        with sqlite3.connect(self.db) as c:
            c.execute('CREATE TABLE estimates(id INTEGER PRIMARY KEY,event_ns INTEGER,result_json TEXT,session_id TEXT)')
        with sqlite3.connect(self.ref) as c:
            c.execute('CREATE TABLE airports(id INTEGER,ident TEXT,name TEXT,latitude_deg REAL,longitude_deg REAL)')
            c.execute('INSERT INTO airports VALUES(1,?,?,0,0)',('TEST','</script><script>bad()</script>'))

    def add(self,session,time,points):
        result={'icao':'ABC123','event_ns':time*10**9,'flight_state':'UNKNOWN',
                'snapshot':{'trajectory':[{'event_ns':t*10**9,'lat':lat,'lon':lon} for t,lat,lon in points]},
                'airport_method':{'destination':{'status':'UNKNOWN'}},
                'destination_candidates':[{'airport_id':1,'ident':'TEST','score':.5}]}
        with sqlite3.connect(self.db) as c:
            c.execute('INSERT INTO estimates(event_ns,result_json,session_id) VALUES(?,?,?)',(time*10**9,json.dumps(result),session))

    def test_session_isolation_deduplication_and_gaps(self):
        self.add('old',1,[(1,50,50)])
        self.add('new',10,[(10,0,0)])
        self.add('new',20,[(10,0,0),(20,0,.001)])
        self.add('new',90,[(90,0,.002)])
        self.add('new',100,[(100,10,10)])
        data=build_data(self.db,self.ref)
        self.assertEqual(data['session'],'new')
        points=data['aircraft']['ABC123']['points']
        self.assertEqual(len(points),4)
        self.assertEqual([p['break'] for p in points],[True,False,True,True])
        self.assertEqual(len(build_data(self.db,self.ref,'old')['aircraft']['ABC123']['points']),1)

    def test_late_point_is_not_shown_before_first_snapshot(self):
        self.add('new',20,[(10,0,0)])
        p=build_data(self.db,self.ref)['aircraft']['ABC123']['points'][0]
        self.assertEqual(p['t'],10000)
        self.assertEqual(p['available_t'],20000)

    def test_export_escapes_script_and_preserves_database(self):
        self.add('new',10,[(10,0,0)])
        before=self.db.read_bytes()
        out=self.root/'view.html'
        export(self.db,self.ref,out)
        self.assertNotIn('</script><script>bad()',out.read_text())
        self.assertIn('\\u003c/script>',out.read_text())
        self.assertEqual(before,self.db.read_bytes())
        with self.assertRaises(ValueError):
            export(self.db,self.ref,self.db)

    def test_aircraft_without_position_and_empty_session(self):
        with self.assertRaises(ValueError):
            build_data(self.db,self.ref)
        self.add('new',10,[])
        self.assertEqual(build_data(self.db,self.ref)['aircraft']['ABC123']['points'],[])
        with self.assertRaises(ValueError):
            build_data(self.db,self.ref,'missing')

    def test_live_data_changes_only_when_new_estimates_arrive(self):
        self.add('first',10,[(10,0,0)])
        live=LiveData(self.db,self.ref)
        tag,body=live.read()
        self.assertEqual(live.read(),(tag,body))
        self.add('first',20,[(10,0,0),(20,0,.001)])
        new_tag,new_body=live.read()
        self.assertNotEqual(new_tag,tag)
        self.assertEqual(len(json.loads(new_body)['aircraft']['ABC123']['points']),2)
        self.add('second',30,[(30,1,1)])
        self.assertEqual(json.loads(live.read()[1])['session'],'second')
        pinned=LiveData(self.db,self.ref,'first')
        self.assertEqual(json.loads(pinned.read()[1])['session'],'first')
