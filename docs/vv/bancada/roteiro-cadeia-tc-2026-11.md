# Roteiro da bancada — a cadeia de telecomando inteira, novembro de 2026

Foco: **ground (página) → ESP32 → LoRa → `ttcd` → IPC → `obcd` → modo →
`adsbd` → tracks → LoRa → ground → trajetória**, a missão como no CONOPS:
`PRE_TEST` → `MISSION_ADSB` (10 min) → `MISSION_DOWNLINK` → `PRE_TEST`, começada
por um clique.

O que já foi feito e o que este roteiro acrescenta:

| Data | O quê | Falta |
|---|---|---|
| 16/11 | a mesma cadeia com `sbs_replay`, à mão; 0 de 359 quadros perdidos, trajetória p95 23 m (`2026-11-16/notas.md`) | rádio A nos pinos da placa; ACK perdido em hardware; `./gama` |
| 19/11, 20/11 | roteiros com tráfego real, não executados | — |

## O que o ACK cobre, e o que não cobre

Ler antes de interpretar os números (ADR-0007 §3 e §6):

- **Só telecomando tem ACK.** Cada TC recebe exatamente um `TC_ACK` com status;
  a ground é *stop-and-wait*, retransmite com o mesmo `seq` (timeout 328 ms em
  NOMINAL, backoff 250 ms dobrando até 2 s, desiste em 60 s). O satélite executa
  no máximo uma vez: uma repetição é respondida reenviando o ACK. Exceções:
  `SET_RATE` é sempre executado, e `SET_TIME` é recarimbado a cada tentativa.
- **Track não tem ACK nem retransmissão.** Um quadro de TM perdido some; a perda se
  mede pelas lacunas de `seq` (HLR-ADS-08).
- **A trajetória não depende do ACK nem da hora.** A ground calcula o instante de
  cada ponto como `chegada − tempo no ar − 100 ms × age_ds`, no relógio dela.
  O ACK entra antes: sem `SET_TIME` OK o `ttcd` recusa a missão (decisão de
  16/11), e sem `SET_MODE` OK o `obcd` não entra em `MISSION_ADSB` e não há tracks.

Então "o ACK funciona para a trajetória" vira três perguntas, uma por parte:
**(A)** os TCs da missão chegam, com um ACK cada e sem execução dupla, mesmo com
retransmissão; **(B)** o modo muda como no CONOPS; **(C)** a trajetória reconstruída
bate com a verdade.

| Etapa | O que prova | Precisa de |
|---|---|---|
| 0 | rede, Pi, ESP32, configuração | PC, Pi, ESP32 |
| 1 | enlace sozinho, rádio A nos pinos da placa | etapa 0 |
| 2 | ACK perdido em hardware (`SET_RATE`) | etapa 1 |
| 3 | a missão de 2 min, simulada com verdade conhecida | etapa 1 |
| 4 | a missão de 10 min, como no CONOPS | etapa 3 |
| 5 | ACK sob retransmissão: TC sem execução dupla | etapa 3 |
| 6 | análise: ACKs, modos, perda, trajetória | etapas 3–5 |
| 7 | recolher e anotar | — |

Anotar em `docs/vv/bancada/AAAA-MM-DD/notas.md` (modelo:
[`modelo-notas-2026-11.md`](modelo-notas-2026-11.md)).

---

## Etapa 0 — Preparação

```bash
./gama net up                       # com o cabo ethernet no PC
./gama doctor                       # PC, rede, Pi, relógio, /etc/gama contra a placa, SDR, rádio A
./gama pi sync
make pi-test                        # na Pi; as falhas de numpy são da ground, não do voo
./gama board check                  # /etc/gama igual a flight/conf/obc-v2? (install, se não)
./gama pi stop                      # nada segurando o rádio, o SDR ou o I²C
./gama esp32 flash                  # só se o firmware da ground mudou desde 16/11
```

**Aprovado:** `doctor` sem falha; a ESP32 imprime `boot` com `"radio":"ok"`.

## Etapa 1 — Enlace sozinho, nos pinos da placa

Fecha o teste 3 de 19/11. Rádio A com RESET no GPIO25 (pino 22) e DIO0 no GPIO4
(pino 7); `/etc/gama/ttcd.conf` igual ao `flight/conf/obc-v2/ttcd.conf`.

```bash
./gama radio check                  # RegVersion 0x12 pelo SPI
./gama link ping                    # 10 PINGs pela ESP32
```

**Anotar:** pings com ACK, tentativas, latência, RSSI, SNR, distância.

**Aprovado:** 10/10 **na 1ª tentativa**, ~276 ms. Com 2 tentativas e ~1 000 ms, o
DIO0 está no pino errado ou mal ligado (achado 2 de 16/11): corrigir antes de seguir,
porque isso também faz a ground perder o TC seguinte na missão.

## Etapa 2 — ACK perdido, em hardware

```bash
./gama link lost-ack                # a ground fica surda 3 s depois do SET_RATE
```

**Aprovado:** o satélite volta ao perfil antigo após 20 s, os dois acabam no novo
perfil (em simulação, 23,4 s; em bancada, em 09/10, 23,2 s com 12 tentativas).

## Etapa 3 — Missão de 2 min, simulada, com verdade conhecida

A simulação de 16/11 (20 aeronaves em torno de SBBR) dá a posição exata de cada
avião em cada instante, que o tráfego real não dá. Encurtada para 2 min, testa a
cadeia inteira em pouco tempo.

```bash
python3 docs/vv/bancada/2026-11-16/analise/gerar_sim20.py /tmp/sim20.ndjson
./gama mission --secs 120 --input /tmp/sim20.ndjson
# abrir http://127.0.0.1:8080
```

Na página, nesta ordem:

1. `ping`, `hk`: `obc_mode` `PRE_TEST`; `flags` com `TIME_SYNCED` (bit 2) já 1, porque
   a página manda `SET_TIME` sozinha no primeiro contato (achado 5 de 16/11).
2. **Iniciar missão ADS-B**: ACK `OK` do `SET_TIME`, depois do `SET_MODE`.
3. `hk`: `MISSION_ADSB` e `DUMP1090_UP` (bit 3).
4. Tracks no mapa; aos 120 s o OBC passa sozinho a `MISSION_DOWNLINK`, chegam o
   roster e as estatísticas finais, e ele volta a `PRE_TEST`.
5. Ctrl-C no terminal do `./gama mission`: ele para tudo e traz os logs.

**Anotar:** do clique ao ACK do `SET_TIME`, ao do `SET_MODE` e ao primeiro track;
as tentativas de cada um; a pasta da Pi (`docs/vv/bancada/<data>/missao-<hora>/`) e a
sessão da ground (`ground/host/sessions/mission-*/`), que o `./gama` imprime no fim.

**Aprovado:** um clique começa a missão, ela termina sozinha, roster e estatísticas
chegam. Referência de 16/11: `SET_TIME` 318 ms, `SET_MODE` 297 ms, ambos na 1ª
tentativa, primeiro track 5,4 s depois.

## Etapa 4 — Missão de 10 min, como no CONOPS

O mesmo, com a duração do voo (`adsb_timeout` = 600 000 ms):

```bash
./gama mission --secs 600 --input /tmp/sim20.ndjson
```

Com o SDR de verdade, só **ao ar livre** (o container bloqueia, 20/11), e depois de
`./gama antenna check --pi` aprovar: `./gama mission --secs 600 --sdr`. Nesse caso
não há verdade para a parte (C) da etapa 6; vale (A) e (B).

**Aprovado:** como a etapa 3, e a perda e a latência p95 não piores que 16/11
(0 de 359; 2,8 s; HLR-ADS-08 pede 5 s).

## Etapa 5 — ACK sob retransmissão

As etapas 3 e 4 provavelmente terão tudo na 1ª tentativa, o que não exercita a
retransmissão dos TCs da missão. Para provocá-la, repetir a etapa 3 com o enlace
pior: afastar os rádios, tirar a antena da ESP32 (**nunca** transmitir sem antena no
rádio A; na ground, só com potência baixa) ou pôr um obstáculo, até o `ping`
mostrar tentativas > 1 em parte dos pings. Então clicar **Iniciar missão ADS-B**
algumas vezes ao longo de alguns minutos (cada clique gera `SET_TIME` + `SET_MODE`).

**Aprovado:** para cada `SET_MODE` com ACK `OK` na ground, **exatamente uma**
transição no `obcd`, mesmo quando `attempts` > 1 (o satélite respondeu à repetição
reenviando o ACK). Nenhum TC sem ACK além dos abandonados aos 60 s, e estes com evento.

## Etapa 6 — Análise

`S` = a sessão da ground, `P` = a pasta da Pi trazida pelo `./gama`:

```bash
S=$(ls -td ground/host/sessions/mission-* | head -1)
P=$(ls -td docs/vv/bancada/*/missao-* | head -1)
```

**(A) Os ACKs**, na ground e no satélite:

```bash
python3 - "$S/ground.jsonl" <<'EOF'
import json, sys, collections
acks = [e for e in map(json.loads, open(sys.argv[1])) if e.get('ev') == 'ack']
by = collections.defaultdict(list)
for a in acks: by[(a['cmd'], a['status'])].append(a)
for (cmd, st), v in sorted(by.items()):
    lat = sorted(x['latency_ms'] for x in v)
    print(f"{cmd:10} {st:8} n={len(v):3}  latência p50 {lat[len(lat)//2]} ms, máx {lat[-1]} ms, "
          f"com retransmissão {sum(x['attempts'] > 1 for x in v)}")
EOF
grep '"event":"tc"' $P/ttcd.jsonl | grep -o '"cmd":"[A-Z_]*","seq":[0-9]*,"status":"[A-Z_]*"' | sort | uniq -c
```

Em 16/11 isso deu `SET_TIME` 2 × 318 ms, `SET_MODE` 1 × 297 ms, todos na 1ª
tentativa. Os `PING` saem com p50 de ~10 s porque quase todos são keepalives
(achado 3 de 16/11, ainda sem explicação); os do operador levam ~280 ms.

Conferir: cada `SET_MODE`/`SET_TIME` da ground aparece no `ttcd.jsonl` com o mesmo
status. Um `seq` repetido no `ttcd` com o mesmo comando é retransmissão; tem de ser
o ACK reenviado, não uma segunda execução (B).

**(B) Os modos:**

```bash
grep -h '"event":"obc_mode"' $P/ttcd.jsonl
grep -h '"event":"mode"' $P/obcd.jsonl     # from, to, o evento e quem pediu (by)
```

Conferir a sequência `PRE_TEST` → `MISSION_ADSB` → `MISSION_DOWNLINK` → `PRE_TEST`,
o `MISSION_DOWNLINK` `--secs` depois da entrada, e uma entrada em `MISSION_ADSB` por
`SET_MODE` `OK`.

**(C) A trajetória**, contra a verdade da simulação (só etapas 3–5):

```bash
python3 docs/vv/bancada/2026-11-16/analise/trajetoria.py /tmp/sim20.ndjson $P/adsb.ndjson $S/ground.jsonl
```

**Aprovado:** erro de posição p95 da ordem de 16/11 (23 m), erro de instante p95
≤ 0,2 s, intervalo entre pontos do mesmo avião ~5 s sem buracos além das lacunas de
`seq`. Um erro de instante grande e constante aponta o `age_ds` ou o tempo no ar; um
erro que cresce com o tempo, o relógio de uma das pontas.

**Perda e latência** (HLR-ADS-08), das estatísticas finais:

```bash
grep '"ev": *"stat"' $S/ground.jsonl | tail -1
python3 - "$S/ground.jsonl" <<'EOF'
import json, sys
seqs = sorted({e['seq'] for e in map(json.loads, open(sys.argv[1])) if 'seq' in e})
gaps = sum(b - a - 1 for a, b in zip(seqs, seqs[1:]) if b > a + 1)
print(f"quadros de TM: {len(seqs)} recebidos, {gaps} perdidos (lacunas de seq)")
EOF
```

Conferido com os logs de 16/11: 608 quadros de TM, 0 perdidos. O `seq` conta HK,
tracks, estatísticas, roster e também os `TC_ACK`; sem eles, aparecem 77 lacunas falsas.

## Etapa 7 — Recolher

```bash
cp $S/ground.jsonl $P/                  # a ground junto dos logs da Pi
./gama pi stop
./gama net down
```

Escrever `notas.md` do dia, atualizar a tabela de progresso do [README](README.md)
e a seção "Validated on the bench" do `AGENTS.md`.
