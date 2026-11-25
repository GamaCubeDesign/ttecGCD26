# Firmware do AOCS (ESP32 da placa do AOCS)

O firmware da equipe do AOCS, **entregue em 25/11/2026** como
`AOCS_Cubetscheck_logica2025-validado-25-11.zip`, versão `2026.11-OL`
(Simple FOC 2.4.0, core esp32 3.3.12, "ESP32 Dev Module"). Ele faz as duas
missões do AOCS:

| Missão | Requisito | Comando no painel / Serial |
|---|---|---|
| Detumbling: de 60 rpm para menos de 5 rpm | HLR-ADCS-02, REG-10 | `det60` (começa a 60±3 rpm), `detlivre` (qualquer rotação ≥57 rpm) |
| Vetor solar: azimute da luz pelos 4 painéis | HLR-ADCS-01/03, REG-11 | `sol` |

| Pasta | O que é |
|---|---|
| `arduino/` | O sketch **como entregue**, congelado: referência, como `ultima_missao/`. Não editar; o `README.md` dela é o da equipe (como gravar, o painel, os avisos) |
| `painel/status.html` | A página do painel Wi-Fi, extraída de `arduino/AOCS_Cubetscheck/pagina_html.h`. `tools/aocs/gera_pagina_h.py` gera o `.h` dela byte a byte (CTest `aocs_pagina_h`) |

## Como ele fala com o resto do satélite hoje

Só pelo **Wi-Fi** (rede `Cubetscheck-ADCS`, `http://192.168.4.1`,
`GET /status.json`, `POST /cmd?c=...`) e pelo **Serial** (115200). Ele lê o
BMI088 como **mestre** do I²C o tempo todo, o que não cabe no contrato com o
OBC (`docs/icd/obc-aocs-i2c.md`, ADR-0017: o ESP32 é escravo em 0x2A fora da
tarefa). O que muda, e em que ordem, está no plano de 25/11:

1. o código no repositório (esta pasta);
2. o contrato v2 e o telecomando `AOCS_TASK`, com o ESP32 avisando o fim da
   missão nas janelas de consulta, e o resultado descendo na telemetria;
3. a aba AOCS da estação de solo com o visual deste painel;
4. o port para ESP-IDF (Arduino como componente), com o escravo I²C, em
   `idf/` — a última etapa antes de gravar na placa.

## Valores que vão mudar

- **Tensão máxima dos painéis: 1,8 V** (informado em 25/11). Os
  `V_MAX_PAINEL` do sketch (+X 2,64, −X 2,56, +Y 2,70, −Y 2,60 V) são os de
  2025 e vão ser medidos de novo; no port eles passam a ser ajustáveis e
  gravados no NVS. Valores medidos, com a data:

  | Data | +X | −X | +Y | −Y | Quem mediu |
  |---|---|---|---|---|---|
  | — | — | — | — | — | — |

## Pinos (de `config_pinos.h`)

BMI088 SDA 21 / SCL 22 (o barramento do satélite) · driver IN1 25, IN2 26,
IN3 27, EN 15 · AS5048A CS 5 (SCK 18, MISO 19, MOSI 23) · painéis +X 34,
−X 35, +Y 32, −Y 33 · LED 2 · BOOT 0.
