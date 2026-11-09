# Roteiro da bancada — novembro de 2026

> **Para o dia da bancada, use o roteiro completo em HTML:**
> [`docs/relatorios/2026-11-05-roteiro-bancada.html`](../../relatorios/2026-11-05-roteiro-bancada.html)
> (revisado em 05/11; substitui o de 03/11). Este arquivo é o resumo dos testes
> e do que cada um prova; os comandos, a pinagem e os critérios estão no HTML.

Desde 09/10 entrou muita coisa que **nunca rodou em hardware**: o protocolo v2
(ADR-0013), o `ttcd` e o `adsbd` dirigidos pelo modo (ADR-0014), 433,92 MHz e
14 dBm (ADR-0015), o `obcd` (ADR-0016) com o empréstimo do I²C ao AOCS
(ADR-0017) e a ground na ESP32 (ADR-0018).

**Fora do escopo desta sessão:** o ESP32 do AOCS (nunca ligado ao I²C; o AOCS ainda fecha o firmware) e o burn-wire real (D1, adiado). O `aocs_absent` (A8) e os pulsos em LED seguem.

## A montagem

Duas montagens separadas, que só se falam pelo ar:

- **Satélite:** Pi Zero 2 W com o rádio A (placa perfurada de outubro), o
  INA219, o DS18B20 e dois LEDs (1 kΩ) no lugar do burn-wire (GPIO22) e dos
  aquecedores (GPIO23). GND do DS18B20 no pino 39.
- **Ground:** ESP32 DevKit V1 com o rádio B, no USB do PC. SCK 18, MISO 19,
  MOSI 23, NSS 5, RESET 14, **DIO0 no GPIO27** (o padrão do repositório,
  GPIO26, é o das PCBs; a bancada compila com 27 em `ground/esp32/sdkconfig`).

O PC comanda a Pi pelo ssh (rede `gama-bancada`, o Wi-Fi do PC como ponto de
acesso pelo `nmcli`, o cabo ethernet dando a internet). Os passos de rádio rodam
do PC com `SPLIT=1`: o `ttcd` sobe na Pi pelo ssh, a ground (ESP32) fica no PC, e
o `run_step.sh` copia o log do `ttcd` de volta; os dois lados do passo ficam em
`docs/vv/bancada/AAAA-MM-DD/`.

## Os testes

| Bloco | Testes | O que prova | Etapa do README |
|---|---|---|---|
| 0 | T0 | `make check` na Pi: 39/39 nos dois builds, citações 71/71 | 8 |
| A · OBC sem rádio | A1–A10 | sensores sem o `obcd` e o erro do 0x2A; HK; V e I contra o multímetro (OBC-7); temperatura; termostato (D4); perda do DS18B20; pulso do burn-wire em LED (D3); `aocs_absent` (ADR-0017); sobrevivência; Ctrl-C e `kill -9` (ADR-0016) | 13 |
| B · enlace | B0–B3 | as 11 suítes na ESP32 (TTC-16); os seis passos de 09/10 em 433,92 MHz com a fiação de outubro (B1) e com os pinos da placa, RESET 25 e DIO0 4 (B2); RegFrf `6c7ae1`; DIO0 no GPIO27 | 9, 10, 11 |
| C · integrado | C1–C6 | Figura 44 em hardware; `adsbd` seguindo o modo; HK com V, I e T pelo ar; INA219 com o rádio transmitindo (mesmo trilho de 3,3 V); temperatura do SoC | 12 |
| D · extras | D1–D3 | ~~corrente do burn-wire com 4,7 kΩ e 470 Ω (P2)~~ (D1 adiado); corrente do RA-02 a 14 dBm (CDR Tabela 13); serviços e watchdog | 13f, 14, 15 |

O B1 não valida os pinos da placa: com RESET no GPIO20 e DIO0 no GPIO21 o
`ttcd` roda a configuração de bancada. O que vale para as placas é o B2, que
muda dois fios (RESET → pino 22, DIO0 → pino 7) e roda
`flight/conf/obc-v2/ttcd.conf`.

## Comandos que mudam em relação a outubro

```bash
make -k bench SPLIT=1 GS=esp32 ESP32_PORT=/dev/ttyUSB0                                  # B1
make -k bench SPLIT=1 GS=esp32 ESP32_PORT=/dev/ttyUSB0 TTCD_CONF=flight/conf/obc-v2/ttcd.conf   # B2
```

`SPLIT=1` assume o rádio `sx1278`, usa `PI_HOST` e `PI_DIR` e exige que a
árvore esteja sincronizada e compilada na Pi (`make pi-sync pi-build`).

## O que anotar

Uma pasta `AAAA-MM-DD/` por dia, como em outubro, com `notas.md` a partir de
[`modelo-notas-2026-11.md`](modelo-notas-2026-11.md): horário, versão (`git
describe`, que aparece no `start` de cada log), a montagem, o resultado contra o
esperado com os números, o que deu errado. No fim, atualize a tabela de
progresso do [README](README.md).
