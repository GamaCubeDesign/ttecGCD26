"""Replay sintético SBBR→SBGR contra o banco mundial real, sem conhecer a rota no estimador."""
import json,math,sqlite3,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from ground.config import Config
from ground.pair_estimator import vectors


def generate():
    import numpy as np
    config=Config()
    with sqlite3.connect(config.reference_db.resolve().as_uri()+'?mode=ro',uri=True) as db:
        coords={ident:(lat,lon) for ident,lat,lon in db.execute("SELECT ident,latitude_deg,longitude_deg FROM airports WHERE ident IN ('SBBR','SBGR')")}
    a,b=vectors([coords['SBBR'],coords['SBGR']]);theta=math.acos(float(a@b))
    output=ROOT/'validation/replay_pares.ndjson'
    messages=[]
    for i in range(21):
        fraction=.15+.15*i/20
        v=(math.sin((1-fraction)*theta)*a+math.sin(fraction*theta)*b)/math.sin(theta)
        messages.append({'icao':'F00001','rx_epoch_ns':1794400000000000000+i*30_000_000_000,
                         'lat':math.degrees(math.atan2(v[2],math.hypot(v[0],v[1]))),
                         'lon':math.degrees(math.atan2(v[1],v[0])),'altitude_ft':33000,'category':'A3','on_ground':0})
    output.write_text(''.join(json.dumps(m)+'\n' for m in messages))
    return output


if __name__=='__main__':
    from dataclasses import replace
    from ground.__main__ import run
    source=generate()
    config=replace(Config(),pairs_enabled=True,telemetry_db=ROOT/'validation/replay_telemetria.db',output=ROOT/'validation/replay_estado.json')
    started=time.monotonic()
    with source.open() as stream: counts=run(config,stream,0)
    elapsed=time.monotonic()-started
    result=json.loads(config.output.read_text())['aircraft']['F00001']
    pairs=result['pair_analysis']
    assert counts=={'accepted':21,'rejected':0},counts
    assert pairs['status']=='CANDIDATES',pairs
    assert pairs['matches_current_version'],pairs
    assert pairs['directed_pairs_examined']==10827390,pairs
    routes=[(c['origin']['ident'],c['destination']['ident']) for c in pairs['candidates']]
    assert ('SBBR','SBGR') in routes,routes
    report={'messages':counts,'elapsed_wall_s':round(elapsed,3),'pair_elapsed_s':pairs['elapsed_s'],
            'worker_elapsed_s':pairs['worker_elapsed_s'],'source_version':pairs['source_version'],
            'matches_current_version':pairs['matches_current_version'],'directed_pairs_examined':pairs['directed_pairs_examined'],
            'top_routes':routes,'mode':pairs['mode'],'scope':'synthetic great-circle replay; not real-world accuracy validation'}
    (ROOT/'validation/resultado_pares.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(report,ensure_ascii=False))
