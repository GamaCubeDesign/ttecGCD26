import json
import sqlite3
from contextlib import contextmanager
from uuid import uuid4

from .models import Observation


class Database:
    """Uma conexão por operação: uso seguro pelo worker, sem alterar referências."""
    def __init__(self, path):
        self.path = path
        self.session_id = uuid4().hex
        path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as con:
            con.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS aircraft (
                    icao TEXT PRIMARY KEY, first_seen_ns INTEGER NOT NULL,
                    last_seen_ns INTEGER NOT NULL, last_received_ns INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS observations (
                    id INTEGER PRIMARY KEY, icao TEXT NOT NULL REFERENCES aircraft(icao),
                    event_ns INTEGER NOT NULL, received_ns INTEGER NOT NULL,
                    timestamp_source TEXT NOT NULL, raw_ndjson TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_observations_time ON observations(icao, event_ns);
                CREATE TABLE IF NOT EXISTS rejected_messages (
                    id INTEGER PRIMARY KEY, received_ns INTEGER NOT NULL,
                    raw_ndjson TEXT NOT NULL, reason TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS estimates (
                    id INTEGER PRIMARY KEY, observation_id INTEGER NOT NULL REFERENCES observations(id),
                    icao TEXT NOT NULL, event_ns INTEGER NOT NULL, result_json TEXT NOT NULL
                );
            ''')
            for table in ('observations', 'estimates', 'rejected_messages'):
                columns = {r[1] for r in con.execute(f'PRAGMA table_info({table})')}
                if 'session_id' not in columns:
                    con.execute(f'ALTER TABLE {table} ADD COLUMN session_id TEXT')
            con.execute('CREATE INDEX IF NOT EXISTS idx_observations_session ON observations(session_id, icao, event_ns)')

    @contextmanager
    def connect(self):
        con = sqlite3.connect(self.path, timeout=30)
        con.execute('PRAGMA foreign_keys=ON')
        try:
            with con:
                yield con
        finally:
            con.close()

    def save(self, obs):
        with self.connect() as con:
            con.execute('''INSERT INTO aircraft VALUES (?,?,?,?) ON CONFLICT(icao) DO UPDATE SET
                first_seen_ns=min(first_seen_ns, excluded.first_seen_ns),
                last_seen_ns=max(last_seen_ns, excluded.last_seen_ns),
                last_received_ns=max(last_received_ns, excluded.last_received_ns)''',
                (obs.icao, obs.event_ns, obs.event_ns, obs.received_ns))
            return con.execute('INSERT INTO observations(icao,event_ns,received_ns,timestamp_source,raw_ndjson,session_id) VALUES (?,?,?,?,?,?)',
                (obs.icao, obs.event_ns, obs.received_ns, obs.timestamp_source, obs.raw, self.session_id)).lastrowid

    def recent(self, icao, window_s, limit):
        with self.connect() as con:
            rows = con.execute('''SELECT event_ns, received_ns, timestamp_source, raw_ndjson
                FROM observations WHERE session_id=? AND icao=? AND event_ns >=
                (SELECT max(event_ns) FROM observations WHERE session_id=? AND icao=?) - ?
                ORDER BY event_ns DESC, id DESC LIMIT ?''',
                (self.session_id, icao, self.session_id, icao, int(window_s * 1e9), limit)).fetchall()
        return [Observation(icao, t, received, source, json.loads(raw), raw)
                for t, received, source, raw in reversed(rows)]

    def reject(self, raw, received_ns, reason):
        with self.connect() as con:
            con.execute('INSERT INTO rejected_messages(received_ns,raw_ndjson,reason,session_id) VALUES (?,?,?,?)', (received_ns, raw, reason, self.session_id))

    def save_estimate(self, observation_id, result):
        with self.connect() as con:
            con.execute('INSERT INTO estimates(observation_id,icao,event_ns,result_json,session_id) VALUES (?,?,?,?,?)',
                (observation_id, result['icao'], result['event_ns'], json.dumps(result, allow_nan=False), self.session_id))
