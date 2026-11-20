# Roteiro da bancada — 20/11/2026

A Etapa B (ADR-0023) em hardware: a categoria ADS-B lida a bordo na saída
bruta (AVR) do `dump1090`, levada nos 2 bits livres do registro de track, até a
página. Três testes, em ordem de prioridade. Se o primeiro não fechar hoje, o
voo vai à competição com `avr_port = 0`, idêntico ao de antes, e a inferência
da Etapa A (ADR-0022) cobre.

Antes de ir para a bancada:

- **No PC:** `nmcli con up bancada-ap`, com o cabo ethernet ligado.
- **Na Pi, o código da branch `feat/adsbd-categoria-avr` ou mais nova:**
  `make pi-sync pi-build`, e `make pi-test T='avr|tracks|adsbd'` (o `test_avr`
  e o grupo AVR do `test_adsbd_integration` são novos).
- **ESP32:** o firmware de 16/11 serve. O console imprime o byte `flags`
  inteiro, e os bits 6–7 vão junto; não precisa regravar.
- **Daemons que sobraram:** conferir com
  `ssh pi@gamapi.local 'pgrep -a -f "ttcd -c|obcd -c|adsbd -c|sbs_replay"'` e
  matar antes de começar.

**Atalho (desde 20/11), com a missão de 2 min:** os testes 1 e 2 sem os comandos
à mão, a partir do PC, sem editar o `/etc/gama/adsbd.conf` da Pi. No fim (Ctrl-C), o
`pi-mission` imprime `avr_idents`, `avr_no_track`, `avr_rejected` e as linhas com
categoria, e traz os logs (com o `cpu.txt` do teste 3):

```bash
make net-up
make pi-mission MISSION_S=120 AVR=1 INPUT=docs/vv/trafego-real/2026-11-18-unsam/capture.ndjson  # teste 1
make pi-mission MISSION_S=120 AVR=1                                                               # teste 2, ao ar livre
make gs-web ADSB=1 MISSION_S=120     # outro terminal; "Iniciar missão ADS-B"
```

Detalhes em `docs/relatorios/2026-11-17-comandos-principais.html`, seção 9.

---

## 1. A categoria pelo LoRa: Buenos Aires real, com o `sbs_replay` nas duas portas

A cadeia de 19/11, agora com a captura **com** categoria e a porta AVR ligada.
O SBS continua sem categoria; ela só chega pela porta AVR, como chegaria do
`dump1090-fa` de verdade.

```bash
# PC: a captura de 18/11 (validação), com a categoria
python3 tools/live_traffic/live_traffic.py convert docs/vv/trafego-real/2026-11-18-unsam
scp docs/vv/trafego-real/2026-11-18-unsam/capture.ndjson pi@gamapi.local:/tmp/ba.ndjson

# Pi: o replay nas duas portas, e o adsbd lendo as duas
D=~/ttec/docs/vv/bancada/2026-11-20/cadeia-avr; mkdir -p $D; cd ~/ttec; rm -f /tmp/ttec.sock
setsid nohup python3 tools/sbs_replay/sbs_replay.py --ndjson /tmp/ba.ndjson --port 31003 --avr-port 31002 > $D/sbs_replay.txt 2>&1 < /dev/null &
setsid nohup build/flight/ttcd -c /etc/gama/ttcd.conf -o ipc_path=/tmp/ttec.sock -o tx_power_dbm=2 -o log_path=$D/ttcd.jsonl > /dev/null 2>&1 < /dev/null &
sleep 1
setsid nohup build/flight/obcd -c /etc/gama/obcd.conf -o ipc_path=/tmp/ttec.sock -o log_path=$D/obcd.jsonl > /dev/null 2>&1 < /dev/null &
setsid nohup build/flight/adsbd -c /etc/gama/adsbd.conf -o ipc_path=/tmp/ttec.sock -o dump1090_cmd= -o sbs_port=31003 -o avr_port=31002 -o ndjson_path=$D/adsb.ndjson -o log_path=$D/adsbd.jsonl > /dev/null 2>&1 < /dev/null &

# PC: a ground
python3 ground/host/gs_web.py --serial /dev/ttyUSB0 --power 2 --adsb
# http://127.0.0.1:8080, aba ADS-B, "Iniciar missão ADS-B"
```

**Anotar:**

- no `adsbd.jsonl`: o `avr_up` e, no `stats`, `avr_idents`, `avr_no_track` e
  `avr_rejected`;
- no `adsb.ndjson`: as linhas `"category"` (`grep -c category`);
- na página: as aeronaves com categoria **sem** asterisco, isto é, recebida e
  não inferida, e as origens decididas;
- a perda de quadros e a latência p95, para comparar com 16/11 e 19/11.

**Aprovado se:**

- `avr_rejected = 0` e `avr_idents > 0`;
- a página mostra categorias recebidas (A1, A3, A7), sem asterisco;
- o estimador sobre o `estado.json` da sessão bate com a mesma janela rodada no
  PC (`make adsb-chain AVR=1`, abaixo), e o que mudar é efeito do rádio;
- nada piorou em relação a 19/11: perda de quadros, latência e retratos.

Para comparar com o PC, a mesma captura sem hardware:
`make adsb-chain INPUT=docs/vv/trafego-real/2026-11-18-unsam/capture.ndjson`.
O script já liga a porta AVR; `AVR=0` roda o voo sem ela.

## 2. O `dump1090-fa` de verdade com a saída AVR

Só se a antena tiver passado em 19/11 (alguma aeronave com posição). Agora é o
`adsbd` que sobe o `dump1090-fa`, com a saída bruta aberta:

```bash
# Pi
sudo sed -i 's/--net-ro-port 0/--net-ro-port 30002/' /etc/gama/adsbd.conf
echo 'avr_port = 30002' | sudo tee -a /etc/gama/adsbd.conf
build/flight/adsbd -c /etc/gama/adsbd.conf --check-config      # tem de aceitar
build/flight/adsbd -c /etc/gama/adsbd.conf -o decoder=always -o ipc_path=/tmp/ttec.sock -o log_path=- | grep -E 'avr_|stats'
```

**Anotar:** `avr_idents` e as categorias que aparecem no `adsb.ndjson`.

**Aprovado se:** pelo menos uma aeronave real chega com categoria à tabela de
tracks (`avr_idents > 0`). Sem tráfego, o teste não vale, e isso não reprova a
etapa: o teste 1 é o que decide.

## 3. O custo na Pi

Com o teste 1 ou o 2 rodando, `top -b -n 30 -d 2 | grep -E 'dump1090|adsbd'` com e
sem `avr_port`.

**Aprovado se:** o `adsbd` fica abaixo de 5% de um núcleo, e o `dump1090-fa` não
sobe mais de 2 pontos sobre os 21,9% de 09/10.

---

## Ao terminar

- Os logs vêm com `make pi-fetch`; as notas em `2026-11-20/notas.md`.
- **Se o teste 1 passar:** `avr_port = 30002` e `--net-ro-port 30002` entram no
  `/etc/gama/adsbd.conf` da Pi e no `adsbd.conf.example` como padrão de voo. É uma
  mudança de configuração, a decidir com a equipe; o ADR-0023 passa a
  *Accepted* com os números da bancada.
- **Se não passar:** nada a desfazer. O voo vai com `avr_port = 0`.
