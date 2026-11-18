#!/usr/bin/env python3
"""O estimador de origem e destino com e sem a categoria da aeronave.

Roda o método local (sem pares) sobre três entradas, todas com uma amostra a
cada 5 s: a simulação com a categoria (A3), a mesma sem a categoria, e o que
desceu pelo LoRa (ground.jsonl), que não traz categoria. Conta as decisões
de destino das chegadas e de origem das saídas que apontam SBBR.

    python3 categoria.py SIM20.ndjson GROUND.jsonl
"""
import collections
import json
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
EST = ROOT / 'ground/host'
sys.path.insert(0, str(ROOT / 'ground/host'))
sys.path.insert(0, str(HERE))
from adsb.tracks_to_ndjson import records  # noqa: E402
from gerar_sim20 import KIND  # noqa: E402


def inputs(sim, ground, d):
    t0 = None
    with open(d / 'com_categoria.ndjson', 'w') as a, open(d / 'sem_categoria.ndjson', 'w') as b:
        for line in open(sim):
            r = json.loads(line)
            t0 = t0 or r['rx_epoch_ns']
            if round((r['rx_epoch_ns'] - t0) / 1e9) % 5:
                continue
            a.write(json.dumps(r) + '\n')
            r.pop('category', None)
            b.write(json.dumps(r) + '\n')
    with open(d / 'lora.ndjson', 'w') as c:
        for line in open(ground):
            ev = json.loads(line)
            if ev.get('ev') == 'tracks':
                for r in records(ev):
                    c.write(json.dumps(r) + '\n')
    return ('com_categoria', 'sem_categoria', 'lora')


def decisions(db_path):
    dec = collections.defaultdict(collections.Counter)
    with sqlite3.connect(f'file:{db_path}?mode=ro', uri=True) as db:
        for icao, js in db.execute('SELECT icao,result_json FROM estimates ORDER BY id'):
            am = json.loads(js).get('airport_method') or {}
            for t in ('origin', 'destination'):
                x = am.get(t) or {}
                if x.get('status') in ('TENTATIVE', 'ESTIMATED'):
                    dec[(icao, t)][(x.get('candidate') or {}).get('ident')] += 1
    return dec


def main(sim, ground):
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        names = inputs(sim, ground, d)
        procs = [subprocess.Popen([sys.executable, '-m', 'adsb.estimator', '--input', str(d / f'{k}.ndjson'),
                                   '--telemetry-db', str(d / f'{k}.db'), '--output', str(d / f'{k}.json'),
                                   '--no-pairs'], cwd=EST, stdout=subprocess.DEVNULL) for k in names]
        for p in procs:
            if p.wait() != 0:
                raise SystemExit('o estimador falhou')
        for k in names:
            dec = decisions(d / f'{k}.db')
            print('==', k)
            for grp, t in (('chegada', 'destination'), ('saída', 'origin')):
                ok = tot = 0
                top = []
                for icao in sorted(i for i, g in KIND.items() if g == grp):
                    c = dec.get((icao, t), collections.Counter())
                    tot += sum(c.values())
                    ok += c.get('SBBR', 0)
                    m = c.most_common(1)
                    top.append('%s:%s' % (icao[-2:], m[0][0] if m else '-'))
                print('  %-8s %-11s decisões %4d, SBBR %4d | mais frequente: %s'
                      % (grp, t, tot, ok, ' '.join(top)))
            cz = sum(sum(dec.get((i, t), {}).values()) for i, g in KIND.items() if g == 'cruzeiro'
                     for t in ('origin', 'destination'))
            print('  cruzeiro decisões %d' % cz)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
