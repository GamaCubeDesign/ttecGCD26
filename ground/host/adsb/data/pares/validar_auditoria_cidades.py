import hashlib
import json
from pathlib import Path
import sqlite3
from datetime import datetime, timezone

ROOT=Path(__file__).resolve().parent
with sqlite3.connect((ROOT/'pares_aeroportos.db').as_uri()+'?mode=ro',uri=True) as db:
    metadata=dict(db.execute('SELECT key,value FROM city_audit_metadata'))
    assert metadata['status']=='complete'
    source=Path(metadata['source_path'])
    assert hashlib.sha256(source.read_bytes()).hexdigest()==metadata['source_sha256']
    db.execute('ATTACH DATABASE ? AS original',(source.as_uri()+'?mode=ro',))
    n=db.execute('SELECT count(*) FROM original.airports').fetchone()[0]
    assert db.execute('SELECT count(*) FROM city_audit_airports').fetchone()[0]==n
    assert db.execute('SELECT sum(facility_count) FROM city_audit_cities').fetchone()[0]==n
    assert not db.execute('''SELECT 1 FROM original.airports o LEFT JOIN city_audit_airports a ON a.airport_id=o.id
        WHERE a.airport_id IS NULL OR a.ident IS NOT o.ident OR a.type IS NOT o.type
           OR a.scheduled_service IS NOT o.scheduled_service LIMIT 1''').fetchone()
    assert not db.execute('''SELECT 1 FROM city_audit_airports a LEFT JOIN airports p ON p.id=a.airport_id
        WHERE a.in_pairs != (p.id IS NOT NULL) LIMIT 1''').fetchone()
    assert not db.execute('''SELECT 1 FROM city_audit_nearby n
        JOIN city_audit_airports a ON a.airport_id=n.anchor_id
        JOIN city_audit_airports b ON b.airport_id=n.airport_id
        WHERE a.in_pairs=0 OR n.anchor_id=n.airport_id OR n.distance_km>100 OR n.distance_km<0
        OR json_extract(a.details_json,'$.iso_country') IS NOT json_extract(b.details_json,'$.iso_country') LIMIT 1''').fetchone()
    assert db.execute('SELECT count(*) FROM route_pairs').fetchone()[0]==int(metadata['original_pair_count'])
    assert db.execute('SELECT count(*) FROM regional_pairs').fetchone()[0]*2==int(metadata['original_pair_count'])
    assert db.execute('PRAGMA foreign_key_check').fetchone() is None
    assert db.execute('PRAGMA integrity_check').fetchone()==('ok',)
    cases={}
    for ident,expected in [('SBGR','selected'),('SBSP','selected'),('SBKP','selected'),('SBMT','medium_reserve'),('SBJH','medium_reserve'),('SBJD','small_local'),('SBDB','scheduled_gap'),('SBJU','scheduled_gap'),('SBJA','scheduled_gap')]:
        row=db.execute('SELECT role FROM city_audit_airports WHERE ident=?',(ident,)).fetchone()
        assert row==(expected,),(ident,row)
        cases[ident]=expected
    world=json.loads((ROOT/'auditoria_cidades/cidades_mundo.json').read_text())
    assert len(world['cities'])==int(metadata['city_group_count'])
    assert sum(c['facility_count'] for c in world['cities'])==n
    assert all(world['cities'][i]['in_latam']>=world['cities'][i+1]['in_latam'] for i in range(len(world['cities'])-1))
    report={'status':'passed','validated_at_utc':datetime.now(timezone.utc).isoformat(),'audited_facilities':n,
            'city_groups':len(world['cities']),'original_directed_pairs_preserved':int(metadata['original_pair_count']),
            'nearby_links':db.execute('SELECT count(*) FROM city_audit_nearby').fetchone()[0],
            'externally_documented_service_groups':db.execute('SELECT count(*) FROM city_audit_service_groups').fetchone()[0],
            'examples':cases,'checks':['full source inventory coverage','source hash unchanged','original selection unchanged','route and regional counts unchanged','100 km same-country neighbor constraints','export totals','Latin America first','SQLite integrity and foreign keys']}
(ROOT/'auditoria_cidades/validacao.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(report,ensure_ascii=False))
