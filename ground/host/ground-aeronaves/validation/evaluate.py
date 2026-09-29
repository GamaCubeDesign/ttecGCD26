"""Avaliação reproduzível, isolada da telemetria de operação.

Execute da raiz do projeto: python3 validation/evaluate.py
Não usa rotas publicadas como verdade nem altera pesos do estimador.
"""
import hashlib
import json
import sqlite3
import sys
import tempfile
import zipfile
from collections import Counter
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ground.config import Config
from ground.database import Database
from ground.aircraft_manager import AircraftManager
from ground.airport_estimator import AirportEstimator
from ground.parser import parse_line
from ground.trajectory import build_snapshot
from ground.temporal import vertical_features, approach_context

BASE = ROOT / 'validation/baseline-2026-09-24'
OUT = ROOT / 'validation/evaluation-2026-09-24'


def synthetic(t, **fields):
    return parse_line(json.dumps(dict(icao='ABC123', rx_epoch_ns=1790000000000000000+t*10**9, **fields)), 0)


def evaluate(name, observations, config, truth):
    with tempfile.TemporaryDirectory() as folder:
        cfg = replace(config,  telemetry_db=Path(folder)/'telemetry.db', output=Path(folder)/'state.json')
        manager = AircraftManager(Database(cfg.telemetry_db), AirportEstimator(cfg), cfg)
        rows = []
        for obs in observations:
            result = manager.process(obs)
            rows.append({'time_s': (obs.event_ns-observations[0].event_ns)/1e9,
                         **{kind: {'status': d['status'], 'airport': (d.get('candidate') or {}).get('ident'), 'reason': d.get('reason')}
                            for kind, d in result['airport_method'].items()}})
        metrics = {}
        for kind, expected in truth.items():
            accepted = [r for r in rows if r[kind]['status']=='ESTIMATED']
            tentative = [r for r in rows if r[kind]['status']=='TENTATIVE']
            final = rows[-1][kind]
            metrics[kind] = {'expected': expected, 'final': final,
                'outcome': 'abstention' if final['airport'] is None else 'correct' if final['airport']==expected else 'wrong',
                'first_estimate_s': accepted[0]['time_s'] if accepted else None,
                'first_tentative_s': tentative[0]['time_s'] if tentative else None,
                'wrong_estimated_messages': sum(r[kind]['airport']!=expected for r in accepted),
                'wrong_tentative_messages': sum(r[kind]['airport']!=expected for r in tentative)}
        if name=='E480F9_user_confirmed':
            baseline = []
            with (BASE/'estimates.ndjson').open() as stream:
                for line in stream:
                    previous = json.loads(line)
                    if previous['icao']=='E480F9':
                        baseline.append({kind: {'status':d['status'], 'airport':(d.get('candidate') or {}).get('ident'), 'reason':d.get('reason')}
                                         for kind,d in previous['airport_method'].items()})
            metrics['baseline_comparison'] = {
                'baseline_messages':len(baseline), 'replay_messages':len(rows),
                'identical_decisions': baseline==[{kind:r[kind] for kind in ('origin','destination')} for r in rows]}
        (OUT/(name+'.json')).write_text(json.dumps({'metrics': metrics, 'timeline': rows}, indent=2))
        print(name, json.dumps(metrics), flush=True)
        return metrics


def main():
    OUT.mkdir(exist_ok=True)
    results = {}
    with tempfile.TemporaryDirectory() as folder:
        ref = Path(folder)/'reference.db'
        with sqlite3.connect(ref) as db:
            db.execute('CREATE TABLE airports(id INTEGER PRIMARY KEY, ident TEXT, icao_code TEXT, name TEXT, type TEXT, latitude_deg REAL, longitude_deg REAL, elevation_ft REAL)')
            db.executemany('INSERT INTO airports VALUES (?,?,?,?,?,?,?,?)', [
                (1,'EAST','EAST','East','small_airport',0,.2,100),
                (2,'WEST','WEST','West','small_airport',0,-.2,100)])
        config = Config(reference_db=ref)
        cases = {
            'descent': ([synthetic(t,lat=0,lon=.02+t*.001,altitude_ft=4000-10*t,track_deg=270) for t in range(0,81,10)], {'origin':None,'destination':'EAST'}),
            'departure': ([synthetic(t,lat=0,lon=-.19+t*.001,altitude_ft=200+10*t,track_deg=90) for t in range(0,81,10)], {'origin':'WEST','destination':None}),
            'cruise': ([synthetic(t,lat=0,lon=t*.001,altitude_ft=35000,track_deg=90) for t in range(0,81,10)], {'origin':None,'destination':None}),
            'missing_position': ([synthetic(t,altitude_ft=4000-10*t) for t in range(0,81,10)], {'origin':None,'destination':None}),
            'expired_position': ([synthetic(0,lat=0,lon=0,altitude_ft=4000)]+[synthetic(t,altitude_ft=4000-10*t) for t in range(10,91,10)], {'origin':None,'destination':None}),
            'duplicate_position': ([synthetic(0,lat=0,lon=0,altitude_ft=4000)]*10, {'origin':None,'destination':None}),
        }
        for name,(observations,truth) in cases.items():
            results[name] = evaluate(name,observations,config,truth)
    with zipfile.ZipFile(BASE/'baseline.zip') as archive:
        lines = archive.read('input/eventos.ndjson').decode().splitlines()
        unchanged = all((ROOT/n).is_file() and hashlib.sha256(archive.read(n)).digest()==hashlib.sha256((ROOT/n).read_bytes()).digest()
                        for n in archive.namelist() if n.startswith('ground/') and n.endswith('.py'))
    observations = [parse_line(line,0) for line in lines if line.strip()]
    known = [o for o in observations if o.icao=='E480F9']
    results['E480F9_user_confirmed'] = evaluate('E480F9_user_confirmed',known,Config(),{'destination':'SBBR'})
    origin_history = [o for o in observations if o.icao=='E49C01']
    estimator = AirportEstimator(Config())
    diagnostics = []
    first = next(o.event_ns for o in origin_history if o.has_position)
    for seconds in (30,60,120,180):
        history = [o for o in origin_history if o.event_ns<=first+seconds*1e9]
        snapshot = build_snapshot(history,60)
        snapshot['vertical'] = vertical_features(history,history[-1].event_ns)
        snapshot['approach_context'] = approach_context(history,history[-1].event_ns)
        candidates = estimator.estimate(snapshot,'CLIMB',origin=True)
        sbbr = next(((i+1,c) for i,c in enumerate(candidates) if c['ident']=='SBBR'), None)
        diagnostics.append({'seconds_after_first_position':seconds, 'top':candidates[:5], 'SBBR':sbbr})
    (OUT/'E49C01-diagnostic.json').write_text(json.dumps(diagnostics,indent=2))
    reasons = Counter()
    with (BASE/'estimates.ndjson').open() as stream:
        for line in stream:
            row=json.loads(line)
            if row['icao']=='E49C01':
                reasons[row['airport_method']['origin'].get('reason')] += 1
    (OUT/'summary.json').write_text(json.dumps({'production_code_unchanged':unchanged,'results':results,
        'E49C01_origin_reasons':dict(reasons),
        'scope':'Airport method. Synthetic cases are not operational accuracy. Only E480F9 destination is externally labelled by user; other real flights are not scored.'},indent=2))


if __name__=='__main__':
    main()
