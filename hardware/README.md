# Placas do Cubetschek — fontes KiCad

As placas como foram **fabricadas**: as fontes dos dois painéis enviados à
Circuibras em 08/10/2026, trazidas de `PCBS_GCD_Com_Paineis.zip` em
22/10/2026. Uma pasta por placa; cada uma abre no KiCad pelo `.kicad_pro`
(os painéis exigem o KiCad 10). A exceção é `obc-v2/`, a placa nova do OBC,
que ainda não foi fabricada (abaixo).

| Pasta | Placa | Observação |
|---|---|---|
| `obc/` | OBC: Raspberry Pi Zero 2 W, INA219, barramento | revisão de 08/10 (`PCB_NOVAS_REVISADAS/PCB_OBC_NOVA`); esquemático e layout **divergem** no I²C (pinos 10 e 12); a `obc-v2/` a substitui |
| `obc-v2/` | OBC, placa nova | KiCad de 23/10 (`PCB_OBC_NEW.zip`, recebido em 31/10); **não fabricada**; corrige A1, A2, A3 e A7 |
| `ttec/` | TT&C: Ra-02, USB do SDR, conector do mecanismo | bibliotecas locais em `Library.pretty/` e `lib.sym/` |
| `eps-potencia/` | EPS: BMS 2S, conversores de 5 V, 3,3 V e 12 V, entrada externa | revisão de 08/10 (`PCB_NOVAS_REVISADAS/PCB_EPS_EXTRA_NOVA`) |
| `bateria-cima/` | bateria de cima: 18650, aquecedores, TIP120, DS18B20 | |
| `bateria-baixo/` | bateria de baixo: 18650, aquecedores, TIP120 | sem sensor de temperatura |
| `aocs/` | AOCS: ESP32, BMI088, AS5048A, DRV8313 | o esquemático estava em `AOCS/grb/` no zip |
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

As fontes de `PCB_OBC_NEW.zip` (KiCad de 23/10/2026), a pinagem em uso
desde 31/10. Ficaram de fora o `.history/` e o `.kicad_prl`. Conferida pelo
netlist do esquemático e pelos pads do `.kicad_pcb`, que concordam.

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
```

A placa fabricada segue o `.kicad_pcb`, não o esquemático: o I²C do OBC é o
exemplo. As divergências encontradas entre as placas, e o que medir no stack,
estão em `docs/relatorios/2026-10-22-validacao-obc.html`.
