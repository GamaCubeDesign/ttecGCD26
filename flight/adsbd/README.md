# adsbd — daemon do payload ADS-B

O `adsbd` é o dono do SDR (ADR-0008). Ele sobe e supervisiona o `dump1090-fa`,
lê as mensagens decodificadas na porta SBS e entrega três coisas:

| Produto | Para quem | Quando |
|---|---|---|
| **Registro de bordo** — toda mensagem, carimbada na chegada, uma linha JSON cada (`ndjson_path`) | análise depois do teste, por USB (HLR-SW-02, HLR-ADS-07) | a cada mensagem |
| **Retrato** — o estado de cada aeronave em 20 bytes (`IPC_TRACKS`) | `ttcd`, que transmite o mais recente a cada 5 s (ADR-0003, ADR-0007) | a cada 1 s |
| **Contadores** — linhas lidas, decodificadas, aeronaves, reinícios do `dump1090` (`IPC_STAT`) | `ttcd`, para o `TM_STAT` e o `TM_HK` (HLR-ADS-08) | a cada 1 s |

| Onde está o quê | |
|---|---|
| Quem é dono do SDR, e por quê | ADR-0008 |
| Processos separados, `ttcd` no centro | ADR-0005 |
| O tempo de cada mensagem | ADR-0009 (*Proposed*) |
| Leitura do SBS-1 | `sbs.c` |
| Tabela de aeronaves e regras do retrato | `tracks.c` (o comentário de `tracks.h` define cada regra) |
| Linha do registro de bordo | `ndjson.c` |
| Núcleo: linhas → tabela → registro, retrato, contadores | `core.c` |
| Casca: `dump1090`, sockets, arquivos, relógio | `main.c` |
| Configuração | `config.c`, `adsbd.conf.example` |

O núcleo segue o do `ttcd`: nenhuma chamada de sistema, testado com relógio
virtual (`tests/test_sbs.c`, `test_tracks.c`, `test_adsbd_core.c`); a
configuração tem o seu (`test_adsbd_config.c`). A casca é testada com o
binário real, fazendo o papel do `dump1090` e do `ttcd`
(`tests/test_adsbd_integration.c`), e a cadeia inteira, com o `ttcd` real e
a ground no rádio UDP (`tests/test_adsb_chain.c`).

## O que muda em relação ao `adsb_capture.c`

O protótipo de 15/09 foi a base. Ficaram o `fork`/`execv` sem shell, a leitura
das linhas SBS, o carimbo de cada mensagem na chegada, o `fflush` por linha e
o **formato da linha, byte a byte** — o orçamento de dados é calculado a partir
dele (160,4 B por linha, 7,34 MiB por missão), e o estimador da ground
(`ground/host/ground-aeronaves`) já o lê.

| No protótipo | Agora |
|---|---|
| Caminhos compilados (`/home/pedro/...`) | Tudo em arquivo de configuração, sobrescrevível com `-o` |
| `CLOCK_REALTIME`, "já ajustado antes do `main()`" | Relógio monotônico + âncora que vem da ground (ADR-0009) |
| `on_ground` lia só `"1"` como verdadeiro; o SBS usa `"-1"` | `-1` é verdadeiro: aeronave no solo sai como `1`, não mais como desconhecida |
| `dump1090` morto, o programa terminava | Reiniciado com espera dobrando (1 s, 2 s, ... 30 s), cada reinício contado e enviado à ground |
| `sleep(2)` esperando o `dump1090` subir | Reconexão a cada `retry_ms`, sem bloquear |
| Só gravava | Tabela de aeronaves, retrato para o `ttcd`, contadores |
| Gravava sem limite | Para de gravar em `ndjson_max_bytes`, com um evento no log (data-budget §7) |
| `--write-json` gravando no SD a cada segundo | Removido: o `adsbd` não usa |

Linhas malformadas são contadas e nunca usadas; as 20 primeiras aparecem no
log com o motivo (AGENTS.md, regra 6). Endereços não-ICAO (`~`, TIS-B) são
contados e não gravados.

## Rodar sem SDR: a cadeia inteira

`tools/sbs_replay/sbs_replay.py` faz o papel do `dump1090`: serve linhas SBS a
partir de uma captura SBS, do registro de bordo ou da saída do simulador da
ground (`simular_trajetorias.py`). O alvo abaixo passa a missão por tudo — o
`adsbd`, o `ttcd` no rádio UDP, a `gs_cli` como ground e o adaptador — em
tempo real:

```bash
make adsb-chain INPUT=eventos.ndjson        # ou uma captura .sbs
```

Tudo fica em `build/adsb-chain/<data>-<hora>/`:

| Arquivo | O que é |
|---|---|
| `adsb.ndjson` | o registro de bordo que o `adsbd` gravou |
| `adsbd.jsonl`, `ttcd.jsonl` | os logs de eventos dos dois daemons |
| `ground.jsonl` | tudo o que a ground recebeu, frame a frame |
| `ground.ndjson` | o que a ground reconstruiu do rádio, no formato do estimador |

E, no estimador da ground:

```bash
cd ground/host/ground-aeronaves
python -m ground --input ../../../build/adsb-chain/<data>-<hora>/ground.ndjson
```

O instante de cada registro em `ground.ndjson` é `chegada − tempo no ar −
idade`, no relógio da ground (ADR-0007 §6); o adaptador está em
`ground/host/tracks_to_ndjson.py`.

Para estudar capturas reais sem esperar o tempo real,
`tools/analysis/downlink_emulation.py` emula o que desceria pelo rádio, com as
mesmas regras, e com `--compare` roda o estimador nos dois e mostra as
conclusões lado a lado.

## Rodar à mão

```bash
python3 tools/sbs_replay/sbs_replay.py --ndjson eventos.ndjson --port 31003 &
build/flight/adsbd -c flight/adsbd/adsbd.conf.example \
    -o dump1090_cmd= -o sbs_port=31003 -o ipc_path=/tmp/ttec.sock \
    -o ndjson_path=/tmp/adsb.ndjson -o log_path=-
```

`dump1090_cmd` vazio: o `adsbd` não sobe nada, só se conecta. Sem um `ttcd`
escutando em `ipc_path`, ele grava assim mesmo e tenta a conexão a cada
`retry_ms`.

## Na Pi

O `dump1090-fa` precisa estar instalado no caminho do `dump1090_cmd` — é a
primeira tarefa da medição do PLANO 3.4. Depois:

```bash
make install          # ttcd e adsbd, as unidades e, se ainda não existirem, /etc/gama/*.conf
make adsbd-enable     # sobe agora e a cada boot
make adsbd-status     # o que o systemd diz, e o fim do log
make adsbd-stop       # libera o SDR, por exemplo para a varredura de ganho
```

O serviço roda como o usuário `gama`, sem root, no grupo `plugdev` (o
dispositivo USB do SDR). O acesso restrito a dispositivos USB (`DeviceAllow`)
precisa ser conferido na primeira execução na Pi.

## Log de eventos

Um objeto JSON por linha, com o `CLOCK_MONOTONIC` absoluto em `mono_ms` — a
mesma base do log do `ttcd`, para os dois se alinharem.

| Evento | Quando |
|---|---|
| `start`, `stop`, `exit` | início e fim, com a versão e o tamanho do registro |
| `dump1090_start`, `dump1090_exit`, `dump1090_restart_in` | supervisão do `dump1090` |
| `sbs_up`, `sbs_down`, `sbs_connect_failed` | a conexão com a porta SBS |
| `ipc_up`, `ipc_down`, `ipc_connect_failed` | a conexão com o `ttcd` |
| `time_anchor` | chegou a hora da ground; `step_ms` é o salto que isso deu |
| `sbs_reject` | uma linha malformada (só as 20 primeiras) |
| `stats` | todos os contadores, a cada minuto |
| `record_full`, `log_full` | o teto de um dos arquivos foi atingido |

## O que falta

- **Medições na Pi (PLANO 3.4):** CPU do `dump1090` a 2,4 MS/s, varredura de
  ganho, taxa de decodificação contra um cenário conhecido.
- **`GAMA_HK_F_DUMP1090_UP`:** o bit do HK depende de um campo novo no
  `IPC_STAT`, o que muda um formato fixado pelos vetores — decisão à parte.
- **Tabela ICAO → callsign para o `REQ_ROSTER`:** precisa de uma mensagem IPC
  nova; o `adsbd` já guarda o callsign de cada aeronave.
