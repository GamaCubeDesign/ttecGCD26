# Seção 3 do guia: as decisões de arquitetura

Dúvidas da leitura da seção 3 (As decisões de arquitetura) do [guia de estudo](../relatorios/2026-11-10-guia-de-estudo.html), com as respostas. Índice e convenções no [README](README.md).

Sessão de 10/11/2026: perguntas 16 a 28.

Para acompanhar visualmente: a página
[máquina de estados](../relatorios/2026-11-10-maquina-de-estados.html), vista
"A arquitetura agora", mostra os processos e o que flui entre eles.

## ADRs: gerais

### 16. O que é IPC, e como isso impacta o sistema?

**IPC** (*inter-process communication*, comunicação entre processos) é
qualquer jeito de dois processos trocarem dados. Precisa de um nome próprio
porque, no Linux, cada processo tem a **sua própria memória**: uma variável do
`adsbd` não existe para o `ttcd`. Para passar um retrato de um para o outro,
alguém tem de copiá-lo, e quem copia é o kernel.

No projeto (ADR-0006, `flight/libipc/`):

- **Um socket Unix** (`AF_UNIX`), em `/run/gama/ttec.sock`. É como um socket de
  rede, mas só dentro da mesma máquina: não sai pela placa de rede, é o kernel
  que copia os bytes de um processo para o outro.
- **Tipo `SOCK_SEQPACKET`**: cada `send` chega como **exatamente um** `recv`. A
  mensagem chega inteira ou não chega; nunca pela metade, nunca grudada na
  seguinte. A missão anterior lia um fluxo byte a byte, e um byte perdido a
  dessincronizava para sempre; esse tipo de socket elimina o problema por
  construção.
- **Estrela, com o `ttcd` no centro** (ADR-0005): o `ttcd` escuta; o `adsbd` e
  o `obcd` se conectam a ele e se apresentam com `IPC_HELLO`, que diz o papel
  de cada um. O `adsbd` e o `obcd` **nunca** falam entre si. O modo do OBC chega
  ao `adsbd` repassado pelo `ttcd`.
- **O mesmo frame do rádio**: cada mensagem de IPC é um frame GAMA (pergunta
  18), só que com tipos `IPC_*` (`0x30`–`0x37`). O mesmo código que codifica e
  confere o CRC no rádio serve para a IPC, então todo teste do rádio exercita
  também o caminho do OBC.

| Mensagem | De → para | O quê |
|---|---|---|
| `IPC_HELLO` | `adsbd`/`obcd` → `ttcd` | "eu sou o adsbd" |
| `IPC_TC_EVENT` | `ttcd` → `obcd` | um evento de modo vindo de um telecomando |
| `IPC_MODE` | `obcd` → `ttcd` → `adsbd` | o modo atual |
| `IPC_TELEMETRY` | `obcd` → `ttcd` | V, I, temperatura, atitude (1 s) |
| `IPC_TIME_SET` | `ttcd` → `obcd`, `adsbd` | a âncora de tempo vinda do solo |
| `IPC_TRACKS`, `IPC_STAT`, `IPC_ROSTER` | `adsbd` → `ttcd` | retratos, contadores, callsigns |

**O impacto no sistema:**
1. **Isolamento.** Um processo que cai não corrompe a memória dos outros, e o
   `ttcd` percebe na hora: o kernel fecha o socket do morto, e o `recv` do
   outro lado devolve fim de arquivo. Não é preciso heartbeat.
2. **Nada espera.** Toda chamada é não bloqueante (`MSG_DONTWAIT`). Se a fila
   do outro lado estiver cheia, a mensagem é descartada e **contada**, em vez de
   travar o laço (regra 6 do `AGENTS.md`).
3. **Um contrato explícito**: os payloads em `common/gama_ipc.h` são a
   interface entre as equipes. Cada lado é testado contra um "outro lado" falso.

### 17. O que significa dizer que o `adsbd` é dono do SDR?

"Dono" quer dizer que **só um processo abre o dispositivo e responde por
ele**. O SDR é um dispositivo USB, e só um programa por vez consegue usá-lo.

A ADR-0008 resolveu um conflito: o OBC da outra equipe tinha um driver
(`sdr.c`) que esperava um receptor que já entrega mensagens decodificadas por
uma porta serial. O receptor real (NESDR Nano 3) só entrega amostras IQ
([pergunta 13](01-02-o-problema.md)), e não decodifica nada. A decisão:

```
antena → NESDR → dump1090-fa (filho do adsbd) → SBS :30003 → adsbd → IPC → ttcd
```

- O **`adsbd`** sobe o `dump1090-fa` como processo filho (pergunta 26),
  supervisiona e reinicia se ele morrer, e conta os reinícios.
- O **`dump1090-fa`** é quem abre o USB do SDR de fato.
- O **OBC não toca no SDR**. Ele só recebe o número de mensagens pelo `ttcd`,
  como qualquer outra informação do payload.

Ser dono implica responsabilidade: se o SDR falha, quem reage é o `adsbd`. Se
o `adsbd` cai, o `ttcd` continua transmitindo, e o satélite segue falando com o
solo.

### 18. Frame é a organização das mensagens do satélite para a ground? É como a informação chega e é decodificada?

Sim. Um **frame** é a unidade que vai pelo rádio: um "envelope" com cabeçalho,
conteúdo e conferência. O formato está em `common/gama_frame.h`:

```
 ver(1) │ type(1) │ seq(2) │ len(1) │ payload(0..248) │ crc16(2)      (bytes)
```

| Campo | Para quê |
|---|---|
| `ver` | versão do protocolo (hoje 2); uma versão diferente é recusada |
| `type` | o que vem dentro: `TC`, `TC_ACK`, `TM_HK`, `TM_TRACKS`, `BEACON`… |
| `seq` | número de sequência. Um buraco na sequência é uma perda contada; um `seq` repetido num TC é uma retransmissão |
| `len` | tamanho do payload |
| `payload` | o conteúdo: um retrato, um HK, um telecomando |
| `crc16` | conferência: um bit errado no caminho, e o frame é recusado e contado |

Três detalhes:
- **Vale nos dois sentidos**: o solo também manda frames (`TC`), e o satélite
  responde com frames `TC_ACK`.
- **Vale também dentro do satélite**: a IPC usa o mesmo formato (pergunta 16).
- **A decodificação é campo a campo**, sempre com `common/gama_bytes.h`, em
  little-endian. Nunca se converte uma struct de C direto em bytes (regra 1 do
  `AGENTS.md`), porque o layout de uma struct na memória muda de compilador
  para compilador. Os bytes exatos de cada registro ficam fixados em
  `tests/test_vectors.c`.

O mesmo `common/` compila no Pi e no ESP32, então os dois lados codificam e
decodificam com o mesmo código: é impossível o formato estar errado num lado
só.

### 19. O relógio monotônico é enviado pela ground em todo TC? É um frame que leva o comando e também o tempo?

Não, e são duas coisas diferentes.

**O relógio monotônico não viaja.** Cada computador tem o seu. O da Pi conta
desde o boot da Pi e nunca sai dela. O solo não sabe nem precisa saber esse
valor.

**O que o solo manda é a hora de parede**, e num telecomando próprio: o
`SET_TIME` (`0x02`, com segundos e nanossegundos Unix). Ele **não** vai junto
com os outros comandos.

Quando ele é mandado: pelo operador, com o comando `time` do console, ou pela
própria ground **a cada vez que ganha ou recupera contato** com o satélite,
se a opção `auto_time` estiver ligada (ADR-0009). A opção vem **desligada**
por padrão no console: liga-se com `--auto-time on` na `gs_cli` ou no
`gs_serial.py`. Não é a cada TC, nem a cada frame.

No satélite, o `ttcd` guarda a âncora, ou seja, o par "monotônico agora ↔ hora
do solo", descontando o tempo que o frame levou no ar, e a repassa ao `obcd` e
ao `adsbd` (`IPC_TIME_SET`). A partir daí:

```
hora de parede = monotônico agora + (âncora_parede − âncora_monotônico)
```

E os frames comuns? Não levam hora nenhuma. Cada registro de avião leva a
**idade** da informação (`age_ds`), medida até o início da transmissão. A ground
carimba a chegada com o relógio dela e subtrai: `chegada − tempo no ar −
idade`. Assim as trajetórias ficam no relógio da ground, sem depender da âncora
(ADR-0007 §6). A âncora serve para o **registro de bordo** (`rx_epoch_ns` no
NDJSON) e para os logs.

### 20. A validação de um comando é centralizada num processo, ou cada um cuida da sua? Quem decide, por exemplo, que durante a missão não se pode mexer em algo?

**Em camadas, e cada camada pertence a quem é dono daquilo.** Não há um
validador central. Um `SET_MODE` passa por três filtros:

| Camada | Quem | O que recusa | Resposta ao solo |
|---|---|---|---|
| 1. O frame | `ttcd`, `link.c` | CRC errado, versão errada, tamanho errado | nada: o frame é descartado e contado; a ground retransmite |
| 2. O comando | `ttcd`, `tc_dispatch.c` | comando desconhecido, argumento fora da faixa, OBC desconectado | `TC_ACK` com `UNKNOWN_CMD`, `BAD_ARGS` ou `FAILED` |
| 3. A regra da missão | `obcd`, `obcd_transition()` | um evento que não vale no modo atual (ex.: `mission_adsb` durante `BASIC_INTERMEDIATE`) | o ACK já saiu como OK; o `obcd` registra `event_refused`, e o HK seguinte mostra o modo inalterado |

**Sim, quem decide o que é permitido em cada modo é o `obcd`.** Ele é a única
fonte da verdade sobre o modo (ADR-0005). O `ttcd` **não guarda uma cópia** da
máquina de modos de propósito: duas cópias acabam divergindo, e o satélite
passaria a reportar um modo e se comportar como outro.

O `ttcd` é dono de outras regras, as do enlace: `SET_RATE`, `SET_TX_POWER`,
`STREAM_*` e `SHUTDOWN`. Nenhuma passa pelo OBC. Por exemplo, um `SET_RATE` para
um perfil que não existe volta como `BAD_ARGS`, e o `SHUTDOWN` exige um número
mágico (`0xA5A5`) para que um byte corrompido não desligue o satélite.

Um detalhe de projeto, que aparece também na página da máquina de estados: o
ACK de um `SET_MODE` só diz que o `ttcd` **entregou** o evento, não que o modo
mudou. Isso acontece porque o `ttcd` responde antes de o `obcd` decidir. A
confirmação real é o `obc_mode` do HK seguinte.

### 21. O que significa na prática "hardware pelas interfaces do kernel: spidev, GPIO por character device, i2c-dev"?

No Linux, **o hardware aparece como arquivos em `/dev`**, e o programa usa
chamadas de sistema comuns (`open`, `read`, `write`, `ioctl`) sobre eles. Quem
fala com os registradores do chip é o driver do kernel.

| Interface | Arquivo | Como o programa usa | No projeto |
|---|---|---|---|
| **spidev** | `/dev/spidev0.0` | `ioctl(fd, SPI_IOC_MESSAGE, …)`: "transfira estes bytes no SPI" | o rádio SX1278 |
| **GPIO character device** | `/dev/gpiochip0` | um `ioctl` pede uma linha (pino) como entrada ou saída. Para uma entrada com detecção de borda, devolve **um fd que fica legível a cada interrupção** | DIO0 e RESET do rádio; burn-wire e aquecedores |
| **i2c-dev** | `/dev/i2c-1` | `ioctl(fd, I2C_RDWR, …)`: uma transação I²C completa por chamada | INA219 e ESP32 do AOCS |
| **w1_therm** | `/sys/bus/w1/devices/28-…/w1_slave` | um arquivo de texto que se lê | DS18B20 |

**O que isso muda na prática** (ADR-0012):
- **A interrupção vira um fd.** Quando o rádio termina de receber, o pino DIO0
  sobe, e o fd da linha fica legível. Esse fd entra no mesmo `epoll` que espera
  os sockets e os timers. Não há callback, nem thread, nem problema de ordem
  entre eventos.
- **Sem root e sem biblioteca.** A missão anterior usava a pigpio, que exige
  root ou um daemon próprio. Aqui basta o usuário pertencer aos grupos `spi`,
  `gpio` e `i2c`. Um processo de voo sem root é uma forma a menos de estragar o
  sistema.
- **Testável sem hardware.** O driver (`sx1278.c`, `ina219.c`) fala com um
  "barramento" abstrato. No Pi, o barramento é o `spidev`; nos testes, é um
  chip emulado em software.

### 22. Por que NDJSON e não JSON?

**NDJSON** (*newline-delimited JSON*) é **um objeto JSON completo por linha**:

```
{"icao":"E48DF5","transmission_type":3,"lat":-23.56,...}
{"icao":"E48DF5","transmission_type":4,"ground_speed_kt":451.7,...}
```

Um JSON comum seria **um único documento**, como `[ {...}, {...} ]`. Para um
registro que cresce durante 10 minutos, isso tem três problemas que o NDJSON
não tem:

1. **Acrescentar é só escrever no fim.** Para manter um array JSON válido, cada
   mensagem nova exigiria reescrever o `]` final. No NDJSON, basta escrever a
   linha nova com `append`.
2. **Uma queda estraga no máximo a última linha.** Se a Pi perde energia no
   meio de uma escrita, um array JSON fica inválido inteiro, porque falta o
   `]`. No NDJSON, todas as linhas anteriores continuam válidas. Para um
   registro de missão, isso é decisivo.
3. **Lê-se aos poucos.** Um programa processa linha a linha, sem carregar os
   7,34 MiB na memória. Ferramentas de linha, como `grep`, `tail -f`, `wc -l` e
   `jq`, funcionam direto.

O mesmo vale para os logs dos daemons (`ttcd.jsonl`, `obcd.jsonl`): um evento
por linha. O formato do registro ADS-B é, byte a byte, o do protótipo
`adsb_capture.c`, porque o estimador da ground já o lia e o orçamento de dados é
calculado a partir dele (160,4 B por linha).

### 23. Todo telecomando entra pelo `ttcd` e só depois chega ao `obcd`?

Sim. **O `ttcd` é a única porta de entrada**, porque é o único processo que
fala com o rádio. O caminho de um `mode mission_adsb`:

```
operador → console (ESP32) → frame TC pelo LoRa → ttcd: link.c (frame ok?)
  → tc_dispatch.c (comando ok?) → TC_ACK ao solo
  → IPC_TC_EVENT (evento 3) → obcd: obcd_transition() (vale neste modo?)
  → muda de modo → IPC_MODE → ttcd → IPC_MODE → adsbd
```

Mas **nem todo telecomando chega ao `obcd`**. Só o `SET_MODE` vira evento para
ele. Os outros são do próprio `ttcd`:

| Telecomando | Quem executa |
|---|---|
| `SET_MODE` | `obcd` (via `IPC_TC_EVENT`) |
| `SET_TIME` | `ttcd` guarda e repassa a âncora a `obcd` e `adsbd` |
| `PING`, `SET_RATE`, `SET_TX_POWER`, `STREAM_START/STOP`, `REQ_HK/STAT/ROSTER`, `SHUTDOWN` | `ttcd` |
| `BULK_START/ABORT` | respondem `FAILED`: adiado (ADR-0007) |

Se o `obcd` estiver desconectado quando chega um `SET_MODE`, o `ttcd` responde
com `FAILED` ("obc not connected"), e o solo sabe na hora.

## Daemons e processos

### 24. Por que um processo para cada (ADS-B, OBC, TT&C), e não threads?

A ADR-0005 discute exatamente isso (seção "One multi-threaded process"). A
razão principal: **o rádio é o único jeito de diagnosticar e recuperar o
satélite**, e ele precisa sobreviver a falhas dos outros.

| | Threads (um processo) | Processos separados |
|---|---|---|
| Memória | compartilhada: um ponteiro errado numa thread corrompe as outras | separada: um processo não enxerga a memória do outro |
| Uma falha grave (segfault) | **derruba o processo inteiro**, rádio incluído | derruba só aquele processo; o systemd o reinicia, e os outros nem param |
| Comunicação | direta (variáveis), mas exige mutex e cuidado com condições de corrida | explícita (IPC), com um contrato escrito |
| Equipes | um binário e um ciclo de entrega para todos | cada subequipe testa o seu contra um "outro lado" falso |

O exemplo da ADR: um segfault no parser de SBS mata o `adsbd`. O systemd o
reinicia em 2 s, e durante esse tempo o `ttcd` continua mandando HK e
respondendo telecomandos. Num processo único, o mesmo erro calaria o rádio, e
a missão estaria perdida.

O custo é a IPC, que a ADR chama de "problema resolvido" (ADR-0006): 10 000
frames em cada sentido em 0,03 s no PC de desenvolvimento (`test_libipc`).

**Threads também são usadas, onde fazem sentido.** O `obcd` tem duas: uma para
o DS18B20, cuja leitura leva ~750 ms, e uma dona do I²C. Elas existem para que
o laço principal nunca espere (regra 3), e conversam com ele por pipes. O
critério é: thread para **não bloquear**, processo para **isolar falhas**.

### 25. Processo é a mesma coisa que daemon?

Quase. Todo daemon é um processo, mas nem todo processo é um daemon.

- **Processo**: um programa em execução, com memória, PID e descritores de
  arquivo próprios. O `ls` que você roda no terminal é um processo que vive
  milissegundos.
- **Daemon**: um processo que **roda em segundo plano, sem terminal, por tempo
  indeterminado**, prestando um serviço. Por convenção, o nome termina em
  **d**: `sshd`, `systemd`, e aqui `ttcd`, `adsbd`, `obcd`.

Antigamente, um daemon tinha de se "desacoplar" sozinho do terminal (fork
duplo, `setsid`). Hoje, com o **systemd**, o programa só roda normalmente em
primeiro plano, e o systemd cuida do resto: inicia no boot, guarda os logs,
reinicia se cair. É o que fazem os três daemons aqui (`flight/*/*.service`).

### 26. Esse processo é criado por um `fork`, ou de outra forma? É uma chamada de sistema?

No Linux, um processo novo **sempre** nasce de chamadas de sistema, em dois
passos:

1. **`fork()`** cria uma **cópia** do processo atual. Pai e filho continuam
   executando a partir da linha seguinte; a diferença é o valor de retorno: 0
   no filho, o PID do filho no pai.
2. **`exec…()`** (por exemplo, `execv`) **substitui** o programa do processo
   por outro binário. O PID continua o mesmo, mas o código passa a ser outro.

No projeto, há dois casos:

**a) Os três daemons são criados pelo systemd**, e não um pelo outro. No boot,
o systemd lê `ttcd.service`, `adsbd.service` e `obcd.service` e faz o
`fork` + `exec` de cada um. Os arquivos `.service` dizem como:

```ini
ExecStart=/usr/local/bin/obcd -c /etc/gama/obcd.conf
User=gama            # sem root
Restart=always       # caiu? sobe de novo
RestartSec=1
WatchdogSec=10       # só o obcd: se travar sem avisar por 10 s, é reiniciado
```

Por isso os três são independentes: nenhum é pai do outro, e eles se acham
pelo caminho do socket (`/run/gama/ttec.sock`), não por parentesco.

**b) O `dump1090-fa` é criado pelo `adsbd`**, com `fork` + `execv`
(`flight/adsbd/main.c`). Há um detalhe a mais: logo depois do `fork`, o filho
chama `prctl(PR_SET_PDEATHSIG, SIGTERM)`, que pede ao kernel "se meu pai
morrer, me mande SIGTERM". Assim um `adsbd` que cai não deixa um `dump1090`
órfão segurando o SDR. Quando o filho termina, o kernel avisa o pai com um
`SIGCHLD`, e o `adsbd` o recolhe com `waitpid` (um filho morto e não recolhido
vira "zumbi").

### 27. Um exemplo bem simples de como fazer o que é feito aqui

O arquivo [`exemplos/dois_processos.c`](exemplos/dois_processos.c) tem ~90
linhas e reproduz o esqueleto do `ttcd`:

```bash
cc -std=c11 -Wall -Wextra -o dois_processos docs/estudo/exemplos/dois_processos.c
./dois_processos
```

Saída (conferida em 10/11):

```
[pai]   recebi do filho: "retrato 1"
[pai]   tic: hora de mandar HK
[pai]   recebi do filho: "retrato 2"
[pai]   tic: hora de mandar HK
[pai]   recebi do filho: "retrato 3"
[pai]   tic: hora de mandar HK
[pai]   o filho fechou o socket: morreu ou terminou
```

O que ele faz, passo a passo, e onde está o equivalente no projeto:

| Passo no exemplo | O que é | No projeto |
|---|---|---|
| `socketpair(AF_UNIX, SOCK_SEQPACKET, …)` | dois sockets já conectados um ao outro | `ipc_server_open` + `ipc_client_connect` (`flight/libipc/ipc.c`): um socket com nome, porque os processos são criados separadamente pelo systemd |
| `fork()` | cria o segundo processo | o systemd cria cada daemon (pergunta 26) |
| o filho: `send(…)` | manda uma mensagem inteira | o `adsbd` mandando `IPC_TRACKS` |
| `timerfd_create` | um timer que é um fd: fica legível a cada tique | os timers do `ttcd` (HK a cada 10 s…) |
| `epoll_wait` | **o único lugar que espera**: acorda quando qualquer fd tem algo | o laço de `flight/ttcd/main.c` |
| `recv` devolve 0 | o outro lado fechou | o `ttcd` percebe que o `adsbd` morreu |
| `waitpid` | recolhe o filho morto | o `adsbd` recolhendo o `dump1090` |

**O que o exemplo simplifica.** O filho usa `sleep(1)`, o que é aceitável só
porque ele não tem um laço para travar. Num daemon do projeto isso é proibido
(regra 2); o tempo vem de um `timerfd`. O exemplo também manda texto, e o
projeto manda frames GAMA com CRC.

Um próximo passo para experimentar: troque o `socketpair` por um socket com
nome. O pai faz `socket` + `bind("/tmp/teste.sock")` + `listen` + `accept`; o
filho, depois do `fork`, faz `socket` + `connect`. Isso dá o desenho do `ttcd`
com o `adsbd`.

### 28. Como os processos se comunicam? Por chamadas de sistema? Por sinais (SIGs)?

**Os dados vão por sockets, e os sockets são usados por chamadas de sistema.**
Os sinais são só para o ciclo de vida.

**Os dados:** todo `send` e `recv` é uma chamada de sistema. O kernel copia os
bytes da memória de um processo para a do outro. O mesmo vale para `epoll_wait`,
`read` de um `timerfd` e `ioctl` no `spidev`. Toda interação com o mundo fora
do processo passa pelo kernel. Por isso a regra do núcleo puro (ADR-0005): os
`core.c` não fazem nenhuma dessas chamadas. Quem as faz é a casca (`main.c`),
que traduz o que aconteceu em eventos para o núcleo.

**Os sinais** são avisos curtos do kernel, sem dados: "termine", "seu filho
morreu". No projeto, aparecem assim:

| Sinal | De → para | Para quê |
|---|---|---|
| `SIGTERM` | systemd → daemon | "termine com calma" (no desligamento ou ao parar o serviço) |
| `SIGINT` | terminal → daemon | Ctrl+C na bancada |
| `SIGTERM`, depois `SIGKILL` | `adsbd` → `dump1090` | ao sair de `MISSION_ADSB`: pede para terminar e, 3 s depois, mata |
| `SIGCHLD` | kernel → `adsbd` | o `dump1090` morreu |

Um detalhe que vale aprender: sinais normalmente **interrompem** o programa no
meio de qualquer coisa e rodam uma função à parte (o *handler*), o que é fonte
clássica de bugs. Os daemons daqui usam **`signalfd`**: bloqueiam os sinais e
os recebem como **mais um fd** no `epoll`. Um `SIGTERM` vira um evento como
outro qualquer, na ordem, no mesmo laço. É a mesma ideia do GPIO como fd
(pergunta 21) e do timer como fd (pergunta 27): **tudo vira um descritor de
arquivo, e um só laço espera por todos**.

---

## Referências

- `docs/adr/0005-process-architecture-and-fault-isolation.md`: por que processos separados, e o `ttcd` como hub.
- `docs/adr/0006-obc-ttec-ipc-transport.md`: o socket `SOCK_SEQPACKET` e as alternativas descartadas.
- `docs/adr/0007-ota-frame-format-and-arq.md`: o frame e o ARQ.
- `docs/adr/0008-adsb-receive-chain-ownership.md`: o SDR, o `dump1090` e o `adsbd`.
- `docs/adr/0009-time-reference-and-synchronisation.md`: o tempo e o `SET_TIME`.
- `docs/adr/0012-hardware-access-through-kernel-interfaces.md`: spidev, GPIO como fd, sem root.
- `flight/libipc/ipc.h`, `common/gama_ipc.h`, `common/gama_frame.h`: a IPC e o frame.
- `flight/ttcd/tc_dispatch.c`: o que o `ttcd` executa e o que repassa.
- `flight/*/*.service`: como o systemd sobe cada daemon.
- [`exemplos/dois_processos.c`](exemplos/dois_processos.c): o exemplo da pergunta 27.
