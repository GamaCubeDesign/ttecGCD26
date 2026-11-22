# Bancada — 22/11/2026: a cadeia de telecomando e o satélite montado

Roteiro: [`roteiro-cadeia-tc-2026-11.md`](../roteiro-cadeia-tc-2026-11.md). Depois,
os três serviços de voo no satélite montado, e o diagnóstico do INA219
(relatório [`2026-11-22-i2c-scl-em-curto.html`](../../../relatorios/2026-11-22-i2c-scl-em-curto.html)).

## Sessão

- Horário: das 16:17 às ~17:55
- Versão na Pi: `antes-gama-fase2-8-g8f54a30-dirty` (a `feat/sdr-check` de 22/11)
- Satélite: todas as placas montadas no stack, com as antenas da equipe. OBC
  `obc-v2`: os arquivos do `PCBS_GAMA_AOCS_EPS1_OBC_TF.zip` têm o mesmo SHA-1 dos de
  `hardware/obc-v2/`. **Sem o AOCS e sem o DS18B20.** Rádio A a 2 dBm nos testes do `./gama`.
- Ground: ESP32 no USB do PC, rádio B, `--power 2`.
- Configuração: `./gama board install` às 16:17, quando o `ttcd.conf` antigo foi para
  `/etc/gama/ttcd.conf.bak-20261122-161740`. Desde então, **RESET no GPIO25 e DIO0 no
  GPIO4** (pinos da placa), e o 1-Wire no GPIO17 (`config.txt`). Todos os testes de
  rádio abaixo vêm depois disso.
- **Relógio da Pi dois dias atrasado** (sem cabo, sem NTP): ela marcava 20/11 13:21
  às 16:26 de 22/11. Por isso as duas missões e a medição de antena estão em
  [`../2026-11-20/`](../2026-11-20/): `missao-132136/`, `missao-132714/`,
  `antena/pi-sala-124358/`. Com o cabo, às 17:39, o relógio estava certo.

## Enlace, nos pinos da placa

| Passo | Resultado | Log |
|---|---|---|
| `./gama link ping` | 10/10 na 1ª tentativa, 281 ms cada; RSSI −98 a −99 dBm, SNR 13 dB | `162231-ping-*.jsonl` |
| `./gama link lost-ack` | o `SET_RATE` para FAST com a ground surda: ACK em 20,9 s, com 11 tentativas; depois, PINGs a 101–232 ms em FAST | `162502-lost-ack-*.jsonl` |

**Fecha o achado 2 de 16/11:** com o DIO0 no pino certo, o ping volta a uma
tentativa só, a 281 ms. Em 16/11, com o `.conf` da placa sobre a fiação de outubro,
9 dos 10 pings precisaram de 2 tentativas. O ACK perdido se recuperou como em 09/10
(23,2 s, 12 tentativas) e como na simulação (23,4 s).

## As missões, com a simulação de 20 aeronaves

`./gama mission --input /tmp/sim20.ndjson`, com a simulação de 16/11
(`../2026-11-16/analise/gerar_sim20.py`). O `ground.jsonl` de cada sessão foi
copiado para a pasta da missão.

| | Missão 1 (`missao-132136`) | Missão 2 (`missao-132714`) |
|---|---|---|
| Duração pedida | `--secs 120` | `--secs 600` |
| Fim | `ADSB_TIMEOUT` aos 120 000 ms, sozinho | `mode downlink` do operador aos 86 s |
| `SET_TIME` → `SET_MODE` | 321 ms → 301 ms, na 1ª tentativa | 321 ms → 301 ms, na 1ª tentativa |
| Transições no `obcd` | `PRE_TEST` → `MISSION_ADSB` → `MISSION_DOWNLINK` → `PRE_TEST`, uma por `SET_MODE` | idem; o downlink pelo TC (`TC_DOWNLINK`), ACK em 637 ms |
| Aeronaves na ground com posição | 20 de 20 (418 registros, 18 a 24 por avião) | 20 de 20 (288 registros) |
| Roster | 20 indicativos | 20 indicativos |
| Quadros de TM | 89, 1 perdido (um erro de CRC) | 53, 0 perdidos |
| Latência p95 a bordo (HLR-ADS-08: 5 s) | 2,4 s | 3,1 s |
| Erro de posição, p50 / p95 | 4 m / 14 m | 2 m / 10 m |
| Erro de instante, p95 | 0,07 s | 0,05 s |
| Intervalo entre pontos do mesmo avião | 5,0 s, máx. 7,0 s | 5,0 s, máx. 5,1 s |

Todos os telecomandos das duas missões tiveram ACK `OK` na 1ª tentativa, então a
retransmissão sem execução dupla (etapa 5 do roteiro) não foi exercitada. Para
reproduzir os erros de trajetória:

```bash
python3 docs/vv/bancada/2026-11-16/analise/gerar_sim20.py /tmp/sim20.ndjson
python3 docs/vv/bancada/2026-11-16/analise/trajetoria.py /tmp/sim20.ndjson \
    docs/vv/bancada/2026-11-20/missao-132136/adsb.ndjson docs/vv/bancada/2026-11-20/missao-132136/ground.jsonl
```

## Os serviços de voo, a missão só pelo telecomando

`./gama service restart`, depois `make gs-web ADSB=1 MISSION_S=600` e o botão
"Iniciar missão ADS-B" (`servicos/ground.jsonl`, 16:45 a 17:01).

- O enlace pelos serviços, a 14 dBm: `SET_TIME` e `SET_MODE` com ACK na 1ª
  tentativa, o OBC em `MISSION_ADSB`, HK a cada 10 s, RSSI −83 a −86 dBm.
- **Nenhum avião:** o `adsbd` reiniciou o `dump1090-fa` 138 vezes em 5 min, e cada um
  saía com status 1 em ~30 ms. **O SDR não estava no USB da Pi** (`lsusb` sem
  RTL). Depois de religado e de um boot às 17:38, o `dump1090-fa` abriu o SDR
  (`RTL2838UHIDIR`, R820T) e processou 21,7 M amostras em 10 s, sem descartar
  nenhuma. Não é defeito de software.
- **Ruído do SDR −23,7 dBFS** dentro da sala, com o satélite montado e ligado, contra
  −33,8 dBFS na medição `pi-sala` de hoje e −37 a −42 dBFS em 16 e 20/11. Nenhuma
  mensagem com CRC certo em 10 s. A refazer ao ar livre, com e sem o stack ligado.
- **Sensores do OBC sem dado:** `battery_mv` 65535, `current_ma` e `temp_bat_ccel`
  −32768. Só o `temp_soc_ccel` tem valor (44,5 °C). O DS18B20 não estava ligado
  (`w1_bus_master1` com 0 escravos, o esperado). O INA219 está no achado 1.

## Achados

1. **O SCL do I²C da OBC está preso em 0 V.** O INA219 responde `Connection timed
   out` a cada leitura (`"which":"ina219","ok":false`).
   - **Não é software:** com o `obcd` parado, o SCL leu 0 em 11 de 11 leituras
     (`pinctrl lev 2,3`: SDA 1, SCL 0), e o `i2cdetect -y 1` trava a partir de 0x1A.
   - **Descartados:** o AOCS, que está fora do stack; a alimentação do INA219, com
     3,3 V no pad 1, vindos do J1.39 e não da Pi; a linha do aquecedor (GPIO23, no
     J1.11), porque com ela solta o SCL continuou em 0; e as outras placas, que não
     ligam nada no J1.12.
   - **Hipótese A:** um curto do SCL ao GND. Os suspeitos, pelo layout: uma ponte no
     INA219 entre o pad 2 (GND) e o pad 3 (SCL), a 0,76 mm; uma ponte no conector da
     Pi entre os pinos 5 e 6, a 1,03 mm; ou a trilha (134 mm, 5 vias) encostando no
     plano de GND.
   - Diagrama e passo a passo com o multímetro: relatório
     [`2026-11-22-i2c-scl-em-curto.html`](../../../relatorios/2026-11-22-i2c-scl-em-curto.html).
     **A confirmar com o multímetro.**
2. **O RSSI caiu ~55 dB em relação a 16/11** (−96 a −100 dBm contra −42 dBm, a
   2 dBm), com o SNR ainda em 12 a 13 dB. Desta vez com as antenas da equipe, no
   satélite montado; distância não anotada. O enlace funcionou sem perda, mas com
   menos margem. Conferir a distância, as conexões das antenas e a sintonia em 433 MHz.
3. **O `make pi-test` falhou em 5 de 48** na Pi.
   - `ground_aeronaves` e `adsb_pipeline` são o NumPy que falta na Pi, já sabido.
   - `relatorios_index`, `ground_data` e `gama_cli` passam no PC. A suspeita é o
     `rsync` do `pi sync`, que não leva `docs/vv/bancada/`, `*.jsonl` nem
     `*.ndjson`. Não confirmado: falta a saída de cada um na Pi.
   - Nenhum dos cinco é do software de voo.
4. **O `make pi-fetch` traz de volta da Pi logs já versionados.** Os de 16/11
   (`cadeia-b/`) vieram mais longos: os daemons seguiram rodando depois da missão, até
   a Pi ser desligada sem parar, com 2 138 bytes nulos no `ttcd.jsonl` e 65 no
   `obcd.jsonl`. Foi mantida a versão versionada, que cobre a missão; a longa está na
   Pi. Os três `.out` vazios de `cadeia-b/` também voltam a cada fetch.
5. O `./gama net up` sem cabo deixa a Pi sem NTP: ver Sessão.

## Pendências

- Medir o SCL com o multímetro (relatório, seção 5) e consertar; então conferir V e I
  contra o multímetro (bloco A3 do roteiro de novembro).
- Ligar o DS18B20 ao GPIO17 (J1.9), com o pull-up de 4,7 kΩ da OBC, e conferir o `28-…`.
- `./gama antenna check --pi` ao ar livre, com o satélite ligado e desligado, para
  separar o ruído da pilha de placas; depois `./gama mission --secs 600 --sdr`.
- A etapa 5 do roteiro (retransmissão sem execução dupla) e a missão de 10 min
  inteira, sem o downlink manual.
- As três falhas do `pi-test` que passam no PC.
