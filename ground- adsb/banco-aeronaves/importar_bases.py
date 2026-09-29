"""Importa os CSVs preservados em fontes/; recusa sobrescrever tabelas existentes."""
import csv
import hashlib
import io
import json
import sqlite3
from pathlib import Path

BASE = Path(__file__).resolve().parent
INTEGER = {'id', 'airport_ref', 'lighted', 'closed'}
RELATIONS = {
    'countries': ['UNIQUE(code)'],
    'regions': ['UNIQUE(code)', 'FOREIGN KEY(iso_country) REFERENCES countries(code)'],
    'airports': ['UNIQUE(ident)', 'UNIQUE(id, ident)',
                 'FOREIGN KEY(iso_country) REFERENCES countries(code)',
                 'FOREIGN KEY(iso_region) REFERENCES regions(code)',
                 'CHECK(latitude_deg BETWEEN -90 AND 90)',
                 'CHECK(longitude_deg BETWEEN -180 AND 180)'],
    'runways': ['FOREIGN KEY(airport_ref, airport_ident) REFERENCES airports(id, ident)'],
}

def field_type(name):
    if name in INTEGER:
        return 'INTEGER'
    if name.endswith(('_deg', '_degT', '_ft')):
        return 'REAL'
    return 'TEXT'

def main():
    manifests = json.loads((BASE / 'fontes/manifesto.json').read_text())
    con = sqlite3.connect(BASE / 'referencias.db')
    con.execute('PRAGMA foreign_keys = ON')
    report = {}
    try:
        con.execute('BEGIN IMMEDIATE')
        con.execute('CREATE TABLE dataset_sources (dataset TEXT PRIMARY KEY, url TEXT, downloaded_at TEXT, last_modified TEXT, sha256 TEXT, row_count INTEGER)')
        for meta in manifests:
            name = meta['dataset']
            if name not in RELATIONS:
                raise ValueError('Dataset inesperado')
            data = (BASE / 'fontes' / (name + '.csv')).read_bytes()
            assert hashlib.sha256(data).hexdigest() == meta['sha256']
            reader = csv.DictReader(io.StringIO(data.decode('utf-8-sig')))
            columns = reader.fieldnames
            assert columns == meta['columns']
            definitions = [f'"{c}" {field_type(c)}' + (' PRIMARY KEY' if c == 'id' else '') for c in columns]
            con.execute(f'CREATE TABLE "{name}" (' + ','.join(definitions + RELATIONS[name]) + ')')
            rows = []
            for row in reader:
                values = []
                for c in columns:
                    value = row[c]
                    if value == '':
                        value = None
                    elif field_type(c) == 'INTEGER':
                        value = int(value)
                    elif field_type(c) == 'REAL':
                        value = float(value)
                    values.append(value)
                rows.append(values)
            con.executemany(f'INSERT INTO "{name}" VALUES (' + ','.join('?' for _ in columns) + ')', rows)
            count = con.execute(f'SELECT COUNT(*) FROM "{name}"').fetchone()[0]
            assert count == meta['rows']
            report[name] = count
            con.execute('INSERT INTO dataset_sources VALUES (?,?,?,?,?,?)',
                        (name, meta['url'], meta['downloaded_at'], meta['last_modified'], meta['sha256'], count))
        for table, column in [('airports','icao_code'), ('airports','iata_code'), ('airports','iso_country'), ('airports','iso_region'), ('airports','type'), ('runways','airport_ref'), ('regions','iso_country')]:
            con.execute(f'CREATE INDEX "idx_{table}_{column}" ON "{table}" ("{column}")')
        assert not con.execute('PRAGMA foreign_key_check').fetchall()
        assert con.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
        con.commit()
        report['integrity_check'] = 'ok'
        report['foreign_key_errors'] = 0
        report['airport_types'] = dict(con.execute('SELECT type, COUNT(*) FROM airports GROUP BY type'))
        report['airports_missing_elevation'] = con.execute('SELECT COUNT(*) FROM airports WHERE elevation_ft IS NULL').fetchone()[0]
        (BASE / 'relatorio-importacao.json').write_text(json.dumps(report, indent=2))
        print(json.dumps(report, indent=2))
    except BaseException:
        con.rollback()
        raise
    finally:
        con.close()

if __name__ == '__main__':
    main()
