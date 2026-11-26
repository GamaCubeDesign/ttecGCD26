# Roteiro da bancada — a recepção ADS-B, do ganho à missão real

Foco: **o satélite montado ouve aviões de verdade, e quanto disso chega à
ground**. Até hoje a cadeia foi validada com o `sbs_replay` no lugar do SDR
(16/11, 22/11): 20 de 20 aeronaves na ground, erro de posição p95 de 10 a 23 m.
O elo que falta medir é o primeiro: antena + SDR + `dump1090-fa` dentro do
satélite, com a pilha ligada.

Data: a definir. Anotar em `docs/vv/bancada/AAAA-MM-DD/notas.md` (modelo:
[`modelo-notas-2026-11.md`](modelo-notas-2026-11.md)).

## Por que agora — o que já se sabe

| Fato | De onde |
|---|---|
| O ganho de voo é `--gain -10` (AGC); todas as medições de antena foram a 49,6 dB fixo. A varredura nunca foi feita | `flight/adsbd/adsbd.conf.example`; `docs/requisitos.md` §7.2, ação 21 |
| Ruído com a pilha ligada, na sala: −23,7 dBFS. Ao ar livre, sem a pilha: −37 a −42 dBFS | [22/11](2026-11-22/notas.md), [20/11](2026-11-20/notas.md) |
| A antena da equipe entrega ~4–5 dB menos que a COTS, mas ouviu avião a 74 km | [20/11](2026-11-20/notas.md) |
| Container e sala bloqueiam: 0 mensagens lá dentro | [20/11](2026-11-20/notas.md), [16/11](2026-11-16/notas.md) |
| A categoria a bordo (ADR-0023) está pronta, mas `avr_port = 0` no voo; o roteiro de 20/11 não foi rodado | [roteiro de 20/11](2026-11-20/roteiro.md) |
| O rádio A não responde desde 25/11; o enlace pelo SSH funciona, mas não é evidência de RF | [25/11](2026-11-25/notas.md) |

As etapas 1 e 2 não usam o enlace (só o SDR). As etapas 3 e 4 valem para o
ADS-B pelo LoRa **ou** pelo SSH; pelo SSH, a perda e a latência medidas não são
as do rádio.

| Etapa | O que mede | Onde | Tempo | Precisa de |
|---|---|---|---|---|
| 0 | ferramentas e linha de base | PC | ~1 h | — |
| 1 | ganho e ruído do SDR no satélite | **ao ar livre** | ~30 min | Pi, SDR, antena, pilha, internet (OpenSky) |
| 2 | interferência da pilha | ao ar livre | ~20 min | só se a etapa 1 mostrar ruído > 3 dB |
| 3 | categoria a bordo (ADR-0023) | bancada + ar livre | ~30 min | etapa 1 |
| 4 | missão real de 10 min com verdade | ao ar livre | ~40 min | etapas 1 e 3, internet (adsb.lol) |
| 5 | recolher e anotar | PC | — | — |

---

## Etapa 0 — Preparação, no PC, sem hardware

| # | O quê | Como | Aprovado se |
|---|---|---|---|
| 0.1 | **A fazer:** comparar o registro completo de bordo com o downlink real | estender `tools/analysis/downlink_emulation.py` com `--downlinked ARQ` (o NDJSON da sessão, de `ground/host/adsb/tracks_to_ndjson.py`) no lugar da emulação | numa `./gama sim mission`, imprime as duas conclusões lado a lado |
| 0.2 | linha de base com as capturas que já temos | `downlink_emulation.py <captura>/capture.ndjson --compare ground/host --reference-db ground/host/adsb/data/referencias/referencias.db` para `docs/vv/trafego-real/2026-11-17-unsam` e `2026-11-18-unsam` | as tabelas "completo × emulado" anotadas, para comparar com a etapa 4 |
| 0.3 | a missão pelo SSH, se o LoRa continuar fora | `./gama service link ssh`, depois `./gama ground --ssh` e uma missão de 120 s pela página | roster no fim e a missão termina sozinha, como em 25/11 |
| 0.4 | o AGC passa pelo `sdr_check` | `./gama antenna check --gain -10 --secs 10 --no-reference` no PC | o `dump1090` sobe e o resumo sai |
| 0.5 | código igual nos dois lados | `./gama pi sync`, `make pi-test T='avr\|tracks\|adsbd'` | os testes passam; `git describe` anotado |

Não passa à etapa 1 sem 0.4 e 0.5. O 0.1 só é preciso na etapa 4.

---

## Etapa 1 — Ganho e ruído do SDR no satélite montado

**Ao ar livre**, satélite montado como no voo, a antena 1090 da equipe. O
`sdr_check` só abre o SDR da Pi com os serviços parados: antes, `./gama pi stop`
(depois, `./gama service start`). Cada medição:

```bash
./gama antenna check --pi --antenna gama-<pilha>-g<G> --secs 60 --gain <G>
```

| Pilha | Ganhos (dB; −10 = AGC) |
|---|---|
| **desligada** (só a Pi e o SDR alimentados) | −10, 49,6, 40,2, 33,8 |
| **ligada** (OBC, EPS, tudo como no voo) | −10, 49,6, 40,2, 33,8, 28,0 |

O tráfego muda de minuto a minuto, por isso:

- **alternar** os ganhos (ex.: 40,2 → −10 → 49,6 → 33,8 → 28,0), não varrer em ordem;
- **repetir duas vezes** o melhor ganho de cada configuração;
- para as aeronaves comuns a duas medições, comparar o RSSI corrigido pela
  distância, `RSSI + 20·log10(d)`, pela mediana (o método de 20/11).

**Anotar** (de `./gama antenna compare docs/vv/bancada/<data>/antena`): ruído
(dBFS), mensagens com CRC certo, aeronaves com posição, alcance, o que a OpenSky
via a até 100 km.

**Aprovado se:** com a pilha ligada, algum ganho dá aeronaves com posição. Esse
é o ganho de voo; entra no `dump1090_cmd` do `/etc/gama/adsbd.conf` (e, depois
de combinar com a equipe, no `adsbd.conf.example`).

**Se nenhum ganho der aeronaves com a pilha ligada**, mas sem ela sim: etapa 2.
Se nem sem a pilha: o problema é a antena ou o lugar; refazer com a COTS para
separar os dois.

---

## Etapa 2 — Interferência da pilha (condicional)

Só se o ruído com a pilha ligada ficar **mais de 3 dB** acima do da pilha
desligada no mesmo ganho. No melhor ganho da etapa 1, **uma mudança por vez**,
60 s cada:

1. o SDR numa extensão USB curta, afastado da Pi;
2. ferrite no cabo USB, perto do SDR;
3. o cabo da antena longe dos reguladores da EPS.

**Aprovado se:** o ruído volta a até 3 dB do da pilha desligada, ou as
mensagens com CRC voltam ao nível dela. Anotar qual mudança resolveu: ela vira
item de integração mecânica.

---

## Etapa 3 — A categoria a bordo (ADR-0023)

Os três testes do [roteiro de 20/11](2026-11-20/roteiro.md), com os comandos
de hoje.

1. **Replay com AVR** (decide a etapa):
   `./gama replay mission --input docs/vv/trafego-real/2026-11-18-unsam/capture.ndjson --avr --secs 120`.
   Aprovado se `avr_rejected = 0`, `avr_idents > 0` e a página mostra
   categorias recebidas (sem asterisco).
2. **`dump1090-fa` real com AVR**, ao ar livre, no ganho da etapa 1:
   `./gama mission --avr --secs 120`. Aprovado se ao menos uma aeronave real
   chega com categoria. Sem tráfego o teste não vale, e isso não reprova.
3. **CPU**, durante o 1 ou o 2: `top -b -n 30 -d 2 | grep -E 'dump1090|adsbd'`
   com e sem `--avr`. Aprovado se o `adsbd` fica abaixo de 5% de um núcleo e o
   `dump1090-fa` não sobe mais de 2 pontos sobre os 21,9% de 09/10.

**Se o 1 passar:** `avr_port = 30002` e `--net-ro-port 30002` como padrão de
voo (a combinar com a equipe); o ADR-0023 passa a *Accepted* com os números.
**Se não:** nada a desfazer; voo com `avr_port = 0` e a inferência do ADR-0022.

---

## Etapa 4 — Missão real de 10 min, com verdade

Ao ar livre, no ganho da etapa 1, com `--avr` se a etapa 3 passou. **Ao mesmo
tempo**, em dois terminais:

```bash
# o satélite e a página (pelo LoRa; se ele estiver fora, ./gama ground --ssh e a missão pela página)
./gama mission --secs 600 [--avr]

# a verdade: o tráfego real em volta, com a categoria e as rotas (precisa de internet)
python3 tools/live_traffic/live_traffic.py capture --out docs/vv/trafego-real/<data>-fga \
    --center -15.99,-48.04 --duration 900
```

A captura começa antes da missão e termina depois (900 s contra 600 s), para
cobrir a janela inteira. O `adsb.ndjson` completo de bordo vem com os logs da
missão (`replay-`/`missao-*` em `docs/vv/bancada/<data>/`).

Três comparações, sobre os mesmos voos:

| Comparação | O que mede | Como |
|---|---|---|
| verdade × registro completo de bordo | o que a antena e o SDR perdem: aeronaves vistas, mensagens por aeronave | ICAOs da captura × ICAOs do `adsb.ndjson`, na janela da missão |
| registro completo × downlink | quanto o enlace custa à estimativa | o script do item 0.1 |
| verdade × downlink | o resultado da missão | `tools/live_traffic/avaliar.sh` com o downlink da sessão |

**Anotar:** aeronaves (verdade / bordo / ground), frames perdidos, latência
p95, erro de posição p95, origens e destinos certos em cada caminho, e se foi
pelo LoRa ou pelo SSH.

**Aprovado se:** há aeronaves reais com posição na ground durante os 10 min
(HLR-ADS-01, 02, 05), e as três tabelas estão completas. Não há limiar de
acerto do estimador: o número é o resultado, não o critério.

---

## Etapa 5 — Ao terminar

- `notas.md` do dia, com as tabelas das etapas 1 a 4.
- O ganho escolhido no `adsbd.conf`; o ADR-0023 *Accepted*, se for o caso.
- O progresso no [README](README.md) desta pasta.
- `docs/requisitos.md`: HLR-ADS-01, 02, 05 e 08 com estes números.
- Um relatório para a equipe em `docs/relatorios/` (a partir do `modelo.html`,
  e no `index.html`).
