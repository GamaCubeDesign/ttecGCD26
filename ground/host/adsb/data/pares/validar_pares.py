"""Valida geometria conhecida e amostras do banco gerado."""
import json
import math
import sqlite3
from pathlib import Path
from gerar_pares import geometry, RADIUS_KM

root = Path(__file__).resolve().parent
assert math.isclose(geometry(0,0,0,90)[0], math.pi*RADIUS_KM/2)
assert geometry(0,0,0,90)[1] == 90
assert geometry(0,90,0,0)[1] == 270
assert math.isclose(geometry(0,179,0,-179)[0], math.pi*RADIUS_KM/90)
assert geometry(0,0,0,0) == (0,None)
assert geometry(0,0,0,180)[1] is None
with sqlite3.connect((root/'pares_aeroportos.db').as_uri()+'?mode=ro',uri=True) as db:
    meta = dict(db.execute('SELECT key,value FROM metadata'))
    assert meta['generation_status'] == 'complete'
    n = db.execute('SELECT count(*) FROM airports').fetchone()[0]
    count = db.execute('SELECT count(*) FROM route_pairs').fetchone()[0]
    assert count == n*(n-1)
    for ident in ['SBBR','SBGR','SBSP','SBGL','SBCT','SCEL','SAEZ']:
        airport_id = db.execute('SELECT id FROM airports WHERE ident=?',(ident,)).fetchone()[0]
        assert db.execute('SELECT count(*) FROM route_pairs WHERE origin_id=?',(airport_id,)).fetchone()[0] == n-1
        assert db.execute('SELECT count(*) FROM route_pairs WHERE destination_id=?',(airport_id,)).fetchone()[0] == n-1
        assert db.execute('SELECT 1 FROM route_pairs WHERE origin_id=? AND destination_id=?',(airport_id,airport_id)).fetchone() is None
    examples = db.execute('''SELECT o.ident,d.ident,p.distance_km,p.initial_bearing_deg
        FROM route_pairs p JOIN airports o ON o.id=p.origin_id
        JOIN airports d ON d.id=p.destination_id
        WHERE o.ident IN ('SBBR','SBGR') AND d.ident IN ('SBBR','SBGR','SBGL','SBCT')
        ORDER BY o.ident,d.ident''').fetchall()
    forward = next(x for x in examples if x[:2] == ('SBBR','SBGR'))
    reverse = next(x for x in examples if x[:2] == ('SBGR','SBBR'))
    assert math.isclose(forward[2],reverse[2])
report = {'status':'passed','airports':n,'directed_pairs':count,
          'geometry_cases':['equatorial quarter circumference','reverse bearing','antimeridian','coincident','antipodal'],
          'database_checks':['complete generation','N*(N-1)','incoming/outgoing counts for seven airports','no self pairs for seven airports','reverse distance symmetry'],
          'examples_columns':['origin','destination','distance_km','initial_bearing_deg'],
          'examples':examples,'size_bytes':(root/'pares_aeroportos.db').stat().st_size}
(root/'validacao.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
print(json.dumps(report,ensure_ascii=False))
