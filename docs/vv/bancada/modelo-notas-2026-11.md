# Bancada — DD/11/2026

Copie para `docs/vv/bancada/AAAA-MM-DD/notas.md` e preencha. Um bloco por
teste; os IDs são os do [roteiro](../../relatorios/2026-11-05-roteiro-bancada.html).
Só o que os logs não contam: montagem, medidas à mão, o que deu errado.

## Sessão

- Início / fim: __ / __
- Versão (`git describe`): __
- Rede: `bancada-ap` · Pi em 10.42.0.__ · sinal __ dBm · internet pelo cabo: sim/não
- Montagem: rádios a __ cm · antenas ou carga · potência __ dBm
- Ground: ESP32 no USB do PC · DIO0 no GPIO27 · firmware com `dio0=27`: sim/não
- Rádio A: RESET/DIO0 em outubro (GPIO20/21) → placa (GPIO25/4) em B2
- Fonte: __ · multímetro: __ · termômetro de referência: __ · shunt do INA219: __ mΩ

## T0 · `make check` na Pi

- Normal: __/39 · UBSan: __/39 · citações: __/71 · warnings: sim/não · build __ min

## Bloco A · OBC sem rádio

- **A1** · INA219 em 0x__ · DS18B20 `28-…` CRC YES, t=__ · `i2ctransfer 0x2a`: "______"
- **A2** · `hk` inicial: ______ (V, I, T com valor ou sentinela?)
- **A3** · V e I contra o multímetro:

  | Fonte | V mult. (VIN−) | `battery_mv` | I mult. | `current_ma` | Erro V / I |
  |---|---|---|---|---|---|
  | 6,0 V | | | | | |
  | 7,4 V | | | | | |
  | 8,4 V | | | | | |

- **A4** · referência __ °C · `w1_slave` __ · `temp_bat_ccel` __
- **A5** · termostato (limites 26/28 °C): liga a __ °C, desliga a __ °C, histerese ok?
- **A6** · `temp_fault` em __ s · recuperou ao religar DQ? sim/não
- **A7** · pulso do burn-wire: __ ms · volta a `PRE_TEST`? · `driven`: __
- **A8** · `mode aocs`: voltou em __ ms · `lent`: __ · `why`: __
- **A9** · sobrevivência: entrou/saiu? termostato seguiu?
- **A10** · Ctrl-C: LED __ · `kill -9`: LED ficou ______ · `pinctrl get 22,23`: ______

## Bloco B · enlace

- **B0** · suítes na ESP32: __/11 · contagens iguais ao PC? · `boot`: rádio ok? `dio0=__`
- **B1** (fiação de outubro, `ttcd.conf.example`):
  1 ping __/10 · 2 ToA pior __% · 3 PER up __ / down __ · 4 trocas __/7 · 5 ACK perdido __ s · 6 LBT header __ / preamble __
- **B2** (pinos da placa, `flight/conf/obc-v2/ttcd.conf`):
  1 __ · 2 __% · 3 up __ / down __ · 4 __/7 · 5 __ s · 6 __ · RegFrf `______`
- **B3** · latência de ACK do ping: mín __ / média __ / máx __ ms · diferenças contra 09/10: __

## Bloco C · integrado

- **C1** · `hk` pelo ar: ______
- **C2** · modos percorridos? latência de cada ACK de `SET_MODE`: __ · 600 s até `MISSION_DOWNLINK` sozinho? sim/não/cortado
- **C3** · dump1090 sobe em __ s, para em __ s · CPU __%
- **C4** · termostato e burn-wire pelo ar iguais ao bloco A? ______
- **C5** · INA219 parado: __ mV, __ mA · transmitindo: __ mV, __ mA
- **C6** · `temp_soc_ccel`: __

## Bloco D · extras

- **D1** · adiado (burn-wire real ainda não é possível); P2 segue aberta
- **D2** · RA-02 a 14 dBm: RX __ mA, pico TX __ mA · PER a __ m: __
- **D3** · serviços: ativos? watchdog reiniciou o `obcd`?

## Problemas e observações

- 
