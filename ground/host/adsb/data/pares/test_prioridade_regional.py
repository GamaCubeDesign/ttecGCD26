import json
import sqlite3
import tempfile
import unittest
from pathlib import Path
import numpy as np
from prioridade_regional import Region, ROOT, migrate, iter_prioritized

class RegionalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.region=Region(ROOT/'regiao_config.json')

    def test_crossing_with_external_endpoints(self):
        # Honolulu -> Miami atravessa o México; ambos os extremos nos EUA.
        a=(21.3187,-157.9225); b=(25.7932,-80.2906)
        self.assertEqual(int(self.region.crossings(a,[b])[0]),1)
        self.assertEqual(int(self.region.crossings(b,[a])[0]),1)

    def test_outside_and_antimeridian(self):
        self.assertEqual(int(self.region.crossings((51.47,-0.4543),[(40.6413,-73.7781)])[0]),0)
        self.assertEqual(int(self.region.crossings((-20,170),[(-20,-170)])[0]),0)

    def test_endpoint_and_ambiguous(self):
        self.assertEqual(int(self.region.crossings((-15.87,-47.92),[(-23.43,-46.47)])[0]),1)
        self.assertEqual(int(self.region.crossings((0,0),[(0,180)])[0]),-1)
        self.assertEqual(int(self.region.crossings((0,0),[(0,0)])[0]),0)

    def test_reverse_consistency(self):
        rng=np.random.default_rng(42)
        for _ in range(40):
            a=(rng.uniform(-70,70),rng.uniform(-180,180))
            b=(rng.uniform(-70,70),rng.uniform(-180,180))
            self.assertEqual(self.region.crossings(a,[b])[0],self.region.crossings(b,[a])[0])

    def test_cap_prefilter_and_segment_resolution(self):
        rng=np.random.default_rng(7)
        fast=Region(ROOT/'regiao_config.json')
        reference=Region(ROOT/'regiao_config.json')
        reference.use_cap=False
        reference.step=25
        for _ in range(8):
            origin=(rng.uniform(-65,65),rng.uniform(-180,180))
            destinations=np.column_stack((rng.uniform(-65,65,30),rng.uniform(-180,180,30)))
            np.testing.assert_array_equal(fast.crossings(origin,destinations),reference.crossings(origin,destinations))

    def test_migration_and_priority_order(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'pairs.db'
            with sqlite3.connect(path) as db:
                db.executescript('''CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT);
                    INSERT INTO metadata VALUES ('generation_status','complete');
                    CREATE TABLE airports(id INTEGER PRIMARY KEY,latitude_deg REAL,longitude_deg REAL,iso_country TEXT);
                    CREATE TABLE route_pairs(origin_id INTEGER,destination_id INTEGER,distance_km REAL,initial_bearing_deg REAL);''')
                db.executemany('INSERT INTO airports VALUES (?,?,?,?)',[(1,-15.87,-47.92,'BR'),(2,21.3187,-157.9225,'US'),(3,25.7932,-80.2906,'US'),(4,51.47,-0.4543,'GB')])
                db.executemany('INSERT INTO route_pairs VALUES (?,?,100,90)',[(a,b) for a in range(1,5) for b in range(1,5) if a!=b])
            migrate(path,ROOT/'regiao_config.json')
            migrate(path,ROOT/'regiao_config.json') # repetição sem duplicatas
            items=list(iter_prioritized(path,batch_size=2))
            self.assertEqual(len(items),12)
            levels=[r['analysis_priority'] for r in items]
            self.assertEqual(levels,sorted(levels,reverse=True))
            self.assertEqual(set(levels),{0,1,2})
            with sqlite3.connect(path) as db:
                row=db.execute('SELECT origin_in_region,destination_in_region,analysis_priority FROM route_pairs_regional WHERE origin_id=1 AND destination_id=2').fetchone()
                self.assertEqual(row,(1,0,2))
                self.assertEqual(db.execute('SELECT count(*) FROM route_pairs').fetchone()[0],12)
                before=db.execute('SELECT * FROM regional_pairs ORDER BY 1,2').fetchall()
            # Uma falha durante cálculo não remove a classificação anterior.
            from unittest.mock import patch
            with patch.object(Region,'crossings',side_effect=RuntimeError('simulated failure')):
                with self.assertRaises(RuntimeError): migrate(path,ROOT/'regiao_config.json')
            with sqlite3.connect(path) as db:
                self.assertEqual(db.execute('SELECT * FROM regional_pairs ORDER BY 1,2').fetchall(),before)

if __name__=='__main__':
    unittest.main()
