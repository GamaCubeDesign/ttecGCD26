#!/usr/bin/env python3
"""Audita todos os registros por cidade sem alterar seleção ou rotas existentes."""
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
import unicodedata
import numpy as np

ROOT=Path(__file__).resolve().parent
FIXED={'large_airport','medium_airport','small_airport'}
EARTH_KM=6371.0088
ROLES={
 'selected':'Já incluído nos pares',
 'scheduled_gap':'Revisar inclusão: serviço regular fora dos pares',
 'medium_reserve':'Reserva: aeroporto médio sem serviço regular confirmado',
 'small_local':'Alternativa local: pequeno, dependente da aeronave/trajetória',
 'closed':'Fechado no cadastro',
 'special_facility':'Instalação especializada; fora dos pares de aviões',
 'invalid_coordinates':'Coordenadas ausentes ou inválidas',
 'runway_conflict':'Revisar: todas as pistas cadastradas estão fechadas',
 'other':'Tipo não reconhecido; revisar'}


def normalize(value):
    text=unicodedata.normalize('NFKD',value or '').casefold()
    text=''.join(c for c in text if not unicodedata.combining(c))
    return re.sub(r'[^\w]+',' ',text,flags=re.UNICODE).strip()


def city_key(a):
    name=normalize(a.get('municipality'))
    # Sem município: nunca agrupar aeroportos distintos como uma única cidade.
    return json.dumps([a.get('iso_country') or '?',a.get('iso_region') or '?',name or '@airport:'+a['ident']],ensure_ascii=False,separators=(',',':'))


def valid_coords(a):
    lat,lon=a.get('latitude_deg'),a.get('longitude_deg')
    return lat is not None and lon is not None and math.isfinite(lat) and math.isfinite(lon) and -90<=lat<=90 and -180<=lon<=180


def role(a):
    if a['in_pairs']: return 'selected'
    if a['type']=='closed': return 'closed'
    if a['type'] not in FIXED: return 'special_facility' if a['type'] in {'heliport','seaplane_base','balloonport'} else 'other'
    if not a['coordinates_valid']: return 'invalid_coordinates'
    if a['all_recorded_runways_closed']: return 'runway_conflict'
    if a['scheduled_service']=='yes': return 'scheduled_gap'
    if a['type']=='medium_airport': return 'medium_reserve'
    if a['type']=='small_airport': return 'small_local'
    return 'other'


def distances(origin, destinations):
    a=np.radians(np.asarray(origin,dtype=float))
    b=np.radians(np.asarray(destinations,dtype=float))
    dlat=b[:,0]-a[0]; dlon=b[:,1]-a[1]
    h=np.sin(dlat/2)**2+np.cos(a[0])*np.cos(b[:,0])*np.sin(dlon/2)**2
    return 2*EARTH_KM*np.arcsin(np.sqrt(np.clip(h,0,1)))


def nearby_links(airports,radius=100):
    """Vizinhança de aeroportos selecionados; hipótese, nunca agrupamento municipal."""
    by_country=defaultdict(list)
    for a in airports:
        if a['type'] in FIXED and a['coordinates_valid']:
            by_country[a['iso_country']].append(a)
    for country in sorted(by_country):
        rows=by_country[country]
        anchors=[a for a in rows if a['in_pairs']]
        if not anchors: continue
        coords=[(a['latitude_deg'],a['longitude_deg']) for a in rows]
        for anchor in anchors:
            ds=distances((anchor['latitude_deg'],anchor['longitude_deg']),coords)
            for idx in np.flatnonzero(ds<=radius):
                candidate=rows[int(idx)]
                if candidate['id']!=anchor['id']:
                    yield (anchor['id'],candidate['id'],float(ds[idx]),int(anchor['city_key']==candidate['city_key']))


def audit(source,database,output_dir):
    source=Path(source).resolve();database=Path(database).resolve()
    regional=json.loads((ROOT/'regiao_config.json').read_text())
    latam=set(regional['country_codes'])
    group_config=json.loads((ROOT/'grupos_atendimento.json').read_text())
    with sqlite3.connect(source.as_uri()+'?mode=ro',uri=True) as src:
        src.row_factory=sqlite3.Row
        airports=[dict(r) for r in src.execute('''SELECT id,ident,name,type,municipality,iso_country,iso_region,
            latitude_deg,longitude_deg,scheduled_service FROM airports ORDER BY id''')]
        source_meta=[dict(r) for r in src.execute('SELECT * FROM dataset_sources')]
        runway_rows=defaultdict(list)
        for row in src.execute('SELECT airport_ref,length_ft,surface,closed FROM runways'):
            runway_rows[row['airport_ref']].append(dict(row))
    with sqlite3.connect(database.as_uri()+'?mode=ro',uri=True) as current:
        selected={r[0] for r in current.execute('SELECT id FROM airports')}
        generation=dict(current.execute('SELECT key,value FROM metadata'))
        pair_count=current.execute('SELECT count(*) FROM route_pairs').fetchone()[0]
    source_hash=hashlib.sha256(source.read_bytes()).hexdigest()
    if source_hash!=generation['source_sha256']:
        raise ValueError('A base de referência difere daquela usada na geração dos pares.')
    if not selected.issubset({a['id'] for a in airports}):
        raise ValueError('Aeroporto selecionado ausente da fonte.')
    cities=defaultdict(list)
    for a in airports:
        runways=runway_rows[a['id']]
        open_rows=[r for r in runways if r['closed']==0]
        lengths=[float(r['length_ft']) for r in open_rows if r['length_ft'] and r['length_ft']>0 and math.isfinite(r['length_ft'])]
        a.update(in_pairs=int(a['id'] in selected),in_latam=int(a['iso_country'] in latam),
                 coordinates_valid=int(valid_coords(a)),runway_count=len(runways),
                 open_runway_count=len(open_rows),
                 max_open_runway_ft=max(lengths,default=None),
                 runway_surfaces=sorted({r['surface'] for r in open_rows if r['surface']}),
                 all_recorded_runways_closed=int(bool(runways) and all(r['closed']==1 for r in runways)))
        a['city_key']=city_key(a)
        a['role']=role(a)
        flags=[]
        if not normalize(a['municipality']): flags.append('missing_municipality')
        if not a['coordinates_valid']: flags.append('invalid_coordinates')
        if a['type'] in FIXED and not runways: flags.append('missing_runway_records')
        if a['all_recorded_runways_closed']: flags.append('all_recorded_runways_closed')
        if a['type']=='closed' and a['scheduled_service']=='yes': flags.append('closed_with_scheduled_service')
        if a['type'] in FIXED and a['scheduled_service'] not in ('yes','no'): flags.append('unknown_scheduled_service')
        if '/' in (a['municipality'] or ''): flags.append('compound_municipality_label')
        a['quality_flags']=flags
        cities[a['city_key']].append(a)
    # Mesmo nome em estados diferentes é um aviso, não uma fusão automática.
    names=defaultdict(set)
    for key,rows in cities.items():
        if normalize(rows[0]['municipality']): names[(rows[0]['iso_country'],normalize(rows[0]['municipality']))].add(rows[0]['iso_region'])
    summaries=[]
    for key,rows in cities.items():
        first=rows[0]; counts=Counter(a['role'] for a in rows)
        fixed=[a for a in rows if a['type'] in FIXED]
        fixed_coords=[a for a in fixed if a['coordinates_valid']]
        extent=0.0
        if fixed_coords:
            ref=fixed_coords[0]
            extent=float(max(distances((ref['latitude_deg'],ref['longitude_deg']),[(a['latitude_deg'],a['longitude_deg']) for a in fixed_coords])))
        flags=[]
        known=bool(normalize(first['municipality']))
        if known and len(names[(first['iso_country'],normalize(first['municipality']))])>1: flags.append('same_name_other_admin_region')
        if extent>100: flags.append('geographically_spread_label')
        if not known: flags.append('missing_municipality')
        if counts['scheduled_gap']: flags.append('scheduled_airport_missing_from_pairs')
        if fixed and not counts['selected']: flags.append('no_airport_in_pairs')
        if any(a['quality_flags'] for a in rows): flags.append('airport_data_quality')
        urgent=3 if counts['scheduled_gap'] or counts['runway_conflict'] else 2 if counts['medium_reserve'] else 1 if counts['small_local'] else 0
        summaries.append({'city_key':key,'municipality':first['municipality'] or '(não informado)',
            'municipality_labels':sorted({a['municipality'] for a in rows if a['municipality']}),
            'country':first['iso_country'],'admin_region':first['iso_region'],'in_latam':first['in_latam'],
            'city_name_known':int(known),'facility_count':len(rows),'fixed_wing_count':len(fixed),
            'selected_count':counts['selected'],'scheduled_gap_count':counts['scheduled_gap'],
            'medium_reserve_count':counts['medium_reserve'],'small_local_count':counts['small_local'],
            'quality_flag_count':sum(bool(a['quality_flags']) for a in rows),
            'review_urgency':urgent,'extent_from_first_airport_km':round(extent,3),'flags':flags,
            'airport_idents':[a['ident'] for a in rows]})
    summaries.sort(key=lambda c:(-c['in_latam'],-c['review_urgency'],c['country'] or '',c['admin_region'] or '',normalize(c['municipality']),c['city_key']))
    airports.sort(key=lambda a:(-a['in_latam'],a['iso_country'] or '',a['city_key'],a['ident']))
    by_ident={a['ident']:a for a in airports}
    for group in group_config['groups']:
        for ident in group['airports']:
            if ident not in by_ident or by_ident[ident]['iso_country']!=group['country']:
                raise ValueError('Grupo de atendimento inconsistente: '+ident)
    print(f'{len(airports)} instalações e {len(summaries)} grupos cadastrais analisados.',flush=True)
    db=sqlite3.connect(database)
    db.execute('PRAGMA foreign_keys=ON')
    try:
        db.execute('BEGIN IMMEDIATE')
        for sql in [
            'CREATE TABLE IF NOT EXISTS city_audit_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)',
            '''CREATE TABLE IF NOT EXISTS city_audit_cities(city_key TEXT PRIMARY KEY,municipality TEXT,country TEXT,admin_region TEXT,
               in_latam INTEGER,city_name_known INTEGER,facility_count INTEGER,fixed_wing_count INTEGER,selected_count INTEGER,
               scheduled_gap_count INTEGER,medium_reserve_count INTEGER,small_local_count INTEGER,review_urgency INTEGER,details_json TEXT NOT NULL)''',
            '''CREATE TABLE IF NOT EXISTS city_audit_airports(airport_id INTEGER PRIMARY KEY,ident TEXT UNIQUE NOT NULL,name TEXT,
               city_key TEXT NOT NULL REFERENCES city_audit_cities(city_key),type TEXT,scheduled_service TEXT,
               in_latam INTEGER,in_pairs INTEGER,role TEXT,details_json TEXT NOT NULL)''',
            '''CREATE TABLE IF NOT EXISTS city_audit_nearby(anchor_id INTEGER REFERENCES city_audit_airports(airport_id),
               airport_id INTEGER REFERENCES city_audit_airports(airport_id),distance_km REAL,same_city_key INTEGER,
               PRIMARY KEY(anchor_id,airport_id)) WITHOUT ROWID''',
            '''CREATE TABLE IF NOT EXISTS city_audit_service_groups(group_id TEXT PRIMARY KEY,label TEXT,country TEXT,source_url TEXT,details_json TEXT NOT NULL)''',
            '''CREATE TABLE IF NOT EXISTS city_audit_service_members(group_id TEXT REFERENCES city_audit_service_groups(group_id),
               airport_id INTEGER REFERENCES city_audit_airports(airport_id),PRIMARY KEY(group_id,airport_id)) WITHOUT ROWID''']:
            db.execute(sql)
        for table in ['city_audit_nearby','city_audit_service_members','city_audit_service_groups','city_audit_airports','city_audit_cities','city_audit_metadata']:
            db.execute('DELETE FROM '+table)
        db.executemany('INSERT INTO city_audit_cities VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)',[
            (c['city_key'],c['municipality'],c['country'],c['admin_region'],c['in_latam'],c['city_name_known'],c['facility_count'],
             c['fixed_wing_count'],c['selected_count'],c['scheduled_gap_count'],c['medium_reserve_count'],c['small_local_count'],c['review_urgency'],json.dumps(c,ensure_ascii=False)) for c in summaries])
        db.executemany('INSERT INTO city_audit_airports VALUES (?,?,?,?,?,?,?,?,?,?)',[
            (a['id'],a['ident'],a['name'],a['city_key'],a['type'],a['scheduled_service'],a['in_latam'],a['in_pairs'],a['role'],json.dumps(a,ensure_ascii=False)) for a in airports])
        db.executemany('INSERT INTO city_audit_nearby VALUES (?,?,?,?)',nearby_links(airports))
        for g in group_config['groups']:
            db.execute('INSERT INTO city_audit_service_groups VALUES (?,?,?,?,?)',(g['id'],g['label'],g['country'],g['source_url'],json.dumps(g,ensure_ascii=False)))
            db.executemany('INSERT INTO city_audit_service_members VALUES (?,?)',[(g['id'],by_ident[ident]['id']) for ident in g['airports']])
        for sql in ['CREATE INDEX IF NOT EXISTS city_audit_review_order ON city_audit_cities(in_latam DESC,review_urgency DESC,country,municipality)',
                    'CREATE INDEX IF NOT EXISTS city_audit_airports_city ON city_audit_airports(city_key)',
                    'CREATE INDEX IF NOT EXISTS city_audit_airports_role ON city_audit_airports(in_latam DESC,role)',
                    'CREATE INDEX IF NOT EXISTS city_audit_nearby_airport ON city_audit_nearby(airport_id)']:
            db.execute(sql)
        metadata={'status':'complete','version':'1','generated_at_utc':datetime.now(timezone.utc).isoformat(),
                  'source_sha256':source_hash,'source_datasets':json.dumps(source_meta,ensure_ascii=False),
                  'source_path':str(source),'regional_config':json.dumps(regional,ensure_ascii=False),
                  'service_groups':json.dumps(group_config,ensure_ascii=False),'nearby_radius_km':'100',
                  'source_airport_count':str(len(airports)),'city_group_count':str(len(summaries)),
                  'original_pair_count':str(pair_count),'code_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  'scope':'full source database audit; external verification only for explicit service groups; no live operational certification'}
        db.executemany('INSERT INTO city_audit_metadata VALUES (?,?)',metadata.items())
        if db.execute('SELECT count(*) FROM city_audit_airports').fetchone()[0]!=len(airports): raise ValueError('Cobertura incompleta')
        if db.execute('PRAGMA foreign_key_check').fetchone(): raise ValueError('Falha de referência')
        db.commit()
    except BaseException:
        db.rollback(); raise
    finally:
        db.close()
    output_dir=Path(output_dir);output_dir.mkdir(parents=True,exist_ok=True)
    stats={}
    for label,filter_region in [('world',False),('latin_america',True)]:
        aa=[a for a in airports if not filter_region or a['in_latam']]
        cc=[c for c in summaries if not filter_region or c['in_latam']]
        stats[label]={'facilities':len(aa),'named_city_groups':sum(c['city_name_known'] for c in cc),
            'named_city_groups_with_fixed_wing_airports':sum(c['city_name_known'] and c['fixed_wing_count']>0 for c in cc),
            'unnamed_facilities':sum(not c['city_name_known'] for c in cc),
            'countries_with_records':len({a['iso_country'] for a in aa}),
            'roles':dict(Counter(a['role'] for a in aa)),
            'cities_with_multiple_fixed_wing_airports':sum(c['city_name_known'] and c['fixed_wing_count']>1 for c in cc),
            'cities_with_multiple_selected_airports':sum(c['city_name_known'] and c['selected_count']>1 for c in cc),
            'cities_with_scheduled_gaps':sum(c['city_name_known'] and c['scheduled_gap_count']>0 for c in cc),
            'cities_without_selected_airport':sum(c['city_name_known'] and c['fixed_wing_count']>0 and c['selected_count']==0 for c in cc)}
    country_stats=[]
    for country in sorted({a['iso_country'] for a in airports},key=lambda x:(x not in latam,x or '')):
        aa=[a for a in airports if a['iso_country']==country]
        country_stats.append({'country':country,'in_latam':int(country in latam),'facilities':len(aa),'roles':dict(Counter(a['role'] for a in aa))})
    (output_dir/'cidades_mundo.json').write_text(json.dumps({'metadata':metadata,'summary':stats,'countries':country_stats,'cities':summaries},ensure_ascii=False,indent=2)+'\n')
    (output_dir/'resumo.json').write_text(json.dumps(stats,ensure_ascii=False,indent=2)+'\n')
    # Relatório completo da região prioritária, sem truncar cidades ou aeroportos de asa fixa.
    lines=['# Auditoria de cidades — América Latina','',
        'Auditoria automática de todos os registros da fonte local, baixada em 22/09/2026. Não é uma confirmação operacional em tempo real.',
        'Cada seção conserva país, divisão administrativa e município cadastrado. Município desconhecido é separado por instalação. A ordem prioriza lacunas com serviço regular, depois aeroportos médios e pequenos.',
        'Instalações especializadas e fechadas entram nas contagens; a tabela detalha aeroportos grandes, médios e pequenos. Todos os registros individuais estão nas tabelas city_audit_* do banco.','',
        'A associação a uma cidade atendida não é inferida apenas por distância. Grupos com fontes e vizinhanças de até 100 km estão registrados separadamente.','']
    def esc(v): return str(v if v is not None else '—').replace('|','/').replace('\n',' ')
    for c in summaries:
        if not c['in_latam']: continue
        lines.extend([f"## {esc(c['municipality'])} — {c['admin_region']} / {c['country']}",'',
            f"Instalações: {c['facility_count']}; aeroportos de asa fixa: {c['fixed_wing_count']}; nos pares: {c['selected_count']}." ])
        if c['flags']: lines.append('Alertas: '+', '.join(c['flags'])+'.')
        fixed=[a for a in cities[c['city_key']] if a['type'] in FIXED]
        if fixed:
            lines.extend(['','| Código | Nome | Porte | Serviço regular cadastrado | Maior pista aberta cadastrada (m) | Resultado |','|---|---|---|---|---:|---|'])
            for a in sorted(fixed,key=lambda a:(not a['in_pairs'],a['ident'])):
                length=round(a['max_open_runway_ft']*.3048) if a['max_open_runway_ft'] else 'não informado'
                lines.append('| '+' | '.join(map(esc,[a['ident'],a['name'],a['type'],a['scheduled_service'],length,ROLES[a['role']]]))+' |')
        lines.append('')
    (output_dir/'cidades_america_latina.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps(stats,ensure_ascii=False),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT.parent/'banco-aeronaves'/'referencias.db')
    parser.add_argument('--database',type=Path,default=ROOT/'pares_aeroportos.db')
    parser.add_argument('--output-dir',type=Path,default=ROOT/'auditoria_cidades')
    args=parser.parse_args()
    audit(args.source,args.database,args.output_dir)
