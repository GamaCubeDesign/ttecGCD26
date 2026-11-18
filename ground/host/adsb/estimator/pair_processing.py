"""Processos limitados e fila que guarda só a versão mais recente por aeronave."""
from collections import OrderedDict
from concurrent.futures import ProcessPoolExecutor
from copy import deepcopy
import multiprocessing
import os
import time
from .combined_estimator import fusion_context


def worker(job):
    # Cada processo tem um núcleo de cálculo NumPy; evita superlotar a máquina.
    os.environ['OPENBLAS_NUM_THREADS']='1'
    os.environ['OMP_NUM_THREADS']='1'
    from .pair_estimator import evaluate_pair_job
    return evaluate_pair_job(job)


def local_comparison(result):
    return {kind:{'status':decision.get('status'),'ident':(decision.get('candidate') or {}).get('ident'),
                  'top_candidate':next(iter(result.get(kind+'_candidates',[])),{}).get('ident')}
            for kind,decision in result.get('airport_method',{}).items()}


class PairScheduler:
    def __init__(self,config,executor=None,clock=time.monotonic):
        self.config=config; self.clock=clock; self.pending=OrderedDict(); self.active={};self.last_started={}
        self.executor=executor;self.owns_executor=executor is None;self.closed=False
        self.coalesced=0

    def offer(self,result):
        job={'icao':result['icao'],'version':result['analysis_version'],'episode':result['tracking_episode'],
             'event_ns':result['event_ns'],'snapshot':deepcopy(result['snapshot']),
             'category':result['aircraft_category']['code'],'local':local_comparison(result),
             'fusion_context':fusion_context(result),
             'database':str(self.config.pairs_db),'top':self.config.top,'corridor_km':self.config.pair_corridor_km}
        icao=job['icao']
        if icao in self.pending:
            self.coalesced+=1
        elif len(self.pending)>=self.config.pair_max_pending:
            # Não descarta mensagens; apenas adia nova análise desta aeronave.
            return {'status':'DEFERRED','reason':'LIMITE_DE_AERONAVES_PENDENTES','source_version':job['version']}
        self.pending[icao]=job
        return {'status':'PENDING','reason':'AGUARDANDO_PROCESSAMENTO','source_version':job['version']}

    def invalidate(self,icao):
        self.pending.pop(icao,None)

    def poll(self,force=False):
        results=[]
        for icao,(future,job) in list(self.active.items()):
            if not future.done(): continue
            del self.active[icao]
            try:
                value=future.result()
            except Exception as exc:
                value={'status':'ERROR','reason':type(exc).__name__+': '+str(exc),'candidates':[]}
            results.append({**value,'icao':icao,'source_version':job['version'],'source_event_ns':job['event_ns'],
                            'episode':job['episode'],'local_at_analysis':job['local'],'fusion_context':job['fusion_context'],'mode':'combined',
                            'completed_received_ns':time.time_ns()})
        for icao in list(self.pending):
            if len(self.active)>=self.config.pair_workers: break
            if icao in self.active: continue
            if not force and self.clock()-self.last_started.get(icao,-float('inf'))<self.config.pair_interval_s: continue
            job=self.pending.pop(icao)
            try:
                if self.executor is None:
                    self.executor=ProcessPoolExecutor(max_workers=self.config.pair_workers,
                        mp_context=multiprocessing.get_context('spawn'))
                future=self.executor.submit(worker,job)
                self.active[icao]=(future,job);self.last_started[icao]=self.clock()
            except Exception as exc:
                results.append({'icao':icao,'source_version':job['version'],'source_event_ns':job['event_ns'],
                    'episode':job['episode'],'local_at_analysis':job['local'],'fusion_context':job['fusion_context'],'mode':'combined',
                    'status':'ERROR','reason':type(exc).__name__+': '+str(exc),'candidates':[],
                    'completed_received_ns':time.time_ns()})
        return results

    @property
    def busy(self): return bool(self.pending or self.active)

    def close(self):
        if self.closed: return
        self.closed=True
        self.pending.clear()
        if self.executor is not None and self.owns_executor:
            if self.active and hasattr(self.executor,'terminate_workers'):
                self.executor.terminate_workers()
            else:
                self.executor.shutdown(wait=not self.active,cancel_futures=True)
