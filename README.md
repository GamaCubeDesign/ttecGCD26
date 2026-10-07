# ttecGCD26 — Sistema de Telemetria, Telecomando e Payload ADS-B

Repositório da equipe de TT&C da Gama Cube Design para a **CubeDesign 2026**.

Cobre três softwares e a documentação de arquitetura:

| Diretório | O quê | Roda em | Estado |
|---|---|---|---|
| `common/` | Codec de pacotes, tabela de perfis LoRa, payloads IPC e o lado da ground do protocolo | Pi **e** ESP32 | pronto |
| `flight/ttcd/` | Daemon de telecomunicação: enlace, telecomandos, telemetria | Raspberry Pi Zero 2 W | pronto; falta a bancada |
| `flight/radio/` | Driver do SX1278 e backends de rádio (hardware e UDP simulado) | Raspberry Pi Zero 2 W | pronto; falta a bancada |
| `flight/libipc/` | Transporte IPC entre os processos de bordo | Raspberry Pi Zero 2 W | pronto |
| `flight/adsbd/` | Daemon do payload ADS-B | Raspberry Pi Zero 2 W | fase 3 |
| `ground/esp32/` | Firmware da ground station | ESP32, ESP-IDF | fase 4 |
| `ground/host/` | CLI do operador, dashboard e gerador de relatório | PC | fase 4 |
| `tools/gs_cli/` | Ground station de bancada (segundo RA-02 ou rádio UDP) | Pi / PC | pronto |
| `tools/bench/` | Roteiros dos passos da bancada e o executor que sobe o `ttcd` para cada um | Pi / PC | pronto; ensaiado no rádio UDP |
| `tools/analysis/` | Orçamento de enlace e dados, tempo no ar medido, checagem das citações | PC | pronto |
| `docs/` | ADRs, ICDs, budgets, requisitos, plano de V&V | — | — |

Os comandos do dia a dia são alvos do `Makefile` (`make help` lista todos).
Para rodar o enlace inteiro sem hardware, para o teste de bancada com os
rádios e para mandar o código do PC para a Pi, veja `flight/ttcd/README.md`.

O computador de bordo (OBC) fica em **outro repositório**, mantido por outra
equipe. O contrato entre os dois está em `docs/icd/obc-ttec-icd.md`.

## Arquitetura em uma figura

```
 CUBESAT (Raspberry Pi Zero 2W)                          GROUND STATION
 ┌─────────────────────────────────────────┐            ┌──────────────────────┐
 │ [NESDR Nano 3] ──USB──► dump1090-fa     │            │  ESP32 (ESP-IDF)     │
 │                            │ TCP :30003 │            │      │ UART          │
 │                            ▼  (SBS-1)   │            │      ▼               │
 │                         ┌──────┐        │            │  host CLI (Python)   │
 │                         │adsbd │        │            │  + dashboard         │
 │                         └──┬───┘        │            └──────▲───────────────┘
 │        eventos.ndjson ◄────┤            │                   │ SPI
 │         (bruto, HLR-SW-02) │ SEQPACKET  │                   │
 │                            ▼            │              [SX1278/RA-02]
 │  obc ──SEQPACKET──►     ┌──────┐  SPI   │                   │
 │  (outro repo)           │ ttcd ├────────┼──[SX1278]── ) ) ) ┘  433 MHz
 │                         └──────┘        │
 └─────────────────────────────────────────┘
```

`ttcd` é o hub: um servidor de socket, dois clientes (`adsbd` e `obc`).

## Build e testes

```bash
make                  # configura e compila (CMake, em build/)
make test             # os testes; make test T=link_sim roda só os que casam
make check            # antes de qualquer merge
```

O `make check` roda os testes, depois compila de novo em `build-asan/` com
ASan e UBSan e com todo warning virando erro, roda os testes ali, e confere
as citações de `docs/requisitos.md` contra o regulamento. Na Raspberry Pi o
segundo build usa só o UBSan: o ASan não consegue iniciar no kernel dela, que
dá a cada processo um espaço de endereços de 39 bits (`SANITIZERS=` escolhe a
lista; bancada de 07/10/2026). É assim que o
código deve estar antes de qualquer merge. Os comandos por baixo — `cmake`,
`ctest`, os scripts de `tools/analysis/` — continuam valendo sozinhos.

O build é mantido em **zero warnings** com `-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion`. Isso não é cosmético: quase todo bug que este
projeto não pode pagar — um campo de tamanho truncado, um sinal perdido numa
latitude — aparece primeiro como conversão implícita.

## Orçamento de enlace e de dados

Todo número citado nos ADRs sai deste script:

```bash
make budget           # tabelas completas (tools/analysis/lora_budget.py)
make budget-check     # confere contra os ADRs (lora_budget.py --check)
```

O `--check` roda como teste no CTest, então a documentação não consegue
divergir do código em silêncio.

## Onde começar a ler

1. `docs/adr/README.md` — o índice das decisões, em ordem de importância.
2. `docs/adr/0003-processing-split-onboard-vs-ground.md` — por que o
   processamento é onboard. É a decisão que define a missão.
3. `docs/adr/0004-lora-phy-configuration.md` — por que a config de rádio
   mudou, com a aritmética.
4. `docs/icd/` — os contratos de interface.

## Convenções

- **Código e comentários: inglês.** ADRs, ICDs, budgets e plano de V&V também
  — o regulamento (§2) exige o Design Package em inglês, e esses documentos
  entram nele quase literalmente.
- **READMEs e notas de trabalho: português.**
- Identificadores de requisito (`HLR-ADS-07` etc.) são citados em comentário
  no ponto do código que os atende. É assim que a matriz de rastreabilidade em
  `docs/vv/` é construída.
- Nunca serializar struct por cast. Todo campo multi-byte passa pelos helpers
  little-endian de `common/gama_bytes.h`. O porquê está no ADR-0002.

## `ultima_missao/`

Código da competição anterior, **congelado**, mantido só como referência. Os
pontos reaproveitados e os descartados estão listados no ADR-0002 e no
ADR-0004, com o motivo de cada descarte.
