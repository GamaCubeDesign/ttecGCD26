# Roteiro da bancada — a cadeia ADS-B, novembro de 2026

Foco: **antena → SDR → `adsbd` → quadro → LoRa → ground (ESP32) → painel e
estimador**. Cada etapa isola um elo, para que uma falha aponte um lugar só.
Só se passa à etapa seguinte com a anterior aprovada.

Código: a branch local `fix/adsbd-sem-recusa-de-hora` (a `ground-adsb` mais a
decisão de 16/11: o `ttcd` recusa a missão sem hora; o `adsbd` não). A Pi e a
ESP32 têm de rodar essa versão; o `git describe` aparece no `start` de cada log.

Anotar em `docs/vv/bancada/AAAA-MM-DD/notas.md` (modelo:
[`modelo-notas-2026-11.md`](modelo-notas-2026-11.md)): horário, versão,
montagem, resultado com números, o que deu errado.

Referência de outubro: em 09/10 a cadeia SDR → `adsbd` → `ttcd` → LoRa → ground
rodou, mas **sem registros de track**, porque nenhuma aeronave com posição
estava ao alcance. Desta vez, a antena tem de ver o céu.

| Etapa | Elo testado | Precisa de |
|---|---|---|
| 0 | rede, firmware, instalação | PC, Pi, ESP32 |
| 1 | antena + SDR + `dump1090-fa` | Pi, SDR, antena |
| 2 | `adsbd` com o SDR real, sem rádio | etapa 1 |
| 3 | enlace LoRa sozinho | Pi com o rádio A, ESP32 com o rádio B |
| 4 | cadeia sem OBC: tracks pelo ar | etapas 2 e 3 |
| 5 | cadeia completa pelo modo, 10 min, painel | etapa 4, `obcd` |
| 6 | estimador com pares, sobre o que foi gravado | etapa 5, banco de pares |
| 7 | recolher e anotar | — |

---

## Etapa 0 — Preparação

**Rede** (detalhes no [README](README.md#rede-da-bancada-o-pc-como-ponto-de-acesso)):
o `bancada-ap` ocupa o único Wi-Fi do PC (`wlp2s0`). Antes de subir, garanta
outra saída de internet para o PC (cabo, ou celular por USB), ou aceite ficar
sem ela durante a sessão.

```bash
nmcli con up bancada-ap
iw dev wlp2s0 station dump | grep -E 'Station|signal:'
ssh pi@gamapi.local 'hostname -I; uptime'
```

**ESP32**: o console mudou na branch (`mode mission_adsb` envia a hora antes).

```bash
make esp32-flash ESP32_PORT=/dev/ttyUSB0
```

**Pi**: sincronizar, compilar, testar e instalar.

```bash
make pi-sync pi-build
make pi-test                          # 40/40
make pi-install CONF_DIR=flight/conf/obc-v2
ssh pi@gamapi.local 'cat /etc/gama/adsbd.conf | grep -E "^(decoder|dump1090_cmd)"; test -x /usr/bin/dump1090-fa && echo dump1090 ok'
```

O `make install` só copia os `/etc/gama/*.conf` **se ainda não existirem**:
conferir se os que estão lá são os da placa (`decoder = mode`; pinos do rádio
iguais à fiação montada).

**Aprovado:** ssh responde, `pi-test` 40/40, a ESP32 imprime `boot` com
`"radio":"ok"`, e `dump1090 ok`.

---

## Etapa 1 — Antena, SDR e `dump1090-fa`, sem nada nosso

Prova que há sinal: a antena, o NESDR e o decodificador. Nenhum código do
projeto roda aqui.

```bash
ssh pi@gamapi.local
sudo systemctl stop adsbd ttcd obcd           # liberar o SDR e o rádio
lsusb | grep -i -E 'realtek|rtl'              # o NESDR aparece (0bda:2838)
timeout 60 /usr/bin/dump1090-fa --device-index 0 --gain -10 --interactive
```

Antena perto de uma janela ou, melhor, ao ar livre; anotar onde ela estava.

**Anotar:** quantas aeronaves aparecem, quantas **com posição** (lat/lon
preenchidos), o RSSI típico e as mensagens por segundo.

**Aprovado:** pelo menos uma aeronave com posição em 60 s. Se nenhuma tiver
posição, mudar a antena de lugar antes de seguir; sem isso as etapas
seguintes não terão tracks, como em 09/10.

---

## Etapa 2 — `adsbd` com o SDR real, sem rádio e sem OBC

Prova que o `adsbd` sobe o `dump1090-fa`, lê o SBS, monta a tabela e grava o
registro de bordo.

```bash
cd ~/ttec
timeout 120 build/flight/adsbd -c /etc/gama/adsbd.conf -o decoder=always \
    -o ipc_path=/tmp/sem-ttcd.sock -o ndjson_path=/tmp/adsb.ndjson -o log_path=- \
    | tee /tmp/adsbd-etapa2.jsonl
wc -l /tmp/adsb.ndjson
grep -c '"lat"' /tmp/adsb.ndjson
grep '"event":"report"' /tmp/adsbd-etapa2.jsonl | tail -1
```

**Anotar:** linhas no NDJSON, linhas com posição, os contadores do último
`report` (linhas, rejeitadas, aeronaves) e se o log diz `time_synced` falso
(é o esperado aqui: não há `ttcd` para dar a hora).

**Aprovado:** `dump1090_start` no log, NDJSON crescendo, rejeitadas perto de 0.

---

## Etapa 3 — Enlace LoRa sozinho

Prova que o rádio A (Pi) e o rádio B (ESP32) se falam, antes de pôr dados ADS-B
no ar. É o B1/B2 do [roteiro de novembro](roteiro-2026-11.md), reduzido ao ping.

```bash
make bench-ping SPLIT=1 GS=esp32 ESP32_PORT=/dev/ttyUSB0 \
    TTCD_CONF=flight/conf/obc-v2/ttcd.conf      # se o rádio A está nos pinos da placa (RESET 25, DIO0 4)
# sem TTCD_CONF se está na fiação de outubro (RESET 20, DIO0 21)
```

**Anotar:** pings acked, tentativas, latência, RSSI e SNR, distância entre os
rádios.

**Aprovado:** 10/10 pings com ACK **na 1ª tentativa**. Com o DIO0 no pino errado o
ping ainda passa 10/10, mas quase todos com 2 tentativas e 1 000 ms de latência:
o `ttcd` só vê o fim da transmissão na varredura de 250 ms (achado 2 de
[16/11](2026-11-16/notas.md)). Em 16/11 o rádio A estava na fiação de outubro.

---

## Etapa 4 — A cadeia sem OBC: tracks pelo ar

Prova o caminho SDR → `adsbd` → IPC → `ttcd` → quadro → LoRa → ESP32 → PC,
sem a máquina de estados. O `STREAM_START` funciona fora de `MISSION_ADSB`
(teste `core: STREAM_START still works as an override outside MISSION_ADSB`).

Na Pi, dois terminais:

```bash
# terminal 1: ttcd no rádio A
make bench-ttcd TTCD_CONF=flight/conf/obc-v2/ttcd.conf     # ou sem TTCD_CONF, como na etapa 3
# terminal 2: adsbd ligado ao ttcd, decoder sempre ligado
build/flight/adsbd -c /etc/gama/adsbd.conf -o decoder=always -o ipc_path=<o SOCK do bench-ttcd> \
    -o ndjson_path=/tmp/adsb-etapa4.ndjson -o log_path=/tmp/adsbd-etapa4.jsonl
```

No PC:

```bash
python3 ground/host/gs_serial.py --port /dev/ttyUSB0 --power 2 | tee /tmp/ground-etapa4.jsonl
# digitar: time    stream 1    (esperar 2 a 3 min)    hk    stop    quit
```

**Anotar:** ACK do `time`; quantos eventos `tracks` chegaram e com quantos
registros; `age_ds` típico; o `hk` com `TIME_SYNCED` (bit 2 de `flags`) e
`DUMP1090_UP` (bit 3); o `time_anchor` no log do `adsbd`.

**Aprovado:** eventos `tracks` com registros que têm `lat`/`lon`, e os mesmos
ICAO no NDJSON de bordo.

---

## Etapa 5 — A cadeia completa pelo modo, com o painel

Prova a operação como na competição: os três serviços, um clique na ground,
10 min de missão (`adsb_timeout` = 600 000 ms), o downlink final.

Na Pi:

```bash
make obcd-enable service-enable adsbd-enable
make service-status adsbd-status obcd-status       # os três ativos; adsbd sem dump1090 (PRE_TEST)
```

No PC, a ground com o processamento ADS-B, **sem pares** nesta etapa:

```bash
python3 ground/host/gs_web.py --serial /dev/ttyUSB0 --power 2 --adsb --no-pairs
# abrir http://127.0.0.1:8080; o painel de aeronaves em http://127.0.0.1:8080/adsb/
```

Sequência na página:

1. `ping`, `hk`: `PRE_TEST`, `TIME_SYNCED` 0 ou 1.
2. **Iniciar missão ADS-B**: ACK OK do `SET_TIME`, depois ACK do `SET_MODE`.
3. `hk`: `MISSION_ADSB` e `DUMP1090_UP`.
4. Acompanhar os tracks na página e o painel de aeronaves por 10 min.
5. Ao fim dos 10 min o OBC passa sozinho a `MISSION_DOWNLINK`: chegam o roster e
   as estatísticas finais, e ele volta a `PRE_TEST`.

**Anotar:**

- Tempo do clique ao ACK do `SET_TIME`, ao ACK do `SET_MODE` e ao primeiro track
  (é a etapa 12 do roteiro de novembro, agora com a hora no meio).
- Aeronaves vistas a bordo contra aeronaves que chegaram à ground.
- Perda (lacunas de `seq`) e latência p95 (HLR-ADS-08, alvo 5 s) das estatísticas finais.
- Quantas aeronaves o painel mostra com estimativa local de origem ou destino.
- A pasta `ground/host/sessions/mission-*/` criada (guardar o `ground.jsonl`).

**Aprovado:** a missão começa com um clique; tracks com posição chegam durante
os 10 min; o OBC volta a `PRE_TEST` com o roster e as estatísticas recebidos.

**Opcional, se der tempo:** `sudo systemctl restart adsbd` no meio da missão;
ele volta, recebe a hora antes do modo e retoma. Anotar o buraco no NDJSON.

---

## Etapa 6 — O estimador com pares, sobre a missão gravada

O banco de pares (~710 MB) não existe ainda nesta máquina. Gerar antes ou
durante as etapas 1 a 5, num ambiente Python à parte:

```bash
python3 -m venv .venv && . .venv/bin/activate
python3 -m pip install -r ground/host/adsb/requirements.txt -r ground/host/adsb/data/pares/requirements-regional.txt
python3 ground/host/adsb/data/pares/gerar_pares.py --source ground/host/adsb/data/referencias/referencias.db \
    --output ground/host/adsb/data/pares/pares_aeroportos.db
python3 ground/host/adsb/data/pares/prioridade_regional.py
```

Depois, reproduzir a missão da etapa 5 com os dois métodos:

```bash
python3 ground/host/gs_web.py --replay ground/host/sessions/mission-XXXX/ground.jsonl \
    --adsb --pairs --pairs-db ground/host/adsb/data/pares/pares_aeroportos.db
```

**Anotar:** tempo da geração e da migração regional; tamanho do banco; tempo
por análise de pares (campo `elapsed_s`); para cada aeronave, o ranking local,
o de pares e a decisão combinada.

**Aprovado:** o painel mostra os dois rankings e a decisão combinada (ou
"pendente") para as aeronaves com trecho suficiente (3 posições, 20 s, 2 km).

---

## Etapa 7 — Recolher

```bash
make pi-fetch                                       # logs da Pi para docs/vv/bancada/
cp ground/host/sessions/mission-XXXX/ground.jsonl docs/vv/bancada/AAAA-MM-DD/
make service-stop adsbd-stop obcd-stop              # se a Pi não deve seguir rodando
nmcli con down bancada-ap
```

Atualizar a tabela de progresso do [README](README.md) e a seção "Validated on
the bench" do `AGENTS.md` com o que fechou.
