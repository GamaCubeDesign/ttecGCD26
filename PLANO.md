# Plano de execução — TT&C CubeDesign 2026

Documento de trabalho. As decisões consolidadas ficam nos ADRs
(`docs/adr/`); aqui fica o que falta fazer e em que ordem.

**Marcos:** Design Package **27/09/2026** · UNSAM **24/11/2026**
**Prioridade atual:** firmware testável. O DP é montado a partir dos ADRs e
das medições que o firmware produzir.

---

## Estado atual

### Concluído (dias 1–2)

| Item | Onde | Evidência |
|---|---|---|
| Estrutura do repositório | — | `common/ flight/ ground/ tools/ tests/ docs/` |
| Build CMake, C11, zero warnings | `CMakeLists.txt` | `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` |
| CRC-16/CCITT-FALSE | `common/crc16.*` | vetor publicado `"123456789"` → `0x29B1` |
| Serialização little-endian explícita | `common/gama_bytes.h` | header-only, sem `memcpy` de struct |
| Camada de framing | `common/gama_frame.*` | round-trip + todo caminho de rejeição |
| Registros de TM (track/roster/HK/stat) | `common/gama_tm.*` | clamping físico, NaN-safe |
| Catálogo de TC + vocabulário do OBC | `common/gama_tc.*` | valores de wire fixados por teste |
| Suíte de testes | `tests/` | 1713 asserções, 6 suítes, limpo sob ASan+UBSan |
| Vetores de wire dourados | `tests/test_vectors.c` | bytes conferidos à mão contra o layout |
| Orçamento reprodutível | `tools/analysis/lora_budget.py` | `--check` roda no CTest |
| ADRs 0001–0004 | `docs/adr/` | inglês, com alternativas rejeitadas |
| Budgets de dados e enlace | `docs/budgets/` | inglês |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j4
(cd build && ctest --output-on-failure)
```

### Dívida imediata

**Três ADRs são citados por documentos já `Accepted` mas não existem.**
Referência pendurada em documento aceito é defeito — resolver antes de
qualquer código novo:

- [ ] `0007-ota-frame-format-and-arq.md` — citado por ADR-0003 (quantização)
- [ ] `0008-adsb-receive-chain-ownership.md` — citado por ADR-0003 e ADR-0004
- [ ] `0011-documentation-language.md` — citado por ADR-0001 e ADR-0002
- [ ] Atualizar o índice em `docs/adr/README.md`

---

## Fase 2 — `ttcd` e o enlace (dias 3–4)

Objetivo: **dois RA-02 na bancada trocando frames**, com troca de taxa
funcionando e reversão por timeout exercitada.

### 2.1 `flight/libipc/` — transporte AF_UNIX SOCK_SEQPACKET

- [ ] `ipc_server_create(path)` — socket, bind, listen; `unlink()` antes do bind
- [ ] `ipc_accept()` / `ipc_close_peer()` — múltiplos clientes (`adsbd`, `obc`)
- [ ] `ipc_recv_nb(fd, buf, cap)` — `MSG_DONTWAIT`; devolve 0 quando vazio
- [ ] `ipc_send(fd, frame, len)` — trata `EAGAIN` sem bloquear
- [ ] Identificação de peer via `GAMA_FRAME_IPC_HELLO`
- [ ] Reuso do mesmo codec: payload IPC **é** um `gama_frame`

**Aceite:** dois processos de teste trocam 10 000 frames sem perda nem
bloqueio; `recv` em socket vazio retorna imediatamente.

### 2.2 `flight/ttcd/drivers/sx1278.*` — porte do driver

Base: `ultima_missao/satellite/LoRa.c` (utilizável, 463 linhas).

- [ ] Portar mantendo a API de registrador; descartar `Moden.cpp`
- [ ] `sx1278_set_profile(gama_rate_profile_t)` — escreve `REG_MODEM_CONFIG_1/2`
      **e recalcula o bit LDRO em `REG_MODEM_CONFIG_3`**.
      Nota: o driver legado só reescreve LDRO dentro de `LoRa_send()`/
      `LoRa_receive()`. Uma troca de taxa que mexa só em CONFIG_1/2 deixa LDRO
      obsoleto até o próximo envio. Verificar por leitura de registrador.
      SF12/BW125 → símbolo 32,8 ms → LDRO **on**. SF9 → 4,1 ms → off.
- [ ] **Substituir `sleep(Tpkt/1000 + 1)` de `Moden.cpp:91` por DIO0 TxDone.**
      Callback pigpio escreve um byte num `eventfd`; o laço principal trata.
      No SAFE esse sleep arredonda até 1 s de air time fora por pacote.
- [ ] Leitura de RSSI e SNR por pacote, para o log da ground
- [ ] `sx1278_rx_continuous()` / `sx1278_standby()` — half-duplex explícito

**Aceite:** ToA medido em bancada bate com `lora_budget.py` dentro de 5% nos
três perfis; LDRO confirmado por leitura de registrador após troca de perfil.

### 2.3 `flight/ttcd/link.*` — máquina de estados do enlace

- [ ] Estados `IDLE → TX → STREAM → BULK → SAFE` (`gama_link_state_t`)
- [ ] Fila de TX com prioridade: ACK > HK > snapshot > bulk
- [ ] Timer de reversão de taxa: sem contato no perfil novo em N s, volta
- [ ] Timeout de contato → `SAFE` + beacon
- [ ] Contadores de `seq` por direção (evidência do HLR-ADS-08)

### 2.4 `flight/ttcd/tc_dispatch.*`

- [ ] Validar tamanho de argumento via `gama_tc_arg_len()` antes de despachar
- [ ] Responder `TC_ACK` com `gama_ack_status_t` correto em todo caminho
- [ ] `SHUTDOWN` exige `GAMA_TC_SHUTDOWN_MAGIC`
- [ ] Traduzir `GAMA_TC_SET_MODE` → `GAMA_FRAME_IPC_TC_EVENT` para o OBC
- [ ] **Nenhum handler pode bloquear.** O pecado do legado
      (`Module.cpp:139,145,149` — `sleep(10)` dentro do handler) não se repete.

### 2.5 `flight/ttcd/main.c` — laço epoll

- [ ] `epoll` sobre: listen fd, fds de cliente, `timerfd` (escalonador de TM),
      `eventfd` (DIO0)
- [ ] Escalonador: snapshot 5 s, HK 10 s, STAT 30 s (ver `data-budget.md`)
- [ ] `SIGTERM`/`SIGINT` por `signalfd` — saída limpa, sem handler assíncrono
- [ ] **Zero `sleep()` no processo inteiro**

### 2.6 `flight/ttcd/config.*`

- [ ] Configuração por arquivo + override por argv. Nada de path hardcoded —
      o legado tinha `/home/pedro/adsb/...` compilado dentro do binário
- [ ] Unidade systemd em `flight/ttcd/ttcd.service`

### 2.7 Teste de bancada

- [ ] Dois RA-02 a ~1 m
- [ ] PER sobre 1000 frames em cada perfil
- [ ] Troca `SAFE ↔ NOMINAL ↔ FAST` nos dois sentidos
- [ ] **Caso do ACK perdido**: a troca aconteceu mas o ACK sumiu — a reversão
      tem que recuperar sozinha
- [ ] Registrar resultados em `docs/vv/`

### ADRs desta fase

- [ ] `0005-process-architecture-and-fault-isolation.md`
- [ ] `0006-obc-ttec-ipc-transport.md`
- [ ] `0007-ota-frame-format-and-arq.md` *(já em dívida)*

---

## Fase 3 — `adsbd` e o payload (dias 5–6)

Objetivo: **track table alimentada por SDR real**, com CPU medida na Pi.

### 3.1 Porte do `adsb_capture.c`

O núcleo é bom: `fork`/`execv` sem shell, remontagem de linha SBS, timestamp
por mensagem, append + `fflush`. Ajustes:

- [ ] Configuração por arquivo/argv (hoje `/home/pedro/*` está hardcoded)
- [ ] **`CLOCK_MONOTONIC` + uma âncora de wall-clock no boot** — a Pi Zero 2 W
      não tem RTC. Mesma convenção que o OBC já documentou em
      `obc/docs/log_schema.md`; adotar idêntica
- [ ] Supervisão do `dump1090`: detectar morte, reiniciar, contar reinícios
      (`gama_stat_t.dump1090_restarts`)
- [ ] Remover o `sleep(2)` da linha 375; laço de retry no connect
- [ ] Teto de tamanho no NDJSON — filesystem cheio derruba o `ttcd` junto

### 3.2 `flight/adsbd/tracks.*` — tabela de pistas

- [ ] Hash por ICAO de 24 bits, capacidade fixa, sem `malloc` no caminho quente
- [ ] Merge por tipo de mensagem: posição, velocidade e identificação chegam
      separadas — flags de validade por campo (`GAMA_TRACK_F_*`)
- [ ] Expiração por idade (aeronave que sumiu sai da tabela)
- [ ] `tracks_snapshot(out, max)` → array de `gama_track_t`

### 3.3 `flight/adsbd/ipc_client.*`

- [ ] Conectar no `ttcd`, `HELLO`, reconectar se cair
- [ ] Enviar `IPC_TRACKS` e `IPC_STAT`

### 3.4 Medições na Pi — **o maior risco não quantificado**

- [ ] CPU do `dump1090` a 2,4 MS/s (`pidstat -p <pid> 1`)
- [ ] Se saturar um core: cair para 2,0 MS/s e remedir
- [ ] Afinidade de CPU: `dump1090` e `ttcd` em cores distintos
- [ ] **Varredura de ganho.** `adsb_capture.c:46` usa `GAIN "-10"`, ambíguo
      entre o dump1090 original (décimos de dB) e o `-fa` (dB). O ambiente de
      teste é SDR a curta distância → sinal forte, AGC pode saturar
- [ ] Taxa de decodificação contra cenário conhecido
- [ ] Registrar tudo em `docs/budgets/data-budget.md` §9

### ADRs desta fase

- [ ] `0008-adsb-receive-chain-ownership.md` *(já em dívida)* — resolve o
      conflito com o `sdr.c` da branch `feat/modulo-aocs` do OBC
- [ ] `0009-time-reference-and-synchronisation.md`
- [ ] `0010-onboard-storage-and-data-retention.md`

---

## Fase 4 — Ground station e entrega do ICD (dias 7–8)

### 4.1 `ground/esp32/` — ESP-IDF

- [ ] Projeto IDF puxando `common/` via `EXTRA_COMPONENT_DIRS`
- [ ] Driver SX1278 sobre SPI (pinos do `groundStation.ino:6-11` como base)
- [ ] Bridge UART para o host
- [ ] **Build de teste com `tests/test_vectors.c` para xtensa.** Passar nos
      dois alvos é o que demonstra que Pi e ESP32 geram bytes idênticos — é a
      afirmação sobre a qual o ADR-0002 se apoia

### 4.2 `ground/host/` — CLI e dashboard

- [ ] CLI: enviar TC, receber TM, log NDJSON com timestamp de recepção
- [ ] Tabela ao vivo de aeronaves
- [ ] Base: `ultima_missao/telemetry_reports/serial_data_collector/`
- [ ] Reaproveitar `thermalControl/gerarRelatorio.py` + `template.tex` para o
      relatório de evidência do DP

### 4.3 Entrega do ICD ao time do OBC — **não deixar para o dia 11**

- [ ] `docs/icd/obc-ttec-icd.md`
- [ ] Implementação de referência do `radio.c` (~120 linhas). A assinatura que
      o OBC já tem é exatamente a forma certa:
      `Event radio_poll_tc(void)` vira `recv(fd, ..., MSG_DONTWAIT)` devolvendo
      `EV_NONE` quando não há nada. Nenhuma mudança arquitetural no OBC
- [ ] Chamador de `obclog_telemetry()` que também publica `IPC_TELEMETRY`
      (hoje a função existe e não tem chamador nenhum)
- [ ] `docs/icd/ota-protocol-icd.md`

### ADRs desta fase

- [ ] `0011-documentation-language.md` *(já em dívida)*

---

## Fase 5 — Ensaio ponta a ponta (dia 9)

- [ ] Missão de 10 min com cenário multi-aeronave
- [ ] Coletar as quatro métricas do HLR-ADS-08
- [ ] Repetir com o `dump1090` morto no meio, para exercitar a supervisão
- [ ] Repetir com a ground desligada por 2 min, para exercitar o `SAFE`

### Critérios de sucesso (HLR-ADS-08)

| Métrica | Alvo | Como medir |
|---|---|---|
| Taxa de decodificação | ≥ 95% | decodificadas / injetadas |
| Perda no downlink | ≤ 5% | lacunas de `seq` no log da ground |
| Latência recepção → início da TX (p95) | ≤ 5 s | limitada em 4,75 s pelo período de 5 s |
| Latência recepção → recepção na ground (p95) | ≤ 7 s | soma os 2,09 s de air time |
| Aeronaves simultâneas | 20 | pico da track table |

---

## Fase 6 — Design Package (dias 10–12)

- [ ] `docs/conops.md` — o do OBC tem 23 bytes, só o título
- [ ] `docs/vv/vv-plan.md` — métodos de verificação e justificativa (HLR-VV-02)
- [ ] `docs/vv/traceability-matrix.md` — Requisito ↔ Método ↔ Caso de teste ↔
      Resultado, gerada a partir dos IDs citados nos ADRs e no código
- [ ] `docs/budgets/power-budget.md` (HLR-EPS-03) — falta
- [ ] Orçamento financeiro e justificativa COTS (HLR-COST-01/02)
- [ ] Diagrama de blocos e esquema de comunicação
- [ ] Missão secundária (HLR-ADS-06) — conceito, implementação não obrigatória
- [ ] Montar o DP em inglês a partir dos ADRs/ICDs/budgets

---

## Riscos abertos

| Risco | Impacto | Mitigação | Quando |
|---|---|---|---|
| **CPU do `dump1090` na Pi Zero 2 W** (512 MB, 4× A53 @1 GHz) disputando com `ttcd` | Missão inviável | Medir; fallback 2,0 MS/s, afinidade de core | Dia 5 |
| **Ganho do SDR mal ajustado** em ambiente de sinal forte | Taxa de decodificação baixa | Varredura manual de ganho | Dia 5 |
| **Time do OBC não integra o cliente IPC** | Sem telecomando de modo (HLR-COMM-01) | Entregar ICD + referência no dia 7 | Dia 7 |
| **Troca de taxa dessincroniza as pontas** | Perda do enlace, irrecuperável | ACK + timer de reversão; exercitar o caso do ACK perdido | Dia 4 |
| **Throttling térmico da Pi** em TVAC a +50 °C (throttle em 80–85 °C) | HLR-ENV-02 | Temperatura do SoC no `TM_HK` como evidência | Dia 9 |
| **Ruído de 433 MHz no local** não caracterizado | Margem menor que a modelada | Medir noise floor ao chegar; `SAFE` como recurso | UNSAM |
| **Caminho de bulk download sub-testado** — não está no caminho crítico | Sem dados brutos por RF | Reservar tempo no dia 8 | Dia 8 |

---

## Regras que não se quebram

Derivadas dos defeitos concretos da missão anterior. O porquê de cada uma está
no ADR citado.

1. **Nunca serializar struct por cast.** `Integration.cpp:149` fazia
   `tx_send((uint8_t*)&hd, hd.length)` — formato dependente de padding do
   compilador, e as duas cópias do header já tinham divergido. ADR-0002.
2. **Nada de bloqueio no laço de eventos.** `Module.cpp:139` tinha `sleep(10)`
   dentro do handler de comando, travando o processo inteiro.
3. **Nada de path hardcoded.** Configuração vem de arquivo/argv.
4. **Nada de IPC por arquivo com nome fixo.** O legado tinha um bug real:
   `verifyFile()` abria `sensor_data.json` (`Integration.cpp:173`) e
   `parseHealth()` abria `HealthData.json` (`Integration.cpp:110`) — a guarda
   checava um arquivo diferente do que era lido.
5. **Build em zero warnings** com `-Wconversion -Wsign-conversion`.
6. **Todo frame rejeitado é contado, nunca descartado em silêncio** — é a
   evidência do HLR-ADS-08.
7. **ADR escrito no momento em que o código materializa a decisão**, não depois.
