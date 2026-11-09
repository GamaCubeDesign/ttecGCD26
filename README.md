# ttecGCD26 — TT&C, OBC, payload ADS-B e ground station

Repositório da Gama Cube Design para a **CubeDesign 2026**: o software de voo
do Cubetschek (telecomunicação, computador de bordo e payload ADS-B), a
ground station e a documentação de arquitetura.

O hardware: uma Raspberry Pi Zero 2 W a bordo; um rádio LoRa SX1278 (módulo
RA-02) em 433,92 MHz de cada lado do enlace; um SDR NooElec NESDR Nano 3 para
o ADS-B em 1090 MHz; e uma ESP32 DevKit V1 como ground station.

## O número que governa tudo

O enlace LoRa desce **58 KB** nos 10 minutos da missão; 20 aviões geram
**7,34 MiB** de ADS-B bruto no mesmo tempo. É uma diferença de 129 vezes, e
ela obriga o satélite a decodificar e agregar a bordo: ele desce retratos
das aeronaves, com 20 bytes cada, em vez do fluxo bruto. Quase toda a
arquitetura sai daí (`docs/budgets/data-budget.md`, ADR-0003).

## O que tem aqui

| Diretório | O quê | Roda em | Estado em 03/11 |
|---|---|---|---|
| `common/` | O codec compartilhado: frames, CRC, telemetria, telecomandos, IPC, perfis LoRa, o lado da ground do protocolo, os frames do AOCS | Pi, PC **e** ESP32 | pronto |
| `flight/ttcd/` | Telecomunicação: o enlace, os telecomandos, a telemetria; o centro da comunicação entre os processos | Pi | pronto; bancada de 09/10 no protocolo v1 |
| `flight/radio/` | Driver do SX1278 e os rádios real e simulado (UDP) | Pi | pronto |
| `flight/libipc/` | Transporte IPC entre os processos de bordo | Pi | pronto |
| `flight/adsbd/` | Payload ADS-B: supervisiona o `dump1090-fa`, grava o registro de bordo e manda retratos ao `ttcd` | Pi | pronto; com o SDR em 09/10 |
| `flight/obcd/` | Computador de bordo: modos da missão, burn-wire, termostato, sensores da bateria, o I²C emprestado ao AOCS | Pi | pronto; **nunca rodou em hardware** |
| `flight/conf/obc-v2/` | A configuração de voo da placa nova do OBC | Pi | pronta; conferida contra os layouts |
| `ground/console/` | O console da ground station, o mesmo no PC e na ESP32 | Pi, PC, ESP32 | pronto |
| `ground/esp32/` | Firmware da ground station (ESP-IDF, no Docker); `ground/esp32-tests/` roda as suítes portáveis na ESP32 | ESP32 | compila; passa no QEMU; **nunca rodou na placa** |
| `ground/host/` | No PC: a ponte serial da ESP32 (`gs_serial.py`), o adaptador do downlink e o estimador de origem e destino | PC | pronto |
| `tools/` | `gs_cli` (ground de bancada), roteiros da bancada, simulador do `dump1090`, análise, ESP32, CI | Pi, PC | pronto |
| `hardware/` | Fontes KiCad das placas fabricadas e das revisões novas, não fabricadas (`-v2`) | — | — |
| `docs/` | ADRs, orçamentos, ICD do AOCS, requisitos, CDR, relatórios, evidência da bancada | — | — |
| `ultima_missao/` | Código da competição anterior, **congelado**, só referência | — | — |

O software do OBC é escrito aqui desde 20/10/2026. O repositório da outra
equipe (`../obc`) ficou congelado como referência.

## Arquitetura em uma figura

```
 CUBESAT · Raspberry Pi Zero 2 W                              GROUND STATION
 ┌──────────────────────────────────────────────────┐      ┌─────────────────────────┐
 │ [NESDR] ─USB─► dump1090-fa ─TCP :30003 (SBS-1)─┐  │      │ [RA-02] ─SPI─► ESP32    │
 │                                                ▼  │      │   ▲              │ USB  │
 │  adsb.ndjson ◄─ grava ─ adsbd                     │      │   │              ▼      │
 │                          │ IPC                    │      │   │        gs_serial.py │
 │                          ▼                        │      │   │              │ JSON │
 │  [RA-02] ◄─SPI+DIO0─ ttcd ◄──IPC──► obcd          │      │   │              ▼      │
 │     ▲                               │             │      │   │       tracks_to_    │
 │     │      INA219 ◄──I²C1 (dividido)┤             │      │   │       ndjson.py     │
 │     │      ESP32 do AOCS ◄──────────┤             │      │   │              ▼      │
 │     │      DS18B20 ◄─1-Wire─────────┤             │      │   │       estimador e   │
 │     │      burn-wire, aquecedores ◄─┘ GPIO        │      │   │       painel        │
 └─────┼─────────────────────────────────────────────┘      └───┼─────────────────────┘
       └──────────────────── LoRa 433,92 MHz ───────────────────┘
```

- O `ttcd` é o centro: o `adsbd` e o `obcd` se conectam a ele por um socket
  `AF_UNIX`, com os mesmos frames do rádio (ADR-0005, ADR-0006).
- Cada processo tem um núcleo sem chamadas de sistema, testado com relógio
  virtual, e uma casca `epoll` que fala com o sistema operacional.
- O modo da missão, decidido pelo `obcd`, liga a telemetria do `ttcd` e o
  decodificador do `adsbd` (ADR-0014).

O guia de estudo (`docs/relatorios/2026-10-31-guia-de-estudo.html`) explica a
arquitetura, os protocolos e cada arquivo.

## Build e testes

```bash
make                  # configura e compila (CMake, em build/)
make test             # as 38 suítes; make test T=obcd roda só as que casam
make check            # antes de qualquer merge: testes, sanitizers, zero warnings, citações
make check-debian     # o mesmo num contêiner Debian 13, com o GCC 14 da Pi (é o que a CI roda)
make help             # todos os alvos
```

O `make check` roda os testes, compila de novo em `build-asan/` com ASan e
UBSan e com todo warning virando erro, roda os testes ali e confere as
citações de `docs/requisitos.md` contra o regulamento. Na Pi, o segundo build
usa só o UBSan, porque o ASan não inicia no espaço de endereços de 39 bits
dela. Os comandos por baixo (`cmake`, `ctest`, os scripts de `tools/`)
continuam valendo sozinhos.

O build é mantido em **zero warnings** com `-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion`. Isso não é cosmético: quase todo bug que este
projeto não pode pagar, um campo de tamanho truncado, um sinal perdido numa
latitude, aparece primeiro como conversão implícita.

A ESP32 compila no Docker, sem instalar o ESP-IDF:

```bash
make esp32-build                          # o firmware da ground
make esp32-tests-qemu                     # as suítes portáveis numa ESP32 emulada
make esp32-flash ESP32_PORT=/dev/ttyUSB0  # grava na placa
```

## Sem hardware, e na bancada

```bash
make bench-ping RADIO=udp                 # um passo da bancada no rádio simulado
make adsb-chain INPUT=<captura.sbs|registro.ndjson>   # uma missão inteira pela cadeia
```

- Para mandar o código do PC para a Pi, os passos da bancada dos rádios e a
  instalação dos serviços, veja `flight/ttcd/README.md`.
- O roteiro da próxima bancada é
  `docs/relatorios/2026-11-03-roteiro-bancada.html`.
- A evidência das bancadas fica em `docs/vv/bancada/`.

## Orçamento de enlace e de dados

Todo número citado nos ADRs sai de um script:

```bash
make budget           # as tabelas completas (tools/analysis/lora_budget.py)
make budget-check     # confere contra os ADRs (também roda no make test)
make boards           # a configuração de voo conferida, fio a fio, contra os layouts das placas
```

## Onde começar a ler

1. `AGENTS.md`: as regras do repositório, o mapa e o estado corrente.
2. O guia de estudo, `docs/relatorios/2026-10-31-guia-de-estudo.html`: do
   conceito ao arquivo.
3. `docs/adr/README.md`: o índice das decisões. Comece pelo ADR-0003 (por que
   o processamento é a bordo) e pelo ADR-0004 (a configuração do rádio, com a
   aritmética).
4. `PLANO.md`: o plano até a prova, as decisões, as questões em aberto e os
   riscos.
5. `docs/requisitos.md`: cada requisito citado literalmente, com o estado.

Os relatórios de estado, datados, ficam em `docs/relatorios/`; o mais recente
é `2026-11-03-status-firmware.html`.

## Convenções

- **Código e comentários: inglês.** ADRs, ICDs e orçamentos também: o
  regulamento (§2) exige o Design Package em inglês, e esses documentos entram
  nele quase literalmente.
- **READMEs, PLANO e notas de trabalho: português.**
- Identificadores de requisito (`HLR-ADS-07` etc.) são citados em comentário
  no ponto do código que os atende; a matriz de rastreabilidade sai daí.
- Nunca serializar struct por cast. Todo campo multi-byte passa pelos helpers
  little-endian de `common/gama_bytes.h`. O porquê está no ADR-0002.

## `ultima_missao/`

O código da competição anterior, **congelado**, mantido só como referência.
O que foi reaproveitado e o que foi descartado está nos ADRs 0002 e 0004, com
o motivo de cada descarte. Os defeitos dele viraram as "regras duras" do
`AGENTS.md`.
