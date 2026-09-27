#!/usr/bin/env python3
import argparse,csv,json
from pathlib import Path

def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def ident(c):
 if not c:return None
 if isinstance(c,str):return c
 return c.get('ident') or c.get('icao_code')
def prediction(state,kind):
 key='takeoff' if kind=='origin' else 'landing';summary=(state.get('airport_summary') or {}).get(key)
 if summary:
  x=ident(summary.get('airport'))
  if x:return x,summary.get('status') or 'HISTORICAL','airport_summary'
 d=(state.get('airport_method') or {}).get(kind) or {};x=ident(d.get('candidate'))
 if x and d.get('status') in ('ESTIMATED','TENTATIVE'):return x,d.get('status'),'airport_method'
 return None,d.get('status') or 'UNKNOWN',d.get('reason') or 'sem decisão'
def classify(expected,predicted):
 if expected is None:return 'UNKNOWN_ESPERADO' if predicted is None else 'FALSO_POSITIVO'
 if predicted is None:return 'UNKNOWN'
 return 'ACERTO' if predicted==expected else 'ERRO'
def evaluate(state_path,truth_path):
 states=load(state_path).get('aircraft',{});truth=load(truth_path)['aircraft'];rows=[]
 for icao,t in truth.items():
  s=states.get(icao)
  for kind in ('origin','destination'):
   exp=t.get(kind)
   if s is None:pred,status,source=None,'AUSENTE_NO_ESTADO','aeronave ausente no estado.json'
   else:pred,status,source=prediction(s,kind)
   rows.append({'icao':icao,'callsign':t.get('callsign'),'kind':kind,'expected':exp,'predicted':pred,'status':status,'result':classify(exp,pred),'source':source})
 return rows
def summarize(rows):
 out={}
 for kind in ('origin','destination','all'):
  sub=rows if kind=='all' else [r for r in rows if r['kind']==kind];known=[r for r in sub if r['expected'] is not None];dec=[r for r in known if r['predicted'] is not None]
  out[kind]={'acertos':sum(r['result']=='ACERTO' for r in sub),'erros':sum(r['result'] in ('ERRO','FALSO_POSITIVO') for r in sub),'unknown':sum(r['result']=='UNKNOWN' for r in sub),'unknown_esperado':sum(r['result']=='UNKNOWN_ESPERADO' for r in sub),'falsos_positivos':sum(r['result']=='FALSO_POSITIVO' for r in sub),'casos_com_ground_truth':len(known),'decisoes_emitidas_em_casos_conhecidos':len(dec),'coverage':len(dec)/len(known) if known else None,'accuracy_quando_decidiu':sum(r['result']=='ACERTO' for r in dec)/len(dec) if dec else None}
 return out
def report(rows,stats):
 print('\nRESULTADO POR AERONAVE');print('-'*104);print(f"{'ICAO':<8} {'TIPO':<12} {'ESPERADO':<12} {'GROUND':<12} {'STATUS':<14} {'RESULTADO':<18}");print('-'*104)
 for r in rows:print(f"{r['icao']:<8} {r['kind']:<12} {(r['expected'] or 'UNKNOWN'):<12} {(r['predicted'] or 'UNKNOWN'):<12} {str(r['status']):<14} {r['result']:<18}")
 print('\nRESUMO')
 for k,label in [('origin','ORIGEM'),('destination','DESTINO'),('all','TOTAL')]:
  s=stats[k];cov='-' if s['coverage'] is None else f"{100*s['coverage']:.1f}%";acc='-' if s['accuracy_quando_decidiu'] is None else f"{100*s['accuracy_quando_decidiu']:.1f}%"
  print(f"{label:<8}: acertos={s['acertos']} | erros={s['erros']} | UNKNOWN={s['unknown']} | UNKNOWN esperado={s['unknown_esperado']} | falso positivo={s['falsos_positivos']} | coverage={cov} | acerto quando decidiu={acc}")
def main():
 p=argparse.ArgumentParser();p.add_argument('--state',type=Path,default=Path('validation/estado_avaliacao.json'));p.add_argument('--truth',type=Path,default=Path('validation/ground_truth.json'));p.add_argument('--json-out',type=Path,default=Path('validation/relatorio_avaliacao.json'));p.add_argument('--csv-out',type=Path,default=Path('validation/relatorio_avaliacao.csv'));a=p.parse_args()
 rows=evaluate(a.state,a.truth);stats=summarize(rows);report(rows,stats);a.json_out.parent.mkdir(parents=True,exist_ok=True);a.json_out.write_text(json.dumps({'rows':rows,'summary':stats},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 with a.csv_out.open('w',encoding='utf-8',newline='') as f:
  w=csv.DictWriter(f,fieldnames=['icao','callsign','kind','expected','predicted','status','result','source']);w.writeheader();w.writerows(rows)
 print(f'\nJSON: {a.json_out}\nCSV:  {a.csv_out}')
if __name__=='__main__':main()
