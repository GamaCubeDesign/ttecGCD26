# Seções 1 e 2 do guia: o problema

Dúvidas da leitura das seções 1 (Como usar este guia) e 2 (O problema) do [guia de estudo](../relatorios/2026-11-10-guia-de-estudo.html), com as respostas. Índice e convenções no [README](README.md).

Primeira rodada: perguntas 1 a 12 (10/11/2026). Segunda rodada, sobre as respostas: perguntas 13 a 15.

## Data budget

### 1. O que são os 7,34 MiB? É o que 20 aviões geram mandando 4 mensagens por segundo, em dado bruto?

Quase: são as **mensagens já decodificadas**, gravadas em texto. Não são os
bits que o avião transmite.

A conta (`docs/budgets/data-budget.md` §2):

```
20 aviões × 4 msg/s × 600 s = 48 000 mensagens
48 000 × 160,4 B por linha  = 7,34 MiB
```

Os 160,4 B são o tamanho médio de **uma linha do registro de bordo**
(`adsb.ndjson`), o JSON que o `adsbd` escreve para cada mensagem. O
`lora_budget.py` não chuta esse valor: ele reproduz o formato da linha e pondera
os três tipos de mensagem pela frequência de cada um. A posição tem 156 B, a
velocidade 170 B e a identificação 109 B. Uma linha de posição, fixada em
`tests/test_sbs.c`:

```json
{"icao":"E48DF5","rx_epoch_ns":1789900000123456789,"transmission_type":3,"callsign":"","altitude_ft":37000,"lat":-23.559616,"lon":-46.658908,"on_ground":0}
```

Os "4 msg/s" vêm da especificação do ADS-B: posição a 2 Hz, velocidade a 2 Hz
e identificação a cada 5 s. É uma hipótese, ainda não medida no cenário dos
organizadores (§9 do orçamento).

"Bruto", no documento, significa **tudo o que foi decodificado, sem
agregar**. Há três volumes diferentes, e vale não confundi-los:

| O quê | Volume na missão | Contra os 58,3 KB do rádio |
|---|---|---|
| Amostras IQ do SDR (2,4 MS/s) | ~2,9 GB | — |
| As mensagens no ar: 48 000 × 112 bits (14 B) | 656 KiB | 11× |
| O registro de bordo em NDJSON (o "bruto" do orçamento) | 7,34 MiB | 129× |
| O que de fato desce: retratos de 20 B por avião | 48,5 KB | cabe |

Mesmo o dado de rádio mais compacto possível, os 14 bytes de cada mensagem,
não caberia: seria 11 vezes a capacidade. É isso que obriga a agregar a bordo
(ADR-0003): descer o **estado** de cada avião a cada 5 s, não cada mensagem.

### 2. O que é, de fato, o ADS-B bruto? O que chega no SDR, e como se decodifica? ADS-B é o nome do protocolo, da decodificação, do quê?

*Conhecimento geral, salvo onde indicado.*

**ADS-B** (Automatic Dependent Surveillance–Broadcast) é o nome do **sistema
de vigilância**, e o nome descreve o que ele faz:

- **Automatic**: transmite sozinho, sem ninguém interrogar o avião;
- **Dependent**: a posição vem do próprio avião, do GNSS dele, e não de um radar;
- **Surveillance–Broadcast**: é transmitido a todos, e qualquer receptor ouve.

O formato no ar é o **1090 ES** (Extended Squitter), uma mensagem do Modo S
de radar secundário, o *Downlink Format* 17.

**O que chega no ar.** Uma rajada em 1090 MHz: um preâmbulo de 8 µs e 112 µs
de dados, a 1 Mbit/s, em **PPM** (pulse position modulation). Cada bit dura
1 µs, e o pulso cair na primeira ou na segunda metade desse tempo diz se o bit
é 1 ou 0. Os 112 bits se dividem assim:

| Campo | Bits | Conteúdo |
|---|---|---|
| DF | 5 | tipo de mensagem (17 = ADS-B) |
| CA | 3 | capacidade do transponder. O nosso `on_ground` sai daqui |
| ICAO | 24 | endereço único da aeronave (ex.: `E48DF5`) |
| ME | 56 | a mensagem em si. Os 5 primeiros bits são o *type code* |
| PI | 24 | CRC: detecta erros e corrige 1 bit |

O *type code* define o que os 56 bits significam:

- **1–4**, identificação: o callsign, 8 caracteres de 6 bits (ex.: `TAM3054`);
- **9–18**, posição em voo: a altitude e a latitude/longitude em **CPR**
  (Compact Position Reporting), 17 bits cada;
- **19**, velocidade: componentes leste-oeste e norte-sul, e a razão de subida.

O CPR é o ponto delicado. Para caber em 17 bits, a posição vem "dobrada" numa
grade, e o avião alterna quadros *par* e *ímpar* com grades diferentes. Com um
par e um ímpar recebidos em menos de ~10 s, a posição sai sem ambiguidade.

**A cadeia de decodificação** (a nossa: ADR-0008, `flight/adsbd/`):

1. O **SDR** (NESDR Nano 3) não decodifica nada: ele só digitaliza o rádio em
   amostras IQ a 2,4 milhões por segundo. São os ~2,9 GB da tabela acima, que
   nunca vão para o disco.
2. O **`dump1090-fa`** faz o trabalho de rádio. Calcula a amplitude das
   amostras, procura o padrão do preâmbulo, fatia os 112 bits pela posição dos
   pulsos, confere o CRC (e corrige 1 bit se preciso), interpreta o *type
   code* e resolve o CPR.
3. O resultado sai em texto **SBS-1** na porta TCP 30003, uma linha por
   mensagem (`MSG,3,…,37000,,,-23.559616,-46.658908,…`).
4. O **`adsbd`** lê essas linhas (`sbs.c`), grava cada uma em NDJSON
   (`ndjson.c`) e funde tudo numa tabela por ICAO (`tracks.c`).

Ou seja: o "ADS-B bruto" que o satélite guarda já passou pelo `dump1090`. A
parte difícil do sinal fica com um software de terceiros consolidado, e o
nosso código começa na linha de texto.

### 3. O que são os 58,3 KB do LoRa? O que é "perfil nominal"? O que é "metade do tempo no ar"?

**Perfil** é um conjunto fixo de parâmetros do rádio LoRa: SF, largura de
banda e coding rate (perguntas 10 e 11). O ADR-0004 define só três, que o solo
escolhe por telecomando:

| Perfil | Para quê |
|---|---|
| `SAFE` (SF12) | recuperação: lento, mas ouve o sinal mais fraco. O `ttcd` cai nele sozinho se o solo sumir por 120 s |
| `NOMINAL` (SF9) | **a missão**: o perfil padrão, e o orçamento é feito sobre ele |
| `FAST` (SF7) | descarga em massa, com o solo perto |

**"Metade do tempo no ar"**: a missão dura 600 s, e o orçamento reserva
**50%**, ou seja, 300 s, para o satélite transmitir. O resto fica para ouvir o
solo (pergunta 4). A conta:

```
1592 bit/s (goodput do NOMINAL) × 300 s ÷ 8 = 59 700 B = 58,3 KiB
```

Um detalhe de unidade: o `lora_budget.py` divide por 1024, então o "KB" do
orçamento é, na verdade, KiB.

### 4. O *duty cycle* de 50% existe por causa do requisito de manter a comunicação sempre ativa, "within a bounded response time"?

Sim. É uma **escolha de projeto para cumprir o HLR-COMM-01**, não um limite
regulatório (`data-budget.md` §4, comentário de `TX_DUTY` no
`lora_budget.py`).

> HLR-COMM-01: *The system shall receive telecommands (TC) from the ground
> segment and execute corresponding actions within a bounded response time
> defined by the team.*

O raciocínio:

1. O SX1278 é **half-duplex**: enquanto transmite, o receptor está desligado.
   Um telecomando que chegue nesse momento se perde.
2. Logo, quanto mais tempo transmitindo, mais o telecomando espera. Um
   satélite transmitindo 100% do tempo nunca ouviria o solo.
3. O time reservou metade do tempo para transmitir. Os retratos a cada 5 s
   usam 251 s dos 300 s; housekeeping, estatísticas e ACKs ficam com o resto,
   e sobram 26 s de margem (§5).

Na prática, não é uma alternância rígida de "1 s fala, 1 s ouve". O satélite
fica ouvindo por padrão, e depois de cada frame que transmite faz um silêncio
curto, o **reply gap**: 112 ms no NOMINAL (ADR-0007 §2). O solo manda o
telecomando assim que um frame do satélite termina. O tempo de resposta que o
time definiu para o HLR-COMM-01, calculado pelo `lora_budget.py`:

| Perfil | Sem perda | Com uma perda | (+1 s se o OBC executa) |
|---|---|---|---|
| SAFE | 4,21 s | 10,89 s | |
| NOMINAL | 1,40 s | 3,37 s | |
| FAST | 0,44 s | 1,30 s | |

### 5. Como interpretar a tabela dos perfis? O que é *goodput*?

| Coluna | Significado |
|---|---|
| **SF** | spreading factor (pergunta 11): quanto cada símbolo é "esticado" no tempo |
| **CR** | coding rate: 4:5 manda 5 bits para cada 4 de dado (1 de correção de erro); 4:8 manda o dobro |
| **ToA (208 B)** | *time on air*: quanto tempo um pacote de 208 bytes ocupa o canal, com preâmbulo e cabeçalho LoRa incluídos |
| **Goodput** | **bits úteis por segundo de ar**: `208 × 8 ÷ ToA`. Só conta o que é dado nosso, não o preâmbulo nem a redundância |
| **Capacity in 600 s @50%** | goodput × 300 s ÷ 8: quantos bytes descem na missão inteira |
| **Sensitivity** | o sinal mais fraco que o receptor ainda decodifica. Quanto **mais negativo**, melhor: −137 dBm ouve um sinal 8 dB (≈6×) mais fraco que −129 dBm |

**Goodput contra bitrate.** O *bitrate* é a velocidade bruta do modem. O
*goodput* desconta tudo o que não é dado útil: o preâmbulo, o cabeçalho e os
bits de correção. É o número que importa para o orçamento: o que de fato chega.

Por que **208 B**? O repositório não justifica o valor; é só o tamanho de
referência que o ADR-0004 fixou para comparar os perfis, e não corresponde a
um frame real. Um frame cheio de retrato tem 7 B de framing + 12 registros de
20 B = 247 B (`TRACKS_PER_FRAME = 12` no `lora_budget.py`). Um retrato de 20
aviões são dois frames, de 247 B e 167 B, que somam os 414 B do orçamento.
*(Corrigido em 10/11: a primeira versão desta resposta dizia "10 registros
por frame".)*

A troca, lida linha a linha: do NOMINAL para o FAST, o goodput sobe 3,2×
(1592 → 5074 bps), mas a sensibilidade piora 6 dB (−129 → −123). Do NOMINAL
para o SAFE, ganham-se 8 dB de sensibilidade, a um custo de 11× no goodput.

### 6. Se houver gargalo no data budget com 20 aviões, dá para mudar para `GAMA_RATE_FAST` e mandar mais informação, com menos alcance?

**Dá, e o entendimento está certo**: mais taxa, menos alcance. Mas há três
detalhes que mudam a resposta.

**a) Como se troca.** Pelo telecomando `GAMA_TC_SET_RATE`; no console, o
comando é `rate fast`. A troca é a operação mais arriscada do enlace: se os dois
lados ficarem em perfis diferentes, nenhum ouve o outro, e o único canal para
corrigir é justamente o que quebrou. Por isso (ADR-0004, ADR-0007):

- o satélite **confirma no perfil antigo** e só depois troca;
- se nada for ouvido no perfil novo em 20 s (`rate_revert_ms`), ele **volta
  sozinho** ao anterior;
- se o solo sumir por 120 s (`contact_timeout_ms`), ele cai no `SAFE`.

**b) Com 20 aviões, não deve haver gargalo, por construção.** Como o satélite
desce **retratos** e não mensagens, o volume do downlink depende de **quantos
aviões** e de **com que frequência** se manda um retrato, e não de quantas
mensagens cada avião transmite. Um retrato de 20 aviões são 2 frames, 2,09 s
de ar; a cada 5 s, isso dá 251 s dos 300 s. O orçamento aguenta até 6 msg/s
por avião sem mudar nada.

O gargalo apareceria com **mais aviões**. Até 24 aviões, o retrato continua
em 2 frames cheios (2,46 s; 120 retratos = 295 s, no limite dos 300 s). Com
30, passaria a 3 frames (3,14 s; 376 s), acima do orçamento. Isso não
acontece porque o `adsbd` corta o retrato em **24 aviões**
(`snapshot_max_aircraft = 24` no `adsbd.conf.example`, "dois frames: os 20
aviões do orçamento") e conta os que ficaram de fora (`left_out`, no log do
`adsbd`). Com mais de 24 aviões, a saída seria o FAST, junto com um corte
maior, ou um período mais longo (`stream 7`, por exemplo). *(Corrigido em
10/11: a primeira versão não mencionava o corte de 24.)*

**c) Trocar para FAST, sozinho, não manda mais informação.** O satélite
continua mandando um retrato a cada 5 s, só que mais rápido: 2 frames em
~0,66 s, em vez de 2,09 s. O ganho é tempo livre. Para transformá-lo em mais
dados, é preciso também encurtar o período (por exemplo, `stream 2`), o que dá
mais pontos de trajetória por avião.

**E o alcance?** A 14 dBm (ADR-0015), a 100 m, a margem cai de 79,8 dB (SF9)
para 73,8 dB (SF7). Depois de descontar os 70 dB de pessimismo que o ADR-0004
assume (multipercurso, antena dessintonizada, interferência…), sobram 9,8 dB no
SF9 e só 3,8 dB no SF7. O solo da competição fica a dezenas de metros, então o
FAST provavelmente funciona, mas com pouca folga. Por isso o ADR-0004 o reserva
para a descarga pós-missão, com o solo perto, e voa a missão no NOMINAL.

## LoRa

*Conhecimento geral de telecomunicações; os números do projeto vêm do
`lora_budget.py`.*

### 7. O que significa dizer que o LoRa é uma modulação *spread spectrum*?

**Espalhamento espectral** é transmitir a informação numa **largura de banda
muito maior** do que a taxa de dados precisaria.

O LoRa no SF9 manda ~1,8 kbit/s de dado bruto, mas ocupa 125 kHz de banda.
Esse excesso é proposital. O receptor sabe exatamente o padrão com que o sinal
foi espalhado, então consegue "juntar" a energia de volta e separar o sinal do
ruído. Esse ganho chama-se **ganho de processamento**. É por ele que o LoRa
decodifica sinais **abaixo do nível do ruído**: no SF12, com SNR em torno de
−20 dB, ou seja, um sinal 100 vezes mais fraco que o ruído.

A contrapartida é usar mal a banda: muitos hertz para poucos bits.

### 8. O que significa "a tecnologia cobre a camada física, e protocolos como o LoRaWAN cobrem as camadas superiores"?

**LoRa** é só o jeito de pôr bits no ar e tirá-los de lá, ou seja, a
modulação. Ele não diz quem fala quando, como endereçar, como confirmar
entrega, nada disso.

**LoRaWAN** é um protocolo construído em cima do LoRa para redes de IoT:
gateways, servidor de rede, criptografia, endereçamento de milhares de
dispositivos.

**Nós não usamos LoRaWAN.** O enlace é ponto a ponto, então o projeto tem o
próprio protocolo sobre o LoRa (pergunta 9).

### 9. A camada física é a do modelo OSI? Como é o modelo OSI?

Sim. O **modelo OSI** divide a comunicação em 7 camadas. Cada uma resolve um
problema e usa o serviço da camada de baixo, sem precisar saber como ela
funciona.

| # | Camada | Problema que resolve | Exemplo na internet |
|---|---|---|---|
| 7 | Aplicação | o significado dos dados | HTTP |
| 6 | Apresentação | formato e codificação | TLS, UTF-8 |
| 5 | Sessão | abrir e manter uma conversa | — |
| 4 | Transporte | entrega de ponta a ponta, ordem, retransmissão | TCP, UDP |
| 3 | Rede | endereçar e rotear entre redes | IP |
| 2 | Enlace | quadros entre dois vizinhos: delimitar, detectar erro, acesso ao meio | Ethernet, Wi-Fi MAC |
| 1 | Física | bits ↔ sinal no meio | o rádio do Wi-Fi, o cabo |

**O nosso enlace, no modelo:**

| Camada | No projeto | Onde |
|---|---|---|
| 7 Aplicação | os registros de TM e os telecomandos (retrato, HK, `SET_MODE`…) | `common/gama_tm.*`, `common/gama_tc.*` |
| 3–6 | **não existem**: ponto a ponto, sem roteamento, sem sessão | — |
| 2 Enlace | o frame (`ver│type│seq│len│payload│crc16`), o ACK e a retransmissão (ARQ), o escutar antes de falar (LBT) | `common/gama_frame.*`, `flight/ttcd/link.c`, ADR-0007 |
| 1 Física | LoRa: SF, BW e CR dos três perfis, no SX1278 | `flight/radio/sx1278.c`, `common/gama_lora.*`, ADR-0004 |

Num sistema embarcado simples, é comum as camadas do meio sumirem: com dois
nós e um rádio, não há o que rotear.

### 10. Como funciona a modulação CSS (chirp spread spectrum)? Modular é "guardar informação discreta numa onda"?

**Modular** é exatamente isso: imprimir informação numa **portadora**, uma
onda senoidal de frequência fixa (a nossa: 433,92 MHz), variando uma das
propriedades dela:

- a **amplitude** (AM, ASK);
- a **frequência** (FM, FSK);
- a **fase** (PSK);
- ou, como no ADS-B, a **posição de pulsos** no tempo (PPM).

A informação digital vira variações discretas dessas grandezas, e o receptor
as mede para recuperar os bits.

**O chirp.** Um *chirp* é um sinal cuja frequência **varia linearmente** ao
longo do tempo. No LoRa, a frequência varre a banda inteira (125 kHz), de uma
ponta à outra, durante um símbolo, e volta ao início quando chega ao fim
(como o som de um "uuuíp").

**Como o chirp carrega bits.** Todos os símbolos são o mesmo chirp; o que muda
é **em que frequência ele começa**. Há 2^SF pontos de partida possíveis. No
SF9, são 512, e cada ponto de partida equivale a 9 bits.

**Como o receptor decodifica.** Ele multiplica o sinal recebido por um chirp
"ao contrário" (descendente). Isso cancela a varredura e sobra **um tom
fixo**, cuja frequência revela o ponto de partida. Uma FFT encontra esse tom.
Ruído e interferência não têm a forma de chirp, então se espalham pela FFT
inteira, enquanto o sinal se concentra num único pico. É daí que vem o ganho
de processamento da pergunta 7.

### 11. O SF "representa o número de bits por símbolo". Então um SF maior manda mais informação, com menos alcance?

É o contrário, e essa frase da Wikipedia confunde mesmo. Um **SF maior manda
menos informação por segundo e alcança mais longe**.

O SF faz duas coisas ao mesmo tempo:

- cada símbolo carrega **SF bits** (há 2^SF pontos de partida);
- mas cada símbolo **dura 2^SF / BW**: cada +1 no SF **dobra** a duração do
  símbolo.

O número de bits cresce de forma linear, e a duração cresce de forma
exponencial, então a taxa cai:

```
taxa bruta = SF × (BW / 2^SF) × CR
```

| SF | Bits por símbolo | Duração do símbolo | Taxa bruta (BW 125 kHz) | Sensibilidade |
|---|---|---|---|---|
| 7 (FAST) | 7 | 1,0 ms | 5,5 kbit/s (CR 4:5) | −123 dBm |
| 9 (NOMINAL) | 9 | 4,1 ms | 1,8 kbit/s (CR 4:5) | −129 dBm |
| 12 (SAFE) | 12 | 32,8 ms | 0,18 kbit/s (CR 4:8) | −137 dBm |

Um símbolo mais longo acumula mais energia, e o receptor separa melhor o sinal
do ruído: cada +1 no SF ganha ~2,5 dB de sensibilidade. Portanto:

- **SF alto**: devagar, longe, robusto;
- **SF baixo**: rápido, perto, frágil.

**O impacto na arquitetura, que já discutimos.** A missão anterior voava no
SF12 com BW 62,5 kHz: 2,6 KB na missão inteira, o que tornava o objetivo
impossível qualquer que fosse o payload. O critério do ADR-0004 é "o **menor**
SF cuja margem continua grande depois de todo pessimismo justificável, e o
resto vai para a taxa". A 100 m, o SF9 sobra 9,8 dB depois de descontar 70 dB
de perdas pessimistas, e o SF12 só acrescentaria margem a um excedente que não
se usa, custando 11× na taxa.

## Extra

### 12. O que significa "monotônico" em "a Pi não tem relógio de tempo real; todo carimbo é monotônico"?

Um computador tem (pelo menos) dois relógios, e eles servem para coisas
diferentes:

| Relógio | O que mede | Pode pular? |
|---|---|---|
| `CLOCK_REALTIME` (o "de parede") | a data e a hora do calendário | **sim**: alguém acerta, o NTP sincroniza, ele salta para frente ou para trás |
| `CLOCK_MONOTONIC` | o tempo decorrido desde o boot | **não**: só anda para frente, no mesmo ritmo |

"**Monotônico**" vem da matemática: uma função monotônica crescente nunca
diminui. Um relógio monotônico não diz "que horas são", mas garante que, se o
evento B foi carimbado depois do evento A, `t(B) ≥ t(A)`, e que a diferença
`t(B) − t(A)` é o tempo que realmente passou.

**Por que isso importa aqui** (ADR-0009):

- A Pi **não tem RTC**, a bateria com relógio que um PC tem. No boot, o
  relógio de parede é o último salvo no desligamento, ou 1970.
- No local da prova não há internet para o NTP. Se o relógio fosse acertado
  no meio da missão, ele **saltaria**, e as trajetórias no registro de bordo
  seriam cortadas ou embaralhadas, já que o estimador ordena por tempo.

**A solução**: todo processo carimba com `CLOCK_MONOTONIC`. A hora de verdade
é calculada como `monotônico + âncora`. A âncora vem do solo: a ground manda
`SET_TIME` (pelo comando `time`, ou sozinha a cada contato se a opção
`auto_time` estiver ligada, o que não é o padrão; ver
[pergunta 19](03-decisoes-de-arquitetura.md)), e o `ttcd` a repassa ao `adsbd` e ao `obcd` como
`IPC_TIME_SET`. Ninguém mexe no relógio do Linux. Assim, o primeiro ajuste
(possivelmente grande) acontece antes da missão, e os seguintes só corrigem a
deriva do cristal da Pi: a 50 ppm, uns 30 ms em 10 minutos.

---

## Segunda rodada (10/11/2026)

Dúvidas que surgiram na leitura das respostas acima.

### 13. O que é a amostra IQ do SDR? E por que ela é tão pesada?

*Conhecimento geral; os números do SDR são os do projeto (ADR-0008,
`data-budget.md` §6).*

**O que o SDR faz.** O SDR não entende ADS-B. Ele é um **digitalizador de
rádio**: sintoniza uma faixa em torno de 1090 MHz, desloca essa faixa para
perto de 0 Hz (*banda base*) e mede o sinal milhões de vezes por segundo. Cada
medida é uma **amostra**.

**Por que dois números (I e Q).** Uma única medida por instante diz quanto o
sinal vale, mas não diz se a onda está subindo ou descendo, ou seja, em que
**fase** ela está. Por isso o SDR mede duas versões do sinal, defasadas de 90°:

- **I** (*in-phase*), a componente alinhada com uma referência cosseno;
- **Q** (*quadrature*), a componente alinhada com uma referência seno.

Cada amostra é um par (I, Q). Esse par equivale a um ponto num plano, um vetor
com **amplitude** `√(I² + Q²)` e **fase** `atan2(Q, I)`. O ADS-B é modulado em
pulsos (PPM), então para ele só a amplitude importa: "tem pulso" é amplitude
alta, "não tem" é amplitude baixa. Outros sinais, como o LoRa, usam a fase e a
frequência, e é por isso que o SDR entrega sempre os dois números.

**Por que é pesado.** É uma conta direta:

```
2,4 milhões de amostras/s × 2 bytes (I e Q, 8 bits cada, no RTL2832U do NESDR)
  = 4,8 MB/s
4,8 MB/s × 600 s = 2,88 GB   (os "~2,9 GB" do orçamento)
```

São dois fatores:

1. **A taxa é alta porque o sinal é rápido.** Um bit do ADS-B dura 1 µs, e o
   pulso dura 0,5 µs. Para enxergar pulsos de 0,5 µs, é preciso medir várias
   vezes por microssegundo (critério de Nyquist: amostrar a pelo menos o dobro
   da banda do sinal, ~2 MHz). Os 2,4 MS/s dão 2,4 amostras por bit, que é o
   mínimo prático; 2,0 MS/s era o plano B, que não foi necessário.
2. **O SDR grava tudo, inclusive o silêncio.** Ele digitaliza a faixa inteira
   o tempo todo, haja avião ou não. Uma mensagem dura 120 µs. Com 20 aviões ×
   4 msg/s, o canal fica ocupado 20 × 4 × 120 µs = **9,6 ms por segundo**, ou
   seja, ~1% do tempo. **Cerca de 99% das amostras são ruído.**

Por isso as amostras nunca vão para o disco: o `dump1090-fa` as consome em
memória e só guarda as mensagens que encontra. É também o que mais pesa na CPU:
os 21,9% de um núcleo medidos em 09/10 são, quase todos, essa varredura das
amostras, e não dependem do número de aviões (`data-budget.md` §9).

### 14. Cada rajada do ADS-B traz 112 bits, decodificados a partir do SDR. Onde se calcula se um bit é 0 ou 1, na decodificação PPM?

**A primeira parte está certa**: cada mensagem ADS-B tem 112 bits (o DF17,
pergunta 2), num pulso de 120 µs (8 µs de preâmbulo e 112 µs de dados).

**A correção: o SDR não decide bit nenhum.** Ele só entrega as amostras IQ pela
USB. Quem decide 0 ou 1 é o **`dump1090-fa`, em software, na CPU da Pi**. Esse
código **não está no nosso repositório**: é software da FlightAware, instalado
como pacote, e o `adsbd` só o sobe e lê a saída (ADR-0008). No código-fonte do
`dump1090-fa`, o caminho é este (*conhecimento geral; confira no repositório
deles se for estudar o código*):

| Etapa | O que faz | Onde, no `dump1090-fa` |
|---|---|---|
| Amplitude | converte cada par (I, Q) em amplitude, com uma tabela pronta para não calcular raiz quadrada 2,4 milhões de vezes por segundo | `convert.c` |
| Preâmbulo | procura o padrão fixo de 4 pulsos (em 0; 1; 3,5 e 4,5 µs) e compara o nível dos pulsos com o das lacunas, para estimar sinal e ruído | `demod_2400.c` |
| Os bits (PPM) | para cada um dos 112 bits, compara a energia da **primeira metade** do microssegundo com a da **segunda**: maior na primeira é 1, maior na segunda é 0 | `demod_2400.c` |
| CRC | confere os 24 bits de paridade e corrige 1 bit errado, se preciso; o que não fecha é descartado e contado | `mode_s.c`, `crc.c` |
| Campos | lê o *type code*, extrai callsign, altitude e velocidade, e resolve o CPR com o par par/ímpar | `mode_s.c`, `cpr.c` |
| Saída | escreve a linha SBS-1 na porta 30003 | `net_io.c` |

**O detalhe dos 2,4 MS/s.** Com 2,4 amostras por bit, as metades de 0,5 µs não
caem exatamente sobre as amostras: uma amostra pode pegar um pedaço de cada
metade. Por isso o demodulador testa algumas **fases** possíveis de início da
mensagem e soma as amostras com pesos que compensam esse desalinhamento.
Depois fica com a fase cuja mensagem passa no CRC com o melhor sinal. É o
trecho mais pesado da CPU: roda continuamente sobre todas as amostras.

**Onde o nosso código começa.** Na linha de texto: `flight/adsbd/sbs.c` lê o
SBS-1, já com posição em graus e altitude em pés. Para o projeto, a
demodulação é uma caixa-preta medida por fora: a taxa de decodificação
(HLR-ADS-08, ≥ 95%) e a CPU (TTC-15).

### 15. Se a comunicação com o satélite cai, os dados ficam salvos no SD e podem ser recuperados depois? Ou o que se corrompe ou não chega à ground simplesmente se perde?

As duas coisas são verdade, em níveis diferentes. **No rádio, sim, o que não
chega se perde. A bordo, nada se perde: tudo fica no cartão SD**, e é
recuperado depois da missão, por cabo e não pelo rádio.

**1. O registro de bordo guarda tudo.** O `adsbd` grava **cada** mensagem
decodificada em `adsb.ndjson`, no SD (são os 7,34 MiB), esteja o rádio
funcionando ou não. Isso cumpre o HLR-SW-02 (registro completo). Esse registro
não depende do enlace, porque o `adsbd` segue o modo do OBC (ADR-0014), e não o
estado do contato. O limite de tamanho é 1 GiB (`ndjson_max_bytes` em
`adsbd.conf.example`), ~140 missões; ao chegar nele, o `adsbd` para de gravar e
registra um evento, em vez de lotar o disco e derrubar o `ttcd` junto
(`data-budget.md` §7).

**2. A recuperação é por USB ou SSH, depois da missão** (ADR-0003). A
alternativa de descer o registro pelo rádio foi avaliada e adiada (ADR-0007,
"Bulk download"): 7,34 MiB, mesmo no FAST e transmitindo 100% do tempo, levam
~3,4 h de ar; nos 50% que o enlace permite, ~6,9 h. Os telecomandos
`BULK_START` e `BULK_ABORT` existem no formato, mas respondem `FAILED`. As
regras pedem que os dados da missão estejam "disponíveis para pós-análise", e
não que cheguem por RF.

**3. No rádio, o que é telemetria não é retransmitido.** Os frames de TM
(retratos, HK, estatísticas) vão uma vez só. Se um se corrompe, o CRC o
rejeita; se não chega, a ground percebe pelo **buraco no `seq`** e conta a
perda. Isso é o HLR-ADS-08, que pede para quantificar a taxa de perda (≤ 5%) e
não para evitá-la. Retransmitir telemetria custaria tempo de ar que o
orçamento não tem. **Os telecomandos, ao contrário, são confirmados**: cada
um recebe um ACK e é retransmitido com o mesmo `seq` até ser confirmado, sem
executar duas vezes (ADR-0007, pergunta 4).

**4. Por que perder um retrato custa pouco.** O retrato é o **estado** de
todos os aviões, e não as mensagens desde o último envio. Se o retrato das
12:00:05 se perde, o das 12:00:10 traz a posição atual de todos os aviões
mesmo assim: perde-se **um ponto da trajetória**, e não o avião. Cada registro
leva a idade da informação, então a ground sabe exatamente a que instante cada
ponto se refere (ADR-0007 §6).

**5. Se o contato cai de vez.** Depois de 120 s sem ouvir o solo
(`contact_timeout_ms`), o `ttcd` cai no perfil `SAFE`. Nele, **os retratos
param**, porque não cabem no SAFE (`tm_sched.c`); o satélite manda só beacon e
housekeeping (um beacon a cada 30 s e HK a cada 60 s), à espera de que o solo
o reencontre. O solo faz o mesmo: depois de 150 s sem ouvir o satélite, também
vai para o SAFE. Assim os dois acabam no mesmo perfil sem combinar nada. A
bordo, o `adsbd` continua gravando no SD.

A volta é automática (ADR-0007 §4): quando o solo ouve o satélite no SAFE,
ele mesmo manda de novo o perfil que queria, e o fluxo de retratos recomeça
sozinho, já com o estado atual dos aviões. Na simulação, depois de 300 s sem
enlace nos dois sentidos, os dois voltaram ao NOMINAL 32 s após o fim da
falha. O trecho perdido no rádio só existe no registro de bordo.

| O dado | Se o rádio falha | Onde recuperar |
|---|---|---|
| Cada mensagem ADS-B | nada se perde | `adsb.ndjson` no SD, por USB/SSH depois |
| Retrato (um ponto da trajetória) | perdido no ar, contado pelo `seq` | o próximo retrato traz o estado atual; o ponto exato, só no SD |
| Telecomando | retransmitido até o ACK | — |
| Logs do `ttcd` e do `obcd` | nada se perde | `/var/lib/gama/*.jsonl` no SD |

**Uma lacuna documental.** O ADR-0010, que deveria registrar a decisão de
armazenamento a bordo, ainda não foi escrito (`docs/adr/README.md`). A
política já está implementada no `adsbd` e descrita em `data-budget.md` §7.

---

## Referências

- `docs/budgets/data-budget.md`: o orçamento de dados.
- `tools/analysis/lora_budget.py`: todas as contas; `make budget` imprime as tabelas.
- `docs/adr/0003-processing-split-onboard-vs-ground.md`: por que agregar a bordo.
- `docs/adr/0004-lora-phy-configuration.md`: os perfis e o critério do SF.
- `docs/adr/0007-ota-frame-format-and-arq.md`: o frame, o ARQ e a troca de taxa.
- `docs/adr/0008-adsb-receive-chain-ownership.md`: o SDR, o `dump1090` e o `adsbd`.
- `docs/adr/0009-time-reference-and-synchronisation.md`: o tempo.
- `docs/adr/0015-carrier-frequency-and-flight-power.md`: 433,92 MHz e 14 dBm.
- `docs/requisitos.md`: o HLR-COMM-01, citado literalmente.
