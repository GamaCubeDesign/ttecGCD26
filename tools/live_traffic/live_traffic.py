#!/usr/bin/env python3
"""live_traffic.py — real air traffic from public ADS-B aggregators, to test
the ground without the satellite.

    live_traffic.py capture --out DIR [--center LAT,LON|IDENT] [--radius-nm 150]
                            [--period 5] [--duration 3600] [--max-tracks 200]
    live_traffic.py convert DIR [--no-category] [--keep-mlat] [--output FILE]
    live_traffic.py truth DIR [--reference-db FILE]

capture polls adsb.lol around a point every PERIOD seconds — the link's
snapshot period — and keeps every response in DIR/raw.jsonl.gz. When an
aircraft leaves, and for every aircraft at the end, it asks the OpenSky
Network for the aircraft's track since take-off (DIR/tracks/ICAO.json). At
the end it runs convert.

convert turns the raw responses into DIR/capture.ndjson: one object per
aircraft update, in the onboard record's fields (what tools/sbs_replay and
the ground's estimator read), in time order. Like our receiver, it keeps
only positions decoded from ADS-B itself (not MLAT or TIS-B) and no older
than adsbd's position_max_age (10 s). --no-category drops the emitter
category, which the satellite cannot downlink (the SBS feed has none): that
is the input the ground really gets.

truth writes DIR/ground_truth.json in the format of the estimator's
validation/avaliar_estimativas.py: origin and destination of each flight,
snapped to the airports of referencias.db, or null when the data does not
show them. Evidence comes from what the capture itself saw on the ground,
and from the OpenSky track.

The default centre is UNSAM (San Martín, Buenos Aires), the competition's
site. Only the centre point and a generic User-Agent leave this machine.
Standard library only.

Data: adsb.lol, under the Open Database License (ODbL 1.0); the OpenSky
Network, for non-commercial research use.
"""

import argparse
import gzip
import json
import math
import sqlite3
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REFERENCE_DB = ROOT / 'ground/host/adsb/data/referencias/referencias.db'
UNSAM = (-34.5772, -58.5225)          # Campus Miguelete, San Martín
USER_AGENT = 'ttec-live-traffic/1'
ADSBLOL = 'https://api.adsb.lol/v2/point/{lat:.4f}/{lon:.4f}/{radius}'
OPENSKY_TRACK = 'https://opensky-network.org/api/tracks/all?icao24={icao}&time=0'

POSITION_MAX_AGE_S = 10.0     # adsbd's position_max_age_ms (adsbd.conf.example)
BACKOFF_MAX_S = 60.0          # adsb.lol answers 429 when polled too often
GONE_AFTER_S = 60.0           # absent this long: the aircraft has left
FT_PER_M = 3.28084
EARTH_KM = 6371.0088

# Snapping a point to an airport. On the ground it must be at the airport; in
# the air (a track's first report after take-off, its last before landing) it
# may be a few kilometres out, but low. Only facilities the aircraft can use
# count: a jet climbing out of Guarulhos is not given the heliport under it.
SNAP_GROUND_KM = 3.0
SNAP_AIR_KM = 8.0
SNAP_AGL_FT = 1000.0
TYPE_RANK = {'large_airport': 0, 'medium_airport': 1, 'small_airport': 2,
             'heliport': 3, 'seaplane_base': 4, 'balloonport': 5}
USABLE = {'A1': ('large_airport', 'medium_airport', 'small_airport'),
          'A2': ('large_airport', 'medium_airport', 'small_airport'),
          'A3': ('large_airport', 'medium_airport'),
          'A4': ('large_airport', 'medium_airport'),
          'A5': ('large_airport', 'medium_airport'),
          'A7': ('large_airport', 'medium_airport', 'small_airport', 'heliport')}
# A flight's first point is a take-off only if it climbs from it, and its last
# a landing only if the flight came down to it.
TAKEOFF_CLIMB_FT = 500.0
LANDING_DESCENT_FT = 2000.0
# An update less than this newer than the last one kept is the same message,
# seen again by the next poll (adsb.lol's `now` and `seen_pos` round apart).
SAME_UPDATE_NS = 200_000_000


# ---- adsb.lol responses -> onboard records ----

def records_from_response(resp, keep_mlat=False, max_pos_age_s=POSITION_MAX_AGE_S):
    """One record per aircraft in a readsb/adsb.lol v2 response, or none.

    rx_epoch_ns is when the position (or, without one, the last message)
    was received: the response's `now` minus `seen_pos` (`seen`)."""
    now_ns = int(resp['now']) * 1_000_000            # milliseconds since the epoch
    out = []
    for a in resp.get('ac') or []:
        hexid = str(a.get('hex', ''))
        if not hexid or hexid.startswith('~'):
            continue                  # not an ICAO address: TIS-B, anonymised
        rec = {'icao': hexid.upper()}
        has_pos = a.get('lat') is not None and a.get('lon') is not None
        if has_pos and not keep_mlat and ('lat' in (a.get('mlat') or []) or 'lat' in (a.get('tisb') or [])):
            has_pos = False           # our receiver cannot multilaterate
        if has_pos and a.get('seen_pos') is not None and a['seen_pos'] > max_pos_age_s:
            has_pos = False           # adsbd leaves stale positions out
        age = a.get('seen_pos') if has_pos else a.get('seen')
        if age is None:
            continue
        rec['rx_epoch_ns'] = now_ns - int(round(float(age) * 1e9))
        callsign = (a.get('flight') or '').strip()
        if callsign:
            rec['callsign'] = callsign
        if a.get('category'):
            rec['category'] = a['category']
        if has_pos:
            rec['lat'] = round(float(a['lat']), 6)
            rec['lon'] = round(float(a['lon']), 6)
        alt = a.get('alt_baro')
        if alt == 'ground':
            rec['on_ground'] = 1
        elif isinstance(alt, (int, float)):
            rec['altitude_ft'] = int(alt)
            rec['on_ground'] = 0
        if a.get('gs') is not None:
            rec['ground_speed_kt'] = round(float(a['gs']), 1)
        if a.get('track') is not None:
            rec['track_deg'] = round(float(a['track']), 2)
        rate = a.get('baro_rate', a.get('geom_rate'))
        if rate is not None:
            rec['vertical_rate_fpm'] = int(rate)
        if a.get('t'):
            rec['aircraft_type'] = a['t']
        rec['source'] = 'adsb.lol'
        if len(rec) > 3:              # icao, rx_epoch_ns, source: nothing else
            out.append(rec)
    return out


class Deduper:
    """Passes only updates newer than the last one kept for that aircraft:
    an aircraft that sent nothing between two polls appears in both."""

    def __init__(self):
        self.last = {}

    def __call__(self, rec):
        prev = self.last.get(rec['icao'])
        if prev is not None and rec['rx_epoch_ns'] < prev + SAME_UPDATE_NS:
            return False
        self.last[rec['icao']] = rec['rx_epoch_ns']
        return True


def convert(raw_path, no_category=False, keep_mlat=False):
    """Every response in raw.jsonl.gz -> records, deduplicated, time-ordered."""
    keep = Deduper()
    out = []
    with gzip.open(raw_path, 'rt', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            for rec in records_from_response(json.loads(line), keep_mlat=keep_mlat):
                if keep(rec):
                    if no_category:
                        rec.pop('category', None)
                    out.append(rec)
    out.sort(key=lambda r: (r['rx_epoch_ns'], r['icao']))
    return out


# ---- the truth: origin and destination ----

def distance_km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    h = (math.sin((p2 - p1) / 2) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2)
    return 2 * EARTH_KM * math.asin(math.sqrt(min(1.0, h)))


def load_airports(db_path):
    """(ident, type, lat, lon, elevation_ft) of every open facility."""
    with sqlite3.connect(Path(db_path).resolve().as_uri() + '?mode=ro', uri=True) as db:
        return [r for r in db.execute(
            "SELECT ident, type, latitude_deg, longitude_deg, elevation_ft FROM airports "
            "WHERE type != 'closed' AND latitude_deg IS NOT NULL AND longitude_deg IS NOT NULL")]


def snap_airport(lat, lon, alt_ft, airports, category=None):
    """The airport a point is at, or None.

    On the ground (alt_ft None) within SNAP_GROUND_KM; in the air within
    SNAP_AIR_KM and no higher than SNAP_AGL_FT above it. With a known
    category, only the facilities it can use, the closest first; without
    one, the largest kind first, then the closest."""
    max_km = SNAP_GROUND_KM if alt_ft is None else SNAP_AIR_KM
    usable = USABLE.get(category)
    best = None
    dlat = max_km / 111.0
    for ident, kind, alat, alon, elev in airports:
        if abs(alat - lat) > dlat or (usable is not None and kind not in usable):
            continue
        d = distance_km(lat, lon, alat, alon)
        if d > max_km:
            continue
        if alt_ft is not None and alt_ft - (elev or 0.0) > SNAP_AGL_FT:
            continue
        key = (d,) if usable is not None else (TYPE_RANK.get(kind, 9), d)
        if best is None or key < best[0]:
            best = (key, ident)
    return None if best is None else best[1]


def track_points(track):
    """OpenSky path rows -> (time_s, lat, lon, alt_ft or None, on_ground)."""
    out = []
    for row in (track or {}).get('path') or []:
        t, lat, lon, alt_m = row[0], row[1], row[2], row[3]
        if lat is None or lon is None:
            continue
        on_ground = bool(row[5]) if len(row) > 5 else False
        alt_ft = None if on_ground or alt_m is None else alt_m * FT_PER_M
        out.append((t, lat, lon, alt_ft, on_ground))
    return out


def observed_points(records):
    """The capture's own records of one aircraft -> the same tuples."""
    out = []
    for r in records:
        if 'lat' not in r:
            continue
        ground = r.get('on_ground') == 1
        out.append((r['rx_epoch_ns'] / 1e9, r['lat'], r['lon'],
                    None if ground else r.get('altitude_ft'), ground))
    return out


def endpoint(points, airports, first, category=None):
    """The airport at the start (first) or the end of a flight, or None.

    The start counts if the aircraft is on the ground there and seen airborne
    after, or low there and climbs at least TAKEOFF_CLIMB_FT after it. The
    end counts if it is on the ground after being airborne, or low after
    being at least LANDING_DESCENT_FT higher. An aircraft that enters the
    capture already flying low has no known start."""
    if len(points) < 2:
        return None
    t, lat, lon, alt, ground = points[0] if first else points[-1]
    rest = points[1:] if first else points[:-1]
    airborne = [p[3] for p in rest if not p[4] and p[3] is not None]
    if not airborne:
        return None
    if not ground:
        if alt is None:
            return None
        need = TAKEOFF_CLIMB_FT if first else LANDING_DESCENT_FT
        if max(airborne) - alt < need:
            return None
    return snap_airport(lat, lon, None if ground else alt, airports, category)


def truth(records, tracks, airports):
    """{icao: {callsign, origin, destination, evidence}} for every aircraft
    seen airborne. What the capture saw itself comes first; the OpenSky
    track fills in what happened before or after it."""
    by_icao = {}
    for r in records:
        by_icao.setdefault(r['icao'], []).append(r)
    out = {}
    for icao, recs in sorted(by_icao.items()):
        recs.sort(key=lambda r: r['rx_epoch_ns'])
        seen = observed_points(recs)
        if not any(not p[4] for p in seen):
            continue                  # parked or taxiing only: not a flight
        track = track_points(tracks.get(icao))
        callsign = next((r['callsign'] for r in recs if r.get('callsign')), None)
        category = next((r['category'] for r in recs if r.get('category')), None)
        entry = {'callsign': callsign, 'category': category, 'evidence': {}}
        for kind, first in (('origin', True), ('destination', False)):
            value, source = endpoint(seen, airports, first, category), 'capture'
            if value is None:
                value, source = endpoint(track, airports, first, category), 'opensky_track'
            entry[kind] = value
            entry['evidence'][kind] = source if value is not None else None
        entry['evidence']['track_points'] = len(track)
        out[icao] = entry
    return out


# ---- the network ----

def fetch_json(url, timeout=20.0):
    """(status, object or None). Never raises for HTTP or network errors."""
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode('utf-8'))
    except urllib.error.HTTPError as e:
        return e.code, None
    except (urllib.error.URLError, TimeoutError, OSError, ValueError):
        return 0, None


def parse_center(text, airports=None):
    if text is None:
        return UNSAM
    if ',' in text:
        lat, lon = (float(x) for x in text.split(','))
        return lat, lon
    for ident, _, lat, lon, _ in airports or load_airports(REFERENCE_DB):
        if ident == text.upper():
            return lat, lon
    raise SystemExit(f'live_traffic: no airport {text} in the reference database')


def capture(out_dir, center, radius_nm, period_s, duration_s, max_tracks,
            fetch=fetch_json, clock=time.time, sleep=time.sleep, log=print):
    """Polls until duration_s or Ctrl-C; returns the meta it wrote."""
    out_dir = Path(out_dir)
    (out_dir / 'tracks').mkdir(parents=True, exist_ok=True)
    url = ADSBLOL.format(lat=center[0], lon=center[1], radius=int(radius_nm))
    meta = {'center': list(center), 'radius_nm': radius_nm, 'period_s': period_s,
            'source': 'adsb.lol v2/point (ODbL 1.0)',
            'truth': 'OpenSky Network tracks/all (non-commercial research)',
            'started_s': clock(), 'polls': 0, 'poll_failures': 0, 'aircraft': 0,
            'tracks': 0, 'track_failures': 0, 'track_stop': None}
    last_seen, fetched = {}, set()

    def get_track(icao):
        if icao in fetched or len(fetched) >= max_tracks or meta['track_stop']:
            return
        fetched.add(icao)
        status, body = fetch(OPENSKY_TRACK.format(icao=icao.lower()))
        if status == 200 and body:
            (out_dir / 'tracks' / f'{icao}.json').write_text(json.dumps(body))
            meta['tracks'] += 1
        else:
            meta['track_failures'] += 1
            if status == 429:
                meta['track_stop'] = 'OpenSky rate limit (429)'
                log('live_traffic: OpenSky refused more tracks (429); stopping track requests')

    start = clock()
    next_poll = start
    backoff = 0.0     # after a 429: the wait, doubled each time, until a 200
    try:
        with gzip.open(out_dir / 'raw.jsonl.gz', 'at', encoding='utf-8') as raw:
            while clock() - start < duration_s:
                status, body = fetch(url)
                meta['polls'] += 1
                now = clock()
                if status == 200 and body is not None and 'now' in body:
                    raw.write(json.dumps(body, separators=(',', ':')) + '\n')
                    raw.flush()
                    for a in body.get('ac') or []:
                        hexid = str(a.get('hex', ''))
                        if hexid and not hexid.startswith('~'):
                            last_seen[hexid.upper()] = now
                    backoff = 0.0
                else:
                    meta['poll_failures'] += 1
                    if status == 429:
                        backoff = min(BACKOFF_MAX_S, backoff * 2 if backoff else period_s)
                        meta['rate_limited'] = meta.get('rate_limited', 0) + 1
                    log(f'live_traffic: poll failed (HTTP {status})')
                for icao, t in list(last_seen.items()):
                    if now - t > GONE_AFTER_S:
                        get_track(icao)
                if meta['polls'] % 12 == 0:
                    log('live_traffic: %d polls, %d aircraft so far, %d tracks'
                        % (meta['polls'], len(last_seen), meta['tracks']))
                next_poll += period_s + backoff
                wait = next_poll - clock()
                if wait > 0:
                    sleep(wait)
                else:
                    next_poll = clock()
    except KeyboardInterrupt:
        log('live_traffic: stopped by the operator')
    for icao in sorted(last_seen):
        get_track(icao)
    meta['aircraft'] = len(last_seen)
    meta['ended_s'] = clock()
    (out_dir / 'meta.json').write_text(json.dumps(meta, indent=1))
    return meta


def write_ndjson(path, records):
    with open(path, 'w', encoding='utf-8') as f:
        for r in records:
            f.write(json.dumps(r, separators=(',', ':')) + '\n')


def load_tracks(out_dir):
    tracks = {}
    for p in sorted((Path(out_dir) / 'tracks').glob('*.json')):
        try:
            tracks[p.stem.upper()] = json.loads(p.read_text())
        except ValueError:
            continue
    return tracks


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture', help='poll adsb.lol and fetch the OpenSky tracks')
    c.add_argument('--out', type=Path, required=True)
    c.add_argument('--center', help='LAT,LON or an airport ident (default: UNSAM)')
    c.add_argument('--radius-nm', type=float, default=150.0)
    c.add_argument('--period', type=float, default=5.0)
    c.add_argument('--duration', type=float, default=3600.0)
    c.add_argument('--max-tracks', type=int, default=200)
    v = sub.add_parser('convert', help='raw responses -> capture.ndjson')
    v.add_argument('dir', type=Path)
    v.add_argument('--no-category', action='store_true', help='drop the emitter category, as the downlink does')
    v.add_argument('--keep-mlat', action='store_true', help='keep MLAT and TIS-B positions')
    v.add_argument('--output', type=Path)
    t = sub.add_parser('truth', help='ground_truth.json for avaliar_estimativas.py')
    t.add_argument('dir', type=Path)
    t.add_argument('--reference-db', type=Path, default=REFERENCE_DB)
    args = ap.parse_args(argv)

    if args.cmd == 'capture':
        if not 1.0 <= args.period <= 60.0 or not 1.0 <= args.radius_nm <= 250.0:
            ap.error('period 1..60 s, radius 1..250 NM')
        center = parse_center(args.center)
        meta = capture(args.out, center, args.radius_nm, args.period, args.duration, args.max_tracks)
        recs = convert(args.out / 'raw.jsonl.gz')
        write_ndjson(args.out / 'capture.ndjson', recs)
        print(json.dumps({'polls': meta['polls'], 'aircraft': meta['aircraft'], 'records': len(recs),
                          'tracks': meta['tracks'], 'dir': str(args.out)}))
    elif args.cmd == 'convert':
        recs = convert(args.dir / 'raw.jsonl.gz', no_category=args.no_category, keep_mlat=args.keep_mlat)
        out = args.output or args.dir / ('capture_sem_categoria.ndjson' if args.no_category else 'capture.ndjson')
        write_ndjson(out, recs)
        print(json.dumps({'records': len(recs), 'aircraft': len({r['icao'] for r in recs}), 'output': str(out)}))
    else:
        recs = convert(args.dir / 'raw.jsonl.gz')
        result = truth(recs, load_tracks(args.dir), load_airports(args.reference_db))
        out = args.dir / 'ground_truth.json'
        out.write_text(json.dumps({'aircraft': result}, indent=1, ensure_ascii=False))
        known = sum(1 for e in result.values() for k in ('origin', 'destination') if e[k])
        print(json.dumps({'flights': len(result), 'known_endpoints': known, 'output': str(out)}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
