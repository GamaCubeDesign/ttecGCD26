"""Geometria esférica de pares; prioridade regional ordena busca, não pontua."""
import math
from contextlib import closing
import sqlite3
import time
from pathlib import Path
import numpy as np
from .aircraft_category import airport_compatibility
from .geo import distance_km

R = 6371.0088


def vectors(points):
    p = np.radians(np.asarray(points, dtype=float))
    return np.column_stack((np.cos(p[:,0])*np.cos(p[:,1]), np.cos(p[:,0])*np.sin(p[:,1]), np.sin(p[:,0])))


def continuous_track(snapshot):
    """Somente o segmento recente contínuo, sem posições conflitantes."""
    points = (snapshot or {}).get('trajectory', [])
    track = []
    for p in points:
        if track:
            prev = track[-1]
            dt = (p['event_ns']-prev['event_ns'])/1e9
            if dt == 0 and p['lat']==prev['lat'] and p['lon']==prev['lon']:
                continue
            if dt <= 0 or dt > 60 or distance_km(prev['lat'],prev['lon'],p['lat'],p['lon']) > 2+dt*.65:
                track=[]
        track.append(p)
    return track


class PairEstimator:
    def __init__(self, path):
        self.path = Path(path)
        # Uma conexão somente leitura por processo; IDs são conferidos antes do cache.
        with closing(sqlite3.connect(self.path.resolve().as_uri()+'?mode=ro', uri=True)) as db:
            meta = dict(db.execute('SELECT key,value FROM metadata'))
            regional = dict(db.execute('SELECT key,value FROM regional_metadata'))
            if meta.get('generation_status')!='complete' or regional.get('status')!='complete':
                raise ValueError('Banco de pares ou classificação regional incompleto')
            if regional.get('bonus_enabled')!='false':
                raise ValueError('Este estimador exige bônus regional desativado')
            self.airports = [dict(zip(('id','ident','name','type','lat','lon'), row)) for row in db.execute(
                'SELECT id,ident,name,type,latitude_deg,longitude_deg FROM airports ORDER BY id')]
            ids = np.array([a['id'] for a in self.airports],dtype=np.int64)
            self.xyz = vectors([(a['lat'],a['lon']) for a in self.airports])
            self.batches=[]
            cursor=db.execute('SELECT airport_a_id,airport_b_id,analysis_priority FROM regional_pairs ORDER BY analysis_priority DESC,airport_a_id,airport_b_id')
            count=0
            while rows:=cursor.fetchmany(65536):
                raw=np.asarray(rows,dtype=np.int64)
                i,j=np.searchsorted(ids,raw[:,0]),np.searchsorted(ids,raw[:,1])
                if np.any(i>=len(ids)) or np.any(j>=len(ids)) or np.any(ids[i]!=raw[:,0]) or np.any(ids[j]!=raw[:,1]):
                    raise ValueError('Referência de aeroporto inválida no banco de pares')
                self.batches.append((i.astype(np.int32),j.astype(np.int32),raw[:,2].astype(np.int8)))
                count+=len(rows)
            if count!=int(regional['unordered_pair_count']) or count*2!=int(meta['pair_count']):
                raise ValueError('Contagem de pares inconsistente')
            self.total=count*2
            self.source_hash=meta.get('source_sha256')

    def estimate(self, job):
        start=time.monotonic()
        track=continuous_track(job['snapshot'])
        base={'status':'INSUFFICIENT','reason':'TRECHO_INSUFICIENTE','candidates':[],
              'score_kind':'heuristic_compatibility_not_probability','confidence':'BAIXA',
              'regional_score_bonus':False,'search_complete':True,'directed_pairs_in_database':self.total}
        if len(track)<3 or (track[-1]['event_ns']-track[0]['event_ns'])/1e9<20:
            return base
        displacement=distance_km(track[0]['lat'],track[0]['lon'],track[-1]['lat'],track[-1]['lon'])
        if displacement<2:
            return {**base,'reason':'DESLOCAMENTO_INSUFICIENTE'}
        indices=np.unique(np.linspace(0,len(track)-1,min(32,len(track))).astype(int))
        samples=[track[k] for k in indices]
        points=vectors([(p['lat'],p['lon']) for p in samples])
        travelled=sum(distance_km(a['lat'],a['lon'],b['lat'],b['lon']) for a,b in zip(track,track[1:]))
        category=job.get('category')
        factors=np.array([airport_compatibility(category,a['type'])['factor'] if airport_compatibility(category,a['type'])['allowed'] else 0 for a in self.airports])
        corridor=job['corridor_km']; best=[]; examined=0; passed=0
        for i,j,priority in self.batches:
            a,b=self.xyz[i],self.xyz[j]
            cross=np.cross(a,b); norm=np.linalg.norm(cross,axis=1)
            theta=np.arctan2(norm,np.clip(np.einsum('ij,ij->i',a,b),-1,1))
            valid=(norm>1e-8)&(factors[i]>0)&(factors[j]>0)
            normal=np.divide(cross,norm[:,None],out=np.zeros_like(cross),where=norm[:,None]>1e-8)
            tangent=np.cross(normal,a)
            last=points[-1]
            along=np.arctan2(tangent@last,a@last)
            xt=np.abs(np.arcsin(np.clip(normal@last,-1,1)))
            # Distância ao arco FINITO: fora dos extremos, usa o aeroporto mais próximo.
            endpoint=np.minimum(np.arccos(np.clip(a@last,-1,1)),np.arccos(np.clip(b@last,-1,1)))
            last_distance=np.where((along>=0)&(along<=theta),xt,endpoint)*R
            chosen=np.flatnonzero(valid&(last_distance<=corridor))
            examined+=len(i)*2
            if not len(chosen): continue
            # Subdivisão limita a memória mesmo em áreas com muitas rotas.
            for offset in range(0,len(chosen),2048):
                k=chosen[offset:offset+2048]
                aa,bb,nn,tt=a[k],b[k],normal[k],tangent[k]
                al=np.arctan2(tt@points.T,aa@points.T)
                # unwrap evita inversão artificial ao cruzar ±180 graus no parâmetro.
                al=np.unwrap(al,axis=1)
                # Escolhe a representação angular próxima ao segmento [0,theta].
                al-=2*np.pi*np.round((np.mean(al,axis=1)-theta[k]/2)/(2*np.pi))[:,None]
                signed=(al[:,-1]-al[:,0])*R
                reverse=signed<0
                steps=np.diff(al,axis=1)*R*np.where(reverse,-1,1)[:,None]
                monotonic=np.mean(steps>=-.5,axis=1)
                cross_track=np.abs(np.arcsin(np.clip(nn@points.T,-1,1)))
                enddist=np.minimum(np.arccos(np.clip(aa@points.T,-1,1)),np.arccos(np.clip(bb@points.T,-1,1)))
                distance=np.where((al>=0)&(al<=theta[k,None]),cross_track,enddist)*R
                rms=np.sqrt(np.mean(distance**2,axis=1))
                alignment=np.minimum(1,np.abs(signed)/max(travelled,.001))
                scores=np.exp(-(rms/corridor)**2)*monotonic*alignment*np.minimum(factors[i[k]],factors[j[k]])
                keep=(np.abs(signed)>=1)&(monotonic>=.8)&(rms<=corridor)&(scores>=.05)
                passed+=int(np.sum(keep))
                options=np.flatnonzero(keep)
                if len(options)>job['top']:
                    options=options[np.argsort(-scores[options],kind='stable')[:job['top']]]
                for q in options:
                    index=k[q]; oi,di=(int(j[index]),int(i[index])) if reverse[q] else (int(i[index]),int(j[index]))
                    best.append({'origin':self.airports[oi],'destination':self.airports[di],
                        'score':round(float(scores[q]),6),'regional_priority':int(priority[index]),
                        'evidence':{'rms_distance_km':round(float(rms[q]),3),'progress_km':round(abs(float(signed[q])),3),
                                    'monotonic_fraction':round(float(monotonic[q]),4),'direction_alignment':round(float(alignment[q]),4),
                                    'route_distance_km':round(float(theta[index]*R),3)}})
            best=sorted(best,key=lambda c:(-c['score'],c['origin']['id'],c['destination']['id']))[:job['top']]
        gap=best[0]['score']-best[1]['score'] if len(best)>1 else None
        return {**base,'status':'CANDIDATES' if best else 'NO_MATCH','reason':'PARES_COMPATIVEIS' if best else 'SEM_PAR_COMPATIVEL_NO_CORREDOR',
                'candidates':best,'directed_pairs_examined':examined,'compatible_pairs':passed,
                'sampled_positions':len(samples),'track_positions':len(track),'track_span_s':(track[-1]['event_ns']-track[0]['event_ns'])/1e9,
                'corridor_km':corridor,'score_gap':round(gap,6) if gap is not None else None,
                'elapsed_s':round(time.monotonic()-start,3),
                'limitations':['BASE_RESTRITA_A_AEROPORTOS_SELECIONADOS','GRANDE_CIRCULO_NAO_REPRESENTA_PROCEDIMENTO_REAL','NOTAS_NAO_CALIBRADAS']}


_CACHE=None
_CACHE_KEY=None


def evaluate_pair_job(job):
    global _CACHE,_CACHE_KEY
    begin=time.monotonic()
    path=Path(job['database']);stat=path.stat();key=(str(path.resolve()),stat.st_size,stat.st_mtime_ns)
    if key!=_CACHE_KEY:
        _CACHE=PairEstimator(path);_CACHE_KEY=key
    result=_CACHE.estimate(job)
    result['worker_elapsed_s']=round(time.monotonic()-begin,3)
    return result
