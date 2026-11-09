# Configuração da placa nova do OBC (`hardware/obc-v2`)

Para quando a placa chegar: ela ainda não foi fabricada. Os pinos são
conferidos, fio a fio, contra os layouts das placas (o da revisão TF de
02/11, que tem as mesmas ligações da de 23/10, e o da placa de antenas, onde
fica o TIP120 do burn-wire) por
`tools/analysis/board_check.py`, que roda em todo `make test`
(`boards_match_conf`). Nada daqui rodou numa placa; a bancada de novembro roda
estes arquivos numa Pi com os mesmos GPIOs e periféricos
(`docs/relatorios/2026-11-03-roteiro-bancada.html`).

| Arquivo | O que leva |
|---|---|
| `ttcd.conf` | o RA-02 da TT&C pelo barramento: SPI0 CE0, RESET no GPIO25, DIO0 no GPIO4 |
| `obcd.conf` | o I²C1 (INA219 em 0x40, ESP32 do AOCS em 0x2A), o DS18B20 achado sozinho (`w1_path = auto`), burn-wire no GPIO22, aquecedores no GPIO23 |
| `config.txt` | as linhas do `/boot/firmware/config.txt`: SPI0, I²C1 a 100 kHz e o 1-Wire no GPIO17 |

Cada `.conf` traz só o que a placa muda. O resto fica no padrão do código,
que é o de voo (14 dBm, perfil nominal), documentado no `.conf.example` de
cada daemon. O `adsbd` não depende da placa (o SDR fica no USB da TT&C) e usa
o exemplo dele. O `make test` valida os dois arquivos com `--check-config`.

## Instalar na Pi

1. Acrescentar o `config.txt` daqui ao `/boot/firmware/config.txt` e
   reiniciar. Não pode sobrar outra linha `dtoverlay=w1-gpio`: sem o
   `gpiopin=17`, o 1-Wire pega o GPIO4, que nesta placa é o DIO0 do RA-02.
2. `make install CONF_DIR=flight/conf/obc-v2` na Pi, ou
   `make pi-install CONF_DIR=flight/conf/obc-v2` do PC. Um
   `/etc/gama/<daemon>.conf` que já exista é mantido, e o `make` avisa; para
   trocá-lo pelo da placa, apague-o antes.
3. Conferir:

   ```bash
   ls /dev/spidev0.0 /dev/i2c-1 /sys/bus/w1/devices/   # um 28-* é o DS18B20
   ttcd -c /etc/gama/ttcd.conf --check-config
   obcd -c /etc/gama/obcd.conf --check-config
   ```

   Para ver quem responde no I²C, com o `obcd` parado (`make obcd-stop`) e
   nunca durante um `ADVANCED_AOCS`, quando o barramento é do ESP32:
   `sudo apt install i2c-tools` uma vez, depois `i2cdetect -y 1` — 0x40 é o
   INA219, 0x2a o ESP32 do AOCS.
