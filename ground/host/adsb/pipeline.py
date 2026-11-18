"""One serial owner, an append-only reception journal and an estimator worker."""
import json
import math
import sqlite3
import threading
import time
import tempfile
from contextlib import closing
from dataclasses import replace
from pathlib import Path
from urllib.parse import urlsplit

from adsb.tracks_to_ndjson import records, SAME_UPDATE_NS
from adsb.estimator.database import Database
from adsb.estimator.airport_estimator import AirportEstimator
from adsb.estimator.aircraft_manager import AircraftManager
from adsb.estimator.parser import parse_line
from adsb.estimator.visualize import LiveData


SUMMARY_MIN_S = 1.0          # the page's aircraft view, rebuilt at most this often
TRACK_POINTS_MAX = 120       # per aircraft in that view: 15 min at ~7.5 s


def _decision(result, kind):
    """What the page shows for an origin or a destination: a decision the
    estimator already closed (takeoff or landing seen) first, then the
    current one, with its first candidates."""
    hist = (result.get('airport_summary') or {}).get('takeoff' if kind == 'origin' else 'landing')
    current = (result.get('airport_method') or {}).get(kind) or {}
    out = {'status': current.get('status') or 'UNKNOWN', 'ident': None,
           'reason': current.get('reason'), 'source': 'local',
           'candidates': [{'ident': c.get('ident'), 'score': c.get('score')}
                          for c in (result.get(kind + '_candidates') or [])[:3]]}
    if hist and (hist.get('airport') or {}).get('ident'):
        out.update(ident=hist['airport']['ident'], status=hist.get('status') or 'HISTORICAL',
                   source='history')
    elif current.get('candidate'):
        out['ident'] = current['candidate'].get('ident')
    return out


def summarize(states, airports):
    """The aircraft view of the page, from the estimator's latest result per
    aircraft: identity, last state, a thinned track, origin and destination
    by each method, and the airports those name (for the map)."""
    out, refs = [], set()
    for icao, r in sorted(states.items()):
        snap = r.get('snapshot') or {}
        fields = snap.get('fields') or {}

        def val(k):
            return (fields.get(k) or {}).get('value')
        traj = snap.get('trajectory') or []
        step = max(1, math.ceil(len(traj) / TRACK_POINTS_MAX))
        pts = traj[::step]
        if traj and pts[-1] is not traj[-1]:
            pts.append(traj[-1])
        comb = r.get('combined_analysis') or {}
        pair = r.get('pair_analysis') or {}
        category = r.get('aircraft_category') or {}
        entry = {
            'icao': icao, 'callsign': val('callsign'), 'category': category.get('code'),
            'category_source': category.get('source'),
            'category_label': category.get('label'), 'phase': r.get('flight_state'),
            't': (r.get('event_ns') or 0) // 1_000_000, 'position_age_s': r.get('last_position_age_s'),
            'lat': snap.get('lat'), 'lon': snap.get('lon'), 'alt': val('altitude_ft'),
            'gs': val('ground_speed_kt'), 'trk': val('track_deg'), 'vr': val('vertical_rate_fpm'),
            'on_ground': val('on_ground'), 'quality': (r.get('data_quality') or {}).get('reason'),
            'track': [[p['event_ns'] // 1_000_000, round(p['lat'], 5), round(p['lon'], 5),
                       p.get('altitude_ft')] for p in pts],
            'origin': _decision(r, 'origin'), 'destination': _decision(r, 'destination'),
            'combined': {k: {'ident': (comb.get(k) or {}).get('ident'),
                             'status': (comb.get(k) or {}).get('status')}
                         for k in ('origin', 'destination')} if comb else None,
            'pairs': {'status': pair.get('status'), 'reason': pair.get('reason'),
                      'top': [{'origin': (c.get('origin') or {}).get('ident'),
                               'destination': (c.get('destination') or {}).get('ident'),
                               'score': c.get('score')} for c in (pair.get('candidates') or [])[:3]]}
            if pair else None,
        }
        out.append(entry)
        for kind in ('origin', 'destination'):
            refs.add(entry[kind]['ident'])
            refs.update(c['ident'] for c in entry[kind]['candidates'])
            if entry['combined']:
                refs.add(entry['combined'][kind]['ident'])
        for c in (entry['pairs'] or {}).get('top', []):
            refs.update((c['origin'], c['destination']))
    return {'aircraft': out,
            'airports': {i: airports[i] for i in sorted(x for x in refs if x) if i in airports}}


class AdsbPipeline:
    def __init__(self, directory, config):
        # Fail before accepting mission commands if either input database is absent.
        estimator = AirportEstimator(config)
        if config.pairs_enabled:
            import numpy  # noqa: F401 -- required by the pair subprocess
            with closing(sqlite3.connect(config.pairs_db.resolve().as_uri()+'?mode=ro', uri=True)) as db:
                meta = dict(db.execute('SELECT key,value FROM metadata'))
                region = dict(db.execute('SELECT key,value FROM regional_metadata'))
                if meta.get('generation_status') != 'complete' or region.get('status') != 'complete' or region.get('bonus_enabled') != 'false':
                    raise ValueError('Pair database is incomplete or incompatible')
        directory.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix='mission-', dir=directory))
        # estado.json is for the evaluation after the mission (the page reads
        # /api/adsb): every 10 s unless the configuration says otherwise, and
        # once more at close. At 1 s it was still 345 MB written in a
        # 20-aircraft mission (17/11).
        self.config = replace(config, telemetry_db=self.directory/'telemetria.db', output=self.directory/'estado.json',
                              publish_interval_s=config.publish_interval_s or 10.0)
        self.database = Database(self.config.telemetry_db)
        self.manager = AircraftManager(self.database, estimator, self.config)
        # ident -> [lat, lon, name, type], for the map and the aircraft view.
        self.airports = {a['ident']: [a['latitude_deg'], a['longitude_deg'], a['name'], a['type']]
                         for a in estimator.airports}
        self._airports_json = None
        self.summary_lock = threading.Lock()
        self.summary_revision = 0
        self.summary_body = b'{"revision":0,"aircraft":[],"airports":{}}'
        self.summary_at = 0.0
        self.summary_seen = -1
        self.live = LiveData(self.config.telemetry_db, config.reference_db, self.database.session_id)
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.stats = dict(processed=0, rejected=0, repeats=0, received=0, error=None,
                          directory=str(self.directory), session=self.database.session_id)
        self.journal = (self.directory/'ground.jsonl').open('a', encoding='utf-8', buffering=1)
        self.thread = threading.Thread(target=self._run, name='adsb-estimator', daemon=True)
        self.thread.start()

    def snapshot(self):
        with self.lock:
            return dict(self.stats)

    def summary(self):
        """(revision, JSON bytes) of the aircraft view; any thread."""
        with self.summary_lock:
            return self.summary_revision, self.summary_body

    def airports_json(self):
        """The large and medium airports, for the map's background."""
        if self._airports_json is None:
            rows = [[i, a[0], a[1], a[2], a[3]] for i, a in self.airports.items()
                    if a[3] in ('large_airport', 'medium_airport')]
            self._airports_json = json.dumps(rows, ensure_ascii=False, separators=(',', ':')).encode()
        return self._airports_json

    def _summarize(self, force=False):
        """Rebuilds the aircraft view in the estimator's own thread, which is
        the only one that changes its results, at most every SUMMARY_MIN_S:
        when an observation was processed, or always with the pair method
        on, whose results arrive by themselves."""
        now = time.monotonic()
        with self.lock:
            processed = self.stats['processed']
        changed = processed != self.summary_seen or self.config.pairs_enabled
        if not force and (now - self.summary_at < SUMMARY_MIN_S or not changed):
            return
        view = summarize(self.manager.states, self.airports)
        with self.summary_lock:
            self.summary_revision += 1
            view['revision'] = self.summary_revision
            self.summary_body = json.dumps(view, ensure_ascii=False, allow_nan=False,
                                           separators=(',', ':')).encode()
        self.summary_at = now
        self.summary_seen = processed

    def feed(self, event):
        with self.lock:
            try:
                self.journal.write(json.dumps(event, allow_nan=False)+'\n')
                self.stats['received'] += 1
            except (OSError, ValueError) as exc:
                self.stats['rejected'] += 1
                self.stats['error'] = str(exc)

    def _count(self, key):
        with self.lock:
            self.stats[key] += 1

    def _run(self):
        last = {}
        roster = {}
        try:
            with (self.directory/'ground.jsonl').open(encoding='utf-8') as stream, (self.directory/'tracks.ndjson').open('w', encoding='utf-8', buffering=1) as observations:
                while True:
                    offset = stream.tell()
                    raw = stream.readline()
                    if not raw.endswith('\n'):
                        stream.seek(offset)
                        self.manager.poll_pairs()
                        self.manager.flush()
                        self._summarize()
                        if self.stop.is_set():
                            break
                        self.stop.wait(.1)
                        continue
                    event = json.loads(raw)
                    if event.get('ev') == 'roster':
                        for aircraft in event.get('aircraft', []):
                            if isinstance(aircraft, dict) and aircraft.get('icao'):
                                roster[str(aircraft['icao']).upper()] = (aircraft.get('callsign'), event.get('wall_ms', 0))
                    if event.get('ev') != 'tracks':
                        continue
                    # HLR-ADS-08: reject records independently and count every rejection.
                    incoming = event.get('records')
                    if not isinstance(incoming, list):
                        self.database.reject(raw, time.time_ns(), 'Invalid track list')
                        self._count('rejected')
                        continue
                    for record in incoming:
                        try:
                            converted = records(dict(event, records=[record]))
                            if not converted:
                                raise ValueError('Track has no valid fields')
                            item = converted[0]
                            identity = roster.get(item['icao'])
                            if identity and 0 <= event['wall_ms']-identity[1] <= 300000:
                                item['callsign'] = identity[0]
                            text = json.dumps(item, allow_nan=False)
                            obs = parse_line(text, time.time_ns())
                            key = tuple(item.get(k) for k in ('lat','lon','altitude_ft','ground_speed_kt','track_deg','vertical_rate_fpm','on_ground','callsign'))
                            previous = last.get(item['icao'])
                            if previous and key == previous[1] and abs(item['rx_epoch_ns']-previous[0]) < SAME_UPDATE_NS:
                                self._count('repeats')
                                continue
                        except (AttributeError, KeyError, TypeError, ValueError, OverflowError) as exc:
                            self.database.reject(json.dumps(record), time.time_ns(), str(exc))
                            self._count('rejected')
                            continue
                        observations.write(text+'\n')
                        self.manager.process(obs)
                        last[item['icao']] = (item['rx_epoch_ns'], key)
                        self._count('processed')
                    self.manager.poll_pairs()
                    self._summarize()
        except Exception as exc:
            with self.lock:
                self.stats['error'] = str(exc)
        finally:
            self.manager.close()

    def page(self, url):
        path = urlsplit(url).path
        if path == '/adsb/data':
            return self.live.read()[1], 'application/json'
        if path == '/adsb/sessions':
            with closing(sqlite3.connect(self.config.telemetry_db)) as db:
                rows = db.execute('SELECT session_id,min(event_ns),max(event_ns),count(*) FROM estimates GROUP BY session_id').fetchall()
            return json.dumps([dict(id=s,start=a//1000000,end=b//1000000,count=n) for s,a,b,n in rows]), 'application/json'
        template = Path(__file__).parent/'estimator/trajectory_view.html'
        html = template.read_text().replace('__TRAJECTORY_DATA__', json.dumps(dict(session=self.database.session_id,aircraft={},airports={})))
        return html.replace("'/data", "'/adsb/data").replace("'/sessions'", "'/adsb/sessions'"), 'text/html; charset=utf-8'

    def close(self):
        self.stop.set()
        self.thread.join()
        self.journal.close()
