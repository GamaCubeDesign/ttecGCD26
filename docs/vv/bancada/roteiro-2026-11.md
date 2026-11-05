# Roteiro da bancada — novembro de 2026

> **Para o dia da bancada, use o roteiro completo em HTML:**
> [`docs/relatorios/2026-11-03-roteiro-bancada.html`](../../relatorios/2026-11-03-roteiro-bancada.html).
> Ele tem a montagem, a pinagem, a rede pelo `nmcli` e os comandos, e põe a
> bancada nos GPIOs e na configuração de voo da placa nova do OBC
> (`flight/conf/obc-v2/`), para o firmware sair validado para as placas.
> Este arquivo fica como o resumo das etapas.

Desde 09/10 entrou muita coisa que **nunca rodou em hardware**:

- o protocolo v2 (ADR-0013);
- o `ttcd` e o `adsbd` dirigidos pelo modo (ADR-0014);
- 433,92 MHz e 14 dBm (ADR-0015);
- o `obcd` (ADR-0016) e o empréstimo do I²C ao AOCS (ADR-0017);
- a ground na ESP32 (ADR-0018).

Este roteiro continua as etapas do [README](README.md) (1 a 7, feitas em
07–09/10) e vai da mais básica à mais dependente: cada etapa supõe as
anteriores. Para cada uma: o que prova, a montagem, os comandos, o que
esperar e o que anotar.

O PC comanda a Pi como em outubro (`local.mk`, rede `gama-bancada`). Os logs
da Pi chegam com `make pi-fetch` (os `make pi-bench…` já fazem isso no fim);
os que o PC grava ficam direto em `docs/vv/bancada/AAAA-MM-DD/`.

## Antes de sair do PC

```bash
make check                 # 37 suítes, nos dois builds, e as citações
make check-debian          # o mesmo num contêiner Debian 13, com o GCC 14 da Pi
make esp32-build           # o firmware da ground
make esp32-tests-qemu      # as 11 suítes portáveis numa ESP32 emulada
```

Material: a Pi, os dois RA-02 na placa perfurada, a ESP32 DevKit V1 e jumpers
para passar o rádio B a ela, o NESDR, um adaptador OTG e um cabo USB longo
(~3 m). Para a etapa 13: um INA219 avulso, um DS18B20 com 4,7 kΩ, dois LEDs
com 330 Ω, uma fonte de bancada (ou a bateria 2S), uma carga conhecida
(ex.: 100 Ω / 1 W) e o multímetro.

## Etapa 8 · O código novo na Pi

Prova que tudo compila e passa no compilador da Pi.

```bash
make pi-check
```

Esperado: 37/37 nos dois builds (na Pi o segundo é só UBSan) e 71/71
citações. Anote a versão (`git describe`) e o tempo de build.

## Etapa 9 · A bancada dos rádios em 433,92 MHz

Os seis passos de 09/10 na frequência nova (ADR-0015), com a mesma montagem:
rádios a 80 cm, 2 dBm, a `gs_cli` no rádio B.

```bash
make pi-bench
```

Comparar com 09/10:

| Passo | 09/10 | Critério |
|---|---|---|
| 1 · ping | 10/10 | 10/10 |
| 2 · tempo no ar | pior erro 1,9% | ≤ 5% do modelo |
| 3 · PER | 0 perdas em ~2,2 mil frames por sentido | 0 |
| 4 · troca de taxa | 7/7 | 7/7 |
| 5 · ACK perdido | recuperou (23 s) | recupera sozinho |
| 6 · LBT | header = preamble | sem diferença |

Confirmar a frequência no chip: depois de um passo, **sem pulsar o RESET**,
o RegFrf (0x06–0x08) do rádio A lê `0x6C7AE1` (433,92 MHz). Em 09/10, recém
resetado, lia `0x6C8000` (434,000 MHz).

```bash
ssh pi@gamapi.local "python3 -c \"import spidev; s = spidev.SpiDev(); s.open(0, 0); s.max_speed_hz = 1000000; print(bytes(s.xfer2([0x06, 0, 0, 0])[1:]).hex())\""
```

Opcional, a prova pelo ar: com o SDR no USB, `sudo apt install rtl-sdr` e,
durante o passo 3, `rtl_power -f 433.7M:434.1M:2k -i 1 -e 30 frf.csv`. O pico
deve estar em 433,92 MHz.

## Etapa 10 · A ESP32: as suítes na placa e o firmware da ground

Prova o TTC-16: as mesmas suítes portáveis passam na ESP32 de verdade, com o
mesmo número de verificações do PC. Tudo no PC, com a ESP32 no USB dele (o
build roda no Docker).

```bash
make esp32-tests-flash ESP32_PORT=/dev/ttyUSB0   # grava as suítes e lê o resultado
make esp32-flash ESP32_PORT=/dev/ttyUSB0         # grava a ground de volta
```

Esperado: 11 suítes, `failed 0`, as mesmas contagens do `make test` do PC.

Depois, **com tudo desligado**, passe o rádio B da Pi para a ESP32:

| RA-02 | ESP32 |
|---|---|
| SCK, MISO, MOSI | GPIO18, GPIO19, GPIO23 |
| NSS | GPIO5 |
| RESET | GPIO14 |
| DIO0 | GPIO26 |
| 3.3V, GND | 3V3, GND (**só 3,3 V**) |

Conferir que a ESP32 enxerga o rádio: `python3 ground/host/gs_serial.py
--port /dev/ttyUSB0 --power 2`; a linha `boot` diz se o rádio respondeu.
Ctrl-D sai.

## Etapa 11 · A bancada dos rádios com a ESP32 de ground

Os mesmos seis passos, com a ESP32 no lugar da `gs_cli`. Mede a ground das
provas, que precisa responder ao satélite em ~20 ms (ADR-0007, ADR-0018).

Montagem: a ESP32 no USB da Pi, pelo OTG (o SDR sai). Uma vez, na Pi:
`sudo usermod -aG dialout pi`, e entre de novo.

```bash
make pi-bench GS=esp32 ESP32_PORT=/dev/ttyUSB0
```

Os critérios da etapa 9. Uma diferença de PER ou de troca de taxa entre as
duas etapas vem do lado da ESP32.

## Etapa 12 · Os três daemons pelo modo, com o SDR

Prova a Figura 44 em hardware e o `adsbd` seguindo o modo (ADR-0014,
ADR-0016).

Montagem: na Pi, o rádio A e o SDR no USB; a ESP32 de ground no USB do
**PC**, a 80 cm. Na Pi, num tmux, uma janela por linha:

```bash
make bench-ttcd
build/flight/obcd -c flight/obcd/obcd.conf.example -o ipc_path=/tmp/ttec-bench.sock -o log_path=- \
    | tee -a docs/vv/bancada/$(date +%F)/obcd-e12.jsonl
build/flight/adsbd -c flight/adsbd/adsbd.conf.example -o ipc_path=/tmp/ttec-bench.sock -o log_path=- \
    -o ndjson_path=docs/vv/bancada/$(date +%F)/adsb-e12.ndjson | tee -a docs/vv/bancada/$(date +%F)/adsbd-e12.jsonl
```

No PC, a ground: `make bench-gs GS=esp32 RADIO=sx1278 ESP32_PORT=/dev/ttyUSB0 GS_OPTS='--auto-time on'`.

| Comando na ground | Esperado |
|---|---|
| `hk` | `PRE_TEST`; V, I e temperatura "sem dado" (nada configurado) |
| `mode mission_adsb` | ACK; o HK mostra `MISSION_ADSB`; o `adsbd` sobe o dump1090 (`dump1090_start`) |
| `stream 5` | retratos; `TM_TRACKS` se algum avião mandar posição (janela, área aberta) |
| espere os 600 s | `MISSION_DOWNLINK` sozinho (OBC-6); roster e estatísticas descem; `PRE_TEST` (`EV_TASK_DONE` do `ttcd`); o dump1090 para |
| `mode basic_inter` | 5 s em `BASIC_INTERMEDIATE` e volta sozinho (`driven: false`: sem linha configurada) |
| `mode aocs` | volta a `PRE_TEST` na hora (`aocs_unavailable`: sem I²C configurado) |
| `mode survival`, depois `mode task_done` | `ENV_SURVIVAL` e de volta a `PRE_TEST` |

Anote a latência de cada ACK de `SET_MODE` e os instantes de cada transição
no log do `obcd`, e quanto o dump1090 leva para subir e parar depois de cada
mudança de modo.

## Etapa 13 · O `obcd` com sensores avulsos e LEDs

Prova o OBC-7 (V, I e temperatura), o termostato (D4), o pulso do burn-wire
(D3), o que acontece com as saídas quando o processo morre (pendência do
ADR-0016) e o NACK do I²C sem o ESP32 (ADR-0017). Na Pi da bancada, não na
placa do OBC:

| Peça | Pinos da Pi |
|---|---|
| INA219: VCC, GND, SDA, SCL | 3V3 (pino 1), GND, GPIO2 (pino 3), GPIO3 (pino 5) |
| INA219: VIN+ → carga → VIN− | da fonte (~8 V) ao VIN+, do VIN− à carga, a carga ao GND da fonte; GND da fonte no GND da Pi |
| DS18B20 | VDD no 3V3 (pino 17), GND, DQ no GPIO17 (pino 11), 4,7 kΩ do DQ ao 3V3 |
| LED do burn-wire | GPIO22 (pino 15) → 330 Ω → LED → GND |
| LED dos aquecedores | GPIO23 (pino 16) → 330 Ω → LED → GND |

Uma vez, na Pi:

```bash
echo 'dtoverlay=w1-gpio,gpiopin=17' | sudo tee -a /boot/firmware/config.txt && sudo reboot
sudo usermod -aG i2c pi                  # e entre de novo
sudo apt install i2c-tools
ls /sys/bus/w1/devices/                  # um 28-… é o DS18B20
i2cdetect -y 1                           # 0x40 é o INA219
i2ctransfer -y 1 w1@0x2a 0x00            # sem ESP32: "Remote I/O error" ou "No such device or address"
```

O último comando mostra o erro que o driver da Pi devolve para um endereço
sem ninguém. O `obcd` lê esses dois como NACK. Se aparecer outro, anote: é
o que a etapa 13e vai mostrar.

Na Pi, `make bench-ttcd` numa janela e, na outra:

```bash
build/flight/obcd -c flight/obcd/obcd.conf.example -o ipc_path=/tmp/ttec-bench.sock -o log_path=- \
    -o i2c_dev=/dev/i2c-1 -o w1_path=auto -o burnwire_line=22 -o heater_line=23 \
    -o i2c_lease_path=/tmp/obcd-i2c.lease \
    | tee -a docs/vv/bancada/$(date +%F)/obcd-e13.jsonl
```

A ground é a ESP32 no PC, como na etapa 12.

- **13a · V e I contra o multímetro.** Três tensões da fonte (ex.: 6,0, 7,4
  e 8,4 V). Compare o `battery_mv` e o `current_ma` do `hk` (ou do `status`
  do log, a cada minuto) com o multímetro. O shunt de 100 mΩ é a calibração
  do OBC anterior: uma diferença fixa na corrente aponta o shunt.
- **13b · O termostato.** Reinicie o `obcd` com os limites perto da
  temperatura da sala: `-o heater_on_below_ccel=2600 -o heater_off_above_ccel=2800`.
  Com a sala a ~25 °C, o LED dos aquecedores acende. Segure o DS18B20 entre
  os dedos: acima de 28 °C ele apaga; ao soltar, abaixo de 26 °C, acende.
  Anote a temperatura de cada evento `heater` do log. Depois solte o DQ: em
  10 s vem `temp_fault`, e o LED apaga.
- **13c · O pulso do burn-wire.** `mode basic_inter`: o LED do GPIO22 fica
  aceso 5 s e o modo volta sozinho. A duração sai do log (`burnwire` on e
  off) e, se quiser, de um vídeo do celular.
- **13d · As saídas depois de um `kill -9`.** Com o LED dos aquecedores
  aceso: `kill -9 $(pidof obcd)`, e anote se o LED fica aceso ou apaga. É a
  pendência do ADR-0016. Suba o `obcd` de novo: ele apaga as duas saídas ao
  começar (D3).
- **13e · `ADVANCED_AOCS` sem ESP32.** `mode aocs`: volta a `PRE_TEST` na
  hora, e o log diz `"lent":false,"why":"aocs_absent"`. Se em vez disso vier
  `"lent":true` e o modo durar ~5 min com V e I "sem dado", o driver deu ao
  NACK um erro que o `obcd` não reconhece: anote o erro do `i2ctransfer`
  acima e avise.
- **13f · O burn-wire de verdade, numa protoboard.** No lugar do LED do
  GPIO22, uma réplica do circuito da placa de antenas: GPIO22 → R_OBC →
  220 Ω → base do TIP120, 10 kΩ da base ao emissor, e o NiCr entre o 5 V da
  fonte de bancada (limite de 1,5 A) e o coletor. Duas rodadas de
  `mode basic_inter`: R_OBC de 4,7 kΩ (a placa do OBC como está) e de
  470 Ω (a correção proposta). Anote a corrente no NiCr e se o nylon corta.
  Decide a pendência P2 das placas.

## Etapa 14 · A corrente do RA-02 a 14 dBm

A Tabela 13 do CDR (orçamento de potência) conta 110 mA a 3,3 V para o RA-02.
Esta etapa mede o valor a 14 dBm e, de quebra, dá um ponto de PER a 14 dBm.

Montagem: o multímetro (mA) em série no 3,3 V do rádio A; a ESP32 no USB da
Pi pelo cabo longo, **a 3 m ou mais**. A 14 dBm e 80 cm o receptor fica perto
da saturação (o comentário do `POWER` no `Makefile`).

```bash
make pi-bench-per GS=esp32 ESP32_PORT=/dev/ttyUSB0 POWER=14
```

Anote a corrente em recepção, parado entre frames, e o pico em transmissão
(função MAX do multímetro, ou um shunt de 1 Ω no osciloscópio: 1 mV = 1 mA).
Referências do datasheet do SX1276/77/78, no PA_BOOST: 87 mA a +17 dBm e
120 mA a +20 dBm; ~11 mA em recepção. Se o rádio reiniciar com o
multímetro em série, use o shunt.

## Etapa 15 · Opcional: os daemons como serviços

Os arquivos `.service` nunca rodaram na Pi: usuário `gama`, `DevicePolicy`,
`ProtectSystem`, o watchdog. Com os rádios a 3 m (o serviço transmite a
14 dBm):

```bash
make pi-install
make pi-service-enable pi-obcd-enable pi-adsbd-enable
ssh pi@gamapi.local "systemctl status ttcd obcd adsbd --no-pager"
ssh pi@gamapi.local 'sudo kill -STOP $(pidof obcd)'   # o watchdog: em 10 s o systemd o mata e reinicia
```

Uma `ping 3` da ground confirma o enlace. No fim, `make pi-service-stop
pi-obcd-stop pi-adsbd-stop`, e `sudo systemctl disable ttcd obcd adsbd` na
Pi, para a bancada voltar a ter o rádio A.

## O que anotar em `notas.md`

Uma pasta `AAAA-MM-DD/` por dia, como em outubro. Para cada etapa:

- horário de início e de fim;
- a versão, que aparece no `start` de cada log;
- a montagem: distância, potência, quem é a ground, o que está no USB de
  quem;
- o resultado contra o esperado, com os números;
- o que deu errado e o que foi feito.

No fim, atualize a tabela de progresso do [README](README.md).
