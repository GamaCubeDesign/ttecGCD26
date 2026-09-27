#!/usr/bin/env python3
import argparse,subprocess,sys
from pathlib import Path

def main():
 p=argparse.ArgumentParser();p.add_argument('--input',type=Path,default=Path('validation/casos_avaliacao.ndjson'));p.add_argument('--truth',type=Path,default=Path('validation/ground_truth.json'));p.add_argument('--keep-db',action='store_true');a=p.parse_args()
 db=Path('validation/telemetria_avaliacao.db');state=Path('validation/estado_avaliacao.json');rj=Path('validation/relatorio_avaliacao.json');rc=Path('validation/relatorio_avaliacao.csv')
 if not a.keep_db:
  for x in (db,Path(str(db)+'-wal'),Path(str(db)+'-shm'),state,rj,rc):
   if x.exists():x.unlink()
 cmd=[sys.executable,'-m','ground','--input',str(a.input),'--telemetry-db',str(db),'--output',str(state)]
 print('1/2 Processando NDJSON...');subprocess.run(cmd,check=True)
 print('\n2/2 Avaliando...');subprocess.run([sys.executable,str(Path(__file__).with_name('avaliar_estimativas.py')),'--state',str(state),'--truth',str(a.truth),'--json-out',str(rj),'--csv-out',str(rc)],check=True)
if __name__=='__main__':main()
