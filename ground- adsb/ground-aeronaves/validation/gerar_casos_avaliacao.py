#!/usr/bin/env python3
import argparse, json, math, sqlite3
from pathlib import Path
EARTH_KM=6371.0088
FALLBACK={
'SBBR':(-15.869167,-47.920834,3497.0),
'SBGO':(-16.632033,-49.220686,2450.0),
'SBKP':(-23.007500,-47.134444,2170.0),
'SBGR':(-23.435556,-46.473056,2459.0),
}
BASE_EPOCH_NS=1790460000000000000

def clamp(x,a=0,b=1): return max(a,min(b,x))
def distance_km(lat1,lon1,lat2,lon2):
 p1,p2=map(math.radians,(lat1,lat2)); dp=p2-p1; dl=math.radians(lon2-lon1)
 a=math.sin(dp/2)**2+math.cos(p1)*math.cos(p2)*math.sin(dl/2)**2
 return 2*EARTH_KM*math.asin(math.sqrt(min(1,max(0,a))))
def bearing_deg(lat1,lon1,lat2,lon2):
 p1,p2=map(math.radians,(lat1,lat2)); dl=math.radians(lon2-lon1)
 y=math.sin(dl)*math.cos(p2); x=math.cos(p1)*math.sin(p2)-math.sin(p1)*math.cos(p2)*math.cos(dl)
 return math.degrees(math.atan2(y,x))%360
def advance(lat,lon,heading_deg,distance):
 p,l,b=map(math.radians,(lat,lon,heading_deg)); d=distance/EARTH_KM
 p2=math.asin(math.sin(p)*math.cos(d)+math.cos(p)*math.sin(d)*math.cos(b))
 l2=l+math.atan2(math.sin(b)*math.sin(d)*math.cos(p),math.cos(d)-math.sin(p)*math.sin(p2))
 return math.degrees(p2),(math.degrees(l2)+180)%360-180
def smooth(u): u=clamp(u); return u*u*(3-2*u)
def point_between(a,b,progress):
 total=distance_km(a[0],a[1],b[0],b[1]); head=bearing_deg(a[0],a[1],b[0],b[1])
 return advance(a[0],a[1],head,total*clamp(progress))
def load_airports(dbpath):
 out=dict(FALLBACK)
 if dbpath and dbpath.is_file():
  wanted=tuple(FALLBACK); marks=','.join('?' for _ in wanted)
  with sqlite3.connect(dbpath.resolve().as_uri()+'?mode=ro',uri=True) as db:
   for ident,lat,lon,elev in db.execute(f'SELECT ident,latitude_deg,longitude_deg,elevation_ft FROM airports WHERE ident IN ({marks})',wanted):
    if None not in (lat,lon,elev): out[ident]=(float(lat),float(lon),float(elev))
 return out

def full(icao,callsign,orig,dest,ap,start,duration):
 a,b=ap[orig],ap[dest]; total=distance_km(a[0],a[1],b[0],b[1]); cruise=max(a[2],b[2])+10000
 def state(t):
  e=t-start
  if not 0<=e<=duration:return None
  u=e/duration; prog=smooth(u); lat,lon=point_between(a,b,prog)
  ce=.30*duration; ds=.60*duration
  if e<=ce:
   q=e/ce; alt=a[2]+(cruise-a[2])*q; vr=(cruise-a[2])/ce*60
  elif e<ds: alt=cruise; vr=0
  else:
   q=(e-ds)/(duration-ds); alt=cruise+(b[2]-cruise)*q; vr=(b[2]-cruise)/(duration-ds)*60
  eps=.5; p0=smooth(clamp((e-eps)/duration)); p1=smooth(clamp((e+eps)/duration))
  speed=total*(p1-p0)/max(.001,2*eps)*3600/1.852
  before=point_between(a,b,p0); after=point_between(a,b,p1)
  track=bearing_deg(before[0],before[1],after[0],after[1]) if before!=after else bearing_deg(a[0],a[1],b[0],b[1])
  ground=int(e==0 or e==duration)
  if ground:speed=0;vr=0
  return lat,lon,alt,speed,track,vr,ground
 return dict(icao=icao,callsign=callsign,start=start,duration=duration,state=state,origin=orig,destination=dest,description=f'voo completo {orig}->{dest}')

def arrival(icao,callsign,dest,ap,start,duration,distance=80,heading=110):
 b=ap[dest]; sp=advance(b[0],b[1],heading,distance); a=(sp[0],sp[1],b[2]+12000)
 def state(t):
  e=t-start
  if not 0<=e<=duration:return None
  u=e/duration; prog=smooth(u); lat,lon=point_between(a,b,prog); alt=a[2]+(b[2]-a[2])*u; vr=(b[2]-a[2])/duration*60
  eps=.5;p0=smooth(clamp((e-eps)/duration));p1=smooth(clamp((e+eps)/duration));total=distance_km(a[0],a[1],b[0],b[1])
  speed=total*(p1-p0)/max(.001,2*eps)*3600/1.852; before=point_between(a,b,p0);after=point_between(a,b,p1)
  track=bearing_deg(before[0],before[1],after[0],after[1]) if before!=after else 0;ground=int(e==duration)
  if ground:speed=0;vr=0
  return lat,lon,alt,speed,track,vr,ground
 return dict(icao=icao,callsign=callsign,start=start,duration=duration,state=state,origin=None,destination=dest,description=f'chegada observada em {dest}; origem deve ficar UNKNOWN')

def departure(icao,callsign,orig,ap,start,duration,distance=80,heading=290):
 a=ap[orig]; ep=advance(a[0],a[1],heading,distance); b=(ep[0],ep[1],a[2]+12000)
 def state(t):
  e=t-start
  if not 0<=e<=duration:return None
  u=e/duration;prog=smooth(u);lat,lon=point_between(a,b,prog);alt=a[2]+(b[2]-a[2])*u;vr=(b[2]-a[2])/duration*60
  eps=.5;p0=smooth(clamp((e-eps)/duration));p1=smooth(clamp((e+eps)/duration));total=distance_km(a[0],a[1],b[0],b[1])
  speed=total*(p1-p0)/max(.001,2*eps)*3600/1.852;before=point_between(a,b,p0);after=point_between(a,b,p1)
  track=bearing_deg(before[0],before[1],after[0],after[1]) if before!=after else heading;ground=int(e==0)
  if ground:speed=0;vr=0
  return lat,lon,alt,speed,track,vr,ground
 return dict(icao=icao,callsign=callsign,start=start,duration=duration,state=state,origin=orig,destination=None,description=f'saída observada de {orig}; destino deve ficar UNKNOWN')

def cruise(icao,callsign,near,ap,start,duration):
 c=ap[near];w=advance(c[0],c[1],270,110);e=advance(c[0],c[1],90,110);a=(w[0],w[1],35000);b=(e[0],e[1],35000);total=distance_km(a[0],a[1],b[0],b[1])
 def state(t):
  x=t-start
  if not 0<=x<=duration:return None
  u=x/duration;lat,lon=point_between(a,b,u);speed=total/duration*3600/1.852;track=bearing_deg(a[0],a[1],b[0],b[1])
  return lat,lon,35000,speed,track,0,0
 return dict(icao=icao,callsign=callsign,start=start,duration=duration,state=state,origin=None,destination=None,description=f'cruzeiro a 35000 ft na região de {near}; ambos devem ficar UNKNOWN')

def cases(ap):
 return [
  full('F10001','TST001','SBGO','SBBR',ap,0,600),
  full('F10002','TST002','SBBR','SBGO',ap,0,600),
  full('F10003','TST003','SBKP','SBGR',ap,60,480),
  full('F10004','TST004','SBGR','SBKP',ap,60,480),
  arrival('F10005','TST005','SBBR',ap,120,480),
  departure('F10006','TST006','SBBR',ap,0,480),
  cruise('F10007','TST007','SBBR',ap,90,510),
 ]
def generate(output,truth,ap,interval=1):
 fs=cases(ap);end=max(f['start']+f['duration'] for f in fs);lines=[];tick=0
 while tick*interval<=end+1e-9:
  sec=tick*interval
  for f in fs:
   s=f['state'](sec)
   if s is None:continue
   lat,lon,alt,speed,track,vr,ground=s
   row={'icao':f['icao'],'callsign':f['callsign'],'simulation':True,'rx_epoch_ns':BASE_EPOCH_NS+round(sec*1e9),'lat':round(lat,7),'lon':round(lon,7),'altitude_ft':round(alt),'ground_speed_kt':round(max(0,speed),2),'track_deg':round(track%360,2),'vertical_rate_fpm':round(vr),'on_ground':int(ground)}
   lines.append(json.dumps(row,ensure_ascii=False,allow_nan=False))
  tick+=1
 output.parent.mkdir(parents=True,exist_ok=True);output.write_text('\n'.join(lines)+'\n',encoding='utf-8')
 gt={'dataset':output.name,'description':'Casos sintéticos limpos para medir acertos, erros e UNKNOWN.','note':'origin/destination null significa que aquele extremo não deve ser inferido a partir do trecho observado.','aircraft':{f['icao']:{'callsign':f['callsign'],'origin':f['origin'],'destination':f['destination'],'description':f['description']} for f in fs}}
 truth.write_text(json.dumps(gt,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');return len(lines),gt

def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,default=Path('validation/casos_avaliacao.ndjson'));p.add_argument('--truth',type=Path,default=Path('validation/ground_truth.json'));p.add_argument('--reference-db',type=Path);p.add_argument('--interval',type=float,default=1.0);a=p.parse_args()
 ap=load_airports(a.reference_db);n,gt=generate(a.output,a.truth,ap,a.interval);print(f'Gerado {a.output}: {n} mensagens');print(f'Ground truth: {a.truth}')
 for icao,x in gt['aircraft'].items():print(f"{icao}: {x['origin'] or 'UNKNOWN'} -> {x['destination'] or 'UNKNOWN'}")
if __name__=='__main__':main()
