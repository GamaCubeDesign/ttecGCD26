#!/usr/bin/env python3
"""The ADR-0019 conclusions against a truth, with the same rule as the
estimator's validation/avaliar_estimativas.py, which scores only the local
method: here the combined conclusion (combined_analysis) and the best
route pair (pair_analysis), from the estimator's estado.json.

    avaliar_combinada.py ESTADO.json GROUND_TRUTH.json [--json OUT]

A decision is an airport named with status ESTIMATED or TENTATIVE (combined)
or the first candidate pair (pairs). ACERTO: it is the truth's airport;
ERRO: another; FALSO_POSITIVO: one named where the truth has none (the
capture did not see that end of the flight, so not necessarily wrong).
"""

import argparse
import json
from pathlib import Path


def decided(state, method, kind):
    if method == 'combined':
        d = ((state.get('combined_analysis') or {}).get(kind)) or {}
        if d.get('ident') and d.get('status') in ('ESTIMATED', 'TENTATIVE'):
            return d['ident']
        return None
    top = ((state.get('pair_analysis') or {}).get('candidates') or [])
    if not top:
        return None
    return ((top[0].get(kind)) or {}).get('ident')


def score(states, truth, method):
    out = {}
    for kind in ('origin', 'destination'):
        r = dict(acertos=0, erros=0, falsos_positivos=0, unknown=0, casos=0)
        for icao, t in truth.items():
            exp = t.get(kind)
            got = decided(states.get(icao) or {}, method, kind)
            if exp is None:
                r['falsos_positivos'] += got is not None
                continue
            r['casos'] += 1
            if got is None:
                r['unknown'] += 1
            elif got == exp:
                r['acertos'] += 1
            else:
                r['erros'] += 1
        dec = r['acertos'] + r['erros']
        r['coverage'] = dec / r['casos'] if r['casos'] else None
        r['acerto_quando_decidiu'] = r['acertos'] / dec if dec else None
        out[kind] = r
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('state', type=Path)
    ap.add_argument('truth', type=Path)
    ap.add_argument('--json', type=Path)
    a = ap.parse_args()
    states = json.loads(a.state.read_text()).get('aircraft', {})
    truth = json.loads(a.truth.read_text())['aircraft']
    result = {m: score(states, truth, m) for m in ('combined', 'pairs')}
    for m, label in (('combined', 'CONCLUSÃO COMBINADA'), ('pairs', 'MELHOR PAR')):
        print(label)
        for kind, name in (('origin', 'ORIGEM'), ('destination', 'DESTINO')):
            r = result[m][kind]
            cov = '-' if r['coverage'] is None else '%.1f%%' % (100 * r['coverage'])
            acc = '-' if r['acerto_quando_decidiu'] is None else '%.1f%%' % (100 * r['acerto_quando_decidiu'])
            print('  %-8s: acertos=%d | erros=%d | UNKNOWN=%d | falso positivo=%d | coverage=%s | acerto quando decidiu=%s'
                  % (name, r['acertos'], r['erros'], r['unknown'], r['falsos_positivos'], cov, acc))
    if a.json:
        a.json.write_text(json.dumps(result, indent=1) + '\n')


if __name__ == '__main__':
    main()
