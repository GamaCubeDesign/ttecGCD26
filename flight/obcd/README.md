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
| `ADVANCED_AOCS` | manda o START ao ESP32 do AOCS e lhe empresta o I²C pela tarefa (ADR-0017); no empréstimo, tensão e corrente vão "sem dado"; a atitude do resultado vai no HK por `aocs_hold_ms` | sozinho, quando o resultado é lido no fim do empréstimo (250 ms + tarefa + 2 s, dentro de `aocs_timeout_ms`); na hora, se nenhum ESP32 responde ou se ele recusa |
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
| INA219 (tensão, corrente da carga) | `i2c-dev`, na thread dona do I²C | `i2c_dev`, `ina219_addr`, `ina219_shunt_mohm` |
| ESP32 do AOCS | a mesma thread, frames fixos (`docs/icd/obc-aocs-i2c.md`) | `aocs_addr`, `aocs_timeout_ms`, `i2c_lease_path` |
| DS18B20 (bateria de cima) | `w1_therm` do kernel, numa thread própria (D9) | `w1_path` |
| Burn-wire | GPIO chardev | `burnwire_line` |
| Aquecedores | GPIO chardev | `heater_line` |

As duas threads são o único lugar onde se espera por um dispositivo: a
conversão do DS18B20 leva ~750 ms, e o I²C é dividido com o ESP32 do AOCS. A
thread do I²C é a única dona do `/dev/i2c-1` e segue o escalonador puro
`i2c_sched.c`: lê o INA219 a cada `power_period_ms`, escreve o START, lê a
resposta antes de t0+100 ms e não toca mais o barramento até o fim do
empréstimo. O laço principal recebe as leituras prontas por um pipe e manda os
STARTs por outro (regra 3 do `AGENTS.md`).

O fim do empréstimo fica em `i2c_lease_path` (`/var/lib/gama/obcd-i2c.lease`)
desde que o START sai. Um `obcd` reiniciado, ou uma Pi que reiniciou (o ESP32
não reinicia junto), fica fora do barramento até o empréstimo acabar.

Na placa como fabricada: burn-wire no GPIO22, aquecedores no GPIO23, DS18B20
no GPIO17 (`dtoverlay=w1-gpio,gpiopin=17`), INA219 em `/dev/i2c-1`, endereço
0x40. Os achados A5, A6 e A7 (`docs/relatorios/2026-10-22-validacao-obc.html`)
dizem por que esses circuitos não funcionam como estão; os pinos da placa nova
entram no `obcd.conf` quando ela existir.

## Compilar e testar

```bash
make                                          # obcd junto com o resto
make test T=obcd                              # núcleo, I²C, drivers, configuração, integração
build/flight/obcd -c flight/obcd/obcd.conf.example --check-config
```

- `test_obcd_core`: cada aresta da Figura 44, o que cada modo recusa, o
  burn-wire, os prazos, o termostato com a histerese e com o sensor em falha,
  a telemetria e o "sem dado". Tempo virtual.
- `test_obcd_i2c_sched`: a linha do tempo do I²C contra um ESP32 e um INA219
  simulados, em tempo virtual; prova que a Pi não faz nenhuma transação de
  t0+100 ms ao fim do empréstimo, em todo desfecho.
- `test_obcd_drivers`: o INA219 contra um chip emulado; o DS18B20 contra o
  texto do `w1_slave` e contra um arquivo falso.
- `test_obcd_integration`: o binário real, com o teste fazendo o papel do
  `ttcd` e do DS18B20; o I²C e o GPIO não existem, então esses caminhos rodam
  como falha. Um empréstimo deixado em disco é respeitado, e o
  `ADVANCED_AOCS` sem ESP32 termina na hora.
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
| `aocs`, `aocs_result` | o START (saiu ou não, emprestado ou não, por quanto tempo) e o resultado lido no fim |
| `lease_resumed`, `lease_file` | empréstimo deixado por um `obcd` anterior; falha ao gravar o arquivo do empréstimo |
| `time_anchor` | a hora da ground, repassada pelo `ttcd` |
| `status` | a cada minuto: modo, leituras, contadores |
| `ipc_up`, `ipc_down`, `stop`, `exit` | conexão com o `ttcd` e fim |

## O que falta

- **O lado do ESP32 (PLANO 2.4):** o contrato está em rascunho
  (`docs/icd/obc-aocs-i2c.md`), esperando a resposta da equipe do AOCS (§6);
  o `obcd` já fala o lado da Pi, testado contra um ESP32 simulado. Na
  bancada, qualquer ESP32 de desenvolvimento fazendo o papel de escravo nos
  pinos 3 e 5 serve.
- **Na bancada:** INA219 e DS18B20 avulsos numa Pi, LEDs no lugar do burn-wire
  e do aquecedor; conferir o que a linha de GPIO faz quando o processo morre
  sem passar pelo `SIGTERM` (o `obcd` desliga as saídas ao sair, e de novo ao
  reiniciar, em ~1 s).
- **O shunt da placa:** `ina219_shunt_mohm = 100` é a calibração do OBC
  anterior; medir o resistor da placa nova.
