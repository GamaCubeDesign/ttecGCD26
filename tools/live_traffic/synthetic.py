#!/usr/bin/env python3
"""synthetic.py — synthetic ADS-B scenarios around UNSAM, with their truth, in
the same folder layout as a real capture (live_traffic.py), so avaliar.sh
scores the ground's estimator on them unchanged.

    synthetic.py rotas OUT_DIR       straight flights between real airports
    synthetic.py ficticio OUT_DIR    invented addresses, ends far from airports

The competition's ADS-B is simulated by the organisers with an SDR
(rules 4.4.4): its aircraft may be replays, straight flights between real
airports, or invented. "rotas" is the second case — the one the pair method
of ADR-0019 models best — and "ficticio" the third, where the right answer is
no airport at all, so any decision counts as a false positive.

Flights use ground/host/adsb/validation/simular_trajetorias.Flight. Half the
departures are first seen already airborne, with no position on the ground
and hence no take-off anchor: the case ADR-0022's climb-back distance is for.
Samples every 5 s, the downlink's snapshot period; 600 s, one mission.
Deterministic: the same command gives the same flights.
"""

import json
import math
import random
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'ground/host'))
from adsb.validation import simular_trajetorias as sim  # noqa: E402
from adsb.estimator.geo import bearing_deg, distance_km  # noqa: E402

REFERENCE_DB = ROOT / 'ground/host/adsb/data/referencias/referencias.db'
UNSAM = (-34.5772, -58.5225)
MISSION_S = 600
PERIOD_S = 5
LIGHT_TOP_FT = 6000      # light aircraft climb to this, not to Flight's 15 000 ft

# (origin, destination) for the departures from Buenos Aires, the arrivals to
# it, and the overflights that pass within reach of UNSAM.
DEPARTURES = [('SABE', 'SBGR'), ('SABE', 'SACO'), ('SABE', 'SAME'), ('SAEZ', 'SCEL'),
              ('SAEZ', 'SBPA'), ('SAEZ', 'SASA'), ('SABE', 'SAZS'), ('SAEZ', 'SARI')]
ARRIVALS = [('SBGR', 'SABE'), ('SACO', 'SABE'), ('SCEL', 'SAEZ'), ('SAME', 'SABE'),
            ('SBPA', 'SAEZ'), ('SASA', 'SABE'), ('SAZS', 'SAEZ'), ('SARI', 'SABE')]
OVERFLIGHTS = [('SUMU', 'SAME'), ('SBPA', 'SAZM'), ('SBGR', 'SAZS'), ('SACO', 'SUMU')]
LIGHT = [('SADF', 'SAAR'), ('SADM', 'SAZM'), ('SADF', 'SUMU')]


def airports(db_path=REFERENCE_DB):
    with sqlite3.connect(db_path.resolve().as_uri() + '?mode=ro', uri=True) as db:
        return {i: (lat, lon, elev or 0.0, kind) for i, lat, lon, elev, kind in db.execute(
            "SELECT ident, latitude_deg, longitude_deg, elevation_ft, type FROM airports WHERE type != 'closed'")}


def closest_point(a, b, to):
    """The point of the great circle a→b closest to `to`, and the heading there."""
    hd = bearing_deg(a[0], a[1], b[0], b[1])
    total = distance_km(a[0], a[1], b[0], b[1])
    best = None
    for k in range(0, int(total), 5):
        p = sim.advance(a[0], a[1], hd, k)
        d = distance_km(p[0], p[1], to[0], to[1])
        if best is None or d < best[0]:
            best = (d, p, k)
    p = best[1]
    q = sim.advance(a[0], a[1], hd, best[2] + 5)
    return p, bearing_deg(p[0], p[1], q[0], q[1])


def generate(flights, out, extra):
    """Samples every PERIOD_S over the mission; first-seen-airborne flights
    start before 0 and are cut at 0, so no ground position is ever seen."""
    epoch = 1_795_000_000_000_000_000
    lines = []
    for f, light in flights:
        for s in range(0, MISSION_S + 1, PERIOD_S):
            m = f.message(s, epoch)
            if m is None:
                continue
            if light:      # a light aircraft does not climb to 15 000 ft
                u = min(1.0, max(0.0, (s - f.start) / f.duration))
                if f.mode == 'departure':
                    m['altitude_ft'] = round(f.elevation + LIGHT_TOP_FT * u)
                    m['vertical_rate_fpm'] = 0 if m['on_ground'] else round(LIGHT_TOP_FT / f.duration * 60)
            m.pop('simulation', None)
            m.update(extra.get(f.icao, {}))
            lines.append(m)
    lines.sort(key=lambda m: (m['rx_epoch_ns'], m['icao']))
    out.mkdir(parents=True, exist_ok=True)
    with open(out / 'capture.ndjson', 'w') as a, open(out / 'capture_sem_categoria.ndjson', 'w') as b:
        for m in lines:
            a.write(json.dumps(m) + '\n')
            m = dict(m)
            m.pop('category', None)
            b.write(json.dumps(m) + '\n')
    return len(lines)


def rotas(ap):
    flights, truth, n = [], {}, 0

    def add(f, light, origin, dest):
        nonlocal n
        flights.append((f, light))
        truth[f.icao] = {'callsign': f.callsign, 'category': f.category, 'origin': origin, 'destination': dest,
                         'evidence': {'origin': 'scenario' if origin else None,
                                      'destination': 'scenario' if dest else None}}

    for k, (o, d) in enumerate(DEPARTURES):
        n += 1
        lat, lon, elev, _ = ap[o]
        hd = bearing_deg(lat, lon, ap[d][0], ap[d][1])
        airborne = k % 2 == 1            # half first seen already climbing
        f = sim.Flight('E1%04X' % n, 'RTA%03d' % n, 'departure', -90 if airborne else 10 * k,
                       540, lat, lon, elev, hd, 95, category='A3')
        add(f, False, o, d)
    for k, (o, d) in enumerate(ARRIVALS):
        n += 1
        lat, lon, elev, _ = ap[d]
        hd = bearing_deg(lat, lon, ap[o][0], ap[o][1])
        f = sim.Flight('E1%04X' % n, 'RTA%03d' % n, 'arrival', 8 * k, 540, lat, lon, elev, hd, 85, category='A3')
        add(f, False, o, d)
    for k, (o, d) in enumerate(OVERFLIGHTS):
        n += 1
        p, hd = closest_point(ap[o][:2], ap[d][:2], UNSAM)
        start = sim.advance(p[0], p[1], (hd + 180) % 360, 60)
        f = sim.Flight('E1%04X' % n, 'RTA%03d' % n, 'cruise', 6 * k, 560, start[0], start[1], 0, hd, 120,
                       category='A3')
        add(f, False, o, d)
    for k, (o, d) in enumerate(LIGHT):
        n += 1
        lat, lon, elev, _ = ap[o]
        hd = bearing_deg(lat, lon, ap[d][0], ap[d][1])
        f = sim.Flight('E1%04X' % n, 'LVL%03d' % n, 'departure', -60 if k % 2 else 20 * k, 540, lat, lon, elev,
                       hd, 26, category='A1')
        add(f, True, o, d)
    return flights, truth


def ficticio(ap):
    """Invented addresses and ends at least 15 km from any facility."""
    rnd = random.Random(20261118)
    pts = [(v[0], v[1]) for v in ap.values()]

    def far_point():
        while True:
            lat = UNSAM[0] + rnd.uniform(-1.6, 1.6)
            lon = UNSAM[1] + rnd.uniform(-1.9, 1.9)
            if all(distance_km(lat, lon, a, b) > 15 for a, b in pts if abs(a - lat) < .3 and abs(b - lon) < .3):
                return lat, lon
    flights, truth = [], {}
    for n in range(1, 13):
        mode = ('departure', 'arrival', 'cruise')[n % 3]
        lat, lon = far_point()
        icao = '%06X' % rnd.randrange(0x100000, 0xFFFFFF)
        cat = rnd.choice(['A3', 'A3', 'A0', None])
        f = sim.Flight(icao, 'FIC%03d' % n, mode, -60 if n % 4 == 0 else rnd.randrange(0, 60), 540, lat, lon, 0,
                       rnd.uniform(0, 360), 90 if mode != 'cruise' else 120, category=cat)
        flights.append((f, False))
        truth[icao] = {'callsign': f.callsign, 'category': cat, 'origin': None, 'destination': None,
                       'evidence': {'origin': None, 'destination': None}}
    return flights, truth


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('rotas', 'ficticio'):
        sys.exit(__doc__)
    kind, out = sys.argv[1], Path(sys.argv[2])
    ap = airports()
    flights, truth = (rotas if kind == 'rotas' else ficticio)(ap)
    n = generate(flights, out, {})
    (out / 'ground_truth.json').write_text(json.dumps({'aircraft': truth}, indent=1, ensure_ascii=False))
    (out / 'meta.json').write_text(json.dumps({
        'scenario': kind, 'flights': len(flights), 'records': n, 'period_s': PERIOD_S, 'mission_s': MISSION_S,
        'generator': 'tools/live_traffic/synthetic.py'}, indent=1))
    print(json.dumps({'scenario': kind, 'flights': len(flights), 'records': n, 'dir': str(out)}))


if __name__ == '__main__':
    main()
