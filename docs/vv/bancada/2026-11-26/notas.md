# 26/11 — o SDA do I²C preso em 0, e a linha de base do ADS-B

Madrugada (01:30–02:20). Satélite montado, **a Pi alimentada pelas baterias
pela EPS**, o enlace pelo SSH (o LoRa segue fora desde 25/11: `./gama ground
--ssh`). Sem SDR na missão: as aeronaves vêm do `sbs_replay`. Versão na Pi:
`antes-gama-fase2-44-gc4fc09c-dirty` (o código de voo do ADS-B igual ao do
PC), depois a correção do INA219 (`e9d4d2c`).

Log da ground: [`ground-mission-k0gxu8o_.jsonl`](ground-mission-k0gxu8o_.jsonl)
(cópia de `ground/host/sessions/mission-k0gxu8o_/`, tirada às 02:16).

## 1. A bateria "caiu a 0 V" no meio da missão — era o I²C

### O que a página mostrou

| Hora | `uptime_s` do `ttcd` | Bateria | Corrente | Modo |
|---|---|---|---|---|
| 01:32:53 | 10 | 7,676 V | 288 mA | `PRE_TEST` |
| 01:33:43 | 60 | 7,616 V | 400 mA | `MISSION_ADSB` (o `adsbd` decodificando) |
| 01:36:03 | 200 | 7,608 V | 405 mA | `MISSION_ADSB` |
| **01:36:13** | 210 | **0 V** | **0 mA** | `MISSION_ADSB` |
| 01:40:23 | 460 | 0 V | 0 mA | `PRE_TEST` |

A Pi seguiu ligada o tempo todo, nas baterias: `vcgencmd get_throttled` =
`0x0` (nenhuma subtensão desde o boot), `uptime_s` contínuo.

### O que foi medido na Pi (só leitura, `I2C_RDWR` de um script; nada escrito)

| Teste | Resultado |
|---|---|
| INA219 (0x40): config, shunt, bus | `0x0000`, `0x0000`, `0x0000`, três vezes. O config de um INA219 nunca é 0: escrito ou de power-on, é `0x399F` |
| Endereços sem dispositivo: 0x41, 0x45, 0x2A, 0x50, 0x77 | **todos "ACK"**, todos `0x00 0x00` |
| `pinctrl get 2,3` | **GPIO2 (SDA1) = `lo`**, GPIO3 (SCL1) = `hi` |

**O SDA do I²C1 está preso em 0.** O mestre lê ACK e `0x00` de qualquer
endereço. O `obcd` tomava esses zeros por leitura válida e mandava 0 V e
0 mA, em vez de "sem dado". Não há sinal de dano: o shunt aberto daria fundo
de escala (±320 mV), não 0.

Em 22/11 era o SCL em 0 V ([notas](../2026-11-22/notas.md)); hoje é o SDA.

### A correção (`e9d4d2c`, instalada na Pi às 01:59)

`ina219_read` lê também o registrador de configuração e recusa a leitura se
ele não for `0x399F` (`flight/obcd/ina219.c`, teste novo em
`tests/test_obcd_drivers.c`). Depois da instalação, o HK chega com
`battery_mv = 65535`, `current_ma = −32768`: "—" na página. Quando o SDA
soltar, a leitura volta sozinha (o `obcd` reconfigura o chip a cada falha).

### Desligar e ligar

Com a Pi desligada (`sudo poweroff`) e religada: **o SDA continuou `lo`**
(02:09, 4 min de boot). Falta saber se a pilha inteira foi desligada ou só a
Pi.

**Pendente** (depois do ADS-B):

- [ ] A placa do AOCS estava na pilha? O I²C1 é dividido com o ESP32 dela
      (ADR-0017), que roda o firmware entregue em 25/11. Tirá-la, religar e
      ver `pinctrl get 2,3`.
- [ ] Sem o AOCS e ainda `lo`: com tudo desligado, a resistência do SDA
      (J1.10) ao GND.
- [ ] Enquanto isso, a tensão da bateria só pelo multímetro.

**À parte:** o `dmesg` da Pi está cheio de `w1_master_driver ... Family 0 for
00.… is not registered`: o 1-Wire vê dispositivos fantasmas, e a temperatura
da bateria segue "sem dado". É a pendência antiga da fiação do DS18B20.

## 2. ADS-B, etapa 0 do [roteiro da recepção](../roteiro-adsb-recepcao.md)

| Item | Estado |
|---|---|
| 0.1 `downlink_emulation.py --downlinked` (o registro de bordo contra o que a ground recebeu) | feito (`c8a27f9`); o primeiro teste, sobre a cadeia de 16/11, ainda rodando |
| 0.2 linha de base nas capturas de 17 e 18/11 | feita, abaixo |
| 0.3 a missão pelo SSH | ok: a missão desta madrugada |
| 0.4 o AGC (`--gain -10`) no `sdr_check` | só por leitura: o script passa `--gain -10.0` ao `dump1090-fa`. Não rodou: o SDR da Pi fica preso pelo `adsbd`; para a etapa 1, `./gama pi stop` antes (o roteiro já diz) |
| 0.5 código e testes na Pi | ok: `ctest -R 'avr\|tracks\|adsbd'` na Pi, 7 de 7 |

### Linha de base: a captura contra o downlink emulado, sem categoria (como o voo de hoje, `avr_port = 0`)

`downlink_emulation.py <captura> --compare ground/host --reference-db …`; as
tabelas em [`analise/`](analise/).

| | 17/11 | 18/11 |
|---|---|---|
| Aeronaves | 47 | 44 |
| Origens decididas, captura → downlink | **7 → 7**, os mesmos aeroportos | **10 → 5** |
| Destinos decididos, captura → downlink | **15 → 14**, os mesmos aeroportos | **17 → 17**, 2 com outro aeroporto |
| Atraso da origem pelo downlink | 0 a 52 s | 15 a 60 s |

O que se perde: em 18/11, 4 das 5 origens são **SADF** (San Fernando, aviação
geral) e a quinta, AR-0126; em 17/11, um destino TENTATIVE (SAEZ); em 18/11,
dois destinos TENTATIVE trocam de pista pequena. SABE, SAEZ e SUMU não mudam.

**Ressalva:** as "capturas" da adsb.lol não são completas (6 391 mensagens em
1 h, contra ~4 por segundo por aeronave de um receptor). A comparação mede as
regras do downlink mais do que a taxa. A que mede a taxa é a da etapa 4, com o
registro de bordo.

### Por que as origens de SADF se perdem: a categoria

Descartado: **o limite de 24 aeronaves por snapshot** (o número de registros
é o mesmo com e sem o limite: 5 487 e 7 938); **o `on_ground = 0`** que o
registro de 20 B não leva (o estimador não usa o 0).

O estimador, só na E0B298 (decola de SADF a ~60 kt), pelos dois caminhos:

| s | Captura (categoria A1, leve) | Downlink (sem categoria) |
|---|---|---|
| 0–23 | `CLIMB`, evidência insuficiente | igual |
| **66** | **`ESTIMATED` SADF** | **`UNKNOWN`, `AMBIGUO`** |
| 192 | janela de origem encerrada | janela encerrada, sem decisão |

As posições, altitudes e velocidades chegam praticamente iguais. A diferença é
a categoria: a captura a tem; o voo com `avr_port = 0` não. A inferência pela
cinemática (ADR-0022) só prova "grande porte", e um monomotor a 60 kt fica sem
classe; os aeródromos pequenos e helipontos em volta competem com SADF.

É o caso que só a categoria recebida a bordo (ADR-0023) resolve.

### Com a categoria a bordo (`--avr`, novo)

`downlink_emulation.py --avr` leva a classe nos 2 bits do track, com o
mapeamento de `flight/adsbd/avr.c` (A1, A2, B1 → leve; A3–A6 → grande; A7 →
rotor) e o de `tracks_to_ndjson.py` na ground.

`downlink_emulation.py <captura> --avr --compare …`; as tabelas em
[`analise/`](analise/) (`*-com-categoria.txt`).

| | 17/11 sem → com | 18/11 sem → com |
|---|---|---|
| Origens no downlink (a captura: 7 e 10) | 7 → **7** | 5 → **9** |
| Destinos no downlink iguais aos da captura (de 15 e 17) | 14 → 14 | 15 → **16** |

- **18/11: as 4 origens de SADF voltam**, no mesmo aeroporto e no mesmo
  instante da captura (E0B04B @2190 s, E0B298 @445 s, E1724F @625 s,
  E200B9 @2800 s). Continua perdida só a AR-0126 (E0B393), uma pista pequena.
- 18/11: o destino da E0B393 passa a ser o da captura (AR-0051 em vez de
  SADZ); o da AB599B segue trocado (AR-0399 em vez de AR-0252), os dois
  TENTATIVE.
- 17/11, sem aviação geral decolando na janela, não muda nada que conte: as
  mesmas 7 origens e 14 destinos; um destino (E4A5C9, SUMU) sai TENTATIVE em
  vez de ESTIMATED.

**Leitura:** com a categoria a bordo, o downlink de 5 s decide 16 das 17
origens que a captura decide nos dois dias (sem ela, 12 de 17). A perda que a
linha de base atribuía ao enlace era, quase toda, a falta da categoria.
Concordar com a captura não é acertar: o acerto contra a verdade das OpenSky é
o `tools/live_traffic/avaliar.sh`, e o da missão real, a etapa 4 do roteiro.

Pesa a favor de ligar o `avr_port` no voo (ADR-0023), se a etapa 3 do roteiro
passar na bancada.
