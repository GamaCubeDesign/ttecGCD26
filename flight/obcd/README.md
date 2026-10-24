# obcd — o software do OBC

Roda os modos da missão (Figura 44 do CDR), dispara o burn-wire, comanda os
aquecedores da bateria e entrega ao `ttcd` a tensão, a corrente e a
temperatura da bateria. Mesma arquitetura do `ttcd` e do `adsbd` (ADR-0005,
ADR-0016): núcleo puro (`core.c`, sem chamadas de sistema, testado em tempo
virtual) e casca epoll (`main.c`).

Reescrito, não portado, do `../obc` (congelado). Fica: os enums de modos e
eventos (já em `common/gama_tc.h`), as transições da Figura 44 e a
configuração do INA219. Sai: o `nanosleep` no laço, o wiringPi, os caminhos
fixos no binário, o `sdr.c` (o SDR é do `adsbd`), o rádio (é do `ttcd`) e o
0 V que o INA219 devolvia quando a leitura falhava.

## Modos

| Modo | O que o `obcd` faz | Como sai |
|---|---|---|
| `PRE_TEST` | nada além do termostato e da telemetria; é o centro (D1) | `SET_MODE` para qualquer outro |
| `BASIC_INTERMEDIATE` | burn-wire ligado por `burnwire_ms` (5 s), uma vez por entrada (D3) | sozinho, no fim do pulso (`EV_TASK_DONE` do próprio `obcd`) |
| `ADVANCED_AOCS` | por enquanto só o prazo: o comando ao ESP32 é o PLANO 2.4 | `EV_TASK_DONE` do ESP32, ou o prazo `aocs_timeout_ms` |
| `MISSION_ADSB` | conta 600 s (OBC-6); o decoder e os retratos seguem o modo pelo `ttcd` (ADR-0014) | `EV_ADSB_TIMEOUT` aos 600 s, ou `SET_MODE downlink` |
| `MISSION_DOWNLINK` | nada: o `ttcd` manda o roster e as estatísticas | `EV_TASK_DONE` do `ttcd` |
| `ENV_SURVIVAL` | nada além do termostato e da telemetria | `SET_MODE task_done` da ground |

Um evento que não vale no modo atual é recusado e registrado
(`event_refused`). O ACK do `SET_MODE` na ground só diz que o `ttcd` o
entregou; se o modo não mudou, o HK mostra o mesmo modo, e o motivo fica no log
do `obcd`. Sair de `BASIC_INTERMEDIATE` por qualquer caminho desliga o
burn-wire. Boot e reinício sempre em `PRE_TEST`, com as duas saídas desligadas
(D3, D5).

## Termostato (D4)

Liga abaixo de `heater_on_below_ccel` (10,00 °C), desliga acima de
`heater_off_above_ccel` (12,00 °C), em todos os modos. Sem leitura válida há
mais de `temp_stale_ms`, ou com o sensor falhando, os aquecedores desligam, e
o log diz uma vez (`temp_fault`, depois `temp_ok` quando volta).

## Sensores e saídas

Nenhum pino nem barramento está no binário: tudo vem do `obcd.conf`. Sem a
chave, o sensor ou a saída simplesmente não existe, e o log diz isso no
início (`sensor`, `output`). A telemetria leva "sem dado" (`0xFFFF`,
`0x8000`) no que não foi medido, nunca zero.

| O quê | Como | Chave |
|---|---|---|
| INA219 (tensão, corrente da carga) | `i2c-dev`, numa thread própria | `i2c_dev`, `ina219_addr`, `ina219_shunt_mohm` |
| DS18B20 (bateria de cima) | `w1_therm` do kernel, numa thread própria (D9) | `w1_path` |
| Burn-wire | GPIO chardev | `burnwire_line` |
| Aquecedores | GPIO chardev | `heater_line` |

As duas threads são o único lugar onde se espera por um dispositivo: a
conversão do DS18B20 leva ~750 ms, e o I²C do INA219 é dividido com o ESP32 do
AOCS nas placas como fabricadas, então pode travar. O laço principal só recebe
as leituras prontas por um pipe (regra 3 do `AGENTS.md`).

Na placa como fabricada: burn-wire no GPIO22, aquecedores no GPIO23, DS18B20
no GPIO17 (`dtoverlay=w1-gpio,gpiopin=17`), INA219 em `/dev/i2c-1`, endereço
0x40. Os achados A5, A6 e A7 (`docs/relatorios/2026-10-22-validacao-obc.html`)
dizem por que esses circuitos não funcionam como estão; os pinos da placa nova
entram no `obcd.conf` quando ela existir.

## Compilar e testar

```bash
make                                          # obcd junto com o resto
make test T=obcd                              # núcleo, drivers, configuração, integração
build/flight/obcd -c flight/obcd/obcd.conf.example --check-config
```

- `test_obcd_core`: cada aresta da Figura 44, o que cada modo recusa, o
  burn-wire, os prazos, o termostato com a histerese e com o sensor em falha,
  a telemetria e o "sem dado". Tempo virtual.
- `test_obcd_drivers`: o INA219 contra um chip emulado; o DS18B20 contra o
  texto do `w1_slave` e contra um arquivo falso.
- `test_obcd_integration`: o binário real, com o teste fazendo o papel do
  `ttcd` e do DS18B20; o I²C e o GPIO não existem, então esses caminhos rodam
  como falha.
- `test_adsb_chain`: o `obcd`, o `ttcd` e o `adsbd` reais no rádio UDP; um
  `SET_MODE` da ground percorre `PRE_TEST` → `MISSION_ADSB` →
  `MISSION_DOWNLINK` → `PRE_TEST`.

## Rodar à mão, sem placa

```bash
build/flight/ttcd -c flight/ttcd/ttcd.conf.example -o radio=udp -o ipc_path=/tmp/ttec.sock -o log_path=- &
build/flight/obcd -c flight/obcd/obcd.conf.example -o ipc_path=/tmp/ttec.sock -o log_path=-
```

e, noutro terminal, a ground: `make bench-gs RADIO=udp` e `mode mission_adsb`,
`hk`, `mode downlink`... Para ver o termostato sem DS18B20, aponte `w1_path`
para um arquivo com o formato do `w1_slave` (`flight/obcd/ds18b20.h`) e mude o
`t=` à mão.

## Na Pi

```bash
make provision        # uma vez: liga o I2C1 e põe o usuário gama no grupo i2c
make install          # ttcd, adsbd e obcd; /etc/gama/obcd.conf se ainda não existir
make obcd-enable      # sobe agora e a cada boot
make obcd-status
```

O serviço é `Type=notify` com `WatchdogSec=10`: o `obcd` avisa o systemd a
cada segundo que o laço gira, e um laço parado por 10 s é morto e reiniciado
(D8). O watchdog de hardware da Pi é configurado no provisionamento da imagem
de voo (PLANO etapa 4).

## Log

Um objeto JSON por linha, com o relógio monotônico absoluto em `mono_ms`, o
mesmo do `ttcd` e do `adsbd` (D7, ADR-0009).

| Evento | Quando |
|---|---|
| `start`, `boot` | início: versão, dispositivos, parâmetros |
| `mode` | toda transição: de, para, evento, quem mandou, quanto durou o modo (OBC-5) |
| `event_refused` | evento que não vale no modo atual |
| `burnwire`, `heater` | cada mudança de saída, com o motivo e se a linha foi de fato acionada |
| `sensor`, `output` | sensor começando ou deixando de responder; saída configurada ou ausente |
| `temp_fault`, `temp_ok`, `power_fault` | sensor de temperatura sem leitura válida; INA219 falhando |
| `time_anchor` | a hora da ground, repassada pelo `ttcd` |
| `status` | a cada minuto: modo, leituras, contadores |
| `ipc_up`, `ipc_down`, `stop`, `exit` | conexão com o `ttcd` e fim |

## O que falta

- **PLANO 2.4, o AOCS:** o contrato I²C com o ESP32 (o firmware está sendo
  escrito e entra no repositório quando terminar). Hoje o `ADVANCED_AOCS` só
  tem o prazo, e roll, pitch e yaw vão como "sem dado".
- **Na bancada:** INA219 e DS18B20 avulsos numa Pi, LEDs no lugar do burn-wire
  e do aquecedor; conferir o que a linha de GPIO faz quando o processo morre
  sem passar pelo `SIGTERM` (o `obcd` desliga as saídas ao sair, e de novo ao
  reiniciar, em ~1 s).
- **O shunt da placa:** `ina219_shunt_mohm = 100` é a calibração do OBC
  anterior; medir o resistor da placa nova.
