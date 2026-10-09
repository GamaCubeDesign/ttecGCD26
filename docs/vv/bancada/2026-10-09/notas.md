# Bancada — 09/10/2026

Continuação da etapa 4. Dia anterior: [07/10](../2026-10-07/notas.md).

## Montagem

- Os dois RA-02 passaram da protoboard para uma **placa perfurada**: com a
  protoboard, nenhum dos chips respondia no SPI (07/10).
- Rádios a 80 cm, 2 dBm.
- Rede: `bancada-ap` (o ponto de acesso do PC) subida às 10:05; a Pi entrou
  sozinha, 10.42.0.75, sinal −20 dBm.

## Os dois chips respondem — 10:08

Mesma leitura de 07/10 (RESET pulsado, `spidev` em Python):

| | CE0 (rádio A) | CE1 (rádio B) |
|---|---|---|
| RegVersion (0x42) | `0x12` ✅ | `0x12` ✅ |
| RegOpMode (0x01) | `0x09` (FSK, espera — o padrão do reset) | `0x09` |
| RegFrf (0x06–0x08) | `0x6C8000` (434 MHz, padrão) | `0x6C8000` |

Com e sem pull-up no MISO, o mesmo resultado: agora o chip aciona a linha.

## Passo 1, primeira tentativa — 10:10, ❌ o LBT nunca libera

`make pi-bench-ping`: o `ttcd` e a `gs_cli` sobem, a ground manda `ping 10`,
e nada vai ao ar. O log do `ttcd` (`101016-ping-ttcd.jsonl`) tem 1727
`lbt_defer` em 2 min, desde o primeiro milissegundo, nenhum `tx` e nenhum
`rx`; a ground perde o contato aos 150 s. Interrompido à mão.

Diagnóstico: cada rádio em LoRa, recepção contínua, **sem nada no ar**,
RegModemStat (0x18) lido 20 vezes:

| | Em espera | Recebendo, canal vazio | RSSI |
|---|---|---|---|
| CE0 | `0x10` (modem clear) | **`0x04`** nas 20 leituras | 65 (≈ −99 dBm) |
| CE1 | `0x10` | **`0x04`** nas 20 leituras | 63 (≈ −101 dBm) |

O bit 2, *RX on-going*, fica ligado o tempo todo em recepção contínua. O
driver o contava como canal ocupado (`flight/radio/sx1278.c`,
`sx1278_rx_busy`), então o LBT adiava toda transmissão, dos dois lados. O
teste do driver modelava o canal livre como `0x00` e não tinha como pegar
isso — medir o RegModemStat era um pendente de bancada do ADR-0007.

**Correção:** o LBT deixa de olhar o *RX on-going*; o canal está ocupado com
*signal synced* ou *header valid* (e, no modo `preamble`, também *signal
detected*). O `test_sx1278` passa a ter o `0x04` medido como canal livre.
`make check` no PC: 23/23 nos dois builds, 71/71.

## Passo 1 — 10:20, ✅ 10 de 10

`make pi-bench-ping` (`102003-ping-*.jsonl`):

| | |
|---|---|
| PINGs | 10 enviados, **10 confirmados**, 0 falhas; 11 tentativas |
| Latência | mín. 275 ms, média 345 ms, máx. 977 ms (o primeiro, com uma retransmissão) |
| Sinal na ground | RSSI −27 a −29 dBm (média −27,9), SNR 9 a 10 dB (média 9,4) |
| Downlink | 10 frames, 0 perdidos, 0 corrompidos |
| LBT | 0 adiamentos |

O primeiro enlace LoRa real entre os dois RA-02.

## Passos 1 a 6 — `make -k bench`, 10:20 a 10:55, ✅ todos

Na Pi, no tmux `bench`; saída em `bench.txt`, logs `102038-<passo>-*.jsonl`.
Rádios a 80 cm, 2 dBm, LBT por cabeçalho (exceto o passo 6, que compara).

### Passo 1 · ping — ✅

10 de 10 de novo, 11 tentativas; RSSI médio −28,3 dBm, SNR 9,3 dB.

### Passo 2 · tempo no ar — ✅ pior erro 1,9% (tolerância 5%)

`102038-toa.txt` (`toa_from_log.py`), 141 transmissões:

| Perfil | Bytes | n | Modelo (ms) | Medido (ms) | Erro |
|---|---|---|---|---|---|
| FAST | 11 | 54 | 41,2 | 42,0 | +1,9% |
| FAST | 32 / 33 | 1 / 2 | 71,9 | 73,0 | +1,5% |
| NOMINAL | 11 | 53 | 144,4 | 145,0 | +0,4% |
| NOMINAL | 32 / 33 | 2 / 2 | 246,8 | 247,5 | +0,3% |
| SAFE | 11 | 24 | 1450,0 | 1451,0 | +0,1% |
| SAFE | 32 / 33 | 1 / 2 | 2498,6 | 2499,0 | +0,0% |

O medido inclui a escrita SPI e a latência da interrupção do DIO0 (~1 ms),
por isso o erro relativo é maior nos frames curtos em FAST. O modelo de
`common/gama_lora.c` vale nos três perfis — inclusive o bit LDRO em SAFE,
que o driver antigo errava.

### Passo 3 · taxa de erro — ✅ nenhuma perda

| Perfil | PINGs | Confirmados | Tentativas | Latência (mín/média/máx, ms) |
|---|---|---|---|---|
| NOMINAL | 1000 | 1000 | 1000 | 275 / 276 / 281 |
| FAST | 1000 | 1000 | 1000 | 80 / 81 / 82 |
| SAFE | 200 | 200 | 200 | 2695 / 2695 / 2696 |

Downlink: 2220 frames, 0 perdidos, 0 corrompidos. Uplink: 2211 TCs
recebidos, nenhuma retransmissão. **PER medido 0** nos dois sentidos; com 0
falhas em N, o limite superior com 95% de confiança é ≈ 3/N: < 0,3% em
NOMINAL e FAST, < 1,5% em SAFE, por sentido. Vale para 80 cm, sinal
forte (RSSI −26 a −34 dBm) — não é o enlace da missão.

O satélite adiou 1174 transmissões pelo LBT (1152 `TM_HK`, 22 `BEACON`):
era a ground transmitindo. Nenhuma colisão chegou a custar um PING — **o LBT
corrigido detecta tráfego real**.

### Passo 4 · troca de taxa — ✅ 7 de 7

Sete `SET_RATE` entre todos os pares de perfis, todos `OK` na primeira
tentativa, 7 `rate_commit` no satélite, e 3 PINGs no perfil final. A troca
mais lenta é a que sai de SAFE (5,44 s: o ACK ainda vai em SAFE).

### Passo 5 · ACK perdido — ✅ recuperou sozinho

A ground fica surda 3 s (`deafen 3`) e perde o ACK do `SET_RATE` FAST:

| Evento (satélite) | Instante |
|---|---|
| `SET_RATE` recebido, troca para FAST | 0 |
| sem ouvir a ground em FAST, volta para NOMINAL (`revert`) | **20,000 s** |
| ouve a retransmissão em NOMINAL, troca para FAST de novo, `rate_commit` | 22,2 s |

Na ground: ACK com **12 tentativas e 23,2 s** de latência (o roteiro
esperava ~21 s), depois 3 PINGs em FAST. Os dois terminam em FAST.

### Passo 6 · LBT, cabeçalho × preâmbulo — ✅ sem diferença

400 PINGs com HK a cada 1 s (canal ocupado de propósito):

| Detecção | Confirmados | Tentativas | Retransmissões | Adiamentos no satélite |
|---|---|---|---|---|
| cabeçalho | 400 | 400 | 0% | 400 (`TM_HK`) |
| preâmbulo | 400 | 400 | 0% | 400 (`TM_HK`) |

A simulação previa ~12% de retransmissões com cabeçalho e ~2% com
preâmbulo; no hardware, os dois dão 0%, com um adiamento por PING. **Fica o
padrão `header`** — o preâmbulo não trouxe ganho medível. (O primeiro
`REQ_STAT` de cada rodada levou 2 tentativas: o satélite acabara de subir.)

## Resumo da etapa 4

Os seis passos passaram. O enlace real confirma o modelo de tempo no ar
(≤ 1,9%), não perdeu nenhum frame em ~2,2 mil de cada lado, troca de taxa e
se recupera do ACK perdido como o protocolo prevê, e o LBT — depois da
correção do RX on-going — detecta o tráfego e evita as colisões. Evidência
para HLR-COMM-01 (resposta a telecomando: 81 ms em FAST, 276 ms em NOMINAL,
2,7 s em SAFE) e HLR-COMM-03, e para a revisão dos ADRs 0007 e 0012.

Limite: tudo a 80 cm e 2 dBm, com RSSI ≈ −28 dBm, muito acima da sensibilidade. A taxa
de erro do enlace real da missão (distância, antenas da equipe) ainda não
foi medida.

## Etapa 5 — o SDR e o dump1090

NESDR Nano 3 na porta micro-USB de dados, pelo adaptador OTG, com a
antena de 1090 MHz; Pi dentro do prédio.

### 5.1 Instalar — 10:55, ✅

- Repositório da FlightAware (`flightaware-apt-repository_1.3`): gera
  `Suites: trixie`, arm64 — há pacote para o Debian 13.
- **`dump1090-fa` 11.1** instalado (43 pacotes novos, entre eles SoapySDR e
  o `lighttpd`, que vêm como recomendados).
- Serviço `dump1090-fa` **desabilitado e parado**: quem sobe o dump1090 é o
  `adsbd` (ADR-0008).
- O kernel pegou o SDR ao conectar (`dvb_usb_rtl28xxu`, `rtl2832_sdr`):
  `blacklist dvb_usb_rtl28xxu` em `/etc/modprobe.d/rtl-sdr.conf` e módulos
  descarregados.
- `lighttpd` (servidor web na porta 80, para o mapa do dump1090)
  **desabilitado e parado**, a pedido: sem uso no satélite.

Esses três ajustes (serviço, blacklist, lighttpd) têm de se repetir no
cartão de voo — hoje são feitos à mão.

### 5.2 O SDR aparece? — 10:59, ✅

`lsusb`: `0bda:2838 Realtek RTL2838 DVB-T`. O dump1090-fa abre o
"Generic RTL2832U OEM", sintonizador **Rafael Micro R820T**, 2,4 MS/s
(24 milhões de amostras em 10 s), **0 amostras perdidas**.

### 5.3 Varredura de ganho — 10:59 a 11:13, ⚠️ inconclusiva

`ganho.txt`. Por minuto (dois minutos por ganho):

| Ganho | Ruído (dBFS) | Aceitas/min | Aeronaves | > −3 dBFS | CPU (dump1090) |
|---|---|---|---|---|---|
| −10 (AGC → 58,6 dB) | −14,1 / −14,4 | 27 / 36 | 3 / 0 | 0 | 21,0 / 20,6% |
| 49,6 | −24,9 / −25,4 | 16 / 5 | 2 / 0 | 0 | 20,6 / 21,3% |
| 42,1 | −30,3 / −30,7 | 14 / 21 | 2 / 1 | 0 | 20,1 / 20,4% |
| 36,4 | −35,6 / −35,6 | 35 / 15 | 2 / 0 | 0 | 19,0 / 19,5% |
| 28,0 | −38,5 / −38,8 | 3 / 5 | 1 / 1 | 0 | 18,4 / 18,7% |
| 20,7 | −43,3 / −43,4 | 55 / 3 | 1 / 0 | 0 | 16,6 / 17,5% |

- **O tráfego decide, não o ganho:** 0 a 3 aeronaves ao alcance, e a contagem
  varia mais entre dois minutos do mesmo ganho (55 → 3 em 20,7 dB) do que
  entre ganhos. Não dá para escolher o ganho com esta medida.
- Nenhuma mensagem acima de −3 dBFS em nenhum ganho: nada perto de saturar.
- O AGC (`--gain -10`, o padrão do `adsbd.conf.example`) leva o R820T ao
  máximo, 58,6 dB, com o ruído em −14 dBFS e o sinal médio só 3 a 6 dB acima
  — o pior dos seis pontos.
- **O ganho fica em `-10` por enquanto.** Repetir com tráfego (janela, área
  aberta) ou com uma fonte conhecida; a prova terá o SDR perto de uma fonte
  forte, onde o AGC no máximo é arriscado.

### 5.4 CPU e temperatura — 11:13, ✅ folga grande

`cpu.txt`, `--gain -10`, `pidstat` 6 × 10 s depois de 20 s de partida:

| | |
|---|---|
| `dump1090-fa` | **21,9% de um núcleo** (20,6% usuário, 1,3% sistema), estável de 21,7 a 22,1% |
| Sistema todo (`mpstat`) | 5,0% dos 4 núcleos — 94,6% ocioso |
| Temperatura | 46,2 °C antes, 45,6 °C depois |
| `get_throttled` | `0x0` |
| Memória | 283 MB disponíveis de 415 |

O maior risco não quantificado do projeto (PLANO 3.4) fica medido: o
dump1090-fa a 2,4 MS/s usa ~22% de um núcleo, abaixo da folga de 50% do
roteiro — não é preciso baixar a taxa de amostragem. Ressalva: com 0 a 3
aeronaves; o custo da demodulação é dominado pela taxa de amostras, mas o de
decodificação cresce com o tráfego, e falta medir com ~20 aeronaves.

## Etapa 6 — o `adsbd` com o SDR, 11:20 a 11:25, ✅

O comando do roteiro, por 5 min (`timeout -s INT 300`, o equivalente a um
Ctrl+C), sem `ttcd`. Log `adsbd.jsonl`, registro de bordo `adsb.ndjson`.
Binário do build das 10:20 (versão `19b324b-dirty`: a árvore com a correção
do LBT ainda não commitada; o `adsbd` não mudou desde então).

| Evento | Instante | |
|---|---|---|
| `start`, `dump1090_start` (pid 3048) | 0 | ✅ o `adsbd` sobe o dump1090-fa real |
| `sbs_connect_failed` (recusada) | 0 | esperado: o dump1090 ainda subindo |
| `sbs_up` 127.0.0.1:30003 | **1,0 s** | ✅ reconexão no `retry_ms` |
| `ipc_connect_failed` | 0 | esperado: sem `ttcd` nesta etapa |
| `stats` a cada 60 s | 60–240 s | 0, 0, 0, depois 9 linhas e 2 aeronaves |
| `stop` (sinal 2) | 300,0 s | ✅ |
| `sbs_down` "closed by dump1090", `dump1090_exit` status 0 | +68 ms | ✅ o dump1090 termina junto |
| `exit` code 0, `record_dropped` 0 | +69 ms | ✅ parada limpa |

Contadores finais: **24 linhas SBS, 24 decodificadas**, 0 rejeitadas, 0
não-ICAO, **2 aeronaves**, 0 reinícios do dump1090. Registro de bordo: 24
linhas, 2700 B (112,5 B por linha — mais curtas que os 160,4 B do
data-budget porque estas mensagens trazem poucos campos). Exemplo:

```
{"icao":"E49C08","rx_epoch_ns":1791555942524267085,"transmission_type":3,"callsign":"","altitude_ft":33975,"on_ground":-1}
```

`E49C08` é uma matrícula brasileira (bloco `E4`), a 33 975 ft — um avião de
verdade sobre o prédio. O carimbo `rx_epoch_ns` vem do relógio do sistema
(acertado pela rede *shared*), porque não houve `SET_TIME`
(`time_synced: false`, como previsto pelo ADR-0009). `snapshots` ficou em 0:
sem `ttcd`, não há para quem mandar o retrato.

A supervisão do dump1090 (ADR-0008), até aqui testada só com um substituto,
funciona com o programa real: sobe, conecta, lê, e encerra junto.
