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
