# ttcd — daemon de TT&C

O `ttcd` é o dono do rádio LoRa: fala o protocolo do enlace com a ground
station, despacha telecomandos e agenda a telemetria. É o hub dos processos
de bordo — `adsbd` e o OBC se conectam a ele por um socket IPC.

| Onde está o quê | |
|---|---|
| Arquitetura de processos, núcleo puro | ADR-0005 |
| Transporte IPC | ADR-0006 |
| Protocolo do enlace, limite do HLR-COMM-01 | ADR-0007 (*Proposed*) |
| Acesso ao hardware (spidev, GPIO chardev) | ADR-0012 (*Proposed*) |
| Núcleo: enlace, telecomandos, telemetria | `link.c`, `tc_dispatch.c`, `tm_sched.c` |
| Casca: epoll, IPC, rádio, log | `main.c` |
| Configuração | `config.c`, `ttcd.conf.example` |
| Driver do rádio | `../radio/` |

## Compilar e testar

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j4
(cd build && ctest --output-on-failure)
```

Na Pi Zero 2 W a compilação leva alguns minutos; use `-j2` para não esgotar a
memória.

## Rodar sem hardware

O rádio UDP simula o enlace entre dois processos da mesma máquina: o frame
chega ao outro lado depois do seu tempo no ar, só se o receptor estiver no
mesmo perfil, e nada é ouvido enquanto se transmite.

```bash
# terminal 1: o satélite
./build/flight/ttcd -c flight/ttcd/ttcd.conf.example \
    -o radio=udp -o ipc_path=/tmp/ttec.sock -o log_path=/tmp/ttcd.jsonl

# terminal 2: a ground station
./build/tools/gs_cli/gs_cli --radio udp
ping 5
rate fast
ping 5
quit
```

## Instalar na Pi

```bash
sudo raspi-config nonint do_spi 0             # habilita SPI0 (spidev0.0 e 0.1)
sudo useradd --system --no-create-home --groups spi,gpio gama
sudo install -m 0755 build/flight/ttcd /usr/local/bin/ttcd
sudo install -D -m 0644 flight/ttcd/ttcd.conf.example /etc/gama/ttcd.conf
sudo install -m 0644 flight/ttcd/ttcd.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now ttcd
journalctl -u ttcd -f                          # erros de inicialização
tail -f /var/lib/gama/ttcd.jsonl               # o log de operação
```

O `ttcd` **não roda como root**: basta o usuário estar nos grupos `spi` e
`gpio`.

## Teste de bancada — PLANO fase 2.7

Objetivo: dois RA-02 trocando frames de verdade, com as medições que o
ADR-0004 e o ADR-0007 pedem.

### Ligação: dois RA-02 numa só Pi

O barramento SPI0 tem dois chip-selects. O rádio A (satélite, `ttcd`) usa a
ligação da missão anterior; o rádio B (ground, `gs_cli`) usa o segundo
chip-select e dois GPIOs livres — nenhum dos dois conflita com I²C, 1-Wire
(GPIO4) nem com o fio de queima da antena (GPIO19) do OBC.

| RA-02 | Rádio A — satélite | Rádio B — ground |
|---|---|---|
| 3.3V | pino 1 | pino 17 |
| GND | pino 6 | pino 9 |
| SCK | pino 23 (GPIO11) | pino 23 (compartilhado) |
| MISO | pino 21 (GPIO9) | pino 21 (compartilhado) |
| MOSI | pino 19 (GPIO10) | pino 19 (compartilhado) |
| NSS | pino 24 (GPIO8, CE0) → `/dev/spidev0.0` | pino 26 (GPIO7, CE1) → `/dev/spidev0.1` |
| RESET | pino 38 (GPIO20) | pino 36 (GPIO16) |
| DIO0 | pino 40 (GPIO21) | pino 37 (GPIO26) |

**Cuidados:**
- O RA-02 é **3,3 V**. Nunca 5 V.
- **Antena (ou carga de 50 Ω) conectada antes de transmitir.** Transmitir
  sem carga pode danificar o amplificador.
- **Potência de 2 dBm nos dois lados.** A ~1 m e 20 dBm chegam cerca de
  −3 dBm ao receptor, perto da saturação, e a taxa de erro medida deixaria de
  representar o enlace.

A alternativa, duas Pis com um rádio cada, usa a mesma ligação do rádio A nas
duas.

### Passo 0 — preparação

```bash
ttcd -c /etc/gama/ttcd.conf -o tx_power_dbm=2 --check-config
```

Rodando `ttcd` e `gs_cli` na mesma Pi, pare o serviço e rode o `ttcd` à mão
para ver o terminal:

```bash
sudo systemctl stop ttcd
ttcd -c /etc/gama/ttcd.conf -o tx_power_dbm=2 -o log_path=bancada.jsonl
```

### Passo 1 — os dois chips respondem

O log do `ttcd` deve ter `boot`, não `radio_open_failed`. O `gs_cli` deve
imprimir `start`, e não `gs_cli: radio: unexpected RegVersion`. RegVersion
`0x00` ou `0xFF` indica ligação errada ou chip ausente.

```bash
gs_cli --radio sx1278 --spi /dev/spidev0.1 --reset 16 --dio0 26 --power 2 | tee ground.jsonl
ping 10
```

**Aceite:** `ping_summary` com `acked` 10.

### Passo 2 — tempo no ar (aceite do PLANO 2.2)

Dentro do `gs_cli`:

```
per 50
rate safe
per 20
rate fast
per 50
rate nominal
quit
```

Depois:

```bash
python3 tools/analysis/toa_from_log.py bancada.jsonl
```

**Aceite:** todos os grupos dentro de 5% do modelo (a ferramenta sai com erro
caso contrário). Também confirma o LDRO, indiretamente: com o bit errado, o
tempo no ar em SAFE sai do modelo.

### Passo 3 — taxa de erro de pacote, 1000 frames por perfil

```
per 1000
rate fast
per 1000
rate safe
per 200
rate nominal
```

Cada `per` imprime `uplink_per` e `downlink_per`: o uplink sai do `TM_STAT`
do satélite antes e depois; o downlink, das lacunas de sequência. Em SAFE,
1000 PINGs levam mais de uma hora; 200 bastam.

### Passo 4 — troca de taxa nos dois sentidos

```
rate safe
rate nominal
rate fast
rate nominal
rate fast
rate safe
rate nominal
```

**Aceite:** cada `ack` com `status` OK e um evento `rate` para o perfil
pedido; no log do `ttcd`, um `rate_commit` para cada troca.

### Passo 5 — o ACK perdido (o caso crítico do ADR-0007)

```
deafen 3
rate fast
```

O `deafen` faz a ground ignorar o que receber por 3 s, então ela perde o ACK
do `SET_RATE`. Esperado (reproduzido no rádio UDP em 2026-09-19):

- o satélite vai para FAST (`rate … commanded`) e, sem confirmação, volta
  para NOMINAL em 20 s (`rate … revert`);
- a ground continua retransmitindo em NOMINAL até ser ouvida; o satélite
  troca de novo e confirma (`rate_commit`);
- o `ack` do `SET_RATE` chega com `attempts` > 1 e latência de ~21 s, e
  termina com os dois em FAST.

### Passo 6 — sensibilidade do listen-before-talk

Mede o *follow-up* do ADR-0007: quão cedo o SX1278 acusa uma recepção em
curso. Gere tráfego frequente do satélite e compare as duas configurações:

```bash
ttcd … -o tx_power_dbm=2 -o hk_period_ms=1000 -o lbt=header
gs_cli … --lbt header          # per 400
# repetir com -o lbt=preamble e --lbt preamble
```

Compare `attempts` do `ping_summary` e a perda do `per` entre as duas. A
simulação prevê que PINGs precisando de retry caiam de ~12% para ~2% se
"signal detected" for confiável. Se for, mude o padrão para `preamble` e
registre no ADR-0007.

### Registrar

Copie os logs (`bancada.jsonl`, `ground.jsonl`) e a saída do
`toa_from_log.py` para `docs/vv/bancada/AAAA-MM-DD/`, com uma nota curta dos
resultados e da montagem (distância, antenas, potência). É a evidência de
teste do HLR-COMM-01 e do HLR-COMM-03 para o Design Package.

## Log

Um objeto JSON por linha, com o relógio monotônico absoluto em `mono_ms`
(o mesmo relógio das épocas do `adsbd`, então os logs se alinham). Eventos
principais:

| Evento | Quando |
|---|---|
| `start`, `boot` | início: versão do build, rádio, parâmetros |
| `tx`, `tx_done` | cada frame transmitido: tipo, seq, perfil, tempo no ar modelado e medido |
| `rx`, `rx_bad`, `rx_foreign` | cada frame recebido, rejeitado ou alheio |
| `tc`, `tc_duplicate` | cada telecomando: comando, seq, status, motivo |
| `rate`, `rate_commit` | troca de perfil (comandada, revertida, por perda de contato) |
| `contact_lost` | queda para SAFE |
| `lbt_defer` | transmissão adiada porque havia recepção em curso |
| `obc_telemetry`, `obc_mode` | o que o OBC reportou (inclui temperatura da bateria) |
| `ipc_peer`, `ipc_drop_peer` | clientes IPC conectando e saindo |
| `time_anchor` | `SET_TIME`: par relógio monotônico ↔ UTC |
| `shutdown`, `stop`, `log_full` | fim |
