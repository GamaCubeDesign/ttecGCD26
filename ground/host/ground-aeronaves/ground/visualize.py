"""Exporta trajetórias já processadas para um painel HTML offline."""
import argparse
import errno
import json
import sqlite3
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock
from dataclasses import replace
from urllib.parse import urlsplit, parse_qs

from .config import Config, add_pair_arguments, pair_options
from .geo import distance_km
from .live_input import LiveInput


def build_data(database, reference, session=None):
    aircraft = {}
    airport_ids = set()
    with sqlite3.connect(Path(database).resolve().as_uri()+'?mode=ro', uri=True) as db:
        if session is None:
            row = db.execute('SELECT session_id FROM estimates WHERE session_id IS NOT NULL ORDER BY id DESC LIMIT 1').fetchone()
            if not row:
                raise ValueError('Nenhuma sessão processada encontrada. Execute python -m ground primeiro.')
            session = row[0]
        # Transaction gives one consistent read while capture continues writing.
        pair_by_aircraft={}
        if db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='pair_results'").fetchone():
            for raw, in db.execute('SELECT result_json FROM pair_results WHERE session_id=? ORDER BY available_event_ns,id',(session,)):
                pair=json.loads(raw)
                pair_by_aircraft.setdefault(pair['icao'],[]).append(pair)
        pair_positions={}; pair_latest={}
        rows = db.execute('SELECT result_json FROM estimates WHERE session_id=? ORDER BY event_ns,id', (session,))
        for (raw,) in rows:
            r = json.loads(raw)
            a = aircraft.setdefault(r['icao'], {'points':{}, 'frames':[]})
            snapshot = r.get('snapshot') or {}
            for p in snapshot.get('trajectory', []):
                key = (p['event_ns'], p['lat'], p['lon'])
                a['points'].setdefault(key, {'t':p['event_ns']//1000000, 'available_t':r['event_ns']//1000000,
                                            'lat':p['lat'], 'lon':p['lon'], 'alt':p.get('altitude_ft')})
            decisions = {}
            for kind, d in r.get('airport_method', {}).items():
                candidates = r.get('origin_candidates' if kind=='origin' else 'destination_candidates', [])[:5]
                chosen = d.get('candidate')
                if chosen and not any(c['airport_id']==chosen['airport_id'] for c in candidates):
                    candidates = [chosen]+candidates
                airport_ids.update(c['airport_id'] for c in candidates)
                decisions[kind] = {'status':d['status'], 'reason':d.get('reason'),
                    'chosen':chosen['ident'] if chosen else None,
                    'candidates':[{'id':c['airport_id'], 'ident':c['ident'], 'score':c['score'],
                        'airport_type':c.get('airport_type'),
                        'type_factor':c.get('evidence',{}).get('aircraft_compatibility',{}).get('factor')}
                        for c in candidates]}
            altitude = snapshot.get('fields', {}).get('altitude_ft')
            a['frames'].append({'t':r['event_ns']//1000000, 'phase':r['flight_state'], 'decisions':decisions,
                'quality':r.get('data_quality', {}).get('reason'), 'position_available':bool(snapshot),
                'alt':altitude['value'] if altitude else None,
                'alt_source':altitude.get('source') if altitude else None})
            pairs=pair_by_aircraft.get(r['icao'],[])
            cursor=pair_positions.get(r['icao'],0)
            while cursor<len(pairs) and pairs[cursor]['available_event_ns']<=r['event_ns'] and pairs[cursor]['source_version']<=r.get('analysis_version',0):
                pair_latest[r['icao']]=pairs[cursor];cursor+=1
            pair_positions[r['icao']]=cursor
            pair=pair_latest.get(r['icao'])
            if pair and pair.get('episode')==r.get('tracking_episode') and r.get('data_quality',{}).get('usable') and r['flight_state']!='GROUND':
                a['frames'][-1]['pair_analysis']={**pair,'matches_current_version':pair['source_version']==r.get('analysis_version')}
            else:
                a['frames'][-1]['pair_analysis']=r.get('pair_analysis')
            a['frames'][-1]['combined_analysis'] = r.get('combined_analysis')
            a['frames'][-1]['category'] = r.get('aircraft_category')
            a['frames'][-1]['airport_filter'] = snapshot.get('airport_filters',{}).get('destination')
            a['frames'][-1]['on_ground']=snapshot.get('fields',{}).get('on_ground',{}).get('value')
    if not aircraft:
        raise ValueError('Sessão sem estimativas: '+session)
    for a in aircraft.values():
        a['points'] = sorted(a['points'].values(), key=lambda p:p['t'])
        previous = None
        for p in a['points']:
            dt = (p['t']-previous['t'])/1000 if previous else 0
            p['break'] = previous is None or dt<=0 or dt>60 or distance_km(previous['lat'],previous['lon'],p['lat'],p['lon'])>2+dt*.65
            previous = p
    with sqlite3.connect(Path(reference).resolve().as_uri()+'?mode=ro', uri=True) as db:
        airports = {str(i):{'ident':ident,'name':name,'lat':lat,'lon':lon}
                    for i,ident,name,lat,lon in db.execute('SELECT id,ident,name,latitude_deg,longitude_deg FROM airports') if i in airport_ids}
    return {'session':session,'aircraft':aircraft,'airports':airports}


def export(database, reference, output, session=None):
    output = Path(output)
    if output.resolve() in (Path(database).resolve(), Path(reference).resolve()):
        raise ValueError('O HTML de saída deve ser diferente dos bancos de dados.')
    try:
        data = build_data(database, reference, session)
    except ValueError:
        data = {'session':session or '', 'aircraft':{}, 'airports':{}}
    template = Path(__file__).with_name('trajectory_view.html').read_text()
    # JSON cannot terminate the script element, even with untrusted airport names.
    payload = json.dumps(data, ensure_ascii=False, allow_nan=False).replace('<','\\u003c').replace('\u2028','\\u2028').replace('\u2029','\\u2029')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(template.replace('__TRAJECTORY_DATA__', payload), encoding='utf-8')
    return data


def bind_server(handler, port):
    """Tenta a porta solicitada; se ocupada, pede uma porta livre ao sistema."""
    try:
        return ThreadingHTTPServer(('127.0.0.1', port), handler)
    except OSError as exc:
        if exc.errno != errno.EADDRINUSE:
            raise
        return ThreadingHTTPServer(('127.0.0.1', 0), handler)


class LiveData:
    """Cache de leitura; só reconstrói o painel quando há novas estimativas."""
    def __init__(self, database, reference, session=None):
        self.database, self.reference, self.session = database, reference, session
        self.lock = Lock()
        self.revision, self.payload = None, None

    def read(self):
        with self.lock:
            with sqlite3.connect(Path(self.database).resolve().as_uri()+'?mode=ro', uri=True) as db:
                row = db.execute('SELECT session_id,id FROM estimates '+
                    ('WHERE session_id=? ' if self.session else '')+'ORDER BY id DESC LIMIT 1',
                    (self.session,) if self.session else ()).fetchone()
            if not row:
                return '"waiting"',json.dumps({'session':self.session or '', 'aircraft':{},'airports':{}}).encode()
            session, last_id = row
            with sqlite3.connect(Path(self.database).resolve().as_uri()+'?mode=ro',uri=True) as db:
                table=db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='pair_results'").fetchone()
                pair_revision=db.execute('SELECT coalesce(max(id),0) FROM pair_results WHERE session_id=?',(session,)).fetchone()[0] if table else 0
            revision = f'"{session}:{last_id}:{pair_revision}"'
            if revision != self.revision:
                self.payload = json.dumps(build_data(self.database,self.reference,session),
                    ensure_ascii=False,allow_nan=False).encode()
                self.revision = revision
            return self.revision, self.payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--telemetry-db',type=Path,default=Config().telemetry_db)
    parser.add_argument('--reference-db',type=Path,default=Config().reference_db)
    parser.add_argument('--output',type=Path,default=Config().output.parent/'trajetorias.html')
    parser.add_argument('--session',help='Sessão específica; padrão: última sessão gravada no SQLite')
    parser.add_argument('--serve',action='store_true',help='Disponibiliza o painel local com mapa-base online')
    parser.add_argument('--port',type=int,default=8766)
    parser.add_argument('--input',type=Path,default=Path.home()/'adsb/eventos.ndjson',help='NDJSON acompanhado automaticamente com --serve')
    parser.add_argument('--no-ingest',action='store_true',help='Apenas visualiza um processamento externo')
    add_pair_arguments(parser)
    args = parser.parse_args()
    pair_config=pair_options(parser,args)
    state_output=args.telemetry_db.parent/'estado.json'
    paths=[state_output.resolve(),args.telemetry_db.resolve(),args.reference_db.resolve(),args.output.resolve(),args.pairs_db.resolve()]
    if len(set(paths))!=5:
        parser.error("Referências, pares, telemetria e HTML devem ser arquivos diferentes")
    if not 1<=args.port<=65535:
        parser.error('Porta deve estar entre 1 e 65535')
    worker=None
    if args.serve and not args.no_ingest and not args.session:
        if args.input.resolve() in (args.telemetry_db.resolve(),args.reference_db.resolve(),args.output.resolve(),args.pairs_db.resolve(),state_output.resolve()):
            parser.error('Entrada NDJSON deve ser diferente dos bancos e do HTML')
        worker=LiveInput(args.input,replace(Config(),telemetry_db=args.telemetry_db,reference_db=args.reference_db,output=state_output,**pair_config))
        worker.start()
    try:
        data=export(args.telemetry_db,args.reference_db,args.output,args.session)
    except (OSError,ValueError,sqlite3.Error) as exc:
        parser.exit(1, f'Não foi possível gerar a visualização: {exc}\n')
    print(f"Visualização: {args.output.resolve()}\nAeronaves: {len(data['aircraft'])} | sessão: {data['session']}")
    if args.serve:
        payload=args.output.read_bytes()
        live=LiveData(args.telemetry_db,args.reference_db,args.session or (worker.session if worker else None))
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                url=urlsplit(self.path)
                if url.path == '/sessions':
                    with sqlite3.connect(args.telemetry_db.resolve().as_uri()+'?mode=ro',uri=True) as db:
                        rows=db.execute('SELECT session_id,min(event_ns),max(event_ns),count(*) FROM estimates WHERE session_id IS NOT NULL GROUP BY session_id ORDER BY max(id) DESC').fetchall()
                    body=json.dumps([{'id':s,'start':a//1000000,'end':b//1000000,'count':n} for s,a,b,n in rows]).encode()
                    self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
                    return
                if url.path == '/data':
                    try:
                        session=parse_qs(url.query).get('session',[None])[0]
                        revision, body = (LiveData(args.telemetry_db,args.reference_db,session) if session else live).read()
                    except (OSError,ValueError,sqlite3.Error):
                        self.send_error(503,'Dados ainda indisponiveis')
                        return
                    unchanged=self.headers.get('If-None-Match')==revision
                    self.send_response(304 if unchanged else 200)
                    self.send_header('ETag',revision)
                    self.send_header('Cache-Control','no-cache')
                    self.send_header('Content-Type','application/json; charset=utf-8')
                    if not unchanged:
                        self.send_header('Content-Length',str(len(body)))
                    self.end_headers()
                    if not unchanged:
                        self.wfile.write(body)
                    return
                if self.path not in ('/','/trajetorias.html'):
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Type','text/html; charset=utf-8')
                self.send_header('Content-Length',str(len(payload)))
                self.send_header('Referrer-Policy','strict-origin-when-cross-origin')
                self.end_headers()
                self.wfile.write(payload)
        try:
            with bind_server(Handler,args.port) as server:
                actual_port = server.server_address[1]
                if actual_port != args.port:
                    print(f'Porta {args.port} ocupada; usando a porta disponível {actual_port}.',flush=True)
                print(f'Abra http://127.0.0.1:{actual_port} · Ctrl+C para encerrar',flush=True)
                server.serve_forever()
        except KeyboardInterrupt:
            pass
        except OSError as exc:
            parser.exit(1,f'Não foi possível iniciar o servidor local: {exc}\n')
        finally:
            if worker:
                worker.close()


if __name__=='__main__':
    main()
