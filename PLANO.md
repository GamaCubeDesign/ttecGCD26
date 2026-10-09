# Plano de execução — TT&C CubeDesign 2026

Documento de trabalho. As decisões consolidadas ficam nos ADRs
(`docs/adr/`); aqui fica o que falta fazer e em que ordem.

**Marcos:** Design Package **27/09/2026** · UNSAM **24/11/2026**
**Prioridade atual (05/10):** hardware — a bancada da fase 2 e as medições
na Pi, pelo roteiro `docs/relatorios/2026-10-05-roteiro-testes-raspberry.html`.
Em paralelo, sem hardware: a troca de formatos, o TT&C seguindo o modo do
OBC, o ICD do enlace e a CI (decisões de 05/10, abaixo). Etapas 1 a 4 do
roteiro concluídas (07 e 09/10), a bancada dos rádios inclusive; progresso em
`docs/vv/bancada/`.

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
make test             # compila e roda a suíte; make check antes de um merge
```

### Fase 2 — concluída; bancada dos rádios feita em 09/10 (dias 3–4)

| Item | Onde | Evidência |
|---|---|---|
| Tabela de perfis e tempo no ar | `common/gama_lora.*` | bate no µs com o `lora_budget.py` (27 casos) |
| Payloads IPC | `common/gama_ipc.*` | vetores dourados |
| Transporte IPC | `flight/libipc/` | 10 000 frames em cada sentido entre dois processos, sem perda |
| Núcleo do `ttcd` | `flight/ttcd/link.c`, `tc_dispatch.c`, `tm_sched.c` | 188 verificações |
| Lado da ground do protocolo | `common/gama_gs_link.*` | 39 verificações |
| Simulação de canal, os dois lados | `tests/test_link_sim.c` | 63 verificações; números no ADR-0007 |
| Driver do SX1278 | `flight/radio/sx1278.c` | 92 verificações contra chip emulado |
| Barramento Linux (spidev + GPIO chardev) | `flight/radio/sx1278_linux.c` | compila; só roda no hardware |
| Backends de rádio | `flight/radio/radio_sx1278.c`, `radio_udp.c` | — |
| Configuração | `flight/ttcd/config.c` | 45 verificações |
| Daemon | `flight/ttcd/main.c` | teste de integração com o binário real (29) |
| Unidade systemd | `flight/ttcd/ttcd.service` | `systemd-analyze verify` |
| Ground de bancada | `tools/gs_cli/` | exercitada contra o `ttcd` real via UDP |
| Análise de tempo no ar | `tools/analysis/toa_from_log.py` | — |
| Makefile: build, testes, bancada, instalação e PC → Pi | `Makefile` | `make help`; `pi-sync` conferido contra um diretório local (não há Pi ligada) |
| Roteiros da bancada, um `ttcd` novo por passo | `tools/bench/` | `make bench` inteiro no rádio UDP: 6 passos, 28 min |
| ADRs | 0005, 0006, 0008, 0011 *Accepted*; 0007, 0012 *Proposed* | — |

15 suítes, zero warnings, limpo sob ASan e UBSan.

#### O que mudou em relação ao plano da fase 2

- **pigpio → interfaces do kernel** (ADR-0012). O descritor da linha DIO0 entra
  direto no epoll: sem thread, callback, daemon ou root.
- **O driver ficou em `flight/radio/`**, não em `flight/ttcd/drivers/`, porque
  a ground de bancada usa o mesmo.
- **O lado da ground do protocolo foi escrito agora**, em `common/`: a
  simulação precisava dele, e o ESP32 vai compilar o mesmo arquivo.
- **Simulação de canal acrescentada.** Achou dois defeitos de protocolo que eu
  não tinha previsto (170 colisões em 401 comandos antes da correção).
- **Bulk download adiado** (ADR-0007): o log bruto de 7,34 MiB levaria ~3,4 h
  em FAST. `BULK_START` responde `FAILED`.
- **`IPC_TRACKS` ganhou `index`/`count`**, para o `ttcd` saber quando tem um
  snapshot completo.
- **`age_ds` definido**: tempo até o início da transmissão (ADR-0007).

#### Falta para fechar a fase 2 — depende do hardware

- [x] Executar o teste de bancada de `flight/ttcd/README.md` — feito em
      09/10, os seis passos passaram (resultados na atualização de 09/10,
      abaixo)
- [x] Registrar em `docs/vv/bancada/2026-10-09/` — `notas.md` e os logs
- [ ] Revisar e aceitar o ADR-0007 e o ADR-0012 (estão *Proposed*) — a
      bancada rodou e os resultados estão nas seções de verificação deles;
      o passo 6 manteve o LBT em `header`. Falta a decisão da equipe

### Fase 3 — concluída em software; faltam as medições na Pi (2026-09-28)

| Item | Onde | Evidência |
|---|---|---|
| Leitura do SBS-1, com o "no solo" corrigido (`-1` é verdadeiro) | `flight/adsbd/sbs.c` | `test_sbs`: 64 verificações |
| Linha do registro de bordo, byte a byte a do protótipo | `flight/adsbd/ndjson.c` | as três linhas de que o `lora_budget.py` deriva os 160,4 B/linha, fixadas em `test_sbs` |
| Tabela de aeronaves e regras do retrato | `flight/adsbd/tracks.c` | `test_tracks`: 51 verificações, inclusive 20 aeronaves por 10 min a 4 msg/s |
| Núcleo do `adsbd`, sem chamadas de sistema | `flight/adsbd/core.c` | `test_adsbd_core`: 66 verificações |
| Configuração por arquivo e `-o`, com validação cruzada | `flight/adsbd/config.c` | `test_adsbd_config`: 35 verificações |
| Casca: supervisão do `dump1090`, SBS, IPC, teto do registro | `flight/adsbd/main.c` | `test_adsbd_integration`: o binário real contra `dump1090` e `ttcd` falsos, 46 verificações |
| A cadeia inteira sem hardware: SBS → `adsbd` → `ttcd` → rádio UDP → ground | `tests/test_adsb_chain.c` | 31 verificações: o rádio sobrevive ao `adsbd` morto; o `adsbd` reiniciado volta à hora da ground |
| `ttcd` reenvia a hora a quem se conecta depois do `SET_TIME` | `flight/ttcd/link.c`, `tc_dispatch.c` | `test_ttcd_core`: 199 verificações (eram 188) |
| Ground manda `SET_TIME` sozinha no contato | `tools/gs_cli` (`--auto-time on`) | exercitado no `make adsb-chain` |
| Referência de tempo | ADR-0009 (*Proposed*) | — |
| `dump1090` sem SDR, a partir de captura ou simulação | `tools/sbs_replay/` | — |
| Estimador de origem e destino do colega, trazido da branch `ADSB-ground` | `ground/host/ground-aeronaves/` | os 49 testes dele no CTest (`ground_aeronaves`), 1 pulado sem o banco local |
| Downlink → NDJSON do estimador | `ground/host/tracks_to_ndjson.py` | 7 testes, cada linha conferida pelo próprio `parser.py` do estimador (CTest `tracks_to_ndjson`) |
| Emulação do downlink sobre capturas reais, com comparação | `tools/analysis/downlink_emulation.py` | mesmas conclusões do estimador com 382 registros (a cada 5 s) e com 1894 mensagens (completo), também com 10% de perda — simulação sintética |
| Missão gravada ou simulada pela cadeia inteira | `make adsb-chain INPUT=...` | ver `flight/adsbd/README.md` |
| A mesma missão pela cadeia real, até o estimador da ground | `make adsb-chain`, simulador do colega | 1894 mensagens → 5682 linhas SBS gravadas a bordo → 125 frames de tracks, nenhum perdido → 386 registros na ground; o estimador chega às mesmas origens e destinos que com o fluxo completo — os destinos nos mesmos instantes, a origem 51 s antes; latência de bordo p95 de 2,0 s, medida pelo próprio `ttcd` no `TM_STAT` (a meta do data-budget §8, derivada do HLR-ADS-08, é p95 ≤ 5 s). Cenário sintético: 4 voos, 10 min, rádio UDP |
| Unidade systemd, sem root | `flight/adsbd/adsbd.service` | `systemd-analyze verify`; `DeviceAllow` a conferir na Pi |

23 suítes — 3406 verificações em C, 7 testes do adaptador e 49 do estimador
em Python —, zero warnings, limpo sob ASan e UBSan.

#### O que mudou em relação ao plano da fase 3

- **Busca linear, não hash, na tabela.** A 128 posições e ~80 mensagens/s são
  ~10 000 comparações por segundo; o custo está no `dump1090`, não aqui.
- **Retrato a cada 1 s para o `ttcd`**, que transmite o mais recente a cada 5 s:
  é a cadência que a simulação do ADR-0007 supôs (latência p95 de 3,2 s). Um
  retrato vazio também vai, para uma aeronave que sumiu não voltar ao ar.
- **A idade do registro é a da posição**, quando ela tem até 10 s; sem posição
  recente, o registro sai sem posição, datado pelo campo mais novo.
- **Tempo: monotônico + âncora da ground** (ADR-0009), com o formato da linha
  intacto. Dois achados: o `ttcd` não mandava a hora a um `adsbd` que se
  conectasse depois do `SET_TIME`; e a ground passou a mandar a hora sozinha
  no contato, como combinado na reunião de 28/09.
- **O `adsb_capture.c` lia errado o "no solo" do SBS** (`-1` é verdadeiro no
  formato BaseStation, conferido no `net_io.c` do dump1090-fa): toda
  aeronave no solo saía como desconhecida no NDJSON, e o estimador da ground
  perdia a âncora de decolagem.
- **Integração com o estimador da ground**, que não estava no plano: o
  estimador de origem e destino e o painel do colega consomem o mesmo NDJSON
  do registro de bordo; o adaptador faz o downlink chegar nele no mesmo
  formato, sem mudar o código dele. A branch `ADSB-ground` entrou por merge
  em 28/09, com a autoria preservada, e mudou para `ground/host/` — a pasta
  original tinha um espaço no nome.
- **O `--write-json` do `dump1090` saiu**: gravava um arquivo no SD por
  segundo que ninguém lia.

#### Falta para fechar a fase 3 — depende do hardware ou de decisão

- [ ] Instalar o `dump1090-fa` na Pi e medir (3.4)
- [ ] `IPC_STAT` com o indicador de `dump1090` vivo — decidido em 05/10:
      entra na troca de versão do protocolo
- [ ] Mensagem IPC ICAO → callsign para o `REQ_ROSTER` — idem
- [x] Revisar e aceitar o ADR-0009 — aceito em 05/10

### Decisões de 05/10/2026

Tomadas a partir dos relatórios de 28/09 e da integração com o OBC (versão de
02/10). O que muda o desenho vira ADR quando for implementado.

| # | Decisão | O que gera |
|---|---|---|
| 1 | Hardware primeiro: a bancada (2.7) e as medições na Pi (3.4) antes de qualquer fase nova | roteiro `docs/relatorios/2026-10-05-roteiro-testes-raspberry.html` |
| 2 | Uma troca de versão do protocolo, com três mudanças: temperatura da bateria no `TM_HK` (HLR-EPS-04), `dump1090` vivo no `IPC_STAT` e mensagem IPC ICAO → callsign, que destrava o `REQ_ROSTER` | ADR novo, vetores, data-budget |
| 3 | O modo do OBC dirige o TT&C: o `ttcd` repassa o modo ao `adsbd`, que só roda o `dump1090` em `MISSION_ADSB`, e começa e para os retratos pelo modo. `STREAM_START` e `STREAM_STOP` ficam como comando manual | ADR novo; `ttcd` e `adsbd` |
| 4 | Proposta ao OBC para `MISSION_DOWNLINK`: o roster com estatísticas (~0,4 KB); ao terminar, o `ttcd` manda `EV_TASK_DONE` ao OBC | depende do item 2 |
| 5 | Posição para o OBC: `SET_MODE(EV_TASK_DONE)` sai da sobrevivência e aborta qualquer prova, sem mudar o fio; a sobrevivência automática do EPS vale em qualquer modo | ICD OBC↔TT&C |
| 6 | ADR-0009 aceito; ADR-0007 e ADR-0012 só depois da bancada | — |
| 7 | Raspberry Pi OS de 32 bits, como o OBC; o código do TT&C é testado nele primeiro — **substituída em 07/10 por 64 bits** (abaixo) | `make check` na Pi |
| 8 | Lote de documentos: `AGENTS.md`, `README.md` e `requisitos.md` com a fase 3; erratas nos ADRs 0002 e 0004; ADR-0010 | — |
| 9 | CI com GitHub Actions rodando o `make check` | `.github/workflows/` |
| 10 | ICD do enlace agora, com o ESP32 decodificando os frames e mandando JSON pela UART, no formato da `gs_cli`; ICD OBC↔TT&C depois da conversa com o OBC | `docs/icd/` |
| 11 | Banco de aeroportos da região da UNSAM, gerado pelo importador do estimador e versionado | com o autor do estimador |
| 12 | As 8 perguntas do `requisitos.md` §8 vão para a organização agora | — |
| 13 | Custo (HLR-COST-01), consumo por modo (HLR-EPS-03), requisitos derivados e plano de V&V ficam com o TT&C, depois do hardware | fase 6 |

Ficam para depois: a chave do evento nos logs (`"ev"` na `gs_cli`, `"event"`
no `ttcd` e no `adsbd`), o `memcmp` sobre padding no `test_tm` e a frase sobre
o CA 5 no data-budget. A missão secundária (HLR-ADS-06) foi decidida fora do
TT&C e do OBC.

### Atualização de 07/10/2026

Primeira sessão com a Pi. O registro completo, com os logs, está em
`docs/vv/bancada/` (o `README.md` tem o progresso por etapa do roteiro).

- **64 bits, não 32.** O time do OBC escolheu o Raspberry Pi OS de 64 bits;
  substitui a decisão 7 de 05/10. A Pi da bancada roda Debian 13 (trixie),
  arm64, gcc 14.2.
- **Etapas 1 e 2 do roteiro concluídas.** A Pi está preparada (SPI ligado,
  ferramentas, usuário `gama`) e o `make check` passa nela: 23 de 23 testes
  nos dois builds, zero warnings, 71 de 71 citações.
- **O ASan não roda na Pi.** O kernel dá a cada processo um espaço de
  endereços de 39 bits, e o ASan do gcc 14 em arm64 precisa de 47. Na Pi o
  `make asan` passa a usar só o UBSan (`SANITIZERS=`); no PC nada muda
  (commit `897e961`).
- **O `memcmp` sobre padding no `test_tm` foi corrigido** (commit `b12e3f4`):
  deixou de ser "para depois" porque reprovou o teste na Pi, compilado com
  UBSan. Os grupos do HK e do STAT comparam campo a campo; o codec não
  mudou.
- **Rede da bancada:** o PC como ponto de acesso Wi-Fi (NetworkManager
  *shared*), repassando à Pi a internet cabeada; procedimento no README da
  bancada.

Próximo: a etapa 3 (ligar os dois RA-02) e a bancada dos rádios (2.7).
Pendente da etapa 2: um erro do UBSan hoje é impresso mas não reprova o
teste (falta `-fno-sanitize-recover=undefined`).

### Atualização de 09/10/2026 — a bancada dos rádios (2.7)

Etapas 3 e 4 do roteiro concluídas; detalhe em
`docs/vv/bancada/2026-10-09/notas.md`.

- **Fiação:** na protoboard nenhum dos dois chips respondia no SPI; numa
  placa perfurada, os dois leem RegVersion `0x12`.
- **Defeito achado no driver** (commit `72deaa5`): no chip real, o bit
  *RX on-going* do RegModemStat fica ligado o tempo todo em recepção
  contínua. O LBT o contava como canal ocupado e não deixava nada ir ao ar.
  O teste modelava o canal livre como `0x00`; agora tem o `0x04` medido.
- **Os seis passos passaram**, a 80 cm e 2 dBm: tempo no ar dentro de 1,9%
  do modelo; nenhuma perda em 1000/1000/200 PINGs (NOMINAL/FAST/SAFE) nem em
  2220 frames de downlink; 7 de 7 trocas de taxa; o ACK perdido recuperado
  em 23,2 s (a simulação dava 23,4 s); LBT `header` e `preamble` com 0% de
  retransmissão — fica `header`. Resposta a telecomando: 81 ms em FAST,
  276 ms em NOMINAL, 2,7 s em SAFE (HLR-COMM-01).
- Os resultados entraram nas seções de verificação dos ADRs 0007 e 0012,
  que seguem *Proposed* até a equipe revisar.

Próximo: a etapa 5 (o SDR e o `dump1090-fa`, PLANO 3.4).

### Dívida imediata

~~Três ADRs citados por documentos aceitos não existiam~~ — **quitada em
2026-09-19** (0007, 0008 e 0011 escritos).

As lacunas encontradas no levantamento dos requisitos estão em
`docs/requisitos.md` §7, com dono e prazo; não são repetidas aqui.

---

## Fase 2 — `ttcd` e o enlace (dias 3–4)

Objetivo: **dois RA-02 na bancada trocando frames**, com troca de taxa
funcionando e reversão por timeout exercitada.

### 2.1 `flight/libipc/` — transporte AF_UNIX SOCK_SEQPACKET

- [x] `ipc_server_open(path)` — socket, bind, listen; remove só um socket antigo, nunca outro tipo de arquivo
- [x] `ipc_server_accept()` — múltiplos clientes (`adsbd`, `obc`, ferramentas)
- [x] `ipc_recv()` — `MSG_DONTWAIT`; `IPC_EMPTY` quando vazio, truncamento reportado
- [x] `ipc_send()` — `EAGAIN` vira `IPC_EMPTY`, sem bloquear
- [x] Identificação de peer via `GAMA_FRAME_IPC_HELLO` (na casca do `ttcd`)
- [x] Reuso do mesmo codec: payload IPC **é** um `gama_frame`

**Aceite:** dois processos de teste trocam 10 000 frames sem perda nem
bloqueio; `recv` em socket vazio retorna imediatamente.

### 2.2 `flight/radio/sx1278.*` — porte do driver *(feito; ficou em `flight/radio/`)*

Base: `ultima_missao/satellite/LoRa.c` (utilizável, 463 linhas).

- [x] Portar mantendo a API de registrador; descartar `Moden.cpp`
- [x] `sx1278_set_profile(gama_rate_profile_t)` — escreve `REG_MODEM_CONFIG_1/2`
      **e recalcula o bit LDRO em `REG_MODEM_CONFIG_3`**.
      Nota: o driver legado só reescreve LDRO dentro de `LoRa_send()`/
      `LoRa_receive()`. Uma troca de taxa que mexa só em CONFIG_1/2 deixa LDRO
      obsoleto até o próximo envio. Verificar por leitura de registrador.
      SF12/BW125 → símbolo 32,8 ms → LDRO **on**. SF9 → 4,1 ms → off.
- [x] **Substituir `sleep(Tpkt/1000 + 1)` de `Moden.cpp:91` por DIO0 TxDone.**
      Em vez de callback pigpio + `eventfd`: o descritor da linha DIO0 do
      GPIO chardev entra direto no epoll (ADR-0012).
      No SAFE esse sleep arredonda até 1 s de air time fora por pacote.
- [x] Leitura de RSSI e SNR por pacote, para o log da ground
- [x] Half-duplex explícito: transmitir e reconfigurar são recusados durante uma transmissão

**Aceite:** ToA medido em bancada bate com `lora_budget.py` dentro de 5% nos
três perfis; LDRO confirmado por leitura de registrador após troca de perfil.

### 2.3 `flight/ttcd/link.*` — máquina de estados do enlace

- [x] Modos `IDLE`, `STREAM`, `SAFE` reportados no HK; `BULK` adiado (ADR-0007)
- [x] Prioridade: ACK > HK > STAT > snapshot > beacon
- [x] Timer de reversão de taxa: 20 s no satélite, 30 s na ground
- [x] Timeout de contato → `SAFE` + beacon, com retorno automático comandado pela ground
- [x] Contadores de `seq` por direção (evidência do HLR-ADS-08)

### 2.4 `flight/ttcd/tc_dispatch.*`

- [x] Validar tamanho de argumento via `gama_tc_arg_len()` antes de despachar
- [x] Responder `TC_ACK` com `gama_ack_status_t` correto em todo caminho
- [x] `SHUTDOWN` exige `GAMA_TC_SHUTDOWN_MAGIC`
- [x] Traduzir `GAMA_TC_SET_MODE` → `GAMA_FRAME_IPC_TC_EVENT` para o OBC
- [x] **Nenhum handler pode bloquear.** O pecado do legado
      (`Module.cpp:139,145,149` — `sleep(10)` dentro do handler) não se repete.

### 2.5 `flight/ttcd/main.c` — laço epoll

- [x] `epoll` sobre: listen fd, fds de cliente, `timerfd` (escalonador de TM),
      descritor da linha DIO0
- [x] Escalonador: snapshot no período comandado, HK 10 s, STAT 30 s
- [x] `SIGTERM`/`SIGINT` por `signalfd` — saída limpa, sem handler assíncrono
- [x] **Zero `sleep()` no processo inteiro** (a única espera é o pulso de reset do rádio, antes do laço)

### 2.6 `flight/ttcd/config.*`

- [x] Configuração por arquivo + override por argv. Nada de path hardcoded —
      o legado tinha `/home/pedro/adsb/...` compilado dentro do binário
- [x] Unidade systemd em `flight/ttcd/ttcd.service`, sem root

### 2.7 Teste de bancada

Procedimento completo em `flight/ttcd/README.md`. `make bench` roda os passos
1 a 6 a partir dos roteiros de `tools/bench/`, sobre `tools/gs_cli` e
`tools/analysis/toa_from_log.py`, e grava os logs em `docs/vv/bancada/`.

- [x] Dois RA-02 a ~1 m, potência de 2 dBm — 80 cm, 09/10
- [x] PER sobre 1000 frames em cada perfil — 0 perdas (200 em SAFE)
- [x] Troca `SAFE ↔ NOMINAL ↔ FAST` nos dois sentidos — 7 de 7
- [x] **Caso do ACK perdido**: a troca aconteceu mas o ACK sumiu — a reversão
      tem que recuperar sozinha — recuperou em 23,2 s
- [x] Registrar resultados em `docs/vv/` — `docs/vv/bancada/2026-10-09/`
- [ ] O mesmo enlace na distância da missão, com as antenas da equipe: a
      80 cm o PER é zero e não diz nada sobre o enlace real

### ADRs desta fase

- [x] `0005-process-architecture-and-fault-isolation.md` (*Accepted*)
- [x] `0006-obc-ttec-ipc-transport.md` (*Accepted*)
- [x] `0007-ota-frame-format-and-arq.md` (*Proposed* — aceitar depois da bancada)
- [x] `0012-hardware-access-through-kernel-interfaces.md` (*Proposed*, não previsto;
      aceitar depois da bancada)

---

## Fase 3 — `adsbd` e o payload (dias 5–6)

Objetivo: **track table alimentada por SDR real**, com CPU medida na Pi.

### 3.1 Porte do `adsb_capture.c` *(feito: `flight/adsbd/`)*

O núcleo é bom: `fork`/`execv` sem shell, remontagem de linha SBS, timestamp
por mensagem, append + `fflush`. Ajustes:

- [x] Configuração por arquivo/argv (hoje `/home/pedro/*` está hardcoded)
- [x] **`CLOCK_MONOTONIC` + uma âncora de wall-clock** — a Pi Zero 2 W não tem
      RTC. A âncora vem do `SET_TIME` da ground, não do boot (ADR-0009)
- [x] Supervisão do `dump1090`: detectar morte, reiniciar, contar reinícios
      (`gama_stat_t.dump1090_restarts`)
- [x] Remover o `sleep(2)` da linha 375; laço de retry no connect
- [x] Teto de tamanho no NDJSON — filesystem cheio derruba o `ttcd` junto
- [x] *(achado)* `on_ground` do SBS: `-1` é verdadeiro

### 3.2 `flight/adsbd/tracks.*` — tabela de pistas *(feito)*

- [x] Capacidade fixa, sem `malloc` no caminho quente — busca linear, não
      hash: 128 posições bastam (ver "O que mudou" acima)
- [x] Merge por tipo de mensagem: posição, velocidade e identificação chegam
      separadas — cada campo com o instante em que chegou (`GAMA_TRACK_F_*`)
- [x] Expiração por idade (aeronave que sumiu sai da tabela)
- [x] `tracks_snapshot(out, max)` → array de `gama_track_t`

### 3.3 `flight/adsbd/ipc_client.*` *(feito, em `main.c` e `core.c`)*

- [x] Conectar no `ttcd`, `HELLO` (papel `adsbd`), reconectar se cair
- [x] Enviar `IPC_TRACKS` (cabeçalho `epoch_ms`/`index`/`count`, idades
      medidas até `epoch_ms`) e `IPC_STAT` — formatos em `common/gama_ipc.h`
- [ ] Acrescentar ao `IPC_STAT` um indicador de `dump1090` vivo: o bit
      `GAMA_HK_F_DUMP1090_UP` do HK depende dele e hoje nunca é ligado.
      Muda um payload fixado pelos vetores — entra na troca de versão
      decidida em 05/10
- [ ] Mensagem IPC com a tabela ICAO → callsign, para o `REQ_ROSTER` (hoje
      responde `FAILED`); o `adsbd` já guarda o callsign

### 3.3b Ponte com a ground *(não estava no plano; feito)*

- [x] `tools/sbs_replay/` — o `dump1090` sem SDR
- [x] `ground/host/ground-aeronaves/` — o estimador do colega (branch
      `ADSB-ground`), com os testes dele no CTest
- [x] `ground/host/tracks_to_ndjson.py` — o downlink no formato do estimador
- [x] `tools/analysis/downlink_emulation.py` — o downlink emulado sobre
      capturas reais, com `--compare`
- [x] `make adsb-chain INPUT=...` — a cadeia inteira, em tempo real

### 3.4 Medições na Pi — **o maior risco não quantificado**

- [ ] CPU do `dump1090` a 2,4 MS/s (`pidstat -p <pid> 1`)
- [ ] Se saturar um core: o `dump1090-fa` v11 roda o RTL-SDR fixo a 2,4 MS/s
      (`Modes.sample_rate` no código), então cair para 2,0 MS/s exigiria
      outro decodificador — estudar com o número medido
- [ ] Afinidade de CPU: `dump1090` e `ttcd` em cores distintos
- [ ] **Varredura de ganho.** `adsb_capture.c:46` usa `GAIN "-10"`. No
      dump1090-fa v11, `-10` liga o AGC do sintonizador; qualquer outro valor
      é em dB, arredondado para o passo mais próximo (`sdr_rtlsdr.c`). O
      ambiente de teste é SDR a curta distância → sinal forte, AGC pode
      saturar
- [ ] Taxa de decodificação contra cenário conhecido
- [ ] Registrar tudo em `docs/budgets/data-budget.md` §9

### ADRs desta fase

- [x] `0008-adsb-receive-chain-ownership.md` (*Accepted* em 2026-09-19) —
      resolve o conflito com o `sdr.c` da branch `feat/modulo-aocs` do OBC
- [x] `0009-time-reference-and-synchronisation.md` (*Accepted* em 2026-10-05)
- [ ] `0010-onboard-storage-and-data-retention.md` — a política já está no
      data-budget §7 e implementada (`ndjson_max_bytes`); falta o registro

---

## Fase 4 — Ground station e entrega do ICD (dias 7–8)

### 4.1 `ground/esp32/` — ESP-IDF

- [ ] Projeto IDF puxando `common/` via `EXTRA_COMPONENT_DIRS` — traz de graça
      o lado da ground do protocolo (`gama_gs_link`), já testado na simulação
- [ ] Driver SX1278: o `flight/radio/sx1278.c` só fala com um `sx1278_bus_t`;
      basta implementar o barramento com o SPI e o GPIO do ESP-IDF (pinos do
      `groundStation.ino:6-11` como base)
- [ ] Bridge UART para o host
- [ ] **Build de teste com `tests/test_vectors.c` para xtensa.** Passar nos
      dois alvos é o que demonstra que Pi e ESP32 geram bytes idênticos — é a
      afirmação sobre a qual o ADR-0002 se apoia

### 4.2 `ground/host/` — CLI e dashboard

- [ ] CLI: enviar TC, receber TM, log NDJSON com timestamp de recepção.
      O `tools/gs_cli` já faz isso em C, na bancada; o formato de saída
      dele (um JSON por evento, frame bruto em hex) serve de modelo para o
      ESP32 enviar pela UART — o host em Python consome JSON e não precisa de
      um segundo codec (ADR-0002)
- [ ] Tabela ao vivo de aeronaves — o painel de `ground/host/ground-aeronaves`
      já faz; falta ligá-lo ao downlink ao vivo (hoje só o `make adsb-chain`
      leva o downlink até o estimador)
- [x] Adaptador downlink → NDJSON do estimador (`ground/host/tracks_to_ndjson.py`)
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

- [x] `0011-documentation-language.md` (*Accepted* em 2026-09-19)

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
