# Roteiro da bancada — 19/11/2026

Três testes, em ordem de prioridade. O primeiro valida em hardware a Etapa A
(origem pela física do voo, ADR-0022). Os outros dois fecham pendências de
16/11 (`../2026-11-16/notas.md`).

Antes de ir para a bancada:

- **No PC:** `nmcli con up bancada-ap`, com o cabo ethernet ligado
  (`../README.md#rede-da-bancada-o-pc-como-ponto-de-acesso`).
- **Na Pi, o código da branch `feat/origem-por-fisica` ou mais nova:**
  `make pi-sync pi-build`. A Pi só roda o voo; a Etapa A é toda da ground, no PC.
- **ESP32:** o firmware de 16/11 serve (`fw 69391c4`); o console não mudou.
- **Daemons que sobraram de 16/11:** os que subi à mão podem estar rodando.
  Conferir com `ssh pi@gamapi.local 'pgrep -a -f "ttcd -c|obcd -c|adsbd -c|sbs_replay"'` e
  matar antes de começar.

---

## 1. A Etapa A em hardware: Buenos Aires real, sem categoria, pelo LoRa

É a cadeia B de 16/11, com o tráfego real de 17/11 no lugar das 20 aeronaves
simuladas: `sbs_replay` → `adsbd` → `ttcd` → rádio A → rádio B → ESP32 → página →
estimador.

```bash
# PC: o tráfego real de 17/11 sem a categoria (o que a ground recebe de verdade)
python3 tools/live_traffic/live_traffic.py convert docs/vv/trafego-real/2026-11-17-unsam --no-category
scp docs/vv/trafego-real/2026-11-17-unsam/capture_sem_categoria.ndjson pi@gamapi.local:/tmp/ba.ndjson

# Pi (um terminal ssh): os três daemons e o replay, como em 16/11
D=~/ttec/docs/vv/bancada/2026-11-19/cadeia-ba; mkdir -p $D; cd ~/ttec; rm -f /tmp/ttec.sock
setsid nohup python3 tools/sbs_replay/sbs_replay.py --ndjson /tmp/ba.ndjson --port 31003 > $D/sbs_replay.txt 2>&1 < /dev/null &
setsid nohup build/flight/ttcd -c /etc/gama/ttcd.conf -o ipc_path=/tmp/ttec.sock -o tx_power_dbm=2 -o log_path=$D/ttcd.jsonl > /dev/null 2>&1 < /dev/null &
sleep 1
setsid nohup build/flight/obcd -c /etc/gama/obcd.conf -o ipc_path=/tmp/ttec.sock -o log_path=$D/obcd.jsonl > /dev/null 2>&1 < /dev/null &
setsid nohup build/flight/adsbd -c /etc/gama/adsbd.conf -o ipc_path=/tmp/ttec.sock -o dump1090_cmd= -o sbs_port=31003 -o ndjson_path=$D/adsb.ndjson -o log_path=$D/adsbd.jsonl > /dev/null 2>&1 < /dev/null &

# PC: a ground com o processamento ADS-B
python3 ground/host/gs_web.py --serial /dev/ttyUSB0 --power 2 --adsb --no-pairs
# abrir http://127.0.0.1:8080, aba ADS-B, "Iniciar missão ADS-B"
```

Os pares ficam desligados, o padrão desde o ADR-0022 (o `--no-pairs` só o deixa
explícito). Para ligá-los: `--pairs --pairs-db ground/host/adsb/data/pares/pares_aeroportos.db`.

**Anotar:**

- os passos da missão na página: `SET_TIME`, `SET_MODE`, o modo, o decodificador, os
  retratos e, aos 10 min, o downlink;
- na tabela de aeronaves: quantas aparecem com a categoria "inferida pelo
  desempenho" e quantas origens a página mostra;
- a perda de quadros e a latência p95 das estatísticas finais, para comparar com
  16/11 (0 de 359 e 2,8 s).

**Aprovado se:**

- a missão começa com um clique e termina sozinha;
- a página mostra origens decididas com a categoria inferida;
- o `avaliar_combinada.py` e o `avaliar_estimativas.py`, rodados sobre o
  `estado.json` da sessão contra o `ground_truth.json` de 17/11, ficam iguais à
  avaliação offline nos primeiros 10 min da captura. O que mudar é efeito do rádio, e
  vira achado.

```bash
S=$(ls -td ground/host/sessions/mission-* | head -1)
python3 ground/host/adsb/validation/avaliar_estimativas.py --state $S/estado.json \
    --truth docs/vv/trafego-real/2026-11-17-unsam/ground_truth.json | sed -n '/^RESUMO/,/^TOTAL/p'
python3 tools/live_traffic/avaliar_combinada.py $S/estado.json docs/vv/trafego-real/2026-11-17-unsam/ground_truth.json
```

## 2. A antena ao ar livre

Fecha a etapa 1 do `../roteiro-adsb-2026-11.md`, que falhou em 16/11 (0 mensagens
dentro da sala, 8 na janela).

1. Identificar a antena: o dipolo de 1090 MHz, não o de 433. Cada braço deve ter ~6,9 cm,
   montado na vertical, e a rosca firme no NESDR.
2. **Ao ar livre**, de preferência do lado do aeroporto de Brasília (SBBR). Nunca dentro
   do container nem da sala: em 20/11, dentro do container, a antena da equipe não recebeu nada
   (`../2026-11-20/notas.md`).
3. Na Pi, com o `adsbd` parado (ele ocupa o SDR), 60 s com cada antena:

   ```bash
   make pi-adsbd-stop
   make pi-sdr-check ANTENNA=cots
   make pi-sdr-check ANTENNA=gama
   make pi-fetch && make sdr-compare
   ```

**Aprovado se:** veredito APROVADA (pelo menos uma aeronave com posição). No PC, em 20/11,
as duas antenas passaram ao ar livre. Se passar, repetir o
teste 1 com o SDR no lugar do `sbs_replay` (`dump1090_cmd` padrão), por 10 min.

## 3. O rádio A nos pinos da placa

Em 16/11 o rádio A estava na fiação de outubro (RESET 20, DIO0 21). Com os pinos da
placa, o ping passou, mas com 2 tentativas: o DIO0 só era visto na varredura de
250 ms.

1. Mudar dois fios: RESET para o GPIO25 (pino 22), DIO0 para o GPIO4 (pino 7).
2. Voltar o `/etc/gama/ttcd.conf` ao da placa:
   `sudo cp /etc/gama/ttcd.conf.obc-v2 /etc/gama/ttcd.conf`.
3. Rodar
   `make bench-ping SPLIT=1 GS=esp32 ESP32_PORT=/dev/ttyUSB0 TTCD_CONF=flight/conf/obc-v2/ttcd.conf`.

**Aprovado se:** 10/10 pings na **1ª tentativa**, e o atraso do fim de transmissão no
log do `ttcd` de até 2 ms.

---

## No fim

`make pi-fetch`, depois copiar o `ground.jsonl` da sessão para
`docs/vv/bancada/2026-11-19/cadeia-ba/`. Escrever `notas.md` a partir do
`../modelo-notas-2026-11.md`. Atualizar a tabela de progresso em `../README.md`.
Desligar com `nmcli con down bancada-ap`.
