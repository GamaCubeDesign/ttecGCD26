import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
import auditar_cidades as audit

class CityAuditTests(unittest.TestCase):
    def test_city_keys(self):
        a={'ident':'A','municipality':'São Paulo','iso_country':'BR','iso_region':'BR-SP'}
        self.assertEqual(audit.city_key(a),audit.city_key({**a,'municipality':'sao PAULO'}))
        self.assertNotEqual(audit.city_key(a),audit.city_key({**a,'iso_region':'BR-MG'}))
        self.assertNotEqual(audit.city_key({**a,'municipality':None}),audit.city_key({**a,'municipality':None,'ident':'B'}))
        self.assertNotEqual(audit.city_key(a),audit.city_key({**a,'municipality':'São Paulo / Guarulhos'}))

    def test_roles(self):
        a={'type':'small_airport','in_pairs':0,'coordinates_valid':1,'all_recorded_runways_closed':0,'scheduled_service':'yes'}
        self.assertEqual(audit.role(a),'scheduled_gap')
        self.assertEqual(audit.role({**a,'all_recorded_runways_closed':1}),'runway_conflict')
        self.assertEqual(audit.role({**a,'scheduled_service':'no'}),'small_local')
        self.assertEqual(audit.role({**a,'type':'heliport'}),'special_facility')
        self.assertEqual(audit.role({**a,'type':'closed'}),'closed')
        self.assertEqual(audit.role({**a,'type':'medium_airport','scheduled_service':'no'}),'medium_reserve')

    def test_full_audit_and_preservation(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'source.db';dest=root/'pairs.db'
            (root/'regiao_config.json').write_text(json.dumps({'country_codes':['BR']}))
            (root/'grupos_atendimento.json').write_text(json.dumps({'groups':[]}))
            with sqlite3.connect(source) as db:
                db.executescript('''CREATE TABLE airports(id INTEGER PRIMARY KEY,ident TEXT,name TEXT,type TEXT,municipality TEXT,iso_country TEXT,iso_region TEXT,latitude_deg REAL,longitude_deg REAL,scheduled_service TEXT);
                    CREATE TABLE runways(airport_ref INTEGER,length_ft REAL,surface TEXT,closed INTEGER);
                    CREATE TABLE dataset_sources(dataset TEXT,downloaded_at TEXT);''')
                rows=[(1,'A','A','large_airport','São Paulo','BR','BR-SP',-23.5,-46.5,'yes'),
                      (2,'B','B','small_airport','sao paulo','BR','BR-SP',-23.6,-46.5,'yes'),
                      (3,'C','C','medium_airport','São Paulo','BR','BR-MG',-23.7,-46.5,'no'),
                      (4,'D','D','small_airport',None,'BR','BR-SP',-23.8,-46.5,'no'),
                      (5,'E','E','small_airport',None,'BR','BR-SP',-23.9,-46.5,'no'),
                      (6,'F','F','heliport','Foo','US','US-TX',30,-90,'yes'),
                      (7,'G','G','closed','Foo','US','US-TX',30,-90,'yes')]
                db.executemany('INSERT INTO airports VALUES (?,?,?,?,?,?,?,?,?,?)',rows)
                db.execute("INSERT INTO runways VALUES(2,3000,'ASP',0)")
            with sqlite3.connect(dest) as db:
                db.executescript('CREATE TABLE airports(id INTEGER PRIMARY KEY); INSERT INTO airports VALUES(1); CREATE TABLE route_pairs(origin_id INTEGER,destination_id INTEGER); CREATE TABLE metadata(key TEXT,value TEXT);')
                db.execute('INSERT INTO metadata VALUES (?,?)',('source_sha256',hashlib.sha256(source.read_bytes()).hexdigest()))
            with patch.object(audit,'ROOT',root):
                audit.audit(source,dest,root/'reports')
                audit.audit(source,dest,root/'reports')
            with sqlite3.connect(dest) as db:
                self.assertEqual(db.execute('SELECT count(*) FROM city_audit_airports').fetchone()[0],7)
                self.assertEqual(db.execute('SELECT count(*) FROM city_audit_cities').fetchone()[0],5)
                self.assertEqual(db.execute("SELECT role FROM city_audit_airports WHERE ident='B'").fetchone()[0],'scheduled_gap')
                self.assertEqual(db.execute('SELECT count(*) FROM airports').fetchone()[0],1)
                self.assertEqual(db.execute('SELECT count(*) FROM route_pairs').fetchone()[0],0)
                self.assertEqual(db.execute('SELECT count(*) FROM city_audit_nearby WHERE airport_id=3 AND same_city_key=0').fetchone()[0],1)
                c=json.loads(db.execute("SELECT details_json FROM city_audit_airports WHERE ident='G'").fetchone()[0])
                self.assertIn('closed_with_scheduled_service',c['quality_flags'])
                self.assertIsNone(db.execute('PRAGMA foreign_key_check').fetchone())
            result=json.loads((root/'reports/cidades_mundo.json').read_text())
            self.assertEqual(sum(c['facility_count'] for c in result['cities']),7)
            self.assertEqual(result['cities'][0]['in_latam'],1)
            self.assertEqual(result['summary']['world']['unnamed_facilities'],2)

if __name__=='__main__': unittest.main()
