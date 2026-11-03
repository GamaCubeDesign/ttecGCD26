# Placas do Cubetschek — fontes KiCad

As placas como foram **fabricadas**: as fontes dos dois painéis enviados à
Circuibras em 08/10/2026, trazidas de `PCBS_GCD_Com_Paineis.zip` em
22/10/2026. Uma pasta por placa; cada uma abre no KiCad pelo `.kicad_pro`
(os painéis exigem o KiCad 10). As exceções são as pastas `-v2`, que ainda
não foram fabricadas: `obc-v2/`, a placa nova do OBC, e `eps-potencia-v2/` e
`aocs-v2/`, da revisão TF de 01–02/11 (abaixo).

| Pasta | Placa | Observação |
|---|---|---|
| `obc/` | OBC: Raspberry Pi Zero 2 W, INA219, barramento | revisão de 08/10 (`PCB_NOVAS_REVISADAS/PCB_OBC_NOVA`); esquemático e layout **divergem** no I²C (pinos 10 e 12); a `obc-v2/` a substitui |
| `obc-v2/` | OBC, placa nova | esquemático de 23/10 (`PCB_OBC_NEW.zip`), layout da revisão TF de 02/11; **não fabricada**; corrige A1, A2, A3 e A7 |
| `ttec/` | TT&C: Ra-02, USB do SDR, conector do mecanismo | bibliotecas locais em `Library.pretty/` e `lib.sym/` |
| `eps-potencia/` | EPS: BMS 2S, conversores de 5 V, 3,3 V e 12 V, entrada externa | revisão de 08/10 (`PCB_NOVAS_REVISADAS/PCB_EPS_EXTRA_NOVA`) |
| `eps-potencia-v2/` | EPS, revisão TF | layout de 02/11, **mesmas ligações** da fabricada; sem esquemático; **não fabricada** |
| `bateria-cima/` | bateria de cima: 18650, aquecedores, TIP120, DS18B20 | |
| `bateria-baixo/` | bateria de baixo: 18650, aquecedores, TIP120 | sem sensor de temperatura |
| `aocs/` | AOCS: ESP32, BMI088, AS5048A, DRV8313 | o esquemático estava em `AOCS/grb/` no zip |
| `aocs-v2/` | AOCS, revisão TF | layout de 02/11, **mesmas ligações** do fabricado; sem esquemático; **não fabricado** |
| `painel-solar-1/`, `painel-solar-2/` | painéis solares | |
| `topo/` | tampa superior | só logotipos, sem componentes |
| `paineis/` | os dois painéis de fabricação, 20 placas cada | ver abaixo |
| `antenas/` | baluns LC do LoRa e do ADS-B: valores comerciais e prioridade de compra (`componentes_baluns.pdf`, co-simulação HFSS, out/2026) | só o PDF; as antenas são descritas no CDR §11.2 |

A placa do **mecanismo** (burn-wire) não estava no zip.

As revisões de 08/10 do OBC e da EPS são as que foram para os painéis
(conferido pelo SHA-256 citado em `paineis/Relatorio_Painelizacao.md`). As
versões anteriores, que ficavam em `OBC/` e `EPS/` no zip, não foram
fabricadas e ficaram de fora. A revisão não mudou nenhuma ligação do
barramento de 40 pinos nas duas placas.

## `obc-v2/` — a placa nova do OBC

O esquemático de `PCB_OBC_NEW.zip` (KiCad de 23/10/2026), a pinagem em uso
desde 31/10, com o layout da revisão TF de 02/11 (seção seguinte; o layout
de 23/10 fica no histórico do git, commit `99adcf1`). Ficaram de fora o
`.history/` e o `.kicad_prl`. Conferida pelo netlist do esquemático e pelos
pads do `.kicad_pcb`, que concordam.

Dos achados de `docs/relatorios/2026-10-22-validacao-obc.html`, corrige:

| Achado | Na placa nova |
|---|---|
| A1: o 8 V passava pelo INA219 em pinos que a EPS não usa | entra do 8 V Bruto no J1.38, onde a EPS o põe, e sai no J1.33; o J1.35 ficou livre |
| A2: o barramento do LoRa trocado com o da TT&C | J1.21 SCK, 23 MISO, 25 MOSI, 27 NSS, 29 RESET, 31 DIO0, como o J3 da TT&C |
| A3: SDA e SCL trocados no layout | J1.10 SDA, J1.12 SCL, como o AOCS |
| A7: DS18B20 sem pull-up | R1, 4,7 kΩ, do 3,3 V à linha do DS18B20 (GPIO17, J1.9) |

Não muda:

- **A5 e A6:** R2 e R3 continuam 4,7 kΩ entre o GPIO e a base dos TIP120
  (GPIO22 → J1.13, burn-wire; GPIO23 → J1.11, aquecedores). São ~0,4 mA de
  base, ~0,4 A de coletor com o ganho mínimo de 1000 do TIP120: pouco para o
  1 A do burn-wire.
- **O INA219 fica no I²C1**, dividido com o ESP32 do AOCS (GPIO2/3, J1.10 e
  J1.12): a proposta de 24/10 de levá-lo ao I²C0 não entrou. O `obcd`
  empresta o barramento ao ESP32 durante o `ADVANCED_AOCS` (ADR-0017).

Pinos da Pi nesta placa:

| Função | GPIO | Barramento (J1) |
|---|---|---|
| LoRa SCK, MISO, MOSI, NSS | 11, 9, 10, 8 (SPI0, CE0) | 21, 23, 25, 27 |
| LoRa RESET, DIO0 | 25, 4 | 29, 31 |
| I²C1 SDA, SCL: INA219 (0x40) e ESP32 do AOCS | 2, 3 | 10, 12 |
| DS18B20 (1-Wire) | 17 | 9 |
| burn-wire, aquecedores (bases dos TIP120) | 22, 23 | 13, 11 |

Com o DIO0 no GPIO4, que é o pino padrão do 1-Wire, o `config.txt` precisa
de `dtoverlay=w1-gpio,gpiopin=17`. A configuração do software para esta
placa está em `flight/conf/obc-v2/`.

## A revisão TF (01–02/11/2026)

`PCBS_GAMA_AOCS_EPS1_OBC_TF.zip`, recebido em 03/11: novos layouts do OBC, da
EPS e do AOCS. **Nenhuma ligação mudou.** Comparados pad a pad, pela net de
cada pad no `.kicad_pcb` (`tools/analysis/board_check.py diff`):

- o OBC TF contra o `obc-v2` de 23/10: 0 mudanças; esquemático, `.kicad_pro`
  e `.kicad_dru` idênticos byte a byte;
- a EPS TF contra a `eps-potencia/` fabricada: 0 mudanças;
- o AOCS TF contra o `aocs/` fabricado: 0 mudanças.

O que mudou é o cobre:

| Placa | Trilhas | Vias | Componentes |
|---|---|---|---|
| OBC | re-roteadas, todas de 1 mm: 258 → 206 segmentos | 40 → 33 | nenhum se moveu |
| EPS | todas de 1 mm, como antes: 107 → 98 segmentos | 343 → 37 | a chave SW1 desceu ~7 mm; furos renumerados |
| AOCS | de 0,5 para 1 mm: 115 → 157 segmentos | 391 → 47 | furos renumerados |

O contorno (83 × 83 mm) e os conectores do barramento estão no mesmo lugar:
o empilhamento não muda.

**O DRC do KiCad** (`kicad-cli pcb drc`) não acha nenhum item desconectado
nem curto nas três, mas acha erros que as versões anteriores não tinham. São
para corrigir antes de fabricar:

- **OBC:** 5 violações da regra própria `caseira_isolacao_1mm`, com 0,86 a
  0,99 mm de folga. Os pares são as vias DIO0 × NSS e a trilha MOSI × a via
  MISO, do LoRa, e as vias TIP120(CT) × DS18B20. Há também 2 pontas de
  trilha soltas (SCL e DIO0).
- **AOCS:** a trilha do D34 do ESP32 passa a 0,24 mm do pad de GND do
  conector X+, contra os 0,5 mm da classe padrão. Há também 6 trilhas com uma
  ponta solta (três delas de ~22 mm, nas entradas IN1–IN3 do DRV8313) e duas
  vias empilhadas no mesmo ponto, no SDA do BMI088.
- **EPS:** só avisos de serigrafia e de footprint.

**Como entraram.** A EPS e o AOCS vieram sem esquemático: o da EPS estava
vazio e ficou de fora, porque um "Update PCB from Schematic" com ele apagaria
todos os componentes. O esquemático que vale para os dois é o da versão
fabricada (`eps-potencia/`, `aocs/`), já que as ligações são as mesmas. O
projeto do AOCS se chamava `AAAAAAAAAAAAAAAAAAAAAAAAAAAAA` no zip e foi
renomeado para `AOCS_TF`. Os footprints estão embutidos nos `.kicad_pcb`; a
biblioteca do AOCS continua em `aocs/`. Os modelos 3D apontam para caminhos
nos computadores dos autores, como nas versões fabricadas.

## O firmware e as pendências de 22/10

`python3 tools/analysis/board_check.py` segue cada fio que o firmware usa, do
GPIO da Pi ao pino do chip na outra placa. Ele lê os layouts do stack
(`obc-v2`, `ttec`, `eps-potencia-v2`, as baterias e `aocs-v2`) e a
configuração em `flight/conf/obc-v2/`. Em 03/11 passa em todas as 17
conferências, e roda no `make test`. Item a item de
`docs/relatorios/2026-10-22-validacao-obc.html`:

| Achado | Nas placas | O firmware |
|---|---|---|
| A1 · 8 V pelo INA219 | corrigido no OBC: entra do 8V(Bruto) da EPS no J1.38, sai no J1.33 para o 8V(Liq) | nada a mudar. Sem a placa do OBC no stack, nada recebe energia (HLR-EPS-05) |
| A2 · barramento do LoRa | corrigido: 21–31 casam com o J3 da TT&C | `ttcd.conf` confere: `spidev0.0`, RESET 25, DIO0 4 |
| A3 · SDA e SCL | corrigido: 10 SDA, 12 SCL no OBC e no AOCS | `obcd.conf` confere: `/dev/i2c-1` |
| A4 · enlace OBC–ESP32 | igual: só o I²C. O BMI088 está nos mesmos fios, e o ESP32 nos pads 26 e 29 do DevKit (GPIO21 e 22); a UART2 (GPIO16/17) segue livre | o empréstimo do barramento (ADR-0017) continua necessário, e é o que o `obcd` faz |
| A5 · base do burn-wire | igual: R2 de 4,7 kΩ | o firmware não compensa; medir (roteiro de novembro, etapa 13) |
| A6 · base dos aquecedores | igual: R3 de 4,7 kΩ para as duas bases | idem |
| A7 · pull-up do DS18B20 | corrigido: R1 de 4,7 kΩ ao 3,3 V | `config.txt` (`gpiopin=17`) e `w1_path = auto` conferem |
| A8 · GPIO no boot | igual: nenhum pull-down nas bases | o `obcd` abre as duas saídas em nível baixo; medir no boot |
| A9 · o INA219 mede o consumo | igual: a EPS não mudou de ligação | a corrente nunca fica negativa: o "negativo = carregando" de `common/gama_tm.h` não acontece. Decisão pendente |
| A10 · conferências rápidas | o EN do ESP32 segue sem ligação; a TT&C e o mecanismo não vieram no zip | — |

## `paineis/`

- `Painel_1.kicad_pcb`, `Painel_2.kicad_pcb` — os painéis, com as logos da
  `topo` acrescentadas à mão (só existem aqui).
- `Gerber_Painel_1/`, `Gerber_Painel_2/` — o que foi enviado ao fabricante,
  com `INSTRUCOES_CAM.txt`.
- `Relatorio_Painelizacao.md` — quantidades, arranjo, abas e pendências de
  fabricação; `Mapa_Paineis.png` e `Painel_N_mapa.json` — onde fica cada
  cópia; `Painel_N_DRC.json` e `Verificacao_Origem/` — os DRCs.
- `gerar_paineis.py`, `gerar_mapa.py`, `verificar_serigrafia.py` — os
  scripts do autor da painelização, para o Python do KiCad 10. Os caminhos de
  `SRC` foram ajustados para as pastas daqui; rodar o gerador de novo
  **apaga as logos** feitas à mão nos painéis (ver o relatório).
  `inspecionar_revisoes.py`, que comparava as versões antiga e revisada do
  OBC e da EPS, ficou de fora junto com as versões antigas.

## O que ficou de fora dos zips

Os backups automáticos do KiCad (`*-backups/`), os `.kicad_prl`
(preferências de cada usuário), `.bak`, `.lck`, `fp-info-cache`,
`.history/`, `__pycache__/`, arquivos `sync-conflict` do Syncthing, um
`.kicad_pcb` "recovered", os gerbers individuais do AOCS, os modelos 3D
(`.step`, `.stl`, `.f3z`), cinco `.kicad_sym` de 7 MB do AOCS que nenhum
esquemático usa, e arquivos de Altium do Ra-02. O `fp-lib-table` do TT&C
apontava para um caminho de outra máquina e foi reescrito para usar só
`Library.pretty`; o da bateria de cima, que também apontava para fora, ficou
de fora. Os `.kicad_pcb` e `.kicad_sch` trazem cópias embutidas dos símbolos
e footprints, então abrem sem essas bibliotecas.

## Conferir as ligações sem abrir o KiCad

```bash
kicad-cli sch export netlist -o /tmp/obc.net hardware/obc/PCB_OBC.kicad_sch
kicad-cli sch export netlist -o /tmp/obc-v2.net hardware/obc-v2/PCB_OBC.kicad_sch

# pelos layouts, que servem também às placas sem esquemático
python3 tools/analysis/board_check.py           # o firmware contra o stack
python3 tools/analysis/board_check.py bus       # o barramento de cada placa, lado a lado
python3 tools/analysis/board_check.py diff hardware/aocs/CUBETSCHEK_AOCS.kicad_pcb hardware/aocs-v2/AOCS_TF.kicad_pcb
kicad-cli pcb drc --severity-all -o /tmp/drc.rpt hardware/aocs-v2/AOCS_TF.kicad_pcb
```

A placa fabricada segue o `.kicad_pcb`, não o esquemático: o I²C do OBC é o
exemplo. As divergências encontradas entre as placas, e o que medir no stack,
estão em `docs/relatorios/2026-10-22-validacao-obc.html`.
