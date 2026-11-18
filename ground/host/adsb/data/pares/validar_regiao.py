"""Verifica a migração aplicada e salva relatório local."""
import json
import sqlite3
from datetime import datetime, timezone
from prioridade_regional import ROOT, iter_prioritized

path=ROOT/'pares_aeroportos.db'
with sqlite3.connect(path.as_uri()+'?mode=ro',uri=True) as db:
    metadata=dict(db.execute('SELECT key,value FROM regional_metadata'))
    assert metadata['status']=='complete' and metadata['bonus_enabled']=='false'
    n=db.execute('SELECT count(*) FROM airports').fetchone()[0]
    original_count=db.execute('SELECT count(*) FROM route_pairs').fetchone()[0]
    count=db.execute('SELECT count(*) FROM regional_pairs').fetchone()[0]
    assert 2*count==original_count==n*(n-1)
    assert db.execute('PRAGMA integrity_check').fetchone()==('ok',)
    assert db.execute('PRAGMA foreign_key_check').fetchone() is None
    priorities=dict(db.execute('SELECT analysis_priority,count(*)*2 FROM regional_pairs GROUP BY analysis_priority'))
    regional_airports=db.execute('SELECT sum(in_region) FROM regional_airports').fetchone()[0]
    assert priorities[2]==original_count-(n-regional_airports)*(n-regional_airports-1)
    assert not db.execute('''SELECT 1 FROM regional_pairs r
        JOIN regional_airports a ON a.airport_id=r.airport_a_id
        JOIN regional_airports b ON b.airport_id=r.airport_b_id
        WHERE (r.analysis_priority=2 AND a.in_region=0 AND b.in_region=0)
           OR (r.analysis_priority<2 AND (a.in_region=1 OR b.in_region=1))
           OR (r.analysis_priority=1 AND (r.arc_intersects_region IS NOT 1))
           OR (r.analysis_priority=0 AND r.arc_intersects_region=1) LIMIT 1''').fetchone()
    examples=[]
    for origin,destination,level in [('SBBR','SBGR',2),('SBGR','KJFK',2),('PHNL','KMIA',1),('EGLL','KJFK',0)]:
        row=db.execute('''SELECT o.ident,d.ident,p.origin_in_region,p.destination_in_region,
            p.arc_intersects_region,p.analysis_priority
            FROM route_pairs_regional p JOIN airports o ON o.id=p.origin_id
            JOIN airports d ON d.id=p.destination_id WHERE o.ident=? AND d.ident=?''',(origin,destination)).fetchone()
        assert row and row[-1]==level,(origin,destination,row)
        examples.append(row)
    plan=[r[3] for r in db.execute('EXPLAIN QUERY PLAN SELECT airport_a_id,airport_b_id,arc_intersects_region,analysis_priority FROM regional_pairs ORDER BY analysis_priority DESC,airport_a_id,airport_b_id')]
    assert any('regional_pairs_priority' in p for p in plan)
    assert not any('TEMP B-TREE' in p for p in plan)
    unknown=db.execute('SELECT count(*)*2 FROM regional_pairs WHERE arc_intersects_region IS NULL').fetchone()[0]
iterator=iter_prioritized(path)
first=next(iterator);reverse=next(iterator);iterator.close()
assert first['origin_id']==reverse['destination_id'] and first['destination_id']==reverse['origin_id']
assert first['analysis_priority']==2
report={'status':'passed','validated_at_utc':datetime.now(timezone.utc).isoformat(),
        'airports':n,'regional_airports':regional_airports,'directed_pairs':original_count,
        'directed_pairs_by_priority':priorities,'ambiguous_arcs':unknown,'bonus_enabled':False,
        'example_columns':['origin','destination','origin_in_region','destination_in_region','arc_intersects_region','analysis_priority'],
        'examples':examples,'query_plan':plan,'database_size_bytes':path.stat().st_size}
(ROOT/'validacao_regional.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(report,ensure_ascii=False))
