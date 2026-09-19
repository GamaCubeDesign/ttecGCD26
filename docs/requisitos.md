# Requisitos da missão — CubeDesign 2026

Transcrição completa e comentada do regulamento, para consulta sem depender
do PDF. Traz todos os requisitos de alto nível (HLR), os requisitos do corpo
do regulamento que não têm ID, o estado de cada um neste repositório e o que
falta para garanti-lo.

| | |
|---|---|
| **Fonte** | `docs/cubedesign2026.pdf` — regulamento (pp. 1–7) e catálogo *High Level Requirements* (pp. 8–13) |
| **Estado registrado em** | 2026-09-19, commit `bc20975` |
| **Prazos** | Design Package **27/09/2026** · testes em UNSAM **24/11/2026** |

## Como ler

- **O texto dos requisitos está literal, em inglês**, copiado do PDF e
  conferido contra a extração de texto do arquivo. Não traduzir nem
  parafrasear: é o texto contra o qual a banca avalia, e a matriz de
  rastreabilidade do DP cita estes IDs.
- Comentários, estado e ações estão em português — este é um documento de
  trabalho do time, não entra no DP.
- Donos conforme `docs/anotacoes_cubedesign2026.pdf`: **L** = equipe LoRa,
  **A** = equipe ADS-B. Os demais requisitos são de outras equipes ou do time
  inteiro.

| Estado | Significado |
|---|---|
| `VERIFICADO` | existe a evidência que o método de verificação exige |
| `PARCIAL` | há código ou documento, mas a evidência exigida ainda não existe |
| `PROJETADO` | decidido em ADR; nada implementado |
| `PENDENTE` | nada feito |
| `OUTRA EQUIPE` | fora deste repositório — o comentário diz o que o TT&C precisa fornecer |

**Nenhum requisito está `VERIFICADO` hoje.** `flight/` e `ground/` estão
vazios. O que existe: o codec compartilhado (`common/`), o protótipo
`adsb_capture.c`, os ADRs 0001–0004 e os budgets de dados e enlace.

## Sumário

1. [Painel geral](#1-painel-geral)
2. [Problemas do próprio regulamento](#2-problemas-do-próprio-regulamento)
3. [Requisitos de alto nível (HLR)](#3-requisitos-de-alto-nível-hlr)
4. [Requisitos do corpo do regulamento](#4-requisitos-do-corpo-do-regulamento)
5. [Conteúdo obrigatório do Design Package](#5-conteúdo-obrigatório-do-design-package)
6. [Comentários gerais](#6-comentários-gerais)
7. [Lacunas e ações](#7-lacunas-e-ações)
8. [Perguntas para a organização](#8-perguntas-para-a-organização)
9. [Como manter este documento](#9-como-manter-este-documento)

---

## 1. Painel geral

32 requisitos HLR. Os 30 itens `REG-nn` do corpo do regulamento estão na
[seção 4](#4-requisitos-do-corpo-do-regulamento).

| ID | Resumo | Verificação | Dono | Estado |
|---|---|---|---|---|
| [HLR-GEN-01](#hlr-gen-01) | Envelope CDS v14.1, 1U a 3U | Inspection (Fit-Check) | Estrutura | `OUTRA EQUIPE` |
| [HLR-GEN-02](#hlr-gen-02) | Subsistemas mínimos | Inspection + Doc. review | Todas | `OUTRA EQUIPE` |
| [HLR-GEN-03](#hlr-gen-03) | Operação autônoma com TC e TM | Doc. review | L | `PROJETADO` |
| [HLR-GEN-04](#hlr-gen-04) | Requisitos de subsistema derivados | Doc. review | Todas | `PENDENTE` |
| [HLR-COMM-01](#hlr-comm-01) | Executar TC em tempo limitado | Test | L | `PENDENTE` |
| [HLR-COMM-02](#hlr-comm-02) | TM estruturada: status, potência, atitude, missão | Test + Data inspection | L | `PARCIAL` |
| [HLR-COMM-03](#hlr-comm-03) | Arquitetura de comunicação definida e justificada | Doc. review + Test | L | `PARCIAL` |
| [HLR-EPS-01](#hlr-eps-01) | Bateria recarregável para a missão inteira | Test | EPS | `OUTRA EQUIPE` |
| [HLR-EPS-02](#hlr-eps-02) | Recarga com fonte de luz | Test | EPS | `OUTRA EQUIPE` |
| [HLR-EPS-03](#hlr-eps-03) | Orçamento de potência de todos os modos | Doc. review | EPS | `PENDENTE` |
| [HLR-EPS-04](#hlr-eps-04) | Bateria em condição segura (V, I, T) | Test + Telemetry analysis | EPS | `OUTRA EQUIPE` · lacuna no TT&C |
| [HLR-EPS-05](#hlr-eps-05) | Operar com fonte externa, bateria removível | Test + Inspection | EPS | `OUTRA EQUIPE` |
| [HLR-ADCS-01](#hlr-adcs-01) | Estimar atitude em ao menos um eixo | Test | ADCS | `OUTRA EQUIPE` |
| [HLR-ADCS-02](#hlr-adcs-02) | Detumbling abaixo de 5 rpm | Test | ADCS | `OUTRA EQUIPE` |
| [HLR-ADCS-03](#hlr-adcs-03) | Quantificar desempenho de atitude | Analysis + Doc. review | ADCS | `OUTRA EQUIPE` |
| [HLR-ADS-01](#hlr-ads-01) | Receber ADS-B em 1090 MHz | Test | A | `PARCIAL` |
| [HLR-ADS-02](#hlr-ads-02) | Decodificar ICAO, posição, altitude, velocidade | Test + Data validation | A | `PARCIAL` |
| [HLR-ADS-03](#hlr-ads-03) | Várias aeronaves simultâneas | Test | A | `PROJETADO` |
| [HLR-ADS-04](#hlr-ads-04) | Arquitetura de processamento implementada e justificada | Doc. review | A + L | `PARCIAL` |
| [HLR-ADS-05](#hlr-ads-05) | 10 min contínuos, até 20 aeronaves | Test + Data validation | A | `PROJETADO` |
| [HLR-ADS-06](#hlr-ads-06) | Conceito de missão secundária | Doc. review | não atribuído | `PENDENTE` |
| [HLR-ADS-07](#hlr-ads-07) | Timestamp por mensagem | Test + Data inspection | A + L | `PARCIAL` |
| [HLR-ADS-08](#hlr-ads-08) | Critérios de sucesso quantitativos | Test + Data inspection | A + L | `PARCIAL` |
| [HLR-SW-01](#hlr-sw-01) | Modos operacionais conforme a ConOps | Test + Doc. review | OBC | `OUTRA EQUIPE` |
| [HLR-SW-02](#hlr-sw-02) | Dados estruturados, documentados, reproduzíveis | Data inspection | A + L | `PARCIAL` |
| [HLR-VV-01](#hlr-vv-01) | Estratégia de V&V | Doc. review | Todas | `PENDENTE` |
| [HLR-VV-02](#hlr-vv-02) | Métodos de verificação definidos e justificados | Doc. review | Todas | `PENDENTE` |
| [HLR-ENV-01](#hlr-env-01) | Executar a missão antes do ambiental | Test | Todas | `PENDENTE` |
| [HLR-ENV-02](#hlr-env-02) | Sobreviver ao ambiental ou justificar | Test + Analysis | Todas | `PENDENTE` |
| [HLR-COST-01](#hlr-cost-01) | Estimativa de custo do sistema | Doc. review | A + L (parte TT&C) | `PENDENTE` |
| [HLR-COST-02](#hlr-cost-02) | Decisões justificadas por desempenho, custo e complexidade | Doc. review | A + L | `PARCIAL` |
| [HLR-SYS-01](#hlr-sys-01) | Coerência de engenharia de sistemas | Integrated review | Todas | `PARCIAL` |

**Do TT&C (L, A ou A + L): 14 requisitos.** Nenhum verificado; 9 parciais,
3 projetados (HLR-GEN-03, HLR-ADS-03, HLR-ADS-05) e 2 pendentes (HLR-COMM-01,
HLR-COST-01). Os requisitos do time inteiro — HLR-GEN-04, HLR-VV-01/02,
HLR-SYS-01 — também dependem de uma parte do TT&C.

---

## 2. Problemas do próprio regulamento

Ler antes de citar o regulamento no DP.

### 2.1 Numeração de seções duplicada

O PDF tem duas seções "8" (*Environmental Testing* e *Evaluation System*),
duas "9" (*Final Presentation* e *General Notes*), duas "8.1" (*Thermal Tests*
e *Budget & Fair Play*) e duas "8.3" (*Post-testing* e *Special Awards*).
**Cite sempre número e título**, como este documento faz.

Dois ADRs já aceitos citam só o número. Como ADR aceito é imutável, a
desambiguação fica registrada aqui:

- ADR-0001, "§8.3" → *8.3 Special Awards*
- ADR-0004, "§9 of the rules" → *9. General Notes*

### 2.2 Nível de vibração contraditório

- Seção *8. Environmental Testing*, texto corrido: "Random vibration test in
  accordance with NASA/GEVS-14.1 requirements: 2 minutes per axis at 1 g."
- Seção *8.2 Vibration Test*, três itens: "Random profile per NASA GEVS",
  "14.1 Grms, 2 minutes per axis", "Inside of the Test-POD".

As duas leituras diferem por mais de uma ordem de grandeza. "GEVS-14.1"
parece a junção de "GEVS" com "14.1 Grms". **Projetar para 14.1 Grms**, a
leitura conservadora, e perguntar à organização ([§8](#8-perguntas-para-a-organização)).

### 2.3 Vácuo ou só ciclagem térmica?

O texto descreve "thermal cycling testing", mas a nota 2 da mesma seção e o
HLR-EPS-05 falam em "thermal vacuum (TVAC)". Em vácuo não há convecção: Pi e
SDR dissipam calor só por condução e radiação, o que muda o projeto térmico.
Perguntar à organização.

### 2.4 HLR-SW-02 é mais fraco que a seção 5

A seção *5. Software and Data Management* exige dados *Structured*,
*Documented*, *Reproducible* e ***Available for post-analysis***. O HLR-SW-02
lista só os três primeiros. Cumprir os quatro.

### 2.5 Duas listas de subsistemas mínimos

O HLR-GEN-02 e a seção *4.4.2 System Architecture* não coincidem. O mínimo
real é a união das duas:

| Item | HLR-GEN-02 | 4.4.2 | Responsável |
|---|:---:|:---:|---|
| Plataforma estrutural (1U a 3U) | ✓ | ✓ | Estrutura |
| EPS com bateria | ✓ | ✓ | EPS |
| OBC | ✓ | ✓ | OBC |
| Sistema de comunicação | ✓ | ✓ | **TT&C (L)** |
| ADCS, capacidade funcional mínima | ✓ | — | ADCS |
| Payload ADS-B | ✓ | ✓ | **TT&C (A)** |
| Antena ADS-B de 1090 MHz | — | ✓ | **TT&C (A)** |
| Software de decodificação e processamento | — | ✓ | **TT&C (A)** |
| Software de voo e de missão | ✓ | — | OBC + TT&C |
| Segmento solo de controle e análise | — | ✓ | **TT&C (L)** |

### 2.6 O catálogo HLR não é o contrato inteiro

Há itens pontuados ou exigidos no corpo do regulamento sem HLR
correspondente. Os que mais importam: **implantação de antena de pelo menos
10 cm**, **estimativa de origem e destino** das aeronaves, decodificação de
**heading**, **classificação** das mensagens e **rastreabilidade entre
requisitos e testes**. Todos estão na [seção 4](#4-requisitos-do-corpo-do-regulamento).

### 2.7 Valores TBC e pontuação em aberto

Estão marcados TBC: os pontos do teste de comunicação, do mecanismo e da
recarga; os 60 rpm e os 5 rpm do detumbling; a faixa térmica de –10 °C a
+50 °C. Além disso, "Scoring details will be specified in the final version
of the rules" e "Clarifications and supplementary materials will be published
prior to the event". Acompanhar as publicações da organização e atualizar
este documento.

### 2.8 Outros

- "Wired" nunca é definido — ver [REG-06](#reg-06).
- Prazo do DP: o texto diz "2 months before the on-site tests"; a data
  confirmada pelo time é 27/09/2026.
- Títulos em espanhol ("Reglamentación General", "Objetivos secundarios")
  indicam um rascunho traduzido. A redação da versão final pode mudar.

---

## 3. Requisitos de alto nível (HLR)

### 3.1 General

### HLR-GEN-01

> The system shall conform to a mechanical envelope compatible with the
> CubeSat Design Specification (CDS v14.1), within a volume equivalent to 1U,
> 2U, or 3U.
>
> **Verification:** Inspection (Fit-Check)

**Dono:** Estrutura · **Estado:** `OUTRA EQUIPE`

- Interface com o TT&C: as duas antenas precisam caber recolhidas no
  envelope. Um quarto de onda em 433 MHz tem ~17 cm; em 1090 MHz, ~6,9 cm. O
  dongle SDR e os cabos de antena também ocupam volume interno.
- O Fit-Check é pré-requisito do teste ambiental, que multiplica o resultado
  da missão por 1,4 ([REG-23](#reg-23)).

### HLR-GEN-02

> The system shall include, at minimum, the following subsystems:
>
> - Structural platform
> - Electrical Power System (EPS) including battery
> - On-Board Computer (OBC)
> - Communication subsystem
> - Attitude Determination and Control System (ADCS, minimum functional capability)
> - ADS-B payload
> - Flight and mission software
>
> **Verification:** Inspection + Documentation review

**Dono:** todas · **Estado:** `OUTRA EQUIPE` (a parte do TT&C está nos
requisitos COMM e ADS)

- O TT&C responde pelo subsistema de comunicação, pelo payload ADS-B e pela
  parte do software de voo que roda em `ttcd` e `adsbd`.
- A seção 4.4.2 acrescenta antena ADS-B de 1090 MHz, software de
  decodificação e processamento, e segmento solo. Os três são do TT&C — ver
  [§2.5](#25-duas-listas-de-subsistemas-mínimos).

### HLR-GEN-03

> The system shall operate autonomously during mission execution, supporting
> remote interaction via telecommands (TC) and generating telemetry (TM)
> without manual intervention.
>
> **Verification:** Documentation review

**Dono:** L · **Estado:** `PROJETADO`

- Como o projeto atende: depois do telecomando que inicia a missão, o `ttcd`
  transmite snapshots em modo `STREAM` sem pedido da ground (a cada 5 s,
  `docs/budgets/data-budget.md` §5); `ttcd` e `adsbd` rodam sob systemd com
  reinício automático; o `adsbd` supervisiona o `dump1090`. Queda do enlace
  leva ao modo `SAFE` com beacon (ADR-0004). Nada disso está implementado.
- **DP:** a verificação é revisão de documentação, então a evidência é a
  ConOps mais o ADR-0005 (arquitetura de processos, ainda não escrito)
  descrevendo a sequência inteira sem intervenção. A ConOps não existe —
  `obc/docs/conops.md` tem só o título.

### HLR-GEN-04

> Each team shall define, document, and maintain a consistent set of
> subsystem-level requirements derived from system-level requirements.
>
> **Verification:** Documentation review

**Dono:** todas · **Estado:** `PENDENTE` na parte do TT&C

- O TT&C não tem requisitos de subsistema escritos. Vários valores já estão
  decididos e só falta numerá-los e ligá-los ao HLR de origem, por exemplo:
  perfil NOMINAL SF9 / BW 125 kHz / CR 4:5 (ADR-0004, ← HLR-COMM-03);
  snapshot a cada 5 s (`data-budget.md` §5, ← HLR-ADS-08); latência p95 de
  até 5 s (`data-budget.md` §8, ← HLR-ADS-08); registro de track de 20 B
  (`tests/test_vectors.c`, ← HLR-SW-02).
- Sugestão: IDs `TTC-nn` em `docs/vv/`, junto da matriz de rastreabilidade,
  cada um citando o HLR de origem.
- Este documento é o nível de sistema; não substitui os requisitos de
  subsistema.

### 3.2 Communications (TM/TC)

### HLR-COMM-01

> The system shall receive telecommands (TC) from the ground segment and
> execute corresponding actions within a bounded response time defined by
> the team.
>
> **Verification:** Test

**Dono:** L · **Estado:** `PENDENTE` — o limite não foi definido

- **O requisito só é cumprido com um número escrito.** Um sistema rápido sem
  limite declarado não atende.
- O limite ponta a ponta é a soma de: espera pela janela em que a ground
  pode transmitir (o rádio é half-duplex) + air time do TC + despacho no
  `ttcd` + IPC + até um ciclo do OBC (laço de 1 s em `obc/src/main.c`, sem
  compensação de deriva).
- O primeiro termo depende de uma regra que ainda não existe: quando a ground
  pode transmitir sem colidir com uma transmissão do satélite. Pertence ao
  ADR-0007.
- **UNSAM:** o OBC registra `tc` antes de agir e `transicao` depois; a
  diferença de `t_ms` entre os dois é a evidência do lado do OBC
  (`obc/docs/log_schema.md`, linhas 71–72).
- O teste básico pontua TC por cabo e por RF separadamente — ver
  [REG-06](#reg-06).

### HLR-COMM-02

> The system shall generate and transmit structured telemetry (TM), including
> at minimum:
>
> - System status
> - Power parameters
> - Attitude information
> - Mission data
>
> **Verification:** Test + Data inspection

**Dono:** L · **Estado:** `PARCIAL` — formato definido e fixado por teste;
nada é transmitido ainda

- Cobertura do registro `TM_HK` (`common/gama_tm.h`):

  | Exigido | Campos |
  |---|---|
  | System status | `obc_mode`, `link_state`, `flags`, `uptime_s` |
  | Power parameters | `battery_mv`, `current_ma` |
  | Attitude information | `roll_cdeg`, `pitch_cdeg`, `yaw_cdeg` |
  | Mission data | `adsb_msgs`, mais os frames `TM_TRACKS` e `TM_STAT` |

- Falta a temperatura da bateria — ver [HLR-EPS-04](#hlr-eps-04).
- Potência e atitude virão do OBC por `IPC_TELEMETRY`. Enquanto o cliente
  IPC não estiver no OBC (ICD pendente, PLANO fase 4.3), esses campos não têm
  fonte.
- "Data inspection" exige que a ground grave os frames brutos, não só os
  valores decodificados — ver [HLR-SW-02](#hlr-sw-02).

### HLR-COMM-03

> The communication architecture (including physical layer, protocols, and
> data rates) shall be defined, justified, and consistent with mission
> requirements.
>
> **Verification:** Documentation review + Test

**Dono:** L · **Estado:** `PARCIAL`

- Camada física e taxas: definidas e justificadas no ADR-0004 e em
  `docs/budgets/link-budget.md`, reproduzíveis com
  `python3 tools/analysis/lora_budget.py`.
- Protocolo: implementado em `common/` e fixado por `tests/test_vectors.c`,
  mas o ADR-0007 (formato de frame e ARQ) e o ICD do enlace
  (`docs/icd/ota-protocol-icd.md`) não foram escritos.
- "consistent with mission requirements": o `data-budget.md` fecha o
  orçamento (tamanho da linha NDJSON corrigido em 2026-09-19).
- **UNSAM:** medir o air time real dos três perfis contra o calculado e a
  taxa de erro de pacote (PLANO fase 2.7).

### 3.3 Power and Energy (EPS)

### HLR-EPS-01

> The system shall include a rechargeable energy storage element enabling
> autonomous operation for the full mission duration.
>
> **Verification:** Test

**Dono:** EPS · **Estado:** `OUTRA EQUIPE`

- O TT&C fornece o consumo dos seus equipamentos por modo, para o orçamento
  de potência ([HLR-EPS-03](#hlr-eps-03)).
- A "full mission duration" é, no mínimo, os 10 minutos contínuos do HLR-ADS-05;
  somar o tempo de preparação e o de download pós-missão.

### HLR-EPS-02

> The system shall demonstrate the capability to recharge its battery using a
> light-based energy source representative of solar input.
>
> **Verification:** Test

**Dono:** EPS · **Estado:** `OUTRA EQUIPE`

- No teste intermediário, "current and voltage will be evaluated"
  ([REG-09](#reg-09)).
- `current_ma` no `TM_HK` tem sinal: negativo é carga. Para a TM mostrar a
  carga, o sensor de corrente (INA219, na branch `basic_intermediate` do OBC)
  precisa estar no caminho da bateria, não só no da carga. Confirmar com a
  EPS.

### HLR-EPS-03

> The system shall define and justify a power budget covering all
> operational modes, including nominal and peak consumption.
>
> **Verification:** Documentation review

**Dono:** EPS, com contribuição do TT&C · **Estado:** `PENDENTE`

- `docs/budgets/power-budget.md` não existe (PLANO fase 6).
- Contribuição do TT&C, a medir em bancada: Pi Zero 2 W com o `dump1090`
  rodando, NESDR Nano 3, SX1278 em recepção contínua e em transmissão a
  20 dBm.
- Pico: transmissão a 20 dBm ao mesmo tempo que o `dump1090` em carga
  máxima. A fração de tempo transmitindo por modo sai do `data-budget.md`:
  até 50% em `STREAM`, maior em `BULK`, mínima em `SAFE`.

### HLR-EPS-04

> The system shall maintain safe battery operating conditions (voltage,
> current, temperature) throughout all test phases.
>
> **Verification:** Test + Telemetry analysis

**Dono:** EPS · **Estado:** `OUTRA EQUIPE` — **com lacuna no TT&C**

- A verificação é por análise de telemetria, então a TM precisa carregar
  tensão, corrente **e temperatura da bateria**. O `TM_HK` tem `battery_mv` e
  `current_ma`, mas nenhuma temperatura de bateria: só `temp_ext_ccel`
  (externa) e `temp_soc_ccel` (SoC da Pi).
- O teste térmico exige a bateria acima de 0 °C o tempo todo
  ([REG-26](#reg-26)). Sem esse campo, não há como demonstrar isso por
  telemetria.
- Acrescentar o campo muda o formato de wire: vetores dourados,
  `data-budget.md` e um ADR. Decidir também quantos sensores — a missão
  anterior tinha dois (`batteryTemperature1` e `2`).

### HLR-EPS-05

> The system shall support operation from an external power source during
> testing phases, including thermal vacuum (TVAC), without requiring
> modification of core subsystems.
>
> If an internal battery is present, the system shall allow it to be removed
> during such tests while maintaining full system functionality.
>
> The external power interface shall provide electrical characteristics
> equivalent to nominal operating conditions and shall not interfere with
> system performance.
>
> **Verification:** Test + Inspection

**Dono:** EPS · **Estado:** `OUTRA EQUIPE`

- Com a bateria removida, `battery_mv` passa a medir a fonte externa.
  Registrar a configuração de alimentação no log de cada ensaio, para que a
  análise de telemetria não confunda as duas situações.

### 3.4 Attitude (ADCS)

### HLR-ADCS-01

> The system shall estimate its attitude along at least one axis using
> onboard sensors.
>
> **Verification:** Test

**Dono:** ADCS · **Estado:** `OUTRA EQUIPE`

- Teste avançado: azimute medido com fonte de luz, nas faixas ≤5°, ≤15° e
  ≤30° ([REG-11](#reg-11)).
- `yaw_cdeg` no `TM_HK` tem resolução de 0,01°, suficiente para reportar o
  resultado.

### HLR-ADCS-02

> The system shall reduce an initial rotational condition (detumbling) to
> below 5 rpm (TBC) within a finite and measurable time.
>
> **Verification:** Test

**Dono:** ADCS · **Estado:** `OUTRA EQUIPE`

- Teste avançado: de 60 rpm (TBC) para menos de 5 rpm, com tempos-alvo
  <10 s, <25 s e <60 s ([REG-10](#reg-10)).
- **A telemetria por rádio não consegue evidenciar isso.** O `TM_HK` está
  previsto a cada 10 s e carrega ângulos, não velocidade angular; 60 rpm é uma volta por
  segundo. A evidência tem que vir de um registro de alta taxa a bordo
  (giroscópio), baixado depois do teste, ou da medição da própria
  organização.

### HLR-ADCS-03

> The system shall quantify and report attitude determination performance,
> including error metrics and limitations.
>
> **Verification:** Analysis + Documentation review

**Dono:** ADCS · **Estado:** `OUTRA EQUIPE`

- Mesma observação do HLR-ADCS-02: a análise precisa do registro de bordo em
  alta taxa, não da TM.

### 3.5 ADS-B Mission

### HLR-ADS-01

> The system shall receive ADS-B signals at 1090 MHz and process them during
> mission execution.
>
> **Verification:** Test

**Dono:** A · **Estado:** `PARCIAL`

- Cadeia: antena de 1090 MHz → NESDR Nano 3 → `dump1090-fa` → porta SBS-1 →
  `adsbd`.
- Existe o protótipo `adsb_capture.c` (sobe o `dump1090`, lê o SBS, grava
  NDJSON). Não há registro de teste no repositório, e ele ainda não foi
  transformado no `adsbd` (PLANO fase 3.1).
- **UNSAM:** o ambiente é SDR a curta distância, portanto sinal forte. O
  ganho em `adsb_capture.c:46` (`"-10"`) tem significado diferente em versões
  diferentes do dump1090; varrer o ganho manualmente (PLANO fase 3.4).
- Maior risco não medido do projeto: o uso de CPU do `dump1090` na Pi Zero
  2 W.

### HLR-ADS-02

> The system shall decode standard ADS-B messages and extract, at minimum:
>
> - ICAO identifier
> - Position (latitude, longitude)
> - Altitude
> - Velocity
>
> **Verification:** Test + Data validation

**Dono:** A · **Estado:** `PARCIAL`

- A decodificação é do `dump1090-fa`. O `adsb_capture.c` extrai do SBS:
  ICAO (campo 4), altitude (11), velocidade no solo (12), rumo (13),
  latitude e longitude (14–15) e razão vertical (16).
- "Velocity" é vetor: reportar velocidade no solo, rumo e razão vertical, não
  só o módulo. O registro de track já carrega os três.
- "Data validation" pede gabarito: comparar o decodificado com o que foi
  injetado no cenário. Perguntar se a organização fornece
  ([§8](#8-perguntas-para-a-organização)).
- O objetivo primário também pede *heading*, que este HLR não lista — ver
  [REG-16](#reg-16).

### HLR-ADS-03

> The system shall detect and process multiple aircraft simultaneously within
> a single mission scenario.
>
> **Verification:** Test

**Dono:** A · **Estado:** `PROJETADO`

- A tabela de pistas por ICAO no `adsbd` fará a agregação (PLANO fase 3.2).
  O NDJSON do protótipo já registra as mensagens de todas as aeronaves, intercaladas.
- Exigir um número mínimo de mensagens antes de reportar uma pista nova:
  frames corrompidos que passam pela correção de erros podem, raramente,
  produzir ICAOs espúrios e inflar a contagem.

### HLR-ADS-04

> The system shall implement and justify a processing architecture (onboard,
> ground, or hybrid).
>
> **Verification:** Documentation review

**Dono:** A + L · **Estado:** `PARCIAL` — justificado, não implementado

- Justificativa: ADR-0003. Arquitetura híbrida com o corte no estado de
  pista: decodificação e agregação a bordo; trajetória e origem/destino na
  ground.
- O requisito diz "implement **and** justify". A justificativa está pronta
  para o DP; a implementação é o `adsbd` mais o escalonador do `ttcd`.
- Os números que sustentam a decisão estavam subestimados; foram corrigidos
  em 2026-09-19 (errata do ADR-0003). A conclusão ficou mais forte: o volume
  bruto é 129× a capacidade do enlace.

### HLR-ADS-05

> The system shall execute a continuous mission of 10 minutes, processing up
> to 20 aircraft per test.
>
> **Verification:** Test + Data validation

**Dono:** A · **Estado:** `PROJETADO`

- Enlace para 20 aeronaves: cada snapshot são 2 frames, 414 B e 2,09 s de
  air time; 120 snapshots em 600 s (`data-budget.md` §5).
- Dimensionar a tabela de pistas com folga acima de 20 (ICAOs espúrios,
  aeronaves que entram e saem do cenário).
- "continuous": um reinício do `dump1090` durante a missão é uma
  descontinuidade. O `adsbd` vai contar os reinícios
  (o campo `gama_stat_t.dump1090_restarts` já existe no codec); cada um precisa aparecer e ser
  justificado na análise.
- O teto de tamanho do NDJSON (PLANO fase 3.1) tem que ficar acima do volume
  real de 10 minutos: ~7,3 MiB com a linha de ~160 B.

### HLR-ADS-06

> Each team shall define a secondary mission concept compatible with the
> primary mission.
>
> **Verification:** Documentation review
>
> Note: This mission does not necessarily have to be implemented in hardware
> or software.

**Dono:** não atribuído · **Estado:** `PENDENTE`

- Não tem dono nas anotações nem aparece no PLANO. Atribuir.
- Basta o conceito. O mais defensável é reaproveitar o hardware ou os dados
  da missão principal. Dois candidatos (sugestões, não decisões):
  - recepção de AIS em 162 MHz, para vigilância marítima em áreas remotas —
    a missão anterior já tinha `AIS_PROTOCOL` no protocolo, e o NESDR cobre a
    faixa, mas não sintoniza 162 MHz e 1090 MHz ao mesmo tempo;
  - estimativa da cobertura do receptor a partir das estatísticas de
    recepção, que conversa com o objetivo secundário de analisar cobertura,
    latência e potência ([REG-17](#reg-17)).

### HLR-ADS-07

> The system shall associate a timestamp with each received or processed
> ADS-B message, with a time resolution sufficient to enable trajectory
> reconstruction and temporal ordering of events within the mission
> duration.
>
> **Verification:** Test + Data inspection

**Dono:** A + L · **Estado:** `PARCIAL`

- A bordo: o `adsb_capture.c` carimba cada mensagem na chegada, com
  resolução de nanossegundos (`rx_epoch_ns`). Mas usa `CLOCK_REALTIME`, e a
  Pi Zero 2 W não tem RTC (AGENTS.md, regra 8): trocar por relógio monotônico
  mais uma âncora de relógio de parede (PLANO fase 3.1).
- O carimbo marca a chegada no socket SBS, não a recepção de RF. Se isso for
  questionado, o formato Beast do `dump1090` traz o timestamp do próprio
  receptor (contador de 12 MHz).
- No enlace: o único campo de tempo do `TM_TRACKS` é `age_ds`, **com
  semântica indefinida** — o nome diz "idade", o comentário em
  `common/gama_tm.h` diz "desde a época do snapshot", e os vetores de teste
  parecem tempo de missão. A reconstrução de trajetória na ground depende
  dele. Definir no ADR-0007 e no ICD do enlace antes do DP.
- Resolução no enlace: 100 ms. A 450 kt, isso são ~23 m, abaixo da precisão
  do próprio ADS-B.

### HLR-ADS-08

> **HLR-ADS-08 - Mission Success Criteria**
>
> The system shall meet defined mission success criteria during the
> 10-minute operational window, including at minimum:
>
> - Successful reception of ADS-B signals
> - Decoding of valid ADS-B messages
> - Extraction of required parameters (ICAO, position, altitude, velocity)
> - Transmission of processed data as telemetry
>
> Each team shall define quantitative success metrics, including at least:
>
> - Minimum number or percentage of correctly decoded messages
> - Maximum acceptable data loss rate
> - Maximum acceptable latency between reception and telemetry transmission
>
> **Verification:** Test + Data inspection

**Dono:** A + L · **Estado:** `PARCIAL` — métricas definidas, nada medido

- Métricas definidas em `data-budget.md` §8:

  | Métrica | Alvo |
  |---|---|
  | Mensagens decodificadas corretamente | ≥ 95% |
  | Perda de dados no downlink | ≤ 5% |
  | Latência p95, recepção → início da transmissão | ≤ 5 s |
  | Latência p95, recepção → chegada na ground | ≤ 7 s |
  | Aeronaves simultâneas | 20 |

- Decodificação: "correctly decoded" precisa do gabarito do cenário.
- Perda: medida pelas lacunas de `seq`, o que exige que todo frame rejeitado
  seja contado (AGENTS.md, regra 6).
- Latência: "recepção → início da transmissão" se mede inteira a bordo, com
  um único relógio. "→ chegada na ground" compara dois relógios e exige
  sincronização (`GAMA_TC_SET_TIME`) — e depende da semântica de `age_ds`.

### 3.6 Software and Data

### HLR-SW-01

> The system shall implement operational modes consistent with the defined
> Concept of Operations (ConOps).
>
> **Verification:** Test + Documentation review

**Dono:** OBC · **Estado:** `OUTRA EQUIPE`

- Os modos são do OBC (`obc/src/fsm.h`): `PRE_TEST`, `BASIC_INTERMEDIATE`,
  `ADVANCED_AOCS`, `MISSION_ADSB`, `MISSION_DOWNLINK`, `ENV_SURVIVAL`. O TT&C
  não duplica essa máquina de estados: traduz o TC em `Event` e reporta o
  modo na TM (`obc_mode`).
- A ConOps não existe (`obc/docs/conops.md` tem só o título). Ela é
  entregável do DP ("Mission concept") e o TT&C depende dela para definir o
  que transmitir em cada fase.
- O estado de enlace do `ttcd` (`IDLE`, `TX`, `STREAM`, `BULK`, `SAFE`) não é
  modo operacional. Não confundir os dois no DP.

### HLR-SW-02

> Mission and telemetry data shall be:
>
> - Structured
> - Documented
> - Reproducible
>
> **Verification:** Data inspection

**Dono:** A + L · **Estado:** `PARCIAL`

- **Structured:** frames binários com formato fixado por
  `tests/test_vectors.c`; NDJSON a bordo.
- **Documented:** o ICD do enlace (`docs/icd/ota-protocol-icd.md`) não existe.
  Sem ele, o formato só está documentado no código.
- **Reproducible:** a ground deve gravar o frame bruto (em hex) com
  timestamp de chegada, RSSI e SNR, não só os valores decodificados, para
  que a decodificação possa ser refeita. O formato do log da ground ainda não
  foi definido.
- A seção 5 acrescenta "Available for post-analysis"
  ([§2.4](#24-hlr-sw-02-é-mais-fraco-que-a-seção-5)): pelo ADR-0003, o NDJSON
  completo fica a bordo e é recuperado por USB ou SSH depois da missão.

### 3.7 Verification and Validation

### HLR-VV-01

> Each team shall define a Verification and Validation (V&V) strategy
> demonstrating compliance with all system requirements.
>
> **Verification:** Documentation review

**Dono:** todas · **Estado:** `PENDENTE`

- `docs/vv/vv-plan.md` e `docs/vv/traceability-matrix.md` não existem (PLANO
  fase 6).
- "all system requirements" inclui os itens `REG-nn` da
  [seção 4](#4-requisitos-do-corpo-do-regulamento), não só os HLR.
- A seção 6 do regulamento avisa: "Not all planned tests will be executed.
  Only those defined by the organization will be performed." O plano precisa
  cobrir todos os requisitos mesmo assim.

### HLR-VV-02

> Verification methods (test, analysis, inspection, demonstration) shall be
> explicitly defined and justified.
>
> **Verification:** Documentation review

**Dono:** todas · **Estado:** `PENDENTE`

- O regulamento já atribui um método a cada HLR. O time precisa justificar
  cada um e ligá-lo a um caso de teste.
- A seção 6 exige também rastreabilidade entre requisitos e testes, que
  nenhum HLR pede — ver [REG-22](#reg-22).

### 3.8 Environmental

### HLR-ENV-01

> The system shall successfully execute the mission prior to environmental
> testing.
>
> **Verification:** Test

**Dono:** todas · **Estado:** `PENDENTE`

- Pré-requisito do teste ambiental. Do lado do TT&C, nada além da missão
  funcionar.

### HLR-ENV-02

> The system shall demonstrate functional survival after thermal and
> vibration testing, or provide justified analysis of observed limitations.
>
> **Verification:** Test + Analysis

**Dono:** todas; o TT&C fornece a telemetria · **Estado:** `PENDENTE`

- Evidência: telemetria "before, during, and after the tests"
  ([REG-25](#reg-25)). O `ttcd` precisa rodar e registrar durante os ensaios.
- Na vibração o CubeSat fica "Inside of the Test-POD". Uma estrutura
  metálica atenua RF: tratar o registro a bordo como fonte primária e o
  rádio como complemento.
- Vibração a 14.1 Grms (leitura conservadora, [§2.2](#22-nível-de-vibração-contraditório)):
  fixar mecanicamente o dongle SDR na USB, os conectores das antenas e o
  cartão SD. São as peças do TT&C mais prováveis de soltar.
- Térmico: `temp_soc_ccel` já está no `TM_HK`; a Pi reduz o clock por volta
  de 80–85 °C. Em vácuo não há convecção ([§2.3](#23-vácuo-ou-só-ciclagem-térmica)).
- Pós-teste: basta enviar qualquer telemetria que demonstre operação
  ([REG-28](#reg-28)) — um `BEACON` ou um `TM_HK` resolve.

### 3.9 Cost and Fair Play

### HLR-COST-01

> Each team shall provide a cost estimation for the complete system,
> including all subsystems and major components.
>
> **Verification:** Documentation review

**Dono:** A + L, na parte do TT&C · **Estado:** `PENDENTE`

- Itens do TT&C: Pi Zero 2 W, 2× RA-02, NESDR Nano 3, ESP32, antenas, cabos
  e conectores.
- A seção *8.1 Budget & Fair Play* pede também custo por subsistema e
  justificativa COTS × customizado; um fator de eficiência pondera
  desempenho, custo e coerência técnica ([REG-30](#reg-30)).

### HLR-COST-02

> Design decisions shall be justified based on performance, cost, and
> complexity trade-offs.
>
> **Verification:** Documentation review

**Dono:** A + L · **Estado:** `PARCIAL`

- Os ADRs 0001–0004 justificam por desempenho e complexidade; custo quase
  não aparece. Um argumento de custo que já existe: o ADR-0004 multiplica a
  capacidade de downlink por 22 sem nenhum custo de hardware.
- Tratar custo explicitamente nos próximos ADRs.

### 3.10 System Coherence

### HLR-SYS-01

> The mission definition, system architecture, requirements, verification
> plan, and results shall be consistent and defensible from a systems
> engineering perspective.
>
> **Verification:** Integrated review

**Dono:** todas · **Estado:** `PARCIAL` — há inconsistências conhecidas nos
nossos documentos

Corrigir antes do DP:

1. ~~**Tamanho da linha NDJSON.**~~ **Resolvido em 2026-09-19.** O
   `lora_budget.py` agora calcula a linha a partir do formato real do
   `adsb_capture.c` (160,4 B, não 110 B); o volume bruto nominal passa a
   7,34 MiB e a razão sobre a capacidade NOMINAL a 129×. `data-budget.md`
   corrigido; ADR-0003 e ADR-0004 receberam errata.
2. ~~**Razão de redução a bordo.**~~ **Resolvido em 2026-09-19.** O valor
   único agora é 155:1 (volume bruto sobre os 48,5 KB efetivamente enviados),
   verificado pelo `--check`; a errata do ADR-0003 registra o 89:1 original.
3. **ADRs citados que não existem:** 0007 (0008 e 0011 escritos em
   2026-09-19).

Regra prática: um número mora em um único lugar e os outros documentos
apontam para ele; o que vier de cálculo entra no `lora_budget.py --check`.
Como o ADR-0003 é aceito e portanto imutável, a correção entrou como errata
no fim dele: a decisão e o texto original ficam intactos.

---

## 4. Requisitos do corpo do regulamento

Os IDs `REG-nn` são **locais deste documento, não oficiais**, numerados na
ordem em que aparecem no PDF, para que a matriz de rastreabilidade consiga
citá-los. Cada item indica a relação com o catálogo HLR:

- **sem HLR** — exigido ou pontuado, mas nenhum HLR cobre;
- **além do HLR** — há HLR, mas o texto do corpo pede algo a mais;
- **= HLR-XXX** — mesmo conteúdo; os comentários estão no HLR.

### 4.1 Regras gerais — seção 2

#### REG-01

**Sem HLR** · Dono: todas · Administrativo

> Each team must consist of 2 to 10 students and 1 faculty advisor.

#### REG-02

**= HLR-GEN-01**

> Building a physical CubeSat is not mandatory. The system must comply with
> the dimensional requirements of the CubeSat Design Specification (CDS
> v14.1), and equivalent systems must perform the specified missions.

#### REG-03

**Sem HLR** · Dono: todas · Prazo confirmado pelo time: **27/09/2026**

> The first stage is a documentation phase. Each team must submit, via email,
> a complete project data package 2 months before the on-site tests. Only
> selected teams will advance to the second stage at UNSAM.

- A etapa presencial só existe para quem passa na documental. O DP é
  eliminatório, não só pontuado.

#### REG-04

**Sem HLR** · Dono: todas · Estado: convenção adotada (AGENTS.md); ADR-0011
pendente

> Documentation must be submitted in English.

### 4.2 Design Package — seção 3.1

#### REG-05

**Sem HLR** · Dono: todas · Checklist completo na
[seção 5](#5-conteúdo-obrigatório-do-design-package)

> Two months before the on-site tests, each team must submit a Design Package
> (DP) including:
>
> - Mission concept
> - System architecture
> - Mass, power, data, and financial budgets
> - ADS-B payload design
> - V&V plan
> - Cost estimation and justification
> - Complete engineering model description:
>   - System block diagram
>   - Communication scheme
>   - Electrical architecture and power management
>   - Implemented algorithms
>   - Testing and validation procedures

### 4.3 Testes técnicos — seções 4.1 a 4.3 e 7

#### REG-06

**Além do HLR** (HLR-COMM-01, HLR-COMM-02) · Dono: L · Estado: `PENDENTE`

> Communication test: The CubeSat must receive telecommands (TC) [0.5 points
> wired, 1 point RF – TBC] and send telemetry (TM) [0.5 points wired, 1 point
> RF – TBC].

Seção *7. Communications Test*:

> The CubeSat must:
>
> - Receive telecommands and transmit telemetry
> - Wired and RF modes will be scored differently

- **"Wired" não está definido.** Pode ser um cabo de dados (UART ou USB) no
  lugar do rádio, ou o próprio rádio ligado por cabo coaxial com atenuador.
  Perguntar à organização.
- **Não há caminho cabeado no projeto.** Se "wired" for cabo de dados, o
  custo é baixo: o codec é o mesmo em qualquer transporte, e o `ttcd` pode
  aceitar frames também por uma porta serial. Mas precisa entrar no PLANO.
- Não está claro se os pontos de "wired" e "RF" somam ou se vale o maior.

#### REG-07

**Sem HLR** · Dono: mecanismos / OBC, com o TC do TT&C · Estado: `PENDENTE`

> Mechanism test: The CubeSat must deploy an antenna or similar device via
> remote command. The deployed element must be at least 10 cm. [0/1 point –
> TBC]. Teams must bring their own ground station.

- **Não há telecomando dedicado.** O catálogo (`common/gama_tc.h`) não tem
  comando de implantação; o caminho atual é `GAMA_TC_SET_MODE` com o evento
  `GAMA_OBC_EV_TC_BASIC_INTER`, que leva o OBC ao modo `BASIC_INTERMEDIATE`.
  Confirmar com o time do OBC que entrar nesse modo dispara a implantação.
  A missão anterior tinha um comando próprio (`OPEN_ANTENNAS`).
- A implantação é irreversível. Vale a mesma proteção do `SHUTDOWN`: um
  argumento de confirmação que um byte corrompido dificilmente produz.
- Se o elemento implantado for a antena LoRa (~17 cm em 433 MHz), o enlace
  opera com a antena recolhida até a implantação. A margem do ADR-0004 cobre
  isso, mas testar.

#### REG-08

**Sem HLR** · Dono: L · Estado: `PENDENTE` — `ground/` está vazio

> Teams must bring their own ground station.

Repetido em *9. General Notes*.

#### REG-09

**= HLR-EPS-02**, com o critério de avaliação

> Battery charging using a light source: current and voltage will be
> evaluated. [0/1 point – TBC].

#### REG-10

**Além do HLR** (HLR-ADCS-02: acrescenta os tempos-alvo)

> Detumbling: reduce initial rotation (60 rpm TBC) to below 5 rpm (TBC).
> Target times: <10 s, <25 s, <60 s.

#### REG-11

**Além do HLR** (HLR-ADCS-01 e 03: acrescenta as faixas de erro)

> Attitude determination (azimuth): measurement based on a light source.
> [≤5°, ≤15°, ≤30°]

### 4.4 Missão ADS-B — seção 4.4

Contexto, sem requisito para o time — *4.4.4 Test Environment*:

> The organization will provide:
>
> - Simulated ADS-B scenarios using SDR
> - Multi-aircraft traffic

#### REG-12

**Além do HLR** (HLR-ADS-01, 02 e 03: acrescenta "Classify" e "store") ·
Dono: A + L · Estado: `PARCIAL`

> The system must:
>
> - Receive ADS-B signals at 1090 MHz
> - Classify, store, and transmit them to the ground station
> - Decode standard ADS-B messages and extract:
>   - ICAO identifier
>   - Position
>   - Altitude
>   - Velocity
> - Handle multiple aircraft simultaneously

- **"Classify" não é definido.** O projeto classifica de duas formas:
  por tipo de mensagem (o NDJSON do protótipo já grava `transmission_type`
  do SBS) e por aeronave (a tabela de pistas, ainda não implementada, agrega
  por ICAO). Escrever isso no DP
  explicitamente; se a banca esperar outra coisa, a pergunta aparece cedo.
- "store": o NDJSON a bordo (HLR-SW-02).

#### REG-13

**= HLR-ADS-04**

> Teams must define whether processing is performed onboard, on the ground,
> or hybrid, and justify the choice.

#### REG-14

**= HLR-ADS-05**

> The mission must be conducted for 10 continuous minutes, with a maximum of
> 20 aircraft per test.

#### REG-15

**= HLR-ADS-06**

> Additionally, each team must propose a secondary mission compatible with
> the main mission. Implementation is not mandatory.

#### REG-16

**Além do HLR** (acrescenta *heading* e origem/destino) · Dono: A + L ·
Estado: `PENDENTE`

*4.4.1 Mission Objectives — Primary objective:*

> Design and validate a CubeSat system capable of:
>
> - Detecting ADS-B signals from aircraft
> - Decoding position, velocity, heading, and altitude
> - Reconstructing trajectories and estimating origin and destination
> - Transmitting processed data as telemetry

- **Heading.** O que o SBS entrega (campo 13) e o registro de track carrega
  (`track_deg`) é o rumo sobre o solo, não a proa magnética. Declarar no DP
  que o *heading* reportado é o rumo, ou extrair a proa das mensagens de
  velocidade que a trazem.
- **Origem e destino não têm plano nenhum.** É objetivo primário e o
  ADR-0003 coloca na ground, mas o PLANO não prevê a ferramenta. Opções:
  extrapolar a trajetória contra uma base de aeroportos, ou usar o callsign.
  Num cenário simulado de 10 minutos, a trajetória pode não chegar perto de
  nenhum aeroporto — definir o método e declarar a incerteza.
- **Trajetória:** depende da semântica de `age_ds` (HLR-ADS-07).

#### REG-17

**Sem HLR** · Dono: todas · Estado: `PARCIAL`

*4.4.1 Mission Objectives — Objetivos secundarios:*

> - Evaluate system architecture decisions
> - Analyze trade-offs between coverage, latency, and power
> - Strengthen technical documentation practices

- Os ADRs e os budgets são exatamente essa avaliação. Cobertura (link
  budget) e latência (data budget) estão analisadas; **potência não**, porque
  o power budget não existe.

#### REG-18

**Além do HLR** (HLR-GEN-02 — ver [§2.5](#25-duas-listas-de-subsistemas-mínimos))

*4.4.2 System Architecture:*

> Each team must implement a system including at least:
>
> - CubeSat platform (1U to 3U)
> - OBC
> - EPS with battery
> - Communication system
> - ADS-B payload
> - ADS-B antenna (1090 MHz)
> - Decoding and processing software
> - Ground segment for control and analysis

#### REG-19

**Além do HLR** (HLR-ADS-01 e 02; "structured telemetry" liga ao
HLR-COMM-02) · Dono: A · Estado: `PARCIAL`

*4.4.3 ADS-B Payload:*

> The payload must:
>
> - Receive ADS-B signals
> - Decode standard messages
> - Extract at least:
>   - ICAO address
>   - Position (latitude, longitude)
>   - Altitude
>   - Velocity
> - Generate structured telemetry

- A telemetria estruturada do payload são os registros `TM_TRACKS` e
  `TM_STAT` (`common/gama_tm.h`).

### 4.5 Software, dados e V&V — seções 5 e 6

#### REG-20

**= HLR-SW-01**

> The system must include software capable of:
>
> - Managing operational modes
> - Executing the mission according to the defined ConOps

#### REG-21

**Além do HLR** (HLR-SW-02: acrescenta "Available for post-analysis")

> Mission and telemetry data must be:
>
> - Structured
> - Documented
> - Reproducible
> - Available for post-analysis

#### REG-22

**Além do HLR** (HLR-VV-01 e 02: acrescenta rastreabilidade) · Dono: todas ·
Estado: `PENDENTE`

> Each team must define a V&V strategy demonstrating compliance with all
> high-level requirements.
>
> The V&V plan must include:
>
> - Verification methods (test, analysis, inspection, demonstration)
> - Justification of methods
> - Traceability between requirements and tests
>
> Not all planned tests will be executed. Only those defined by the
> organization will be performed.

- A convenção de citar o ID do requisito no código que o atende (AGENTS.md)
  existe para construir essa matriz.

### 4.6 Testes ambientais — seção 8

Opcionais, mas com efeito multiplicador:

> Environmental testing will take place after the mission is completed and is
> not mandatory. Survival results in a 1.4 multiplier on mission results.

#### REG-23

**Sem HLR** (liga ao HLR-GEN-01) · Dono: estrutura

> Only for teams passing Fit-Check inspection.

> To conduct environmental testing, it is mandatory to pass a "Fit-Check"
> inspection (which verifies the dimensions of the mechanical enclosure
> specified in the CDS) and inspect loose parts (which could cause accidents
> during vibration testing).

- Peças soltas do TT&C a verificar: dongle SDR, cabos e conectores de
  antena, cartão SD.

#### REG-24

**= HLR-ENV-01**

> The system must execute the mission before testing.

> The system must be capable of performing its mission prior to
> environmental testing.

#### REG-25

**Além do HLR** (HLR-ENV-02: acrescenta "before, during, and after")

> After thermal and vibration tests, it must:
>
> - Demonstrate functional survival, or
> - Justify limitations
>
> based on telemetry analysis.

> Following the vibration and thermal tests specified by the organization,
> the system must:
>
> - demonstrate functional survival, or
> - justify the observed limitations,
>
> by analyzing the telemetry data collected before, during, and after the
> tests.

#### REG-26

**Além do HLR** (HLR-EPS-04 e 05: acrescenta o perfil térmico) · Dono:
estrutura / EPS, com a TM do TT&C

*Thermal Tests* e *8.1 Thermal Tests*:

> The system will undergo thermal cycling testing to evaluate its functional
> performance and the response of critical subsystems to temperature
> variations. Initially, two (2) thermal cycles are planned, with the
> following reference parameters:
>
> - Range: –10 °C to +50 °C (TBC)
> - Ramp rate: ~1.5 °C/min
> - Stabilization: 30 minutes
> - Battery must remain above 0 °C

> Note:
>
> 1. If the system includes an internal battery, its temperature shall be
>    maintained above 0 °C at all times during the test. If necessary, the
>    team must implement and justify active thermal control strategies to
>    meet this requirement.
> 2. Alternatively, teams may remove the internal battery during thermal
>    vacuum testing and operate the system using an external power source,
>    provided that:\
>    a. the system remains fully functional during the test, and\
>    b. the selected configuration is clearly documented and justified.
> 3. The final parameters of the thermal test, as well as the number of
>    cycles, may be adjusted depending on the number of participating teams,
>    infrastructure availability, and total event duration. These parameters
>    will be confirmed and communicated by the organization prior to testing.

- A nota 1 é o motivo da lacuna do [HLR-EPS-04](#hlr-eps-04): sem
  temperatura de bateria na TM, não há como demonstrar os 0 °C.
- Dois ciclos com rampa de ~1,5 °C/min entre –10 °C e +50 °C, mais 30 min de
  estabilização em cada extremo, somam algumas horas de ensaio. O registro a
  bordo precisa cobrir esse tempo, não só os 10 minutos da missão.

#### REG-27

**Sem HLR** (liga ao HLR-ENV-02) · Dono: estrutura

*Vibration Test* e *8.2 Vibration Test* — as duas versões se contradizem
([§2.2](#22-nível-de-vibração-contraditório)):

> Random vibration test in accordance with NASA/GEVS-14.1 requirements:
> 2 minutes per axis at 1 g.

> - Random profile per NASA GEVS
> - 14.1 Grms, 2 minutes per axis
> - Inside of the Test-POD

#### REG-28

**Sem HLR** (liga ao HLR-ENV-02) · Dono: L

*8.3 Post-testing:*

> Send any telemetry data to demonstrate subsequent operation.

### 4.7 Apresentação e avaliação — seções 9 e 8 (*Evaluation System*)

#### REG-29

**Sem HLR** · Dono: todas

*9. Final Presentation:*

> Each team will present:
>
> - CubeSat design
> - Test results
> - Environmental telemetry
> - Performance analysis
>
> Evaluated by a specialist panel.

- "Environmental telemetry" e "Performance analysis" vão sair do gerador
  de relatório da ground (`ground/host/`, PLANO fase 4.2), alimentado pelos logs
  de bordo e da ground.

#### REG-30

**Além do HLR** (HLR-COST-01 e 02: acrescenta o fator de eficiência)

*8.1 Budget & Fair Play:*

> Teams must present:
>
> - Total budget
> - Subsystem costs
> - COTS vs custom justification
>
> An efficiency factor will consider:
>
> - Performance
> - Cost
> - Technical coherence

Contexto — *8. Evaluation System* e *8.3 Special Awards*:

> Final score is based on:
>
> - Documentation
> - Technical tests
> - Mission
> - Environmental tests
> - Final presentation

| Prêmio | Critério (literal) |
|---|---|
| Best System Design | Clear, coherent, and defensible architecture |
| Best ADS-B payload implementation | Technical performance of the receiver and processing |
| Best technical documentation | Clarity, traceability, and professional standard |
| Grand Prize Winner: CubeDesign CubeSat | Highest weighted total score |

---

## 5. Conteúdo obrigatório do Design Package

Checklist da [REG-05](#reg-05): onde cada item vai estar e em que estado está.

| Item da REG-05 | Onde vai estar | Dono | Estado |
|---|---|---|---|
| Mission concept | `docs/conops.md` — a criar; `obc/docs/conops.md` tem só o título | todas | `PENDENTE` |
| System architecture | ADRs, diagrama do `README.md`, `docs/arquitetura.png` | todas | `PARCIAL` — ADRs 0001–0004 escritos |
| Mass budget | — | estrutura | não acompanhado aqui |
| Power budget | `docs/budgets/power-budget.md` | EPS, com o TT&C | `PENDENTE` |
| Data budget | `docs/budgets/data-budget.md` | TT&C | `PARCIAL` — números corrigidos; falta a medição de CPU do `dump1090` |
| Financial budget | — | todas | `PENDENTE` |
| ADS-B payload design | ADR-0003, ADR-0008 (a escrever) | A | `PARCIAL` |
| V&V plan | `docs/vv/vv-plan.md` | todas | `PENDENTE` |
| Cost estimation and justification | — | todas | `PENDENTE` |
| System block diagram | `docs/arquitetura.png`, diagrama do `README.md` | todas | `PARCIAL` |
| Communication scheme | ADR-0004, `docs/budgets/link-budget.md`, `docs/icd/ota-protocol-icd.md` (a escrever) | L | `PARCIAL` |
| Electrical architecture and power management | — | EPS | não acompanhado aqui |
| Implemented algorithms | código de `flight/` e `ground/`, com os ADRs | A + L | `PENDENTE` — `flight/` e `ground/` vazios |
| Testing and validation procedures | `docs/vv/` | todas | `PENDENTE` |

"Implemented algorithms" pede código que exista na data de entrega, não só
projeto. É o que torna o firmware prioridade antes de 27/09.

---

## 6. Comentários gerais

**Dois prazos, duas barras.** O DP (27/09) é avaliação de documentação e é
eliminatório ([REG-03](#reg-03)): cada requisito precisa chegar com a forma
de atendimento, o método de verificação justificado e um caso de teste
rastreável. Os testes em UNSAM (24/11) executam o que for verificado por
teste ou inspeção. Um requisito verificado por teste pode chegar ao DP como
`PROJETADO`, desde que com procedimento e critério de aceite escritos.

**Para nove HLR, o DP é a própria verificação.** HLR-GEN-03, HLR-GEN-04,
HLR-EPS-03, HLR-ADS-04, HLR-ADS-06, HLR-VV-01, HLR-VV-02, HLR-COST-01 e
HLR-COST-02 são verificados só por revisão de documentação. Não existe
"demonstrar depois": o que não estiver no DP está descumprido.

**O que o time define faz parte do requisito.** HLR-COMM-01 (limite de
tempo de resposta), HLR-ADS-08 (métricas de sucesso), HLR-GEN-04 (requisitos
derivados) e HLR-EPS-03 (orçamento de potência) só são cumpridos quando o
número está escrito. Um sistema rápido sem limite declarado não atende o
HLR-COMM-01.

**O método de verificação diz qual evidência produzir.**

| Método | Evidência |
|---|---|
| Test | procedimento, critério de aceite e registro do resultado |
| Analysis | cálculo reproduzível — script em `tools/analysis/` |
| Inspection | medição ou verificação física, com registro |
| Documentation review | o documento em si |
| Data inspection / Data validation / Telemetry analysis | dado bruto gravado e, na validação, um gabarito para comparar |

**Oito HLR dependem de logs que ainda não têm formato.** Sete têm inspeção
de dados, validação de dados ou análise de telemetria no próprio método de
verificação: HLR-COMM-02, HLR-EPS-04, HLR-ADS-02, HLR-ADS-05, HLR-ADS-07,
HLR-ADS-08 e HLR-SW-02. O oitavo, HLR-ENV-02, depende de telemetria pelo
texto da [REG-25](#reg-25). Sem o NDJSON a bordo e o log da ground com o
frame bruto, não há evidência para nenhum deles. Definir o formato dos dois
logs vem antes dos testes, não depois.

**O catálogo HLR não é o contrato inteiro.** A seção 4 tem 30 itens do corpo
do regulamento; os marcados "sem HLR" ou "além do HLR" também são avaliados
— alguns valem pontos diretos, como a implantação de antena (REG-07) e o
caminho cabeado (REG-06).

**Rastreabilidade se constrói agora, não no fim.** Citar o ID (`HLR-…` ou
`REG-…`) no comentário do código e no documento que atende o requisito. A
matriz do DP sai disso; o prêmio de documentação avalia "Clarity,
traceability, and professional standard".

**Um número, um lugar** (HLR-SYS-01). Cada valor mora em um único documento
e os outros apontam para ele. O que vier de cálculo entra no
`lora_budget.py --check`, que roda no CTest.

**Acompanhar a organização.** Há valores TBC, a pontuação final ainda não
foi publicada e a organização promete esclarecimentos antes do evento
([§2.7](#27-valores-tbc-e-pontuação-em-aberto)). Quando chegarem, atualizar
este documento ([§9](#9-como-manter-este-documento)).

---

## 7. Lacunas e ações

### 7.1 Antes do DP — 27/09

| # | Ação | Requisitos | Dono sugerido | Onde registrar |
|---|---|---|---|---|
| 1 | Definir o limite de tempo de resposta a TC, incluindo a regra de quando a ground pode transmitir | HLR-COMM-01 | L | ADR-0007 |
| 2 | Definir a semântica de `age_ds` | HLR-ADS-07, HLR-ADS-08, REG-16 | A + L | ADR-0007, ICD do enlace |
| 3 | Acrescentar a temperatura da bateria ao `TM_HK` | HLR-EPS-04, REG-26 | L, com a EPS | novo ADR, vetores, `data-budget.md` |
| 4 | ~~Corrigir a linha NDJSON (110 → ~160 B) e conciliar 89:1 × 106:1~~ — feito em 2026-09-19 | HLR-SYS-01, HLR-ADS-04 | A + L | `data-budget.md`, errata do ADR-0003, `--check` |
| 5 | Escrever os ADRs citados que não existem: 0007 (0008 e 0011 feitos em 2026-09-19) | HLR-SYS-01, HLR-COMM-03 | L e A | `docs/adr/` |
| 6 | Escrever o ICD do enlace e o ICD OBC↔TT&C | HLR-SW-02, HLR-COMM-03 | L | `docs/icd/` |
| 7 | Definir o formato do log da ground: frame bruto, timestamp, RSSI, SNR | HLR-SW-02, HLR-ADS-08 | L | ICD do enlace |
| 8 | Escrever os requisitos derivados do TT&C (`TTC-nn`) | HLR-GEN-04 | L + A | `docs/vv/` |
| 9 | Definir o método de estimativa de origem e destino | REG-16 | A | novo ADR |
| 10 | Declarar no DP o que é "classificar" e que *heading* é o rumo | REG-12, REG-16 | A | ADR-0008 |
| 11 | Atribuir um dono e escrever o conceito da missão secundária | HLR-ADS-06 | a definir | DP |
| 12 | Medir o consumo de Pi, SDR e LoRa em cada modo | HLR-EPS-03, REG-17 | L + A | `power-budget.md` |
| 13 | Levantar o custo dos itens do TT&C | HLR-COST-01, REG-30 | L + A | a definir |
| 14 | Plano de V&V e matriz de rastreabilidade | HLR-VV-01, HLR-VV-02, REG-22 | todas | `docs/vv/` |
| 15 | Implementar `ttcd`, `adsbd` e a ground — o DP pede algoritmos implementados | REG-05 | L + A | PLANO, fases 2 a 4 |

### 7.2 Antes de UNSAM — 24/11

| # | Ação | Requisitos | Dono sugerido |
|---|---|---|---|
| 16 | Caminho cabeado de TC e TM, conforme a resposta da organização | REG-06 | L |
| 17 | Comando de implantação da antena com argumento de confirmação; confirmar o fluxo com o OBC | REG-07 | L + OBC |
| 18 | Registro a bordo cobrindo as horas de ensaio ambiental; plano para RF atenuado no Test-POD | HLR-ENV-02, REG-25, REG-26 | L |
| 19 | Fixação mecânica do dongle SDR, dos conectores de antena e do cartão SD para 14.1 Grms | REG-23, REG-27 | L + estrutura |
| 20 | Avisar a ADCS de que o detumbling precisa de registro de alta taxa a bordo | HLR-ADCS-02, HLR-ADCS-03 | ADCS + OBC |
| 21 | Medir a CPU do `dump1090` na Pi e varrer o ganho do SDR | HLR-ADS-01, HLR-ADS-05 | A |

---

## 8. Perguntas para a organização

1. **Vibração:** 1 g ou 14.1 Grms? As seções *8. Environmental Testing* e
   *8.2 Vibration Test* se contradizem ([§2.2](#22-nível-de-vibração-contraditório)).
2. **Térmico:** o ensaio é em vácuo (TVAC) ou só ciclagem térmica em ar?
   ([§2.3](#23-vácuo-ou-só-ciclagem-térmica))
3. **Teste de comunicação:** o que conta como "wired" — cabo de dados ou
   rádio ligado por cabo coaxial? Os pontos de "wired" e "RF" somam?
   ([REG-06](#reg-06))
4. **Cenário ADS-B:** vem com gabarito (aeronaves e mensagens injetadas),
   para calcular a taxa de decodificação? ([HLR-ADS-08](#hlr-ads-08))
5. **Heading:** o rumo sobre o solo é aceito? ([REG-16](#reg-16))
6. **Classificação:** o que se espera de "Classify"? ([REG-12](#reg-12))
7. **433 MHz no local:** há restrição de frequência, potência ou tempo de
   transmissão na UNSAM? (ADR-0004)
8. **Detumbling:** a rotação é medida pela organização ou pela telemetria do
   time? ([HLR-ADCS-02](#hlr-adcs-02))

---

## 9. Como manter este documento

- **Estado:** quando um requisito mudar de estado, atualizar o painel e o
  bloco do requisito, citando a evidência (commit, arquivo, resultado de
  teste). Atualizar também a data e o commit no topo.
- **Texto literal:** conferir as citações contra o PDF com

  ```bash
  python3 tools/analysis/check_requirements.py
  ```

  Quando sair a versão final do regulamento, trocar o PDF em `docs/` e rodar
  de novo: o script aponta toda citação que deixou de existir no texto.
- **IDs `REG-nn` são estáveis.** Item novo recebe o próximo número livre;
  item removido do regulamento fica marcado como removido, sem renumerar.
