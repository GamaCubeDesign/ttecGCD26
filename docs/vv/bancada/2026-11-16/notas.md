# Bancada — 16/11/2026

Roteiro: [`roteiro-adsb-2026-11.md`](../roteiro-adsb-2026-11.md), a cadeia ADS-B.
Versão `69391c4` (branch local `fix/adsbd-sem-recusa-de-hora`: a `ground-adsb`
mais a recusa de hora só no `ttcd`), na Pi e na ESP32.

## Montagem

- **Satélite:** Pi Zero 2 W (10.42.0.75, −39 dBm na `bancada-ap`), rádio A na
  placa perfurada de outubro, NESDR Nano 3 com a antena COTS.
- **Ground:** ESP32 DevKit V1 com o rádio B (RESET 14, DIO0 27), no USB do PC.
- **Rede:** `bancada-ap` com o PC no cabo ethernet.
- `make pi-install CONF_DIR=flight/conf/obc-v2` instalou `/etc/gama/*.conf` pela
  primeira vez. O `ttcd.conf` foi ajustado à fiação da bancada (RESET 20, DIO0 21;
  achado 2); o original está em `/etc/gama/ttcd.conf.obc-v2`.

## Resultados

| Etapa | Resultado |
|---|---|
| **0** · Pi | ✅ compilada sem warnings; `pi-test` 38/40: as 2 falhas são `ground_aeronaves` e `adsb_pipeline`, `No module named 'numpy'` (ground, não voo) |
| **0** · ESP32 | ✅ gravada; `boot` com `"radio":"ok"`, `fw 69391c4` |
| **1** · antena + SDR | ❌ SDR bom (142 M amostras em 59 s, 0 descartadas, `throttled=0x0`), mas sem sinal: dentro da sala 0 mensagens; na janela 8 mensagens de 1 aeronave (`e49c06`, sem posição) em 60 s e 1 mensagem em 3 min; varredura de ganho 49,6/40,2/29,7/19,7 dB com 0 aceitas (ruído −36 → −42 dBFS) |
| **2** · `adsbd` com SDR | ✅ parcial: sobe o `dump1090-fa`, SBS em 1 s, sai limpo; 0 linhas, por falta de sinal |
| **3** · enlace | ✅ 10/10 na 1ª tentativa, 276 ms, com a fiação de outubro; ❌ com os pinos da placa (achado 2) |
| **B** · cadeia com `sbs_replay` | ✅ ver abaixo |

### Cadeia B: `sbs_replay` → `adsbd` → `ttcd` → LoRa → ESP32 → `gs_web --adsb`

O SDR foi trocado por uma missão simulada de 20 aeronaves (6 chegadas e 6 saídas
em SBBR, 8 em cruzeiro; 10 980 mensagens, 581 s), gerada com a classe `Flight` de
`simular_trajetorias.py`. Daí em diante, tudo real: os três daemons na Pi, o rádio A
a 2 dBm, a ESP32, a interface com o processamento ADS-B (`--no-pairs`).

| Grandeza | Valor |
|---|---|
| Início pelo botão | `SET_TIME` OK em 318 ms → `SET_MODE` OK em 297 ms, ambos na 1ª tentativa; `obcd` em `MISSION_ADSB`; primeiro track na ground 5,4 s depois |
| Fim | `MISSION_DOWNLINK` 600 000 ms após a entrada (timeout do `obcd`); `downlink_done` em 360 ms; volta a `PRE_TEST` |
| A bordo | 32 940 mensagens decodificadas, 0 rejeitadas, 20 aeronaves, latência p95 2,8 s (HLR-ADS-08: 5 s) |
| Enlace | 359 quadros de TM, **0 perdidos**; RSSI −42 a −44 dBm, SNR 13 a 14 dB; 0 quadros ruins |
| Idade no ar (missão ativa) | p50 1,0 s, p95 2,7 s, máx. 32 s (2 236 registros) |
| Ground | 2 232 observações processadas, 0 rejeitadas; as 20 aeronaves; roster com os 20 indicativos e as estatísticas finais recebidos |
| Estimador (método local) | origem: nenhuma decisão em 2 232 estimativas; destino: 385 `TENTATIVE` e 97 `ESTIMATED`, só para as 6 chegadas, quase sempre um aeródromo pequeno perto de SBBR (achado 4) |

Logs em `cadeia-b/`: da Pi, o `ttcd`, o `obcd`, o `adsbd` e o NDJSON de bordo
(`make pi-fetch`); da ground, o `ground.jsonl`.

### A trajetória reconstruída, contra o gabarito da simulação

Cada posição que desceu pelo LoRa foi comparada com a posição verdadeira da
simulação no mesmo instante (`analise/trajetoria.py`):

| Grandeza | Valor |
|---|---|
| Posições comparadas | 2 216, das 20 aeronaves |
| Erro de posição | p50 3 m, p95 23 m, máx. 24 m |
| Erro de instante | p50 +0,01 s, p95 0,11 s |
| Erro de altitude | p50 3 ft, p95 22 ft |
| Intervalo entre pontos do mesmo avião | 5,0 s (máx. 5,1 s): nenhum buraco |

Os 23 m são o erro de 0,11 s no instante a ~420 kt. Reconstruir o instante pela
idade no ar, sem sincronizar relógios (ADR-0007 §6), funciona.

### Reproduzir

```bash
python3 docs/vv/bancada/2026-11-16/analise/gerar_sim20.py sim20.ndjson
python3 docs/vv/bancada/2026-11-16/analise/trajetoria.py sim20.ndjson \
    docs/vv/bancada/2026-11-16/cadeia-b/adsb.ndjson docs/vv/bancada/2026-11-16/cadeia-b/ground.jsonl
python3 docs/vv/bancada/2026-11-16/analise/categoria.py sim20.ndjson \
    docs/vv/bancada/2026-11-16/cadeia-b/ground.jsonl
```

A geometria da simulação é sempre a mesma; só os carimbos mudam com a hora da
geração, e o `trajetoria.py` alinha os relógios pelo registro de bordo.

## Achados

1. **A antena não entrega sinal de 1090 MHz.** O SDR e o `dump1090-fa` estão bons;
   o ruído é o do próprio receptor. Pendente: identificar a antena, o conector e o
   local (vidro, janela aberta, lado do aeroporto).
2. **Com o DIO0 no pino errado, o enlace funciona pior, sem erro nenhum.** Com
   `flight/conf/obc-v2/ttcd.conf` (DIO0 no GPIO4) na fiação de outubro (DIO0 no
   GPIO21), o ping deu 10/10 mas 9 com 2 tentativas: o fim de transmissão só era
   visto na varredura de 250 ms (`RADIO_POLL_MS`), 106 ms depois do tempo no ar,
   e o satélite perdia o TC seguinte. Proposta: o `ttcd` contar no log os fins de
   transmissão achados pela varredura, para que um DIO0 mal soldado na placa nova
   apareça.
3. **PINGs automáticos da ground (keepalive, `id 0`) com 1,6 a 10,3 s de
   latência**, todos na 1ª tentativa, enquanto os comandos do operador levaram
   ~280–320 ms. Causa não investigada.
4. **O estimador erra porque a categoria da aeronave não chega à ground.** Nas 6
   chegadas a SBBR, o candidato mais frequente foi um aeródromo pequeno vizinho
   (`BR-0422`, `SIQE`, `BR-2672`…); saídas e cruzeiros não tiveram decisão, e
   nenhuma origem saiu. Com o mesmo estimador e a mesma simulação a uma amostra
   a cada 5 s (`analise/categoria.py`):

   | Entrada | Chegadas: destino mais frequente | Saídas: origem |
   |---|---|---|
   | simulação com a categoria (A3) | SBBR nas 6 | SBBR em 2 |
   | simulação sem a categoria | aeródromos pequenos | nenhuma |
   | o que desceu pelo LoRa | os mesmos de "sem a categoria" | nenhuma |

   O resultado do LoRa coincide com o de "sem a categoria": não é o rádio nem a
   simulação. Sem a categoria, o estimador não descarta os aeródromos pequenos.
   **E a categoria não existe a bordo:** o SBS do `dump1090-fa` não a traz, e o
   registro de track também não (o ADR-0021 já registrava isso). Com aviões
   reais vai acontecer igual. Os cruzeiros são o caso do método de pares (etapa
   6, não feita: falta gerar o banco), que também usa a categoria.
5. **O HK traz `TIME_SYNCED` antes de qualquer comando**, porque a ground mandou
   `SET_TIME` sozinha no primeiro contato. Quem liga isso é o `gs_web.py`: com
   `--serial`, ele chama o `gs_serial.py` com `--auto-time on`
   (`ground/host/gs_web.py`, `build_source`). O padrão do `gs_serial.py` sozinho
   e do `gs_cli` continua desligado.

## Não feito

Etapas 4 e 5 com o SDR (sem sinal), etapa 6 (pares).
