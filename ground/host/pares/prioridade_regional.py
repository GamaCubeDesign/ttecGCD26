#!/usr/bin/env python3
"""Acrescenta prioridade regional transacionalmente, sem alterar rotas ou notas."""
import argparse
import hashlib
import json
import math
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'deps'))
import numpy as np
import shapely
from shapely.geometry import shape
from shapely.affinity import translate

RADIUS_KM = 6371.0088


def vectors(lat_lon):
    p = np.radians(np.asarray(lat_lon, dtype=float))
    return np.column_stack((np.cos(p[:,0])*np.cos(p[:,1]),
                            np.cos(p[:,0])*np.sin(p[:,1]),np.sin(p[:,0])))


class Region:
    def __init__(self, config_path):
        self.config_path = Path(config_path).resolve()
        self.config = json.loads(self.config_path.read_text())
        self.codes = set(self.config['country_codes'])
        self.step = float(self.config['max_segment_km'])
        if not 0 < self.step <= 100:
            raise ValueError('max_segment_km deve estar entre 0 e 100 km.')
        if self.config['score_bonus_enabled']:
            raise ValueError('Esta versão implementa somente ordem de análise, sem bônus.')
        if not self.config['priority']['endpoint'] > self.config['priority']['crossing_only'] > self.config['priority']['other']:
            raise ValueError('Prioridades devem ser endpoint > crossing_only > other.')
        path = self.config_path.parent/self.config['geometry_file']
        self.geometry_bytes = path.read_bytes()
        data = json.loads(self.geometry_bytes)
        selected = [f for f in data['features'] if f['properties']['iso_country'] in self.codes]
        if {f['properties']['iso_country'] for f in selected} != self.codes:
            raise ValueError('Geometria ausente para algum país configurado.')
        self.land = shapely.union_all([shape(f['geometry']) for f in selected])
        if not shapely.is_valid(self.land):
            raise ValueError('Polígonos regionais inválidos.')
        shapely.prepare(self.land)
        # Três cópias evitam uma linha falsa através do mapa no antimeridiano.
        self.land_wrapped = shapely.union_all([self.land,translate(self.land,xoff=-360),translate(self.land,xoff=360)])
        shapely.prepare(self.land_wrapped)
        # Pré-filtro conservador: só usado quando TODOS os limites cabem no cap.
        self.cap_center = vectors([[-10,-80]])[0]
        coords = shapely.get_coordinates(self.land)
        dots = vectors(coords[:,[1,0]]) @ self.cap_center
        self.use_cap = bool(np.all(dots >= math.cos(math.radians(60))))

    def crossings(self, origin, destinations):
        """Retorna 0/1; -1 significa arco antípoda não único."""
        a = vectors([origin])[0]
        b = vectors(destinations)
        dots = np.clip(b @ a, -1, 1)
        theta = np.arccos(dots)
        same = theta < 1e-10
        ambiguous = np.pi-theta < 1e-8
        w = b - dots[:,None]*a
        norm = np.linalg.norm(w,axis=1)
        w = np.divide(w,norm[:,None],out=np.zeros_like(w),where=norm[:,None]>1e-12)
        output = np.zeros(len(b),dtype=np.int8)
        output[ambiguous] = -1
        # Um extremo contido nos limites já garante interseção.
        end_inside = shapely.intersects(self.land,shapely.points(np.asarray(destinations)[:,[1,0]]))
        start_inside = bool(shapely.intersects(self.land,shapely.Point(origin[1],origin[0])))
        output[end_inside | start_inside] = 1
        output[ambiguous] = -1
        mask = ~(same | ambiguous | end_inside | start_inside)
        if self.use_cap:
            # Máximo exato do produto escalar ao longo do arco menor.
            ca = float(a @ self.cap_center)
            cw = w @ self.cap_center
            t = np.arctan2(cw,ca)
            maximum = np.maximum(ca,b @ self.cap_center)
            maximum = np.where((t>=0)&(t<=theta),np.maximum(maximum,np.hypot(ca,cw)),maximum)
            mask &= maximum >= math.cos(math.radians(65))
        indices = np.flatnonzero(mask)
        for offset in range(0,len(indices),128):
            ids = indices[offset:offset+128]
            # Mesmo número de pontos no lote; segmentos de no máximo step km.
            steps = max(1,math.ceil(float(np.max(theta[ids]))*RADIUS_KM/self.step))
            ts = theta[ids,None]*np.linspace(0,1,steps+1)[None,:]
            xyz = a[None,None,:]*np.cos(ts)[:,:,None]+w[ids,None,:]*np.sin(ts)[:,:,None]
            lon = np.degrees(np.unwrap(np.arctan2(xyz[:,:,1],xyz[:,:,0]),axis=1))
            lat = np.degrees(np.arctan2(xyz[:,:,2],np.hypot(xyz[:,:,0],xyz[:,:,1])))
            lines = shapely.linestrings(np.stack((lon,lat),axis=2))
            output[ids] = shapely.intersects(self.land_wrapped,lines).astype(np.int8)
        return output


def migrate(database, config):
    region = Region(config)
    db = sqlite3.connect(database)
    db.execute('PRAGMA foreign_keys=ON')
    try:
        if dict(db.execute('SELECT key,value FROM metadata')).get('generation_status') != 'complete':
            raise ValueError('A geração original não está completa.')
        airports = db.execute('SELECT id,latitude_deg,longitude_deg,iso_country FROM airports ORDER BY id').fetchall()
        expected = len(airports)*(len(airports)-1)//2
        # BEGIN + DDL individual: falha desfaz a migração inteira.
        db.execute('BEGIN IMMEDIATE')
        db.execute('CREATE TABLE IF NOT EXISTS regional_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)')
        db.execute('CREATE TABLE IF NOT EXISTS regional_airports(airport_id INTEGER PRIMARY KEY REFERENCES airports(id),in_region INTEGER NOT NULL CHECK(in_region IN (0,1)))')
        db.execute('''CREATE TABLE IF NOT EXISTS regional_pairs(
            airport_a_id INTEGER REFERENCES airports(id), airport_b_id INTEGER REFERENCES airports(id),
            arc_intersects_region INTEGER CHECK(arc_intersects_region IN (0,1)),
            analysis_priority INTEGER NOT NULL,
            PRIMARY KEY(airport_a_id,airport_b_id), CHECK(airport_a_id<airport_b_id)) WITHOUT ROWID''')
        db.execute('DELETE FROM regional_metadata')
        db.execute('DELETE FROM regional_pairs')
        db.execute('DELETE FROM regional_airports')
        db.executemany('INSERT INTO regional_airports VALUES (?,?)',[(r[0],int(r[3] in region.codes)) for r in airports])
        priority = region.config['priority']
        for i,a in enumerate(airports[:-1]):
            others = airports[i+1:]
            crossings = region.crossings(a[1:3],[r[1:3] for r in others])
            values=[]
            for b,cross in zip(others,crossings):
                endpoint = a[3] in region.codes or b[3] in region.codes
                level = priority['endpoint'] if endpoint else priority['crossing_only'] if cross==1 else priority['other']
                values.append((a[0],b[0],None if cross<0 else int(cross),level))
            db.executemany('INSERT INTO regional_pairs VALUES (?,?,?,?)',values)
            if (i+1)%100 == 0:
                print(f'{i+1}/{len(airports)-1} origens classificadas',flush=True)
        db.execute('CREATE INDEX IF NOT EXISTS regional_pairs_priority ON regional_pairs(analysis_priority DESC,airport_a_id,airport_b_id)')
        db.execute('DROP VIEW IF EXISTS route_pairs_regional')
        db.execute('''CREATE VIEW route_pairs_regional AS
            SELECT p.*,o.in_region AS origin_in_region,d.in_region AS destination_in_region,
                   r.arc_intersects_region,r.analysis_priority
            FROM route_pairs p
            JOIN regional_airports o ON o.airport_id=p.origin_id
            JOIN regional_airports d ON d.airport_id=p.destination_id
            JOIN regional_pairs r ON r.airport_a_id=min(p.origin_id,p.destination_id)
                                 AND r.airport_b_id=max(p.origin_id,p.destination_id)''')
        count = db.execute('SELECT count(*) FROM regional_pairs').fetchone()[0]
        if count != expected:
            raise ValueError(f'Contagem incorreta: {count} != {expected}')
        if db.execute('PRAGMA foreign_key_check').fetchone():
            raise ValueError('Falha de integridade referencial')
        metadata = {'status':'complete','schema_version':'1',
                    'generated_at_utc':datetime.now(timezone.utc).isoformat(),
                    'config_json':json.dumps(region.config,ensure_ascii=False,sort_keys=True),
                    'geometry_sha256':hashlib.sha256(region.geometry_bytes).hexdigest(),
                    'code_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    'shapely_version':shapely.__version__, 'numpy_version':np.__version__,
                    'unordered_pair_count':str(count),'bonus_enabled':'false'}
        db.executemany('INSERT INTO regional_metadata VALUES (?,?)',metadata.items())
        db.commit()
        print('Migração regional concluída.',flush=True)
    except BaseException:
        db.rollback()
        raise
    finally:
        db.close()


def iter_prioritized(database, batch_size=1000):
    """Lê todos os pares por prioridade, sem descartar os normais ou ordenar 10M linhas."""
    db = sqlite3.connect(Path(database).resolve().as_uri()+'?mode=ro',uri=True)
    try:
        if dict(db.execute('SELECT key,value FROM regional_metadata')).get('status') != 'complete':
            raise ValueError('Migração regional incompleta.')
        cursor = db.execute('''SELECT airport_a_id,airport_b_id,arc_intersects_region,analysis_priority
            FROM regional_pairs ORDER BY analysis_priority DESC,airport_a_id,airport_b_id''')
        while batch := cursor.fetchmany(batch_size):
            for a,b,cross,priority in batch:
                yield {'origin_id':a,'destination_id':b,'arc_intersects_region':cross,'analysis_priority':priority}
                yield {'origin_id':b,'destination_id':a,'arc_intersects_region':cross,'analysis_priority':priority}
    finally:
        db.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database',type=Path,default=ROOT/'pares_aeroportos.db')
    parser.add_argument('--config',type=Path,default=ROOT/'regiao_config.json')
    args = parser.parse_args()
    migrate(args.database,args.config)
