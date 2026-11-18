#!/usr/bin/env python3
"""Gera um banco independente de rotas candidatas; fonte sempre somente leitura."""
import argparse
import hashlib
import json
import math
import sqlite3
from datetime import datetime, timezone
from pathlib import Path

RADIUS_KM = 6371.0088
SELECTION = "type = 'large_airport' OR (type = 'medium_airport' AND scheduled_service = 'yes')"


def geometry(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    a = math.sin((p2-p1)/2)**2 + math.cos(p1)*math.cos(p2)*math.sin(dl/2)**2
    angle = 2 * math.asin(math.sqrt(max(0.0, min(1.0, a))))
    y = math.sin(dl)*math.cos(p2)
    x = math.cos(p1)*math.sin(p2)-math.sin(p1)*math.cos(p2)*math.cos(dl)
    # Coincidentes e antípodas não têm rumo inicial único.
    bearing = None if abs(x)+abs(y) < 1e-12 else math.degrees(math.atan2(y,x)) % 360
    return RADIUS_KM*angle, bearing


def generate(source, output):
    source, output = source.resolve(), output.resolve()
    if output.exists():
        raise SystemExit(f'Destino já existe; use outro nome: {output}')
    with sqlite3.connect(source.as_uri()+'?mode=ro', uri=True) as src:
        rows = src.execute(f'''SELECT id,ident,name,type,municipality,iso_country,
            latitude_deg,longitude_deg,scheduled_service FROM airports
            WHERE ({SELECTION}) AND latitude_deg BETWEEN -90 AND 90
            AND longitude_deg BETWEEN -180 AND 180 ORDER BY id''').fetchall()
        provenance = src.execute('SELECT * FROM dataset_sources').fetchall()
    if len(rows) < 2:
        raise SystemExit('Menos de dois aeroportos válidos.')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Reserva exclusiva: nunca sobrescreve outro banco.
    output.touch(exist_ok=False)
    with sqlite3.connect(output) as db:
        db.execute('PRAGMA foreign_keys=ON')
        db.executescript('''
          CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
          CREATE TABLE airports(
            id INTEGER PRIMARY KEY, ident TEXT NOT NULL UNIQUE, name TEXT,
            type TEXT, municipality TEXT, iso_country TEXT,
            latitude_deg REAL NOT NULL CHECK(latitude_deg BETWEEN -90 AND 90),
            longitude_deg REAL NOT NULL CHECK(longitude_deg BETWEEN -180 AND 180),
            scheduled_service TEXT);
          CREATE TABLE route_pairs(
            origin_id INTEGER NOT NULL REFERENCES airports(id),
            destination_id INTEGER NOT NULL REFERENCES airports(id),
            distance_km REAL NOT NULL CHECK(distance_km>=0),
            initial_bearing_deg REAL CHECK(initial_bearing_deg>=0 AND initial_bearing_deg<360),
            PRIMARY KEY(origin_id,destination_id),
            CHECK(origin_id<>destination_id)) WITHOUT ROWID;
        ''')
        db.executemany('INSERT INTO airports VALUES (?,?,?,?,?,?,?,?,?)', rows)
        metadata = {
            'schema_version':'1', 'generation_status':'building',
            'generated_at_utc':datetime.now(timezone.utc).isoformat(),
            'source_path':str(source),
            'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
            'source_datasets':json.dumps(provenance,ensure_ascii=False),
            'selection_sql':SELECTION, 'scope':'worldwide',
            'geometry':'shortest great-circle arc on sphere',
            'earth_radius_km':str(RADIUS_KM),
            'airport_count':str(len(rows)),
            'expected_pair_count':str(len(rows)*(len(rows)-1)),
            'meaning':'geometric candidates; not confirmed airline routes',
        }
        db.executemany('INSERT INTO metadata VALUES (?,?)', metadata.items())
        db.commit()
        for n, origin in enumerate(rows,1):
            pairs = []
            for dest in rows:
                if origin[0] == dest[0]:
                    continue
                distance, bearing = geometry(origin[6],origin[7],dest[6],dest[7])
                pairs.append((origin[0],dest[0],distance,bearing))
            db.executemany('INSERT INTO route_pairs VALUES (?,?,?,?)',pairs)
            if n % 500 == 0:
                db.commit()
                print(f'{n}/{len(rows)} origens processadas',flush=True)
        db.commit()
        db.execute('CREATE INDEX route_pairs_destination ON route_pairs(destination_id)')
        count = db.execute('SELECT count(*) FROM route_pairs').fetchone()[0]
        assert count == len(rows)*(len(rows)-1)
        assert db.execute('PRAGMA foreign_key_check').fetchone() is None
        assert db.execute('PRAGMA integrity_check').fetchone() == ('ok',)
        db.execute("UPDATE metadata SET value='complete' WHERE key='generation_status'")
        db.execute('INSERT INTO metadata VALUES (?,?)',('pair_count',str(count)))
        db.commit()
        print(json.dumps({'airports':len(rows),'pairs':count,'database':str(output)},ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source,args.output)
