# Bancada — 05/11/2026

Primeira sessão com o código novo em hardware. Roteiro:
[`2026-11-05-roteiro-bancada.html`](../../../relatorios/2026-11-05-roteiro-bancada.html)
(modelo desta nota: [`modelo-notas-2026-11.md`](../modelo-notas-2026-11.md)).
Dia anterior de bancada: [09/10](../2026-10-09/notas.md). A sessão foi
interrompida no meio do bloco A, por causa do 1-Wire (achado 5).

**Fora do escopo desta sessão:** o ESP32 do AOCS (nunca ligado ao I²C) e o
burn-wire de verdade (D1, adiado). O pulso de 5 s foi medido nos LEDs.

## Montagem

- **Satélite:** Pi Zero 2 W (`pi@gamapi.local`, 10.42.0.75), rádio A na placa
  perfurada de outubro, INA219, DS18B20 e dois LEDs com 1 kΩ numa protoboard.
  GND do DS18B20 no pino 39 (não no 9). A fonte da carga do INA219 ficou
  **desligada** a sessão toda.
- **Ground:** ESP32 DevKit V1 com o rádio B, no USB do PC, separada da Pi.
  DIO0 no **GPIO27** (padrão do repositório: 26, das PCBs); o `sdkconfig`
  local leva `CONFIG_GAMA_GS_PIN_DIO0=27`. O RESET estava soldado no GPIO12
  (pino de boot da ESP32) e foi **ressoldado no GPIO14**.
- **Rede:** `bancada-ap` pelo `nmcli` (o PC no cabo ethernet). Sinal da Pi
  −40 dBm. O NetworkManager devolveu o Wi-Fi do PC à rede da universidade
  mais de uma vez; foi preciso subir o `bancada-ap` de novo.
- **LEDs (confirmado com `pinctrl set`, sem daemon):** GPIO22 (mecanismo) =
  **vermelho**; GPIO23 (aquecedor) = **azul**.
- **Pi preparada:** o `config.txt` não tinha o I²C nem o 1-Wire; entraram
  `dtparam=i2c_arm=on`, `i2c_arm_baudrate=100000` e
  `dtoverlay=w1-gpio,gpiopin=17`. Os grupos `i2c gpio spi dialout` já
  existiam. Instalado `i2c-tools`. Nos comandos por ssh sem login, o
  `i2cdetect` e o `i2ctransfer` precisam do caminho `/usr/sbin/`.

## Resultados

| Teste | Resultado |
|---|---|
| **T0** · `make check` na Pi | ✅ 38/38 no build normal e 38/38 no UBSan, citações 71/71, nenhum warning; versão `42c1d84-dirty`; **3 min 54 s** (o `build/` já existia) |
| **B0** · suítes na ESP32 | ✅ 11 suítes, 0 falhas, contagens **idênticas** às do PC (260, 58, 1325, 51, 70, 832, 65, 39, 34, 102, 55): TTC-16 |
| **B0** · rádio na ESP32 | ✅ `"radio":"ok"`, `reset=14 dio0=27`, depois de dois defeitos (achados 1 e 2) |
| **A1** · sensores sem o `obcd` | ✅ INA219 em `0x40`; DS18B20 `28-989fc3356461`, CRC `YES`, 26,125 °C (30 leituras seguidas: 25,625 a 25,687 °C). `i2ctransfer` em endereço vazio: **EIO** (achado 3) |
| **A2** · `ttcd` + `obcd` + ground UDP | ✅ pings 3/3 (268 ms, rádio simulado); `hk` em `PRE_TEST`, T = 25,88 °C, INA219 lendo; boot do `obcd` com as saídas 22 e 23 apagadas |
| **A3** · V e I contra o multímetro | ⏸ adiado: sem multímetro nem fonte montada. O INA219 respondeu (888 mV e 0 mA com a fonte desligada) |
| **A7** · pulso do burn-wire (LED) | ✅ `SET_MODE` com ACK em 289 ms; `burnwire` on em 733083 ms e off em 738083 ms: **5000 ms**; volta sozinho a `PRE_TEST`. LED vermelho (GPIO22) aceso |
| **A9** · sobrevivência | ✅ `PRE_TEST → ENV_SURVIVAL` (755313 ms) e de volta a `PRE_TEST` (764324 ms, 9,0 s depois); ACK em 289 ms |
| **A5** · termostato | ⏸ parcial: com limites 26/28 °C o aquecedor ligou em 25,69 °C (`why:"cold"`, LED azul); a histerese não foi medida (achados 4 e 5) |
| A4, A6, A8, A10 | não feitos |
| B1–B3, C1–C6, D2, D3 | não feitos |
| D1 | adiado (burn-wire real) |

## Achados

1. **Defeito no firmware da ground, corrigido.** `radio_esp32.c` passava
   `pdMS_TO_TICKS(10)` ao `spi_device_polling_start()`; o ESP-IDF 5.3 só
   aceita `portMAX_DELAY` ali e devolve `ESP_ERR_INVALID_ARG`
   ("currently timeout is not available for polling transactions"). Toda
   transação SPI falhava (`radio: error, SPI read failed`). As suítes não
   pegam isso, porque nenhuma executa esse arquivo: é a primeira vez que ele
   roda na placa. O limite de tempo ficou no `polling_end`.
2. **`RegVersion` errado em quatro execuções seguidas, depois funcionou.**
   O erro sumiu depois de revisar os fios e reencaixar a placa; **a causa não
   foi identificada** (contato intermitente é a hipótese). Para a próxima vez,
   o firmware agora imprime o valor lido no campo `why` do `boot`
   (`read 0x…, expected 0x12`). Falta repetir uns ciclos de ligar e desligar o
   USB para saber se é estável.
3. **EIO, não `EREMOTEIO`, num endereço I²C sem ninguém.** Nesta Pi (kernel
   `6.18.34+rpt-rpi-v8`) 0x2A, 0x30 e 0x50 dão `Input/output error`, escrita ou
   leitura. O `obcd` só trata `EREMOTEIO` e `ENXIO` como "ninguém respondeu"
   (`flight/obcd/main.c:347`). Sem ESP32, `mode aocs` abriria o empréstimo do
   barramento por até ~5 min, com tensão e corrente "sem dado", em vez de
   `aocs_absent`. **Não é defeito de hardware.** Decisão: rodar o A8 como está,
   para registrar o comportamento, e corrigir depois (`-EIO` como NACK, mais o
   teste e uma nota no ADR-0017). Perder o INA219 durante esse teste é aceito.
4. **O `obcd` leu 0,00 °C duas vezes, e o aquecedor alternou.** Entre
   557071 ms e 565518 ms o `temp_ccel` alternou entre 0 e 2569, e o LED do
   aquecedor ligou e desligou quatro vezes, enquanto o `w1_slave` lido à mão
   estava normal (30 leituras sem nenhum zero). Hipótese, **não provada**: um
   `scratchpad` todo em zeros tem CRC8 igual a zero, e o kernel o entrega como
   `crc=00 YES t=0`. Convém o `obcd` rejeitar a leitura cujo byte de
   configuração não seja de DS18B20 (0x1F/0x3F/0x5F/0x7F). Um 0 °C falso liga o
   aquecedor por um ciclo.
5. **O DS18B20 sumiu, e o DQ não sobe.** Cronologia:
   - o primeiro sensor foi ligado **ao contrário**: ficou muito quente, o
     plástico da protoboard queimou, e o kernel só via entradas `00-…`. O
     sensor foi trocado e a Pi não sofreu (`throttled=0x0`);
   - o sensor novo funcionou por mais de 30 min;
   - depois de desligar a Pi para revisar os LEDs, ele não voltou: GPIO17 em
     `lo` estável com o sensor, oscilando e com fantasmas sem ele;
   - **pull-down interno** (~50 kΩ): `lo` em 8 de 8, então o 4,7 kΩ não leva o
     DQ ao 3,3 V; **pull-up interno**: `hi`, então o pino da Pi está bom e sem
     curto ao GND;
   - o resistor mede 4,7 kΩ fora e no circuito (entre VDD e DQ); os fios dos
     pinos 11, 17 e 39 conduzem; o fio do pino 11 foi trocado de furo, sem
     efeito.

   **Pendente:** medir a tensão nas linhas do VDD e do DQ com a Pi ligada.
   Hipótese a testar: um fio no pino errado do conector (contagem trocada) ou
   contato ruim na área queimada da protoboard. O circuito e os pontos suspeitos estão
   ilustrados em [`diagnostico-1wire.html`](diagnostico-1wire.html).
6. **O `ttcd` cai em SAFE depois de 120 s sem ouvir a ground**
   (`contact_timeout_ms`), e a ground começa em NOMINAL: um `SET_MODE` depois
   de uma pausa falha com `tc_failed` (27 tentativas). A ground manual precisa
   de um keepalive; foi usado um console fixo (FIFO no tmux) com `ping 1` a
   cada 50 s.
7. **O `ttcd` sobrevive a `tmux kill-session`** (o `tee -i` ignora o sinal), e o
   seguinte recusa subir. Parar com `pkill -x ttcd`.
8. **Os logs do `make bench-gs RADIO=udp` ficam em `build/bench-udp/`**, que o
   `make pi-fetch` não traz.

## Mudanças no repositório

- `ground/esp32/main/radio_esp32.c`: o `polling_start` com `portMAX_DELAY`
  (achado 1) e o valor do `RegVersion` no erro (achado 2).
- `tools/bench/run_step.sh` e `Makefile`: `SPLIT=1`, o `ttcd` na Pi pelo ssh e a
  ground no PC. Testado com um ssh simulado na mesma máquina (ping 10/10); com
  duas máquinas de verdade, **ainda não**.
- `ground/host/gs_web.py`, `gs_web_ui.html`, `test_gs_web.py` (CTest `gs_web`), `make gs-web` e
  `ground/host/README.md`: a interface no navegador, para a apresentação e para
  ler os modos com as PCBs. Conferida com um log sintético; falta com a ground
  de verdade.
- `docs/relatorios/2026-11-05-roteiro-bancada.html`,
  `docs/vv/bancada/roteiro-2026-11.md` e `modelo-notas-2026-11.md`: o roteiro
  revisado, com a ground separada, o D1 adiado e as cores dos LEDs.
- `ground/esp32/README.md` (nota do DIO0 em 27) e os caminhos do roteiro em
  `AGENTS.md` e `flight/ttcd/README.md`.

## Logs

Ficaram na Pi (a rede da bancada caiu no fim e não deu para trazê-los):
`~/ttec/docs/vv/bancada/2026-11-05/obcd-A.jsonl`, `obcd-A7.jsonl`,
`obcd-A5.jsonl`, `obcd-A5b.jsonl`; `~/ttec/build/bench-udp/manual-ground.jsonl`
e `manual-ttcd.jsonl`; `~/check-T0.log`. Trazer com `make pi-fetch` e com
`scp pi@gamapi.local:ttec/build/bench-udp/manual-*.jsonl pi@gamapi.local:check-T0.log`.
A Pi ficou com o `ttcd`, o `obcd` e a ground rodando na sessão `bench`.

## Para a próxima sessão

1. Achar o fio do DQ (achado 5) e retomar A4, A5, A6.
2. A8 (`aocs_absent`), A10 (Ctrl-C e `kill -9`), por último o A8.
3. A3 com multímetro e fonte.
4. Bloco B: os seis passos com `SPLIT=1`, primeiro com a fiação de outubro
   (B1) e depois nos pinos da placa, RESET 25 e DIO0 4 (B2); antes, ciclos de
   ligar e desligar a ESP32 (achado 2).
5. Decidir as correções do `obcd` (achados 3 e 4).
