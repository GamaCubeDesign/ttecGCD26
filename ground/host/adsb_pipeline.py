"""One serial owner, an append-only reception journal and an estimator worker."""
import json
import sqlite3
import threading
import time
import tempfile
from contextlib import closing
from dataclasses import replace
from pathlib import Path
from urllib.parse import urlsplit

from tracks_to_ndjson import records, SAME_UPDATE_NS
from ground.database import Database
from ground.airport_estimator import AirportEstimator
from ground.aircraft_manager import AircraftManager
from ground.parser import parse_line
from ground.visualize import LiveData


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
        self.config = replace(config, telemetry_db=self.directory/'telemetria.db', output=self.directory/'estado.json')
        self.database = Database(self.config.telemetry_db)
        self.manager = AircraftManager(self.database, estimator, self.config)
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
        template = Path(__file__).parent/'ground-aeronaves/ground/trajectory_view.html'
        html = template.read_text().replace('__TRAJECTORY_DATA__', json.dumps(dict(session=self.database.session_id,aircraft={},airports={})))
        return html.replace("'/data", "'/adsb/data").replace("'/sessions'", "'/adsb/sessions'"), 'text/html; charset=utf-8'

    def close(self):
        self.stop.set()
        self.thread.join()
        self.journal.close()
