# Plano de execução — TT&C, OBC e ground station, CubeDesign 2026

Documento de trabalho. As decisões consolidadas ficam nos ADRs
(`docs/adr/`); aqui fica o que falta fazer e em que ordem. O histórico das
fases 1 a 3 e da bancada, item a item, está na versão anterior deste arquivo
(`git show ddb5c7d:PLANO.md`) e em `docs/vv/bancada/`.

**Marcos:** CDR entregue em **11/10/2026** · testes da equipe até **20/11** ·
UNSAM **24/11/2026**. Em 24/10 faltam **4 semanas e meia**.

**Escopo desde 20/10:** o Bruno assume o **OBC** (software e requisitos),
além do TT&C, do payload ADS-B e da ground station. O software do OBC passa a
morar neste repositório, em `flight/obcd/`, reescrito no padrão do `ttcd` e do
`adsbd`; o `../obc` fica congelado como referência, como a `ultima_missao/`.

**Objetivo agora (24/10): terminar o firmware o quanto antes** — satélite,
OBC e ground rodando ponta a ponta em software e na bancada até ~07/11 —, para
que a chegada da placa do OBC seja só configurar pinos e medir, sem reescrever
código.

**A referência é o CDR entregue** (versão 4, 220 p.; cópia local em
`docs/cdr/`, fora do git — a fonte de verdade é o Overleaf). O que ele
promete e o plano tem de cumprir está em cinco tabelas:

| Tabela do CDR | O quê | Etapa |
|---|---|---|
| 32 (§9.3) | Critérios de aceite do OBC, OBC-1 a OBC-9 — todos *planned*, menos OBC-2 | 2, 5 |
| 55 (§11.3.7) | O que o TT&C faz em cada modo do OBC | 1 |
| 65 (§11.4.5) | Requisitos derivados TTC-01 a TTC-16 | 1–6 |
| 66 (§11.4.6) | Matriz de rastreabilidade, com os critérios ainda em P | 6 |
| 67 (§11.4.7) | Validação restante, em ordem do que pode mudar o projeto | 6 |

As placas como foram fabricadas (painéis de 08/10) estão em `hardware/`, uma
pasta por placa (`hardware/README.md`).

---

## Estado em 24/10

**Pronto e testado em software** (fases 1 a 3): `common/` (codec, TM/TC, IPC,
perfis LoRa, lado da ground do protocolo), `flight/libipc/`, `flight/ttcd/`,
`flight/radio/` (SX1278 + barramento Linux + rádio UDP), `flight/adsbd/`,
`tools/gs_cli/`, `tools/bench/`, `tools/sbs_replay/`, `ground/host/` (adaptador
do downlink e estimador do colega). 23 suítes no CTest, zero warnings, limpo
sob ASan e UBSan.

**Validado em hardware** (07 e 09/10, `docs/vv/bancada/`): `make check` na Pi
(Debian 13 arm64); os seis passos da bancada dos rádios a 80 cm (tempo no ar
dentro de 1,9%, nenhuma perda, ACK perdido recuperado em 23,2 s);
`dump1090-fa` a 21,9% de um núcleo; `adsbd` com o SDR real; a cadeia SDR →
`adsbd` → `ttcd` → LoRa → ground, sem `TM_TRACKS` (nenhuma posição no ar).

**Não começado:** `flight/obcd/`, `ground/esp32/`, a CLI do operador,
`docs/icd/`, a CI.

**Hardware:** as placas fabricadas têm defeitos de ligação que impedem o stack
de funcionar (lista em `docs/relatorios/2026-10-22-validacao-obc.html`). A
equipe está refazendo **só a placa do OBC**; a revisão precisa cobrir A1
(8 V no pino 38), A2 (LoRa), A3 (I²C trocado no layout), A5/A6 (resistores
de base dos TIP120) e A7 (pull-up do DS18B20); proposta de 24/10 para
ligar o INA219 a um I²C próprio da Pi, enviada à equipe. Ainda sem data.

**ADRs:** 0007 e 0012 *Proposed*, com a evidência da bancada; 0010 por
escrever.

---

## Decisões de 22 a 24/10

| # | Decisão | Onde entra |
|---|---|---|
| D1 | `PRE_TEST` é o centro: todo modo, inclusive `ENV_SURVIVAL`, só é alcançado a partir dele e volta a ele | 2.1 |
| D2 | Cada tarefa gera o seu `EV_TASK_DONE`: implantação → o próprio OBC; AOCS → o ESP32; downlink → o `ttcd`; sobrevivência → telecomando | 2.1, 1.2 |
| D3 | Burn-wire: GPIO ligado por 5 s (configurável), uma tentativa por entrada no modo, só na entrada de `BASIC_INTERMEDIATE`, nunca no boot; o GPIO desliga se o processo morrer | 2.1, 2.3 |
| D4 | Aquecedores comandados pelo software do OBC (não há termostato nas placas): liga abaixo de 10 °C, desliga acima de 12 °C, em todos os modos; sensor falhou → desligado, com registro | 2.1, 2.3 |
| D5 | Reinício no meio da missão volta a `PRE_TEST`, sem retomar os 600 s | 2.1 |
| D6 | `IPC_TELEMETRY` a cada 1 s; valor reservado para "sem dado" | 1.1, 2.2 |
| D7 | Log do OBC em JSON por linha, no formato dos logs do `ttcd`/`adsbd` (tempo monotônico + âncora da ground) | 2.2 |
| D8 | `obcd` como serviço systemd, sem root, `Restart=always`, watchdog de hardware da Pi | 2.2, 4 |
| D9 | Leitura do DS18B20 (~750 ms) numa thread própria; o laço principal nunca espera | 2.3 |
| D10 | Contrato com o AOCS por **I²C**; o firmware do ESP32 entra no repositório quando estiver pronto | 2.4 |
| D11 | O comando remoto das provas (REG-07) sai da **ground ESP32** | 3 |
| D12 | Hardware corrigido só na placa do OBC; as outras ficam como fabricadas | 5 |

---

## Prioridade

O que dá para fazer agora é o firmware; o que depende do stack de voo é
validá-lo. A ordem:

| # | Quando | O quê | Pronto quando |
|---|---|---|---|
| 1 | 24–28/10 | **Protocolo v2** + TT&C dirigido pelo modo + 433,92 MHz/14 dBm + ADR | `make check` verde; o `make adsb-chain` só faz streaming em `MISSION_ADSB` |
| 2 | 27/10–02/11 | **`obcd`**: núcleo, casca, IPC, termostato, burn-wire, drivers com simulação, contrato I²C do AOCS | um `SET_MODE` da ground percorre todos os modos da Fig. 44 no rádio UDP, com o `ttcd` e o `adsbd` reais, registrado no log do OBC |
| 3 | 31/10–07/11 | **Ground ESP32** (é ela que manda o comando remoto) + CLI mínima em Python | a ESP32 faz os seis passos da bancada contra a Pi; vetores idênticos nas duas pontas |
| 4 | em paralelo, ½ dia | **CI** + `make provision` da imagem de voo | o cartão da Pi sai pronto de um comando |
| 5 | quando a placa chegar | **Bring-up** pela lista de validação | todos os itens A marcados |
| 6 | depois | AOCS real, enlace na distância, tráfego, ensaio de 10 min, ambiental | Tabela 67 do CDR |

**Se apertar, corta primeiro:** o painel do estimador ao vivo, os ICDs
formais, o aceite dos ADRs 0007 e 0012, o banco de aeroportos. **Não corta:**
protocolo v2, `obcd`, ground ESP32, provisionamento.

---

## Etapa 1 — Protocolo v2 e TT&C dirigido pelo modo (24–28/10)

É o contrato que o `obcd` vai falar; fazer o OBC antes seria escrevê-lo duas
vezes. Decisões 2, 3 e 4 de 05/10.

### 1.1 Troca de versão do protocolo (`GAMA_FRAME_VERSION` 1 → 2)

- [ ] Temperatura da bateria no `TM_HK` (25 → 27 B) (HLR-EPS-04, TTC-06,
      OBC-7) — revisar `data-budget.md` (+120 B em 10 min)
- [ ] Valor reservado de "sem dado" nos campos do `IPC_TELEMETRY` e do `TM_HK`
      (D6)
- [ ] Indicador de `dump1090` vivo no `IPC_STAT`, que liga o bit
      `GAMA_HK_F_DUMP1090_UP` (hoje nunca ligado)
- [ ] Mensagem IPC `adsbd` → `ttcd` com a tabela ICAO → callsign, que destrava
      o `REQ_ROSTER` (hoje `FAILED`)
- [ ] Mensagem IPC `ttcd` → `adsbd` com o modo do OBC
- [ ] `make vectors`, diff revisado byte a byte; ADR novo da troca de versão

### 1.2 O modo do OBC dirige o `ttcd` e o `adsbd` (Tabela 55)

- [ ] `ttcd` repassa ao `adsbd` cada `IPC_MODE` do OBC; o `adsbd` só roda o
      `dump1090` em `MISSION_ADSB` (tentando a cada 1 s até abrir o SDR)
- [ ] Retratos a cada 5 s e estatísticas a cada 30 s começam e param com o
      modo; `STREAM_START`/`STREAM_STOP` ficam como comando manual (TTC-07,
      GEN-03.3)
- [ ] `MISSION_DOWNLINK`: o `dump1090` para, o `ttcd` manda o roster e as
      estatísticas finais (260 B, 1,4 s no ar) e então `EV_TASK_DONE` ao OBC
      (D2)
- [ ] Sem OBC conectado, o HK reporta o modo como desconhecido e o enlace segue
- [ ] ADR novo (TT&C dirigido pelo modo); testes de integração atualizados

### 1.3 Frequência e potência do CDR (TTC-01, TTC-03)

- [ ] 433,92 MHz em `common/gama_lora.h` (hoje 433,000 MHz), no `test_sx1278`
      e no `lora_budget.py`
- [ ] 14 dBm de voo, 2,15 dBi e 2,3 dB de perdas no `lora_budget.py`, no
      `link-budget.md` e no `ttcd.conf.example` (hoje 20 dBm)
- [ ] ADR que substitui a frequência e a potência do ADR-0004
- [ ] `data-budget.md` §8 com a latência máxima de 10 s (TTC-09); §9 com a CPU
      medida (21,9%) e sem o recuo de 2,0 MS/s

---

## Etapa 2 — OBC: `flight/obcd/` (27/10–02/11)

Reescrito, não portado. Do `../obc` fica: os enums de modos e eventos do
`fsm.h` (já fixados em `common/gama_tc.h`), as transições da Figura 44 do
CDR e a lógica de leitura do INA219 como referência. Sai: o `nanosleep` no
laço (regra 2), o wiringPi (ADR-0012: interfaces do kernel), o
`obc_mission.jsonl` fixo (regra 4), o `sdr.c` (ADR-0008), o `radio_init()`
que derruba o OBC quando falha e o formato de log em português (D7).

### 2.1 Núcleo puro (ADR-0005: sem chamadas de sistema)

- [ ] Máquina de estados da Figura 44 com `PRE_TEST` como centro (D1);
      `EV_TC_*` vindos do `SET_MODE`; o que não vale no modo atual é recusado
      e registrado
- [ ] Fim de tarefa por modo (D2); janela da missão: `EV_ADSB_TIMEOUT` 600 s
      depois de entrar em `MISSION_ADSB`, ou antes por `EV_TC_DOWNLINK`
      (OBC-6)
- [ ] Burn-wire na entrada de `BASIC_INTERMEDIATE` (D3), seguido de
      `EV_TASK_DONE`
- [ ] Termostato com histerese 10 → 12 °C em todos os modos, sensor em falha
      → desligado (D4)
- [ ] Boot e reinício em `PRE_TEST` (D5)
- [ ] Toda transição no log, com tempo monotônico e âncora (OBC-5, OBC-9)
- [ ] Testes do núcleo em tempo virtual, cobrindo cada aresta da Figura 44

### 2.2 Casca: epoll, como o `ttcd`

- [ ] `timerfd` de 1 s; cliente IPC do `ttcd` (`HELLO`, `IPC_MODE`,
      `IPC_TELEMETRY` a cada 1 s; recebe `IPC_TC_EVENT` e `IPC_TIME_SET`),
      reconectando se o `ttcd` cair
- [ ] Log em JSON por linha (D7); configuração por arquivo + `-o`; unidade
      systemd sem root, `Restart=always`, watchdog (D8, OBC-1)
- [ ] Teste de integração com o `ttcd` e o `adsbd` reais e o rádio UDP: o
      `SET_MODE` da ground vira transição no log do OBC dentro do limite + 1 s
      (OBC-3, COMM-01.4), em todos os modos — substitui o cliente substituto
      dos testes atuais

### 2.3 Drivers (interfaces do kernel), testados com simulação

- [ ] INA219 por `i2c-dev`: tensão e corrente (OBC-7). Mede o consumo do
      satélite, não a carga da bateria (A9)
- [ ] DS18B20 pelo 1-Wire do kernel (`w1_therm`) numa thread própria (D9); em
      teste, um arquivo falso no lugar do `w1_slave`
- [ ] Burn-wire (GPIO22) e aquecedores (GPIO23) por GPIO chardev; em teste, o
      `gpio-sim` do kernel do PC
- [ ] Pinos só no arquivo de configuração: os da placa nova do OBC entram
      quando ela existir

### 2.4 Contrato I²C com o AOCS

- [ ] Documento do contrato (endereço do ESP32, mapa de registradores,
      comandos, atitude, fim de tarefa, timeout), para o firmware do ESP32
      seguir
- [ ] Lado do OBC, com um ESP32 simulado; só em `ADVANCED_AOCS`
- [ ] Esquema de posse do barramento compartilhado (pinos 10/12): fora do
      `ADVANCED_AOCS`, o ESP32 é só escravo e a Pi é a única mestre; ao entrar
      no modo, a Pi manda o comando e larga o barramento, o ESP32 vira mestre
      do BMI088 até terminar e volta a escravo; a Pi lê o resultado depois de
      um prazo máximo, com checksum, e gera `EV_TASK_DONE` mesmo sem resposta
- [ ] INA219 num I²C próprio da Pi (I²C0, GPIO0/1), fora do barramento
      compartilhado, para a tensão e a corrente continuarem durante o AOCS —
      **proposta enviada à equipe da PCB, aguardando resposta (Q2)**:
      `docs/relatorios/2026-10-24-proposta-i2c-ina219-obc.html`. Se for
      recusada, o INA219 fica "sem dado" durante o `ADVANCED_AOCS`; nos dois
      casos o firmware só muda o número do barramento na configuração

### 2.5 Documentos

- [ ] ADR do OBC: processo próprio no repositório, núcleo puro + casca epoll,
      a thread do 1-Wire, o I²C com o AOCS, o que foi descartado do `../obc`
- [ ] `AGENTS.md`: o OBC deixa de ser "outro repositório, não editar"
- [ ] `docs/icd/obc-ttec-icd.md` — o CDR cita o ICD; agora documenta a
      interface entre dois processos nossos (corta se apertar)

---

## Etapa 3 — Ground station (31/10–07/11)

A ground ESP32 manda o comando remoto das provas (D11): está no caminho
crítico. A Pi com um RA-02 e a `gs_cli` continua como reserva.

### 3.1 `ground/esp32/` — ESP-IDF (§11.3.8 do CDR)

- [ ] Projeto IDF puxando `common/` como componente: traz o lado da ground do
      protocolo (`gama_gs_link`), já testado na simulação
- [ ] Barramento do `sx1278_bus_t` com o SPI e o GPIO do ESP-IDF; o driver
      `flight/radio/sx1278.c` é o mesmo
- [ ] Telecomandos recebidos pela serial; um JSON por evento de volta, no
      formato da `gs_cli` (frame bruto, RSSI, SNR, hora de chegada; ACK, troca
      de perfil, perda)
- [ ] `tests/test_vectors.c` compilado para o ESP32: mesmos bytes nas duas
      pontas (TTC-16)
- [ ] Os seis passos da bancada com a ESP32 no lugar da `gs_cli`

### 3.2 `ground/host/` — CLI do operador

- [ ] Programa em Python: envia telecomandos (`SET_MODE` de cada prova),
      grava o log de eventos, alimenta o adaptador `tracks_to_ndjson.py`
- [ ] Painel do estimador ao vivo (corta se apertar)

### 3.3 `docs/icd/ota-protocol-icd.md`

- [ ] ICD do enlace, incluindo o JSON da serial (corta se apertar)

---

## Etapa 4 — CI e imagem de voo (em paralelo, ½ dia)

- [ ] CI com GitHub Actions rodando o `make check` (decisão 9 de 05/10)
- [ ] UBSan reprovando o teste: `-fno-sanitize-recover=undefined` no
      `make asan`
- [ ] `make provision`: `dump1090-fa` e o repositório da FlightAware, serviço
      dele desabilitado, blacklist do `dvb_usb_rtl28xxu`, `lighttpd`
      desligado, 1-Wire e I²C ligados no `config.txt`, os três serviços
      (`ttcd`, `adsbd`, `obcd`); testado na Pi da bancada

---

## Etapa 5 — Bring-up da placa nova do OBC (quando chegar)

Pela lista `docs/relatorios/2026-10-22-validacao-obc.html`, nesta ordem:

- [ ] Energia (A1): 8 V no pino 33, 5 V e 3,3 V da EPS
- [ ] GPIOs no boot (A8): nenhum TIP120 conduz antes do comando
- [ ] Rádio (A2): RegVersion `0x12`; pinos no `ttcd.conf`
- [ ] I²C (A3): `i2cdetect` vê o INA219 e, com o AOCS, o BMI088
- [ ] Sensores e atuadores (A5–A7, A9): DS18B20, INA219 contra multímetro,
      corrente do burn-wire e dos aquecedores, queda do 5 V
- [ ] Boot do cartão de voo até `PRE_TEST` sem intervenção (OBC-1)
- [ ] Registrar em `docs/vv/bancada/`

---

## Etapa 6 — Validação e ensaio

Na ordem da Tabela 67 do CDR: primeiro o que pode mudar uma decisão.

- [ ] **AOCS real**: o firmware do ESP32 no repositório, o contrato I²C no
      stack, `ADVANCED_AOCS` de ponta a ponta
- [ ] **Enlace na distância da missão**, com as antenas da equipe, 14 dBm,
      antena recolhida e aberta: PER sobre 1000 frames por perfil
      (COMM-03.3, ADS-08.2)
- [ ] **Convivência LoRa × ADS-B:** taxa de mensagens com o LoRa a 14 dBm
      ligado e desligado e a potência na entrada do SDR (≤ 0 dBm, R3). Decidir
      entre manter, bloquear pelo IPC (troca de versão, ADR) ou filtro de
      1090 MHz (ADS-08.1)
- [ ] **SDR com tráfego:** varredura de ganho e CPU com ~20 aeronaves ou fonte
      conhecida (ADS-01.2)
- [ ] **Cadeia com tracks descendo** pelo LoRa (COMM-02.2, ADS-07.2)
- [ ] **OBC no stack:** todas as transições pela ground ESP32, burn-wire com o
      nylon, termostato no frio (OBC-4, -5, -7)
- [ ] Erro da âncora contra evento conhecido (ADS-07.3); portadora de
      433,92 MHz lida de volta no registrador (COMM-03.5)
- [ ] **Ensaio de 10 min**: cenário conhecido, 20 aeronaves, `dump1090` morto
      uma vez, ground desligada 2 min — as métricas do HLR-ADS-08:

| Métrica | Alvo |
|---|---|
| Mensagens decodificadas enquanto o LoRa escuta | ≥ 95% |
| Perda no downlink (lacunas de `seq`) | ≤ 5% |
| Latência recepção → início da TX | p95 ≤ 5 s, máx. ≤ 10 s |
| Latência recepção → ground | p95 ≤ 7 s, máx. ≤ 12 s |
| Aeronaves simultâneas | 20 |

- [ ] Ambiental: telemetria antes, durante e depois (OBC-8, ENV-02.1)
- [ ] Atualizar `docs/vv/bancada/`, a matriz (P → S/H) e `docs/requisitos.md`

---

## Fechamento (contínuo; 17–23/11 de folga)

- [ ] Aceitar os ADRs 0007 e 0012; escrever o ADR-0010 (armazenamento a bordo)
      (corta se apertar)
- [ ] `docs/requisitos.md`: status de 19/09 → o do CDR, mais os requisitos do
      OBC agora nossos
- [ ] Banco de aeroportos da região da UNSAM para o estimador (corta se
      apertar)
- [ ] Na UNSAM: ruído em 433,92 MHz antes do teste, margem reavaliada
- [ ] Afinidade de CPU (`dump1090` e `ttcd` em núcleos distintos), se a medição
      com tráfego pedir

---

## Questões em aberto

| # | Questão | Bloqueia |
|---|---|---|
| Q1 | Quem escreve o firmware do ESP32 do AOCS — o contrato I²C (2.4) tem de ser combinado com essa pessoa | 2.4, 6 |
| Q2 | Resposta da equipe da PCB: INA219 no I²C0 (proposta de 24/10); a revisão cobre A1, A2, A3, A5, A6 e A7; data da placa nova | 2.4, 5 |
| Q3 | Placa do mecanismo (burn-wire): esquemático e corrente real; o 5 s de D3 pode mudar | 5 |
| Q4 | Organização: ordem das provas; como avaliam a tensão e a corrente de recarga (REG-09) — o INA219 não vê a corrente de carga | 6 |

---

## Riscos abertos

| Risco | Impacto | Mitigação | Quando |
|---|---|---|---|
| **Uma pessoa para OBC, TT&C e ground** | Algo fica sem validar em hardware | Firmware pronto antes do hardware (etapas 1–3); lista do que corta; `gs_cli` na Pi como ground de reserva | Sempre |
| **Placa nova do OBC atrasa** | Sem bring-up nem validação do OBC | Drivers testados com simulação; Pi da bancada com sensores soltos | Etapa 5 |
| **Dois mestres no I²C (Pi e ESP32)** | Leituras corrompidas, AOCS ou INA219 mudos | INA219 num I²C próprio (proposta à PCB); posse do barramento por modo para os comandos do AOCS (2.4) | Etapa 2, 6 |
| **Convivência LoRa × ADS-B** | Meta de ≥ 95% não cumprida | Medir na bancada; bloqueio por IPC ou filtro | Etapa 6 |
| **Enlace na distância real não medido** | Margem menor que a modelada | Bancada com as antenas da equipe; `SAFE` como recurso | Etapa 6 |
| **CPU com ~20 aeronaves não medida** | Disputa com `ttcd` e `obcd` | 21,9% com pouco tráfego; repetir com fonte conhecida | Etapa 6 |
| **Procedimento das provas desconhecido** | Comando ou medida diferente do previsto | Perguntar à organização; ground ESP32 manda qualquer `SET_MODE` | UNSAM |
| **Throttling térmico da Pi** a +50 °C | HLR-ENV-02 | Temperatura do SoC no `TM_HK` | Ambiental |
| **Ruído de 433 MHz no local** | Margem menor | Medir ao chegar | UNSAM |

---

## Regras que não se quebram

Derivadas dos defeitos concretos da missão anterior; o porquê de cada uma
está no `AGENTS.md` e no ADR citado. Valem agora também para o `obcd`.

1. **Nunca serializar struct por cast** (ADR-0002).
2. **Nada de bloqueio no laço de eventos** — o `../obc` dormia 1 s por ciclo.
3. **Nada de path hardcoded.** Configuração vem de arquivo/argv.
4. **Nada de IPC por arquivo com nome fixo.**
5. **Build em zero warnings** com `-Wconversion -Wsign-conversion`.
6. **Todo frame rejeitado é contado** — evidência do HLR-ADS-08.
7. **ADR escrito no momento em que o código materializa a decisão.**
