# Tráfego aéreo real, com gabarito

Capturas de tráfego real para testar a ground sem a Pi e sem antena: o
estimador de origem e destino, a cadeia no PC (`make adsb-chain`) e a página
(`make sim`). Uma pasta por captura, `AAAA-MM-DD-<local>/`.

## Como foi feita

`tools/live_traffic/live_traffic.py capture` consulta a adsb.lol a cada 5 s (a
cadência do enlace) num raio de 150 NM e, quando cada aeronave sai da área,
pede à OpenSky a trilha dela desde a decolagem. Por que essas duas fontes
(medido em 17/11):

| Fonte | Brasília, 150 NM | Buenos Aires, 150 NM | Categoria |
|---|---|---|---|
| adsb.lol `/v2/point` | 0 a 3 aviões | 18 a 20 aviões, todos por ADS-B | sim: A1, A3, A7… |
| OpenSky `/states/all` | 6 a 7 aviões | 20 aviões | quase sempre 0 (sem dado) |
| OpenSky `/tracks/all` | — | a trilha desde a decolagem | — |

O centro padrão é a UNSAM (San Martín), o local da competição.

O que vai para o `capture.ndjson` é só o que o nosso receptor veria:

- posições decodificadas de ADS-B (não MLAT nem TIS-B);
- posições com até 10 s de idade, como o `adsbd`.

O `capture_sem_categoria.ndjson` tira também a categoria, que o enlace não leva
(o SBS do `dump1090-fa` não a tem). É a entrada que a ground de fato recebe.

O gabarito (`ground_truth.json`, no formato do
`ground-aeronaves/validation/avaliar_estimativas.py`) só afirma origem ou
destino quando os dados mostram:

- **Origem:** a aeronave no solo, ou baixa e subindo, num aeroporto que a
  categoria dela pode usar.
- **Destino:** a aeronave no solo depois de voar, ou baixa depois de descer
  2 000 ft.

O resto fica `null`, que o avaliador conta como "UNKNOWN esperado". Fontes:
adsb.lol (ODbL 1.0) e OpenSky Network (pesquisa não comercial).

## Reproduzir

```bash
python3 tools/live_traffic/live_traffic.py capture --out docs/vv/trafego-real/AAAA-MM-DD-unsam --duration 3600
tools/live_traffic/avaliar.sh docs/vv/trafego-real/AAAA-MM-DD-unsam            # só o método local
tools/live_traffic/avaliar.sh docs/vv/trafego-real/AAAA-MM-DD-unsam --pairs-db ground/host/pares/pares_aeroportos.db
make sim INPUT=docs/vv/trafego-real/AAAA-MM-DD-unsam/capture_sem_categoria.ndjson
```

O banco de pares é gerado localmente em ~1,5 min (`gerar_pares.py` 30 s,
`prioridade_regional.py` 53 s, 599 MB, medido em 17/11); veja
`ground/host/README.md`.

## 2026-11-17 · Buenos Aires (UNSAM), 1 h

`2026-11-17-unsam/`: das 14h56 às 15h56 UTC, 150 NM em volta da UNSAM.

- **Coleta:** 692 consultas, 47 aeronaves, 6 391 registros, 36 trilhas da
  OpenSky. 204 consultas (30%) voltaram **HTTP 429**: a adsb.lol limita a
  taxa. O intervalo entre respostas boas ficou em p50 5 s, p95 10 s, com um
  buraco de 265 s. O `capture` agora espera em dobro a cada 429 e volta ao
  período na primeira resposta boa.
- **Gabarito:** 42 voos, 27 com origem e 10 com destino.
  - Origens: Aeroparque 9, Ezeiza 5, Guarulhos 4, San Fernando 3, Morón 2,
    Montevidéu 2, El Palomar 1, Galeão 1.
  - Destinos: Aeroparque 7, Montevidéu 3.
  - Categorias: A3 31, A1 7, A5 2, A2 1, A0 1.

O estimador (`tools/live_traffic/avaliar.sh`): "decidiu" é o número de casos
com gabarito em que ele apontou um aeroporto, e a porcentagem é o acerto
entre eles.

| Método | Categoria | Origem (27 casos) | Destino (10 casos) |
|---|---|---|---|
| local | com | decidiu 6, acertou 5 (83%) | decidiu 10, acertou 9 (90%) |
| local | **sem** (o real) | **decidiu 0** | decidiu 10, acertou 9 (90%) |
| combinada, sem pares | com | decidiu 3, acertou 3 | decidiu 2, acertou 2 |
| combinada, sem pares | sem | decidiu 0 | decidiu 0 |
| melhor par (ADR-0019) | com | decidiu 12, acertou 3 (25%) | decidiu 3, acertou 0 |
| melhor par (ADR-0019) | sem | decidiu 12, acertou 2 (17%) | decidiu 3, acertou 0 |
| combinada, com pares | com ou sem | decidiu 0 | decidiu 0 |

Além disso, o método local e o melhor par apontaram destinos onde o gabarito
não tem nenhum (5 e 12). São voos cujo pouso a captura não viu, então não
necessariamente erros.

O que isso diz, com a ressalva de que é 1 h e 42 voos:

1. **O destino sobrevive à falta da categoria; a origem, não.** Os pousos vistos
   bastam ao método local; a decolagem, não.
2. **O método de pares erra a maioria com tráfego real**, e a discordância dele
   com o método local faz a **conclusão combinada se abster sempre**. Sem pares,
   ela decidia pouco, mas acertava tudo o que decidia.
3. O avaliador do estimador (`validation/avaliar_estimativas.py`) só pontua o
   método local. A conclusão combinada e o melhor par são pontuados por
   `tools/live_traffic/avaliar_combinada.py`, com a mesma regra.

Os arquivos de trabalho do estimador (`*.db`, `*.estado.json`) e o
`capture.ndjson` ficam fora do Git. Regeneram-se com `live_traffic.py convert`
e com o `avaliar.sh`, a partir do `raw.jsonl.gz`.

## Cenários sintéticos com gabarito

O ADS-B da competição é simulado pela organização (regulamento 4.4.4): os aviões
podem ser gravados, sintéticos ou inventados. `tools/live_traffic/synthetic.py`
gera, em volta da UNSAM, os dois casos que a captura real não cobre, no mesmo
formato de uma captura:

- **`sintetico-rotas/`**: 23 voos em linha reta entre aeroportos reais. São 8
  decolagens de SABE e SAEZ, metade vista já no ar (sem posição no solo); 8 chegadas;
  4 sobrevoos; e 3 aviões leves saindo de SADF e SADM. Origem e destino conhecidos
  em todos.
- **`sintetico-ficticio/`**: 12 aeronaves com endereços inventados e pontas a mais
  de 15 km de qualquer aeroporto. Qualquer decisão é falso positivo.

O `capture.ndjson` se regenera com o mesmo comando, que é determinístico; ficam
versionados o gabarito e os metadados.

## 2026-11-18 · Buenos Aires (UNSAM), 1 h: a validação

`2026-11-18-unsam/`: das 11h22 às 12h22 UTC, os mesmos 150 NM.

- **Coleta:** 544 consultas, 44 aeronaves, 37 trilhas. Com a espera progressiva, os
  429 caíram para 8% das consultas (30% em 17/11).
- **Gabarito:** 38 voos, 26 com origem e 12 com destino.
  - Origens: Aeroparque 5, Ezeiza 5, San Fernando 5, e voos chegando de Madri,
    Houston, Miami, Dallas, Atlanta, Galeão, Santiago, Córdoba e Montevidéu.
  - Destinos: Aeroparque 5, Ezeiza 4, San Fernando 2, Montevidéu 1.

Nada desta captura foi usado para ajustar os limites da Etapa A. Ela só confere.

## A Etapa A: origem pela física do voo (ADR-0022)

Duas mudanças no estimador, só na ground:

- **Classe "grande" pela cinemática:** um jato prova o porte pela velocidade, pela
  altitude ou pela subida.
- **Distância de decolagem pela subida:** sem posição no solo, a altura, a razão de
  subida e a velocidade dizem a que distância a pista deve estar.

Origem decidida e acertos, **sem a categoria** (o que a ground recebe), pelo
método local:

| Conjunto | Antes | Depois | O que se fazia **com** a categoria |
|---|---|---|---|
| Real 17/11, 27 origens (calibração) | 0 | **7 de 7** | 5 de 6 |
| Real 18/11, 26 origens (**validação**) | 4 de 4 | **6 de 6** | 6 de 6 |
| Sintético `rotas`, 23 origens | 3 de 3 | **10 de 10** | 7 de 7 |
| Sintético `ficticio` (o certo é não decidir) | 0 decididas | 0 decididas | — |

- **O destino não mudou:** 9 de 10 em 17/11, 10 de 12 em 18/11.
- **A conclusão combinada** passou a decidir onde antes se abstinha: em 17/11, de 0
  para 4 origens e 3 destinos; em 18/11, de 4 para 6 origens e de 1 para 3 destinos.
- **Nenhuma decisão errada de origem,** em nenhum conjunto.
- **Com a categoria,** a distância pela subida leva a origem ainda mais longe:
  18/11, de 6 para 10; 17/11, de 5 de 6 para 7 de 7.

**As duas partes andam juntas.** A distância pela subida sozinha, sem saber que é um
jato, favoreceu heliportos à distância certa (17/11: 2 de 4 origens erradas). Por
isso ela só vale quando a classe é conhecida.

**O classificador contra a categoria verdadeira** (17/11, 47 aeronaves):

| Categoria real | Inferida "grande" | Desconhecida |
|---|---|---|
| A3, 31 | 29 | 2 |
| A1, 7 | **0** | 7 |
| A2, 1 | 1 | — |
| A0 ou sem categoria, 6 | 5 | 1 |

**O método de pares** ficou desligado por padrão na página e no `make sim`. Ligado,
a conclusão combinada não decidiu nenhum voo em nenhum conjunto. E ele mesmo:

- real 17/11: 3 de 12 origens;
- sintético `rotas`: 12 de 21 origens e 15 de 21 destinos;
- `ficticio`: 11 falsos positivos.

Os relatórios de 17/11 em `2026-11-17-unsam/avaliacao-*` são do estimador **antes**
da Etapa A; os de 18/11, depois. Para comparar as duas versões no mesmo conjunto:
`--no-kinematic-category --climb-back-weight 0` no `python -m ground` reproduz o
antes.
