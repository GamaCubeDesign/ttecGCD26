# Placas do Cubetschek — fontes KiCad

As placas como foram **fabricadas**: as fontes dos dois painéis enviados à
Circuibras em 08/10/2026, trazidas de `PCBS_GCD_Com_Paineis.zip` em
22/10/2026. Uma pasta por placa; cada uma abre no KiCad pelo `.kicad_pro`
(os painéis exigem o KiCad 10).

| Pasta | Placa | Observação |
|---|---|---|
| `obc/` | OBC: Raspberry Pi Zero 2 W, INA219, barramento | revisão de 08/10 (`PCB_NOVAS_REVISADAS/PCB_OBC_NOVA`); esquemático e layout **divergem** no I²C (pinos 10 e 12) |
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
```

A placa fabricada segue o `.kicad_pcb`, não o esquemático: o I²C do OBC é o
exemplo. As divergências encontradas entre as placas, e o que medir no stack,
estão em `docs/relatorios/2026-10-22-validacao-obc.html`.
