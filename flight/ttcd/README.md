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
| Roteiros da bancada | `../../tools/bench/` |

Os comandos abaixo são alvos do `Makefile` da raiz. `make help` lista todos,
e cada alvo imprime os comandos que roda por baixo.

## Compilar e testar

```bash
make                  # configura e compila em build/
make test             # os 15 testes; make test T=link_sim roda só os que casam
```

Na Pi Zero 2 W a compilação leva alguns minutos; lá o `make` usa `-j2`
sozinho, para não esgotar os 512 MB de memória.

## Rodar sem hardware

O rádio UDP simula o enlace entre dois processos da mesma máquina: o frame
chega ao outro lado depois do seu tempo no ar, só se o receptor estiver no
mesmo perfil, e nada é ouvido enquanto se transmite. Fora da Pi ele é o
padrão (`RADIO=udp`), e os alvos da bancada rodam com ele exatamente como nos
rádios: é o ensaio do procedimento. Os logs vão para `build/bench-udp/`,
descartáveis.

```bash
make bench-ping       # um terminal só: sobe o ttcd, faz 10 PINGs, desliga e resume
make bench            # a bancada inteira, passos 1 a 6 (28 min)
```

À mão, em dois terminais:

```bash
make bench-ttcd       # terminal 1: o satélite, com o log na tela
make bench-gs         # terminal 2: a ground station; digite os comandos
ping 5
rate fast
ping 5
quit
```

Na Pi o padrão é o rádio de verdade; para ensaiar lá sem os rádios,
acrescente `RADIO=udp`.

## Do PC para a Pi

A Pi fica ligada na rede e é acessada por ssh. `make pi-<alvo>` copia esta
árvore para a Pi e roda `make <alvo>` lá, com a saída no seu terminal: a
compilação é nativa, na Pi, e o código testado é o que está no PC, commitado
ou não.

A cópia é do `rsync`, que usa o mesmo acesso ssh e só manda o que mudou: a
primeira vez são 4,4 MB, depois poucos KB por edição. A pasta na Pi (`~/ttec`,
ou `PI_DIR`) vira um espelho do PC — um arquivo apagado no PC some lá também,
então **edite só no PC**. Ficam intocados na Pi: `build*/`, `docs/vv/bancada/`
(os logs da bancada), qualquer `*.jsonl` ou `*.ndjson` e o `local.mk` dela. O
`.git` vai junto, para o `git describe` nomear a versão em cada log do `ttcd`
(por exemplo, `c570692-dirty`).

Uma vez, no PC:

```bash
ssh-copy-id pi@raspberrypi.local                   # entra sem senha (sem chave ainda? ssh-keygen -t ed25519)
echo 'PI_HOST = pi@raspberrypi.local' > local.mk   # o usuário@host do seu ssh; o git ignora este arquivo
make pi-provision                                  # na Pi: ferramentas de build, tmux, SPI0, usuário gama
```

Cada `make pi-…` abre duas conexões ssh, a cópia e o comando; sem a chave,
seriam duas senhas a cada vez.

No dia a dia:

```bash
make pi-test          # copia, compila na Pi e roda os testes lá
make pi-bench-ping    # um passo da bancada, nos rádios da Pi; os logs voltam para docs/vv/bancada/
make pi-fetch         # traz os logs da bancada da Pi (os pi-bench… já trazem ao terminar)
```

Variáveis da linha de comando vão junto: `make pi-test T=link_sim`,
`make pi-bench-ping RADIO=udp`.

Uma rodada longa, como a bancada inteira, morre se o ssh cair. Rode-a dentro
de um `tmux` na Pi, que continua sem você:

```bash
make pi-sync
ssh -t pi@raspberrypi.local tmux new -A -s bench   # abre a sessão, ou volta para ela se o ssh caiu
make -C ttec bench                                 # dentro da sessão
make pi-fetch                                      # no fim, de volta no PC: traz os logs
```

## Instalar na Pi

```bash
make pi-install           # ttcd, a unidade systemd e, se ainda não existir, /etc/gama/ttcd.conf
make pi-service-enable    # sobe agora e em todo boot
make pi-service-status    # o que o systemd diz, e o fim do log
make pi-service-logs      # acompanha o log de operação, /var/lib/gama/ttcd.jsonl
```

Num terminal dentro da Pi, os mesmos alvos sem o `pi-`. O `make install`
nunca sobrescreve um `/etc/gama/ttcd.conf` existente — compare-o com
`flight/ttcd/ttcd.conf.example` quando este mudar — e reinicia o serviço se
ele estiver rodando. O `ttcd` **não roda como root**: o usuário `gama`,
criado pelo `provision`, só está nos grupos `spi` e `gpio`. Se
`/dev/spidev0.0` não aparecer depois do `provision`, reinicie a Pi.

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
duas. Aí os passos roteirizados não servem, porque sobem os dois lados na
mesma Pi, e a bancada é feita à mão: `make bench-ttcd` numa e, na outra,
`make bench-gs GS_SPI=/dev/spidev0.0 GS_RESET=20 GS_DIO0=21`.

### Como cada passo roda

`make bench-<passo>`:

1. sobe o `ttcd` no rádio A, com `flight/ttcd/ttcd.conf.example`, 2 dBm e o
   socket IPC em `/tmp/ttec-bench.sock`;
2. alimenta o `gs_cli` no rádio B com os comandos de
   `tools/bench/<passo>.txt`, mostrando cada evento na tela;
3. desliga o `ttcd` e imprime o resumo: os resultados, as trocas de perfil
   dos dois lados, o RSSI e o SNR na ground e a contagem de eventos do
   `ttcd`.

O `ttcd` recomeça a cada passo porque, sem ouvir a ground por 2 min, ele cai
para SAFE (`contact_timeout_ms`): um passo rodado depois de uma pausa
começaria recuperando o enlace em vez de medir. Assim todo passo parte do
mesmo estado — os dois lados em NOMINAL, contadores zerados.

Os logs vão direto para `docs/vv/bancada/AAAA-MM-DD/`, um par por passo:
`HHMMSS-<passo>-ttcd.jsonl` e `HHMMSS-<passo>-ground.jsonl`. A data e a hora
vêm do relógio da Pi, que não tem RTC: sem rede, confira `date` antes.

O serviço precisa estar parado (`make service-stop`): com qualquer `ttcd`
rodando, o `make` recusa e mostra qual.

`make bench` roda os passos 1 a 6 em ordem. No rádio UDP levou 28 min — 17 no
passo 3, 8 no passo 6, menos de 2 em cada um dos outros —, e nos rádios deve
levar o mesmo, porque o simulador respeita o tempo no ar.
Um passo que falha interrompe a sequência; `make -k bench` segue para os
próximos.

### Passo 0 — configuração

```bash
make bench-config
```

Valida o arquivo e as sobrescritas da bancada sem tocar no rádio. Para usar a
configuração instalada em vez da do repositório, em qualquer alvo:
`TTCD_CONF=/etc/gama/ttcd.conf`.

### Passo 1 — os dois chips respondem

```bash
make bench-ping
```

**Aceite:** `ping_summary` com `acked` 10. Se o `ttcd` não subir, o log dele
tem `radio_open_failed` com o motivo; se o `gs_cli` não abrir o rádio B, ele
imprime `gs_cli: radio: unexpected RegVersion`. RegVersion `0x00` ou `0xFF`
indica ligação errada ou chip ausente.

### Passo 2 — tempo no ar (aceite do PLANO 2.2)

```bash
make bench-toa
```

`per 50` em NOMINAL, `per 20` em SAFE e `per 50` em FAST. No fim, o
`tools/analysis/toa_from_log.py` compara o tempo no ar medido no log do
`ttcd` com o modelo, e a tabela fica em `HHMMSS-toa.txt`. Para outro log:
`make toa LOG=arquivo.jsonl`.

**Aceite:** todos os grupos dentro de 5% do modelo (o `make` termina com erro
caso contrário). Também confirma o LDRO, indiretamente: com o bit errado, o
tempo no ar em SAFE sai do modelo.

### Passo 3 — taxa de erro de pacote, 1000 frames por perfil

```bash
make bench-per
```

`per 1000` em NOMINAL e em FAST, `per 200` em SAFE: 1000 PINGs em SAFE
levariam mais de uma hora. Cada `per` imprime `uplink_per` e `downlink_per`:
o uplink sai do `TM_STAT` do satélite antes e depois; o downlink, das lacunas
de sequência. O número inclui colisões, não só RF: no rádio UDP, que não perde
frame por ruído, este passo deu `uplink_per` 0,0272 em NOMINAL (28 de 1029),
0 em FAST e 0,1336 em SAFE (31 de 232) — telecomandos que chegaram enquanto o
satélite transmitia HK ou STAT. Nos rádios, o número mede RF e colisões
juntos; o SX1278 ainda tem o listen-before-talk, que o rádio UDP não tem.

### Passo 4 — troca de taxa nos dois sentidos

```bash
make bench-rates
```

Sete trocas, entre todos os pares de perfis, e 3 PINGs no perfil final.

**Aceite:** cada `ack` com `status` OK e um evento `rate` para o perfil
pedido; no log do `ttcd`, um `rate_commit` para cada troca.

### Passo 5 — o ACK perdido (o caso crítico do ADR-0007)

```bash
make bench-lost-ack
```

O `deafen 3` faz a ground ignorar o que receber por 3 s, então ela perde o
ACK do `SET_RATE` que vem em seguida. Esperado (reproduzido no rádio UDP em
2026-09-19, e de novo pelo `make bench-lost-ack` em 2026-09-22):

- o satélite vai para FAST (`rate … commanded`) e, sem confirmação, volta
  para NOMINAL em 20 s (`rate … revert`);
- a ground continua retransmitindo em NOMINAL até ser ouvida; o satélite
  troca de novo e confirma (`rate_commit`);
- o `ack` do `SET_RATE` chega com `attempts` > 1 e latência de ~21 s, e
  termina com os dois em FAST: os 3 PINGs do fim do roteiro passam em FAST.

### Passo 6 — sensibilidade do listen-before-talk

```bash
make bench-lbt
```

Mede o *follow-up* do ADR-0007: quão cedo o SX1278 acusa uma recepção em
curso. Roda `per 400` duas vezes, com o `ttcd` mandando HK a cada segundo
para ocupar o canal: com `lbt=header` nos dois rádios, depois com
`lbt=preamble`. Compare `attempts` do `ping_summary` e a perda do `per` entre
`HHMMSS-lbt-header-*` e `HHMMSS-lbt-preamble-*`. A simulação prevê que PINGs
precisando de retry caiam de ~12% para ~2% se "signal detected" for
confiável. Se for, mude o padrão para `preamble` e registre no ADR-0007. O
rádio UDP não tem listen-before-talk, e no ensaio sem hardware as duas rodadas
saíram iguais: 599 tentativas para 400 PINGs, `uplink_per` 0,3328. É o canal
sem LBT nenhum — a referência que as duas configurações têm de bater nos
rádios.

### Explorar à mão

Para o que está fora dos roteiros — `hk`, `stat`, `stream 5`, `mode aocs`,
`power 10`; a lista completa está no cabeçalho de `tools/gs_cli/gs_cli.c`:

```bash
make bench-ttcd       # terminal 1: o satélite; log na tela e em manual-ttcd.jsonl; Ctrl-C para
make bench-gs         # terminal 2: a ground; comandos digitados, log em manual-ground.jsonl; quit sai
make toa              # tempo no ar do manual-ttcd.jsonl do dia
```

Os parâmetros valem para todos os alvos da bancada: `POWER=10`,
`LBT=preamble`, `TTCD_OPTS='-o hk_period_ms=1000'` (sobrescritas extras do
`ttcd`), `GS_OPTS=...` (opções extras do `gs_cli`). `make help` mostra os
valores em uso.

### Registrar

Os logs já estão em `docs/vv/bancada/AAAA-MM-DD/`; rodando do PC, os
`make pi-bench…` os trazem ao terminar, e `make pi-fetch` traz o que tiver
ficado na Pi. Acrescente ali uma `notas.md` curta com a montagem
(distância, antenas, potência) e o resultado de cada passo, e commite tudo
junto. É a evidência de teste do HLR-COMM-01 e do HLR-COMM-03 para o Design
Package.

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
