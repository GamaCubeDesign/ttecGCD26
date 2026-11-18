# Ground station no PC

O que roda no computador da ground station, em Python. A entrada do operador fica
na raiz desta pasta; o ADS-B da ground, em `adsb/`.

| Caminho | O quê |
|---|---|
| `gs_serial.py` | A ponte com a ESP32 da ground (ADR-0018). Reseta a placa, dá a ela o relógio do PC (a ESP32 não tem), manda os comandos e grava as linhas JSON. Para os roteiros da bancada, a ESP32 fica igual à `gs_cli`. Uso em `ground/esp32/README.md` |
| `gs_web.py`, `web/` | A interface no navegador, central para a operação (abaixo) |
| `iniciar_ground.sh` | `gs_web.py --adsb` com os argumentos dados |
| `iniciar_painel.sh` | O painel de trajetórias do estimador (`python -m adsb.estimator.visualize --serve`) |
| `adsb/estimator/` | O estimador de origem e destino (pacote `adsb.estimator`, do Pedro): recepção de NDJSON, aeroportos e pistas, memória temporal, pares (ADR-0019), conclusão combinada, painel. Detalhes em `adsb/README.md` |
| `adsb/pipeline.py` | O estimador ligado à ground ao vivo (ADR-0021): o log, a reconstrução do horário, os pares em subprocessos |
| `adsb/tracks_to_ndjson.py` | Do log da ground ao NDJSON do estimador (abaixo) |
| `adsb/data/referencias/` | O cadastro OurAirports (`referencias.db`) e o importador |
| `adsb/data/pares/` | O gerador do banco de pares, a prioridade regional e a auditoria de cidades |
| `adsb/validation/` | Avaliação e diagnóstico do estimador; o simulador de trajetórias (`simular_trajetorias.py`) |
| `adsb/run/` | O que uma execução do estimador escreve por padrão (fora do Git) |
| `tests/estimator/`, `tests/host/` | Os testes do estimador e os da entrada do operador; `tests/support.py`, o que os dois usam |
| `sessions/` | Uma pasta por execução do `gs_web.py --adsb` (fora do Git) |

Tudo roda **a partir desta pasta** (`ground/host`), que fica no caminho do Python
sem `sys.path`: `python3 -m adsb.estimator ...`, `python3 gs_web.py ...`.

## Testes

Requer Python 3.10 ou mais recente; o núcleo usa só a biblioteca padrão, o
processamento ADS-B ao vivo e os pares usam NumPy (`adsb/requirements.txt`).

```bash
cd ground/host
python3 -m unittest discover -s tests/estimator     # CTest ground_aeronaves
python3 -m unittest discover -s tests/host          # CTest gs_serial, gs_web, adsb_pipeline, tracks_to_ndjson
node web/test_web.mjs                               # CTest gs_web_modules (onde houver Node)
node tests/estimator/test_viewer.cjs                # o painel de trajetórias
```

Os mesmos rodam no `make test`.

## A interface no navegador

`gs_web.py` lê as linhas JSON da ground e serve a página com o estado do satélite.
Só biblioteca padrão (com `--adsb`, NumPy); escuta em `127.0.0.1:8080`.

Pela raiz do repositório, `make gs-web` escolhe a fonte por variável:
`ESP32_PORT=/dev/ttyUSB0` (padrão), `UDP=1` ou `REPLAY=<log.jsonl>` (com
`SPEED=10`). `POWER`, `WEB_HOST` e `WEB_PORT` ajustam a potência da ground, o
endereço e a porta. Equivale a:

```bash
python3 gs_web.py --serial /dev/ttyUSB0 --power 2        # a ESP32 da ground
python3 gs_web.py --udp                                   # gs_cli no rádio UDP (build/ primeiro)
python3 gs_web.py --replay ../../docs/vv/bancada/AAAA-MM-DD/x-ground.jsonl --speed 10   # sem hardware
```

Abra `http://127.0.0.1:8080`. Sem a Pi, `make sim INPUT=<captura>` sobe o satélite
inteiro no PC (rádio UDP, os três daemons de voo, o `sbs_replay` no lugar do SDR)
com esta página na frente; tráfego aéreo real vem do `tools/live_traffic`.

A página (`web/`, módulos ES servidos pelo próprio `gs_web.py`, sem passo de build
e sem nada da internet):

- **Barra fixa:** contato, modo do OBC e há quanto tempo, RSSI/SNR e idade do
  último quadro, bateria, corrente, temperaturas, tempo ligado, hora sincronizada
  (bit do HK) e mensagens ADS-B.
- **Abas por teste:**
  - Operação: o diagrama da Figura 44 (clicar num modo manda o `SET_MODE`) e as
    execuções de cada comando de modo.
  - ADS-B: o botão da missão com o passo a passo `SET_TIME` → `SET_MODE` → modo →
    decodificador → retratos → downlink, e o cronômetro dos 10 min; as
    estatísticas; o mapa offline; a tabela e o detalhe de cada aeronave, com
    origem e destino por método. A categoria inferida pela cinemática (ADR-0022)
    leva um asterisco.
  - Implantação (burn-wire), AOCS (a atitude do HK e o slot da página da equipe do
    AOCS, `web/aocs/README.md`), Sobrevivência e Enlace.
- **Utilitários e eventos** ao lado de todas as abas.
- **Identidade visual da Gama** (`web/css/gama.css`): tema escuro por padrão, claro
  no botão; Open Sans; o favicon oficial sem alteração. Português, espanhol e
  inglês; "projetar" aumenta as letras; `#adsb` na URL abre a aba.

O servidor agrupa o que empurra para a página (no máximo 2 mensagens por segundo,
e só o log novo), e o estimador refaz a vista das aeronaves no máximo uma vez por
segundo (`/api/adsb`, com `ETag`). `tools/analysis/gs_web_load.py` mede CPU,
memória e escrita em disco com uma sessão gravada.

- **Só os comandos de operação passam:** `ping`, `hk`, `stat`, `roster`, `time`,
  `stream`, `stop`, `rate` e `mode`. `shutdown`, `deafen` e o resto do console são
  recusados no servidor.
- **`mode basic_inter` e `mode aocs` pedem para armar** (o burn-wire e o
  empréstimo do I²C ao AOCS). A chave desliga sozinha em 15 s e depois de cada
  disparo, e o servidor confere de novo.
- Com `--replay` a página avisa que é demonstração e que os comandos não chegam a
  lugar nenhum: serve para ensaiar a apresentação.
- `--host 0.0.0.0` abre a página para a rede; não há senha, então só numa rede da
  equipe.

### Com o ADS-B (`--adsb`)

```sh
./iniciar_ground.sh --serial /dev/ttyUSB0             # o mesmo que gs_web.py --adsb
./iniciar_ground.sh --replay /caminho/ground.jsonl    # ensaio, sem hardware
```

Clique em **Iniciar missão ADS-B**. O comando passa pela console compartilhada:
primeiro `SET_TIME`, depois `SET_MODE`, só depois do ACK da hora (ADR-0020). Enviar
um comando não confirma que o OBC entrou no modo; a página mostra os ACKs e o
modo do HK.

Uma única conexão serial alimenta a telemetria e o processamento ADS-B. A ground
reconstrói o horário dos registros, tira as repetições e roda o método local. O
método de pares fica **desligado por padrão** (ADR-0022: com tráfego real, ligado,
a conclusão combinada nunca decidiu); `--pairs` o liga, com o banco em
`adsb/data/pares/pares_aeroportos.db` ou em `--pairs-db`. O cadastro padrão é
`adsb/data/referencias/referencias.db` (`--reference-db` troca).

Cada execução cria uma pasta em `sessions/` (ou `--session-dir`) com
`ground.jsonl`, `tracks.ndjson`, `telemetria.db` e `estado.json`. Os bancos de
referência ficam só leitura.

- A recepção grava o log, e uma thread independente o consome.
- Registros rejeitados são contados e guardados no SQLite.
- Uma falha fatal aparece na página e bloqueia um novo comando de missão; o log
  recebido fica para reprocessar.
- Não há fsync por mensagem: uma queda de energia pode perder o fim do log.

## Do downlink ao estimador

`adsb/tracks_to_ndjson.py` converte o log da ground station (um JSON por evento,
como o `tools/gs_cli` escreve e como a ESP32 manda pela USB, pelo `gs_serial.py`)
no NDJSON que o estimador lê. É um objeto por registro de aeronave recebido, com
o horário refeito no relógio desta máquina (chegada menos tempo no ar menos idade,
ADR-0007), e só os campos que o registro marca como válidos. O teste dele passa
cada linha pelo `parser.py` do estimador.

```bash
gs_cli --radio udp ... | python3 adsb/tracks_to_ndjson.py | python3 -m adsb.estimator --input -
python3 gs_serial.py --port /dev/ttyUSB0 --auto-time on | python3 adsb/tracks_to_ndjson.py | python3 -m adsb.estimator --input -
```

A cadeia inteira sem hardware (captura ou simulação, `adsbd`, `ttcd`, rádio UDP,
ground e estimador) é o `make adsb-chain INPUT=...`, descrito em
`flight/adsbd/README.md`.

## Os dados

- **Cadastro:** o `adsb/data/referencias/referencias.db` está no Git. Para refazê-lo,
  copie os CSVs e o manifesto do OurAirports para `adsb/data/referencias/fontes/`
  e rode `python3 adsb/data/referencias/importar_bases.py`. O importador verifica os
  hashes e recusa sobrescrever tabelas existentes.
- **Pares (~600 MB, fora do Git):** gerado em ~1,5 min (medido em 17/11):

  ```sh
  python3 -m pip install -r adsb/requirements.txt -r adsb/data/pares/requirements-regional.txt
  python3 adsb/data/pares/gerar_pares.py --source adsb/data/referencias/referencias.db \
      --output adsb/data/pares/pares_aeroportos.db
  python3 adsb/data/pares/prioridade_regional.py
  python3 adsb/data/pares/auditar_cidades.py      # documenta a cobertura; não muda a seleção
  ```

  O gerador recusa sobrescrever um banco existente. Shapely só é usado na geração
  regional.

As estimativas são heurísticas: TENTATIVE é uma hipótese provisória; ESTIMATED
não confirma o evento nem é uma probabilidade calibrada. Nada disso muda o
protocolo LoRa.

## A organização antiga (até 18/11)

Documentos antigos, logs da bancada e ADRs aceitos (imutáveis) citam os caminhos
de antes:

| Antes | Agora |
|---|---|
| `ground-aeronaves/ground/` (pacote `ground`, `python -m ground`) | `adsb/estimator/` (pacote `adsb.estimator`, `python -m adsb.estimator`) |
| `ground-aeronaves/tests/` | `tests/estimator/` |
| `ground-aeronaves/validation/`, `ground-aeronaves/simular_trajetorias.py` | `adsb/validation/` |
| `ground-aeronaves/README.md`, `docs/`, `requirements.txt` | `adsb/` |
| `ground-aeronaves/data/` (saídas) | `adsb/run/` |
| `banco-aeronaves/` | `adsb/data/referencias/` |
| `pares/` | `adsb/data/pares/` |
| `adsb_pipeline.py`, `tracks_to_ndjson.py` | `adsb/pipeline.py`, `adsb/tracks_to_ndjson.py` |
| `test_*.py` da raiz | `tests/host/` |
| `README-integracao-adsb.md` | a seção "Com o ADS-B", acima |
