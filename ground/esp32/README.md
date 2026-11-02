# Ground station na ESP32

A ESP32 DevKit V1 com um RA-02 é a ground das provas: é ela que manda o
comando remoto (PLANO D11). Roda o mesmo console da `gs_cli` da bancada
(`ground/console/`), com os mesmos comandos e as mesmas linhas JSON, sobre o
mesmo driver do SX1278 do satélite. Decisão e motivos: ADR-0018.

## Ligação do RA-02

| RA-02 | ESP32 | |
|---|---|---|
| SCK | GPIO18 | VSPI |
| MISO | GPIO19 | VSPI |
| MOSI | GPIO23 | VSPI |
| NSS | GPIO5 | chip select |
| RESET | GPIO14 | |
| DIO0 | GPIO26 | interrupção |
| 3.3V / GND | 3V3 / GND | **o RA-02 é só 3,3 V** |

Os pinos ficam no menuconfig ("Gama ground station"); para mudar sem o
menuconfig, edite os `default` em `main/Kconfig.projbuild` e apague
`ground/esp32/sdkconfig`.

## Compilar e gravar

Só precisa do Docker, com a imagem `espressif/idf:release-v5.3`
(`tools/esp32/idf.sh`):

```bash
make esp32-build                          # firmware em ground/esp32/build
make esp32-flash ESP32_PORT=/dev/ttyUSB0  # grava (o usuário precisa poder abrir a porta:
                                          # grupo uucp no Arch, dialout no Debian/Raspberry Pi OS)
make esp32-tests-qemu                     # as 11 suítes portáveis numa ESP32 emulada
make esp32-tests-flash ESP32_PORT=...     # as mesmas suítes na placa (TTC-16); depois
                                          # grave a ground de volta com make esp32-flash
```

## Usar

A ESP32 conversa pela USB com `ground/host/gs_serial.py`, que a faz parecer a
`gs_cli`: reseta a placa, manda o relógio do computador (a ESP32 não tem
relógio) e as opções, e depois os comandos, um por linha.

```bash
# à mão: digite ping, hk, mode mission_adsb, ...; Ctrl-D encerra
python3 ground/host/gs_serial.py --port /dev/ttyUSB0 --power 2 --auto-time on | tee ground.jsonl

# qualquer passo da bancada, com a ESP32 no lugar do rádio B (ESP32 no USB da Pi)
make bench-ping GS=esp32 ESP32_PORT=/dev/ttyUSB0
make bench GS=esp32 ESP32_PORT=/dev/ttyUSB0
```

Comandos: os da `gs_cli` (lista em `ground/console/gs_console.h`), mais
`ground_power DBM` para a potência da própria ground. A potência no boot é
14 dBm (TTC-03); a bancada manda 2.

O que a ESP32 imprime, além do que a `gs_cli` imprime:

| Linha | Quando |
|---|---|
| `boot` | a cada partida: versão do firmware e do IDF, motivo do reset, pinos, se o rádio respondeu |
| `set` | cada configuração aceita antes do `begin` |
| `uart` | no fim da sessão, se alguma linha não coube no buffer da serial (`lines_dropped`) |

## Por dentro

- `main/gs_main.c`: um laço só. Espera a interrupção do DIO0 ou o próximo
  prazo do enlace (no máximo 20 ms), consulta o rádio, passa as linhas da
  serial ao console só quando ele pode pegar a próxima, e nunca espera pela
  UART: a ground tem ~20 ms para responder ao satélite (ADR-0007).
- `main/radio_esp32.c`: SPI3 com DMA (uma rajada de FIFO inteira numa
  transação), reset e DIO0.
- `components/ttec`: `common/`, `flight/radio/sx1278.c` e
  `ground/console/gs_console.c`, compilados por caminho com os avisos do
  projeto.
