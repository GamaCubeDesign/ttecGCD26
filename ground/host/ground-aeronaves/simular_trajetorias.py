#!/usr/bin/env python3
"""Simulação local de mensagens ADS-B decodificadas; não transmite sinais SDR."""
import argparse
import json
import math
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path

from ground.config import Config
from ground.geo import EARTH_KM, bearing_deg, distance_km


def advance(lat, lon, heading, distance):
    lat, lon, heading = map(math.radians, (lat, lon, heading))
    angle = distance / EARTH_KM
    out_lat = math.asin(math.sin(lat)*math.cos(angle)+math.cos(lat)*math.sin(angle)*math.cos(heading))
    out_lon = lon+math.atan2(math.sin(heading)*math.sin(angle)*math.cos(lat),math.cos(angle)-math.sin(lat)*math.sin(out_lat))
    return math.degrees(out_lat), (math.degrees(out_lon)+180)%360-180


@dataclass(frozen=True)
class Flight:
    icao: str
    callsign: str
    mode: str
    start: float
    duration: float
    lat: float
    lon: float
    elevation: float
    heading: float
    distance: float

    def position(self, elapsed):
        u = max(0,min(1,elapsed/self.duration))
        if self.mode=='arrival':
            remaining = 1-(1.4*u-.4*u*u)
            return advance(self.lat,self.lon,self.heading,self.distance*remaining)
        progress = .6*u+.4*u*u if self.mode=='departure' else u
        return advance(self.lat,self.lon,self.heading,self.distance*progress)

    def message(self, seconds, epoch_ns):
        elapsed=seconds-self.start
        if not 0<=elapsed<=self.duration:
            return None
        u=elapsed/self.duration
        lat,lon=self.position(elapsed)
        before=max(0,elapsed-.5);after=min(self.duration,elapsed+.5)
        p,q=self.position(before),self.position(after)
        speed=distance_km(*p,*q)/max(.001,after-before)*3600/1.852
        if self.mode=='arrival':
            altitude=self.elevation+11000*(1-u)
            vertical=-11000/self.duration*60
            ground=elapsed==self.duration
        elif self.mode=='departure':
            altitude=self.elevation+15000*u
            vertical=15000/self.duration*60
            ground=elapsed==0
        else:
            altitude=35000
            vertical=0
            ground=False
        return {'icao':self.icao,'callsign':self.callsign,'simulation':True,
            'rx_epoch_ns':epoch_ns+round(seconds*1e9),'lat':round(lat,7),'lon':round(lon,7),
            'altitude_ft':round(altitude),'ground_speed_kt':0 if ground else round(speed,2),
            'track_deg':round(bearing_deg(*p,*q),2)%360,
            'vertical_rate_fpm':0 if ground else round(vertical),'on_ground':int(ground)}


def scenarios(reference):
    with sqlite3.connect(reference.resolve().as_uri()+'?mode=ro',uri=True) as db:
        row=db.execute('SELECT latitude_deg,longitude_deg,elevation_ft FROM airports WHERE ident=?',('SBBR',)).fetchone()
    if row is None or any(x is None for x in row):
        raise ValueError('SBBR precisa ter coordenadas e elevação no cadastro local.')
    lat,lon,elevation=row
    return [
        Flight('F00001','SIM001','arrival',0,480,lat,lon,elevation,110,60),
        Flight('F00002','SIM002','departure',0,480,lat,lon,elevation,290,55),
        Flight('F00003','SIM003','cruise',90,510,lat+.35,lon-.55,elevation,85,115),
        Flight('F00004','SIM004','arrival',180,420,lat,lon,elevation,290,55),
    ]


def generate(output, flights, duration=600, interval=1, speed=1):
    """Uma escrita por instante comum, flush imediato e timestamps de evento."""
    output.parent.mkdir(parents=True,exist_ok=True)
    # Append only; preserve earlier captures, including an unterminated final line.
    needs_newline=False
    if output.exists() and output.stat().st_size:
        with output.open('rb') as existing:
            existing.seek(-1,2)
            needs_newline=existing.read(1)!=b'\n'
    epoch=time.time_ns();started=time.monotonic();count=0
    with output.open('a',encoding='utf-8',buffering=1) as stream:
        if needs_newline:
            stream.write('\n')
        tick=0
        while tick*interval<=duration:
            seconds=tick*interval
            remaining=started+seconds/speed-time.monotonic()
            if remaining>0:
                time.sleep(remaining)
            messages=[m for flight in flights if (m:=flight.message(seconds,epoch)) is not None]
            stream.writelines(json.dumps(m,allow_nan=False)+'\n' for m in messages)
            stream.flush()
            count+=len(messages)
            if tick==0 or int(seconds//10)!=int((seconds-interval)//10):
                print(f'{seconds:6.1f} s simulados | {len(messages)} aeronaves emitindo | {count} mensagens',flush=True)
            tick+=1
    return count


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=Path.home()/'adsb/eventos.ndjson',help='Arquivo NDJSON; sempre acrescenta, nunca apaga')
    parser.add_argument('--duration',type=float,default=600,help='Duração em segundos simulados (padrão: 600)')
    parser.add_argument('--interval',type=float,default=1,help='Intervalo entre amostras em segundos simulados')
    parser.add_argument('--speed',type=float,default=1,help='Velocidade da simulação: 1=tempo real, 10=dez vezes mais rápido')
    parser.add_argument('--reference-db',type=Path,default=Config().reference_db)
    args=parser.parse_args()
    if not 1<=args.duration<=600 or not .1<=args.interval<=10 or not .1<=args.speed<=100:
        parser.error('Use duração de 1 a 600 s, intervalo de 0,1 a 10 s e velocidade de 0,1 a 100.')
    if args.output.resolve()==args.reference_db.resolve():
        parser.error('A saída deve ser diferente do banco de referência.')
    try:
        flights=scenarios(args.reference_db)
        print(f'SIMULAÇÃO — acrescentando dados em {args.output.resolve()}',flush=True)
        print('F00001: chegada; F00002: saída; F00003: cruzeiro aos 90 s; F00004: chegada aos 180 s.',flush=True)
        print('Trechos sintéticos na região de Brasília. Ctrl+C interrompe e preserva as linhas escritas.',flush=True)
        count=generate(args.output,flights,args.duration,args.interval,args.speed)
        print(f'Concluído: {count} mensagens.',flush=True)
    except KeyboardInterrupt:
        print('\nSimulação interrompida; dados escritos foram preservados.')
    except (OSError,ValueError,sqlite3.Error) as exc:
        parser.exit(1,f'Não foi possível simular: {exc}\n')


if __name__=='__main__':
    main()
