# Bancada — testes em hardware

Evidência dos testes do TT&C no hardware de verdade: a Raspberry Pi Zero 2 W,
os dois RA-02 e o NESDR Nano 3. O procedimento é o
[roteiro de testes na Raspberry Pi](../../relatorios/2026-10-05-roteiro-testes-raspberry.html);
os passos da bancada dos rádios estão em `flight/ttcd/README.md`.

## Como a pasta se organiza

Uma pasta por dia de bancada, `AAAA-MM-DD/`, com:

| Arquivo | De onde vem |
|---|---|
| `notas.md` | escrito à mão: montagem, resultados de cada etapa, problemas — o que os logs não contam |
| `HHMMSS-<passo>-ttcd.jsonl`, `HHMMSS-<passo>-ground.jsonl` | cada passo do `make bench`, um par por passo |
| `HHMMSS-toa.txt` | tempo no ar medido × modelo (passo 2) |
| `ganho.txt`, `cpu.txt` | varredura de ganho e CPU do `dump1090` (etapa 5) |
| `adsb.ndjson`, `adsbd.jsonl`, `manual-*.jsonl` | `adsbd` com o SDR e a cadeia completa (etapas 6 e 7) |

Os logs são gravados na Pi, em `~/ttec/docs/vv/bancada/`, e chegam aqui com
`make pi-fetch` (os alvos `make pi-bench-…` já fazem isso no fim). O
`make pi-sync` nunca toca nesta pasta na Pi, então os logs de lá não se perdem.

## Progresso

Etapas do roteiro. ✅ feito · ⏳ em andamento · — não começado.

| Etapa | O que prova | Estado | Onde |
|---|---|---|---|
| 1 · Conectar e preparar a Pi | o PC comanda a Pi; sistema, data e SPI conferidos | ✅ 07/10 | [notas](2026-10-07/notas.md#etapa-1) |
| 2 · O código na Pi | compila e passa nos testes no compilador da Pi | ✅ 07/10 — 23/23 nos dois builds, citações 71/71; na Pi o ASan é impossível e roda só o UBSan, que achou um defeito no `test_tm` (corrigido) | [notas](2026-10-07/notas.md#etapa-2) |
| 3 · Ligar os dois rádios | a fiação está certa | ✅ 09/10 — com placa perfurada (na protoboard nenhum chip respondia); 80 cm | [07/10](2026-10-07/notas.md#etapa-3), [09/10](2026-10-09/notas.md) |
| 4 · A bancada dos rádios | tempo no ar, PER, troca de taxa, ACK perdido, LBT (HLR-COMM-01, HLR-COMM-03; aceite dos ADRs 0007 e 0012) | ✅ 09/10 — os 6 passos: ToA ≤ 1,9%, PER 0 em ~2,2 mil frames por sentido, 7/7 trocas, ACK perdido recuperado (23 s), LBT header = preamble; driver corrigido (RX on-going) | [notas](2026-10-09/notas.md) |
| 5 · O SDR e o dump1090 | o receptor funciona; ganho; CPU do `dump1090` (risco do PLANO 3.4) | — | |
| 6 · O `adsbd` com o SDR | o daemon com o hardware de verdade | — | |
| 7 · ADS-B real pelo LoRa real | a cadeia da missão, em hardware (bônus) | — | |

## A Pi da bancada

Conferido em 07/10/2026; atualize quando mudar.

| | |
|---|---|
| Placa | Raspberry Pi Zero 2 W Rev 1.0 · 415 MB de RAM utilizáveis · 4 núcleos |
| Sistema | Debian 13 (trixie), **arm64**, kernel 6.18.34+rpt-rpi-v8 |
| Ferramentas | gcc 14.2.0 (o PC: gcc 16.2.1), CMake 3.31.6 |
| Swap | 415 MB em zram |
| Acesso | `pi@gamapi.local`, por ssh com chave; rede `gama-bancada`, o ponto de acesso do PC (seção abaixo); reserva: o hotspot do celular |
| Console local | monitor pelo micro-HDMI, para ver a rede (`nmcli device status`) sem ssh |
| Usuários | `pi` nos grupos `spi`, `gpio` e `plugdev`; `gama` (serviço) em `spi` e `gpio` |

## Rede da bancada: o PC como ponto de acesso

A Pi Zero 2 W não tem porta de rede, e o hotspot do celular é lento e
instável. A solução adotada em 07/10: **o PC vira o ponto de acesso Wi-Fi**,
a Pi se conecta nele, e o PC repassa a ela a internet que recebe pelo cabo.

```
Pi (wlan0, 10.42.0.x) ──Wi-Fi 2,4 GHz── PC (wlp2s0, 10.42.0.1) ──cabo── rede da universidade
```

No NetworkManager isso é uma conexão com `ipv4.method shared`: ele sobe o
Wi-Fi em modo AP, roda um `dnsmasq` próprio que entrega endereço (DHCP) e DNS
à Pi, liga o encaminhamento de pacotes e faz NAT para a saída do PC. A rede da
universidade só enxerga o PC.

- **Sem o cabo, a bancada funciona igual** (ssh, `make pi-…`, os rádios); só a
  Pi fica sem internet — sem `apt` e sem acertar a hora pela rede.
- O nome `gamapi.local` continua resolvendo (mDNS), então `local.mk` e
  `~/.ssh/config` não mudam.

### Uma vez, no PC

Valores deste PC: Wi-Fi `wlp2s0`, cabo `eth0`, firewall `ufw`. Troque `SENHA`
por uma de 8 caracteres ou mais, a mesma no PC e na Pi.

```bash
# o NetworkManager usa o dnsmasq para o DHCP da rede shared — instalar, não ativar o serviço
sudo pacman -S --needed dnsmasq

# o ufw bloqueia por padrão o DHCP e o DNS que chegam pelo Wi-Fi e o repasse Wi-Fi → cabo
sudo ufw allow in on wlp2s0 to any port 67 proto udp
sudo ufw allow in on wlp2s0 to any port 53
sudo ufw route allow in on wlp2s0 out on eth0

# o ponto de acesso: 2,4 GHz (a Pi Zero 2 W não tem 5 GHz), WPA2 (ela lida mal com WPA3),
# autoconnect no (não sobe sozinho a cada boot do PC)
nmcli con add type wifi ifname wlp2s0 con-name bancada-ap autoconnect no ssid gama-bancada \
    802-11-wireless.mode ap 802-11-wireless.band bg 802-11-wireless.channel 6 \
    wifi-sec.key-mgmt wpa-psk wifi-sec.proto rsn wifi-sec.pairwise ccmp wifi-sec.group ccmp \
    wifi-sec.psk "SENHA" ipv4.method shared ipv6.method disabled
```

### Uma vez, na Pi

Pelo ssh, **enquanto ela ainda está em outra rede** (sem monitor, não há outro
jeito de entrar depois):

```bash
sudo nmcli con add type wifi ifname wlan0 con-name gama-bancada ssid gama-bancada \
    wifi-sec.key-mgmt wpa-psk wifi-sec.psk "SENHA" connection.autoconnect-priority 20
```

A prioridade 20 (as outras redes estão em 0) faz a Pi preferir a
`gama-bancada` no boot ou quando perde a rede atual. Já conectada em outra, ela
não troca sozinha.

### A cada sessão

```bash
nmcli con up bancada-ap      # no PC: o Wi-Fi do PC sai da rede em que estava; o cabo segue
nmcli con down bancada-ap    # no fim: o Wi-Fi do PC volta para a rede de costume
```

### A Pi conectou? Sem monitor, pelo PC

```bash
iw dev wlp2s0 station dump | grep -E 'Station|signal:'   # aparelhos associados, com o sinal
journalctl -b -u NetworkManager | grep DHCPACK            # o endereço que a Pi recebeu
ssh pi@gamapi.local hostname -I
```

### Se a Pi não aparecer

Ela guarda a rede do celular. Ligue o hotspot do celular e rode
`nmcli con down bancada-ap` no PC: os dois voltam para o celular, e o ssh
volta a funcionar.

### Medido em 07/10/2026

| | |
|---|---|
| Endereço da Pi | 10.42.0.75 |
| Sinal no PC | −53 dBm, 52 Mbit/s (MCS 5) |
| Internet da Pi | HTTP 200 de `deb.debian.org` em 0,25 s; DNS ok |
| `ping` para fora | não responde: a rede da universidade bloqueia ICMP — sem efeito para a bancada |
| Hora | sincronizada (`System clock synchronized: yes`) |
