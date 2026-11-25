# AOCS Cubetscheck · firmware "lógica 2025" + painel de controle

**Versão:** 2026.11-OL · **Motor:** iPower GBM2804H-100T · Gama CubeDesign (UnB) · V CubeDesign 2026

Este firmware volta à lógica de detumbling que funcionou em 2025 (`Stability.ino`) e acrescenta o painel de controle web. **Não há calibração nem alinhamento da roda:** o motor gira em **malha aberta**, como em 2025, e o encoder serve só para mostrar a velocidade real da roda.

---

## 1. Como funciona o detumbling (igual a 2025)

A cada 10 ms (100 Hz):

1. lê o giroscópio Z (BMI088) e aplica o filtro exponencial (α = 0,2);
2. calcula a **velocidade da roda = GANHO × giro do satélite**, no **mesmo sentido** do giro. Assim a roda "absorve" o momento angular e o satélite desacelera;
3. aplica zona morta, histerese de direção, velocidade mínima (3 rad/s) e máxima;
4. muda a velocidade da roda em **rampa**, sem trancos.

O motor recebe essa velocidade em **malha aberta** (`velocity_openloop` do Simple FOC): o campo magnético gira na velocidade pedida e o rotor acompanha. Não precisa de encoder, alinhamento nem zero elétrico.

**Valores alterados em relação a 2025** (todos ajustáveis no painel):

| Ajuste | 2025 | Agora | Por quê |
|---|---|---|---|
| Ganho | 5 | **8** rad/s por °/s | em 2025 o giro era lido em °/s; o ganho continua nessa unidade |
| Velocidade máxima | 80 rad/s | **140 rad/s** (1337 rpm) | a roda de 2026 precisa de ~95 rad/s para absorver 60 rpm |
| Rampa | 5 rad/s² | **20 rad/s²** | com 5 rad/s², a roda levaria ~19 s para chegar lá |

Simulação com esses valores: de 60 rpm para menos de 5 rpm em **~4,5 s**, terminando em ~2 rpm. Com os valores exatos de 2025, o satélite ficaria em ~9 rpm e **não cumpriria** o requisito.

A tensão no motor acompanha a velocidade (0,5 A × 10 Ω + força contraeletromotriz, até 6,9 V), para o rotor não perder o passo em alta rotação.

## 2. Gravar

| Item | Valor |
|---|---|
| Placa | **ESP32 Dev Module** · core "esp32 by Espressif" **3.3.12** |
| Biblioteca | **Simple FOC 2.4.0** |
| Monitor Serial | 115200, "Nova linha" |

1. Abra `AOCS_Cubetscheck/AOCS_Cubetscheck.ino` (todos os arquivos da pasta fazem parte do firmware) e grave.
2. No Monitor Serial devem aparecer:
   ```
   # BMI088: ok
   # Driver DRV8313: ok | ganho 8.0, max 140 rad/s, rampa 20 rad/s2, sentido +1
   # Wi-Fi "Cubetscheck-ADCS" (senha cubesat2026) -> http://192.168.4.1
   ```
3. Ao ligar, o firmware **não mexe na roda**. Ela só gira quando um modo é escolhido.

## 3. Painel

Conecte o celular ou notebook na rede **`Cubetscheck-ADCS`** (senha **`cubesat2026`**) e abra **http://192.168.4.1**. Se o celular abrir sozinho uma tela de "entrar na rede", feche e use o navegador normal.

| Botão | O que faz |
|---|---|
| **VETOR SOLAR** | Roda desligada; mostra o azimute da luz (0° = +X, 90° = +Y), como o `SolarVector` de 2025. |
| **DETUMBLING · INICIAR A 60 RPM** | Calibra o giro (1 s parado) e arma. Gire o satélite e solte: a roda começa quando, já solto, o giro estiver em 60 rpm (±3). Se soltar mais rápido, ele espera cair até 63 rpm. |
| **DETUMBLING · QUALQUER ROTAÇÃO** | Igual, mas começa assim que soltar, a partir de 57 rpm (inclusive bem mais alto). |
| **PARAR** | Desliga o motor na hora: a roda gira solta até parar. O botão **BOOT** da ESP32 (1,5 s) faz o mesmo. |
| **Testar roda** | A roda sobe até 300 rpm, fica 3 s e volta a zero. |
| **Ajustes** | Ganho, velocidade máxima e rampa (botão **Aplicar**) e **Inverter sentido**. Ficam gravados na ESP32. Só com a roda parada. |

O painel mostra o estado e o que fazer, o gráfico da velocidade, o cronômetro do requisito, a roda (comandada e **medida pelo encoder**), o vetor solar, a **sombra digital** (satélite 3D girando com a atitude medida) e o sistema.

**Depois do detumbling** (CONCLUÍDO), a roda continua girando, porque guarda o giro do satélite. Ao escolher outro modo, ela **desacelera em rampa**: **segure o satélite** até ela parar.

## 4. Primeiro teste (um passo de cada vez)

1. **Testar roda com o satélite seguro na mão.** A roda deve girar e o cartão *Roda de reação* deve mostrar a **medida (encoder) acompanhando a comandada** (~300 rpm). Se a roda não girar, confira o 12 V, os fios M1/M2/M3 e o EN (GPIO 15). Se girar, mas aparecer "Roda fora do passo", aumente a rampa devagar ou diminua o máximo.
2. **Conferir o sentido, com o satélite pendurado e solto.** Aperte **Testar roda** e olhe a velocidade do satélite: enquanto a roda acelera, ela deve ficar **negativa** (o satélite gira ao contrário da roda, vários rpm). Se ficar **positiva**, aperte **Inverter sentido**.
3. **Detumbling · qualquer rotação**, girando o satélite a ~60 rpm. Se o satélite **acelerar**, o firmware desliga o motor sozinho e pede para inverter o sentido.
4. **Detumbling · iniciar a 60 rpm** para a medição oficial do requisito.

## 5. Avisos do painel

| Aviso | Significado |
|---|---|
| **Satélite ACELEROU: inverta o sentido** | O giro subiu mais de 5 rpm no detumbling: a roda estava girando para o lado errado. O motor foi desligado. |
| **Roda fora do passo** | A velocidade medida pelo encoder é muito diferente da comandada: o rotor não acompanhou o campo. Diminua a rampa ou o máximo. |
| **Roda na velocidade máxima** | O giro inicial é alto demais para a roda absorver tudo. |
| **Driver não iniciou** | O DRV8313 não respondeu: confira o 12 V e a placa. |
| **Satélite mexeu na calibração** | Ele balançou durante a medição do bias do giro: segure-o parado; ela recomeça sozinha. |

O encoder (AS5048A) **não é usado no controle**, só para mostrar a velocidade real. Se ele não responder, o detumbling funciona igual e o painel mostra "sem leitura".

## 6. Serial (opcional)

`sol`, `det60`, `detlivre`, `parar`, `teste`, `sentido`, `status`.

## 7. Valores no código (topo do `.ino`, seção AJUSTES)

| Constante | Valor | Origem |
|---|---|---|
| `GANHO`, `W_RODA_MAX`, `RAMPA`, `SENTIDO` | 8, 140 rad/s, 20 rad/s², +1 | padrões (o painel muda e grava) |
| `W_RODA_MIN`, `ZONA_MORTA`, `LIMIAR_DIR`, `ALFA_FILTRO` | 3 rad/s, 0,02 °/s, 0,3 °/s, 0,2 | iguais a 2025 |
| `R_FASE`, `KV`, `I_OL`, `U_MAX` | 10 Ω, 154,5 rpm/V, 0,5 A, 6,9 V | GBM2804H-100T com 12 V |
| `V_MAX_PAINEL` (+X, −X, +Y, −Y) | 2,64 / 2,56 / 2,70 / 2,60 V | valores de 2025 (tensão de cada painel com a lanterna de frente) |
| `SINAL_GIRO` | +1 | medido em bancada (eixo Z do chip para cima) |

**Pinos:** BMI088 SDA 21 / SCL 22 · driver IN1 25, IN2 26, IN3 27, EN 15 · AS5048A CS 5 (SCK 18, MISO 19, MOSI 23) · painéis +X 34, −X 35, +Y 32, −Y 33 · LED 2 · BOOT 0.

| Arquivo | Função |
|---|---|
| `AOCS_Cubetscheck.ino` | tudo: lógica de 2025, estados, motor em malha aberta, painel, vetor solar |
| `imu_bmi088.*` | leitura do giroscópio BMI088 (I2C, com recuperação do barramento) |
| `config_pinos.h` | pinos, rede Wi-Fi e versão |
| `pagina_html.h` | página do painel (gerada; fontes Open Sans embutidas) |
