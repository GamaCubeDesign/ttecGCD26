#!/usr/bin/env python3
"""Erro da trajetória reconstruída na ground contra o gabarito da simulação.

Cada posição que desceu pelo LoRa (ground.jsonl, convertida como a ground faz,
por ground/host/tracks_to_ndjson.py) é comparada com a posição verdadeira da
simulação no mesmo instante. O replay começou quando o adsbd se conectou; o
deslocamento entre os relógios sai do registro de bordo.

    python3 trajetoria.py SIM20.ndjson ADSB_DE_BORDO.ndjson GROUND.jsonl
"""
import bisect
import collections
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / 'ground/host'))
from tracks_to_ndjson import records  # noqa: E402


def hav(a, b, c, d):
    p1, p2 = math.radians(a), math.radians(c)
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(d - b) / 2) ** 2
    return 2 * 6371008.8 * math.asin(math.sqrt(h))


def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def main(sim, onboard, ground):
    truth = collections.defaultdict(list)
    for line in open(sim):
        r = json.loads(line)
        truth[r['icao']].append((r['rx_epoch_ns'], r['lat'], r['lon'], r['altitude_ft']))
    first = {}
    for line in open(onboard):
        r = json.loads(line)
        if 'lat' in r and r['icao'] not in first:
            first[r['icao']] = (r['rx_epoch_ns'], r['lat'], r['lon'])
    offs = []
    for icao, (t, la, lo) in first.items():
        tr = truth[icao]
        k = min(range(len(tr)), key=lambda i: hav(la, lo, tr[i][1], tr[i][2]))
        offs.append(t - tr[k][0])
    off = sorted(offs)[len(offs) // 2]
    errs, terr, alt, per = [], [], [], collections.defaultdict(list)
    for line in open(ground):
        ev = json.loads(line)
        if ev.get('ev') != 'tracks':
            continue
        for r in records(ev):
            if 'lat' not in r:
                continue
            tr = truth[r['icao']]
            ts = [x[0] + off for x in tr]
            t = r['rx_epoch_ns']
            i = bisect.bisect_left(ts, t)
            if i <= 0 or i >= len(ts):
                continue
            f = (t - ts[i - 1]) / (ts[i] - ts[i - 1])
            la = tr[i - 1][1] + f * (tr[i][1] - tr[i - 1][1])
            lo = tr[i - 1][2] + f * (tr[i][2] - tr[i - 1][2])
            errs.append(hav(r['lat'], r['lon'], la, lo))
            per[r['icao']].append(t)
            k = min(range(max(0, i - 15), min(len(tr), i + 15)),
                    key=lambda j: hav(r['lat'], r['lon'], tr[j][1], tr[j][2]))
            terr.append((t - ts[k]) / 1e9)
            a = tr[i - 1][3] + f * (tr[i][3] - tr[i - 1][3])
            alt.append(abs(r.get('altitude_ft', a) - a))
    gaps = [(b - a) / 1e9 for v in per.values() for a, b in zip(sorted(v), sorted(v)[1:])]
    print('posições comparadas: %d, de %d aeronaves' % (len(errs), len(per)))
    print('erro de posição  : p50 %.0f m, p95 %.0f m, máx %.0f m' % (q(errs, .5), q(errs, .95), max(errs)))
    print('erro de instante : p50 %+.2f s, p95 |%.2f| s' % (q(terr, .5), q([abs(x) for x in terr], .95)))
    print('erro de altitude : p50 %.0f ft, p95 %.0f ft' % (q(alt, .5), q(alt, .95)))
    print('intervalo entre pontos da mesma aeronave: p50 %.1f s, p95 %.1f s, máx %.1f s'
          % (q(gaps, .5), q(gaps, .95), max(gaps)))


if __name__ == '__main__':
    main(*sys.argv[1:4])
