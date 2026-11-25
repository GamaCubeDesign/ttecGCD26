// =========================================================================
//  AOCS_CUBETSCHECK.INO - FIRMWARE FINAL "LÓGICA DO ANO PASSADO" + PAINEL
// =========================================================================
// Detumbling com a MESMA lógica do Stability.ino de 2025:
//   - roda em MALHA ABERTA de velocidade (Simple FOC velocity_openloop):
//     NÃO usa encoder nem alinhamento para girar o motor;
//   - velocidade da roda = GANHO x giro do satélite (mesmo sentido do giro,
//     a roda "absorve" o momento angular), com zona morta, histerese de
//     direção, velocidade mínima, saturação e RAMPA;
//   - laço de controle a 100 Hz com filtro exponencial no giroscópio.
// Diferenças em relação a 2025 (necessárias com o hardware de 2026):
//   - giroscópio BMI088 (o código de 2025 lia o MPU9250 em graus/s achando
//     que era rad/s; aqui as unidades são explícitas e o GANHO continua
//     em "rad/s da roda por grau/s do satélite", como na prática era);
//   - roda mais pesada e motor GBM2804H-100T: velocidade máxima 140 rad/s
//     e rampa 20 rad/s^2 (em 2025: 80 rad/s e 5 rad/s^2), para conseguir
//     absorver 60 rpm em poucos segundos;
//   - tensão do motor ajustada à velocidade (I x R + força contraeletromotriz)
//     para o rotor não perder o passo em alta rotação.
//
// Painel web (sem cabo): rede "Cubetscheck-ADCS" (senha cubesat2026)
//   -> http://192.168.4.1
//   VETOR SOLAR · DETUMBLING (início a 60 rpm | qualquer rotação) · PARAR
//   + ajustes do detumbling (ganho, velocidade máxima, rampa, sentido).
//
// Hardware: ESP32 DevKit 30 pinos · BMI088 (I2C 21/22) · SimpleFOC Mini
//   (DRV8313: IN1 25, IN2 26, IN3 27, EN 15) · GBM2804H-100T (7 pares de
//   polos, 10 ohm) · AS5048A (SPI, CS 5) só para MOSTRAR a velocidade real
//   · painéis +X 34, -X 35, +Y 32, -Y 33.
// Software: core esp32 3.3.12 · Simple FOC 2.4.0 · "ESP32 Dev Module".
//
// Núcleo 1: loop() = FOC em malha aberta (rápido) · tarefa_controle 100 Hz
// Núcleo 0: tarefa_rede (painel) · painéis solares
// Projeto: Cubetscheck - Gama CubeDesign (UnB) · V CubeDesign 2026
// =========================================================================
#include <Arduino.h>
#include <SimpleFOC.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "config_pinos.h"
#include "imu_bmi088.h"
#include "pagina_html.h"

// =========================================================================
//  AJUSTES (valores iniciais; o painel muda e grava ganho/máx/rampa/sentido)
// =========================================================================
// --- Lógica do Stability.ino (2025) ---
static float GANHO        = 8.0f;    // [rad/s da roda por grau/s do satélite] (2025: 5)
static float W_RODA_MAX   = 140.0f;  // [rad/s] ~1340 rpm (2025: 80)
static float RAMPA        = 20.0f;   // [rad/s por segundo] (2025: 0,05 a cada 10 ms = 5)
static float SENTIDO      = 1.0f;    // +1/-1: sentido da roda (SIGN_CORRECTION de 2025)
const float  W_RODA_MIN   = 3.0f;    // [rad/s] mínimo para vencer a inércia (igual 2025)
const float  ZONA_MORTA   = 0.02f;   // [grau/s] abaixo disso: "parado" (igual 2025)
const float  LIMIAR_DIR   = 0.3f;    // [grau/s] histerese de direção (igual 2025)
const float  ALFA_FILTRO  = 0.2f;    // filtro exponencial do giro (igual 2025)
const uint32_t PERIODO_MS = 10;      // laço de controle 100 Hz (igual 2025)

// --- Sinal do giroscópio (eixo Z do chip para cima, medido em bancada) ---
const float SINAL_GIRO = 1.0f;

// --- Motor GBM2804H-100T em malha aberta ---
const float V_FONTE   = 12.0f;   // [V] EPS
const float U_MAX     = 6.9f;    // [V] máximo por fase com SVPWM e 12 V
const float R_FASE    = 10.0f;   // [ohm] datasheet
const float KV        = 154.5f;  // [rpm/V] datasheet (1468-1622 rpm a 10 V)
const float I_OL      = 0.5f;    // [A] corrente "de trabalho" em malha aberta

// --- Início do detumbling e requisito ---
const float W_ALVO_RPM  = 60.0f;   // opção "60 rpm": começa em 60 +- W_TOL
const float W_TOL_RPM   = 3.0f;
const float W_ARMA_RPM  = 20.0f;   // acima disso alguém está girando
const float ALFA_SOLTO  = 1.5f;    // [rad/s^2] |aceleração| abaixo disso = solto
const float T_SOLTO     = 0.25f;   // [s]
const float W_META_RPM  = 5.0f;    // requisito: < 5 rpm
const float T_REQ_MAX   = 10.0f;   // [s]
const float T_CONFIRMA  = 2.0f;    // [s] abaixo da meta por 2 s = concluído
const float SUBIDA_SUSP = 5.0f;    // [rpm] giro SUBIU isso: sentido da roda errado

// --- Painéis solares (lógica do SolarVector de 2025) ---
// Tensão de cada painel com a lanterna de frente (0 grau). Ordem: +X, -X, +Y, -Y.
static float V_MAX_PAINEL[4] = {2.64f, 2.56f, 2.70f, 2.60f};
const float  LUZ_MINIMA = 0.3f;    // [V] soma das 4 faces para "há luz"

// =========================================================================
//  CONSTANTES E ESTADOS
// =========================================================================
static const float RPM = 0.10471976f;          // rpm -> rad/s
static const float GRAUS = 57.2957795f;        // rad -> grau

enum Estado : uint8_t {
  E_CALIBRANDO = 1, E_ARMADO = 2, E_GIRANDO = 3, E_DETUMBLING = 4,
  E_CONCLUIDO = 5, E_DESCARGA = 6, E_PRONTO = 7, E_FALHA = 8, E_SOL = 9, E_TESTE = 10
};
enum Pedido : uint8_t { P_NADA = 0, P_SOL = 1, P_DET60 = 2, P_DETLIVRE = 3, P_TESTE = 4, P_PARAR = 6 };
enum Sub : uint8_t { S_NADA = 0, S_SEGURANDO = 1, S_ESPERA_CAIR = 2, S_GIRE_MAIS = 3 };
// Flags (mesmos bits que o painel entende)
enum : uint16_t {
  F_RODA_MAX = 1u << 1, F_REQ_OK = 1u << 3, F_REQ_FALHOU = 1u << 4, F_IMU_FALHA = 1u << 5,
  F_PERDEU_PASSO = 1u << 6, F_SOL_VALIDO = 1u << 7, F_CALIB_MEXEU = 1u << 8,
  F_SENTIDO_SUSP = 1u << 11, F_MOTOR_LIGADO = 1u << 12, F_DRIVER_FALHOU = 1u << 14
};

// =========================================================================
//  HARDWARE
// =========================================================================
BLDCMotor         motor(PARES_DE_POLOS);
BLDCDriver3PWM    driver(PINO_IN1, PINO_IN2, PINO_IN3, PINO_EN);
MagneticSensorSPI encoder(AS5048_SPI, PINO_AS5048_CS);   // só leitura (não ligado ao motor)
WebServer         servidor(80);
Preferences       nvs;

// =========================================================================
//  DADOS COMPARTILHADOS ENTRE NÚCLEOS
// =========================================================================
struct Dados {
  uint8_t est, sub, opc, alvo;
  uint16_t flags;
  float w;            // [rad/s] satélite (filtrado, sem bias)
  float yaw;          // [rad] ângulo integrado (animação)
  float wr_cmd;       // [rad/s] roda comandada (no corpo)
  float wr_med;       // [rad/s] roda medida pelo encoder (no corpo)
  float uq;           // [V]
  float tm, treq, w0; // [s], [s], [rad/s]
  float bias;         // [rad/s]
  float temp;
  float painel[4], n[4], az, si;
  bool  sv, imu_ok, enc_ok;
  float fctl, ffoc;
};
static Dados D = {};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static void le_dados(Dados &d) { portENTER_CRITICAL(&mux); d = D; portEXIT_CRITICAL(&mux); }

static volatile uint8_t pedido = P_NADA;
static volatile float   w_roda_eixo = 0.0f;     // [rad/s] comando para o motor (no eixo)
static volatile bool    motor_quer = false;     // ligar o driver?
static volatile bool    driver_ok = false;
static volatile float   wr_medida = 0.0f;       // [rad/s] encoder, no eixo
static volatile bool    enc_ok = false;
static volatile float   ffoc = 0.0f;
static const char * volatile msg_evento = "";
static uint32_t         n_evento = 0;

// =========================================================================
//  LÓGICA DE 2025 (Stability.ino), com unidades explícitas
// =========================================================================
static float giro_filtrado = 0.0f;     // [grau/s]
static int   ultima_direcao = 0;       // -1, 0, +1

// Velocidade alvo da roda [rad/s, no corpo] a partir do giro [grau/s]
static float velocidade_alvo_roda(float giro_gs) {
  float a = fabsf(giro_gs);
  if (a < ZONA_MORTA) return 0.0f;                     // zona morta
  int sinal_agora = giro_gs > 0 ? 1 : -1;
  if (a > LIMIAR_DIR) ultima_direcao = sinal_agora;    // histerese de direção
  if (ultima_direcao == 0) return 0.0f;
  float alvo = GANHO * a * (float)ultima_direcao;      // mesmo sentido do giro
  if (fabsf(alvo) < W_RODA_MIN) alvo = ultima_direcao >= 0 ? W_RODA_MIN : -W_RODA_MIN;
  return constrain(alvo, -W_RODA_MAX, W_RODA_MAX);     // saturação
}

static float rampa(float atual, float alvo, float passo) {
  float d = alvo - atual;
  return atual + constrain(d, -passo, passo);
}

// =========================================================================
//  MOTOR EM MALHA ABERTA (loop(), núcleo 1)
// =========================================================================
static float bemf(float w) { return fabsf(w) / (KV * _SQRT3) / _RPM_TO_RADS; }

static void executa_motor() {
  if (!driver_ok) { delay(1); return; }
  bool q = motor_quer;
  if (q && !motor.enabled) motor.enable();
  if (!q && motor.enabled) motor.disable();
  if (motor.enabled) {
    float w = w_roda_eixo;
    // Tensão = corrente de trabalho x R + força contraeletromotriz (limitada)
    float u = I_OL * R_FASE + bemf(w);
    motor.voltage_limit = u < U_MAX ? u : U_MAX;
    motor.loopFOC();             // na Simple FOC 2.4 a malha aberta precisa dos dois
    motor.move(w);
  }
}

// =========================================================================
//  TAREFA DE CONTROLE (núcleo 1, 100 Hz)
// =========================================================================
static void tarefa_controle(void *) {
  uint8_t est = E_CALIBRANDO, sub = S_NADA, opc = 0, alvo = P_NADA;
  uint16_t flags_lat = 0;
  float bias = 0, yaw = 0, w_ant = 0, alfa_f = 0;
  float t_estado = 0, acum = 0, acum_parou = 0, t0 = -1, treq = -1, w0 = 0;
  double soma = 0, soma2 = 0; uint32_t n = 0;
  float wr_cmd = 0;                    // [rad/s] roda comandada, no corpo
  float g[3] = {0, 0, 0}, temp = 0;
  uint32_t falhas = 0, ciclo = 0, n_taxa = 0;
  uint32_t t_taxa = millis();
  float fctl = 0;
  const float dt = PERIODO_MS / 1000.0f;
  TickType_t ultimo = xTaskGetTickCount();

  auto entra = [&](uint8_t e, float t) {
    est = e; t_estado = t; acum = 0; acum_parou = 0; sub = S_NADA;
    if (e == E_CALIBRANDO) { soma = soma2 = 0; n = 0; }
  };

  for (;;) {
    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(PERIODO_MS));
    float t = millis() / 1000.0f;
    ciclo++;

    // ---- Giroscópio --------------------------------------------------------
    float gl[3];
    if (imu_le_giro(gl)) { for (int i = 0; i < 3; i++) g[i] = gl[i]; falhas = 0; }
    else if (++falhas % 50 == 0) { imu_recupera_barramento(); imu_inicia(); }
    bool imu_ok = falhas < 25;
    if (ciclo % 100 == 0) imu_le_temp(&temp);
    float gz = SINAL_GIRO * g[2];                        // [rad/s] corpo, com bias
    float wz = gz - bias;
    giro_filtrado += ALFA_FILTRO * (wz * GRAUS - giro_filtrado);   // [grau/s]
    float w = giro_filtrado / GRAUS;                     // [rad/s]
    alfa_f += 0.1f * ((w - w_ant) / dt - alfa_f);
    w_ant = w;
    yaw += w * dt;
    if (yaw > PI) yaw -= 2 * PI;
    if (yaw < -PI) yaw += 2 * PI;

    // ---- Pedidos do painel ----------------------------------------------------
    uint8_t p = pedido;
    pedido = P_NADA;
    if (p == P_PARAR) {
      wr_cmd = 0; w_roda_eixo = 0; motor_quer = false;  // driver desligado: roda solta
      alvo = P_NADA;
      entra(E_PRONTO, t);
    } else if (p == P_TESTE) {
      // Teste da roda: só com a roda parada e fora de uma manobra
      if (fabsf(wr_cmd) < 0.5f && est != E_DETUMBLING && est != E_CONCLUIDO) { alvo = P_NADA; entra(E_TESTE, t); }
    } else if (p != P_NADA) {
      alvo = p;
      if (p == P_DET60) opc = 0;
      if (p == P_DETLIVRE) opc = 1;
      flags_lat = 0; t0 = -1; treq = -1; w0 = 0;
      entra(fabsf(wr_cmd) > 1.0f ? E_DESCARGA : E_CALIBRANDO, t);
    }
    if (!imu_ok && est != E_FALHA) { flags_lat |= F_IMU_FALHA; entra(E_FALHA, t); }

    // ---- Estados ------------------------------------------------------------------
    float alvo_roda = wr_cmd;            // por padrão a roda mantém a velocidade
    float passo = RAMPA * dt;
    switch (est) {
      case E_CALIBRANDO: {                // bias do giro: 1 s parado
        alvo_roda = 0;
        soma += gz; soma2 += (double)gz * gz; n++;
        if (t - t_estado >= 1.0f && n > 20) {
          double m = soma / n, dp = sqrt(fmax(0.0, soma2 / n - m * m));
          if (dp < 0.01 && fabs(m) < 0.2) {
            bias = (float)m;
            giro_filtrado = 0; ultima_direcao = 0; w_ant = 0; alfa_f = 0;
            flags_lat &= ~F_CALIB_MEXEU;
            if (alvo == P_SOL) entra(E_SOL, t);
            else if (alvo == P_DET60 || alvo == P_DETLIVRE) entra(E_ARMADO, t);
            else entra(E_PRONTO, t);
          } else { flags_lat |= F_CALIB_MEXEU; entra(E_CALIBRANDO, t); }
        }
        break;
      }
      case E_ARMADO:
        alvo_roda = 0;
        if (fabsf(w) > W_ARMA_RPM * RPM) entra(E_GIRANDO, t);
        break;
      case E_GIRANDO: {
        alvo_roda = 0;
        if (fabsf(w) < 0.5f * W_ARMA_RPM * RPM) acum_parou += dt; else acum_parou = 0;
        if (acum_parou > 1.0f) { entra(E_ARMADO, t); break; }
        if (fabsf(alfa_f) < ALFA_SOLTO) acum += dt; else acum = 0;
        float a = fabsf(w) / RPM;
        if (acum < T_SOLTO)                                   sub = S_SEGURANDO;
        else if (a < W_ALVO_RPM - W_TOL_RPM)                  sub = S_GIRE_MAIS;
        else if (opc == 0 && a > W_ALVO_RPM + W_TOL_RPM)      sub = S_ESPERA_CAIR;
        else {
          t0 = t; w0 = w; treq = -1;
          flags_lat &= ~(F_REQ_OK | F_REQ_FALHOU | F_SENTIDO_SUSP);
          ultima_direcao = 0;
          entra(E_DETUMBLING, t);
        }
        break;
      }
      case E_DETUMBLING:
      case E_CONCLUIDO: {
        alvo_roda = velocidade_alvo_roda(giro_filtrado);       // LÓGICA DE 2025
        float tm = t - t0;
        if (treq < 0 && fabsf(w) < W_META_RPM * RPM) {
          treq = tm;
          flags_lat |= treq <= T_REQ_MAX ? F_REQ_OK : F_REQ_FALHOU;
        }
        if (treq < 0 && tm > T_REQ_MAX) flags_lat |= F_REQ_FALHOU;
        if (est == E_DETUMBLING) {
          // Segurança: o giro AUMENTOU -> a roda gira no sentido errado
          if (tm > 0.3f && fabsf(w) > fabsf(w0) + SUBIDA_SUSP * RPM) {
            flags_lat |= F_SENTIDO_SUSP;
            wr_cmd = 0; w_roda_eixo = 0; motor_quer = false;
            alvo = P_NADA;
            msg_evento = "Detumbling interrompido: o satelite acelerou. Troque o SENTIDO da roda nos ajustes.";
            n_evento++;
            entra(E_PRONTO, t);
            break;
          }
          if (fabsf(w) < W_META_RPM * RPM) acum += dt; else acum = 0;
          if (acum >= T_CONFIRMA) entra(E_CONCLUIDO, t);
        }
        break;
      }
      case E_DESCARGA:                     // roda desacelera em rampa (segure o satélite)
        alvo_roda = 0;
        if (fabsf(wr_cmd) < 0.5f) { wr_cmd = 0; entra(E_CALIBRANDO, t); }
        break;
      case E_FALHA:                        // giro sem resposta: roda mantém a velocidade
        alvo_roda = wr_cmd;
        if (imu_ok) { alvo = P_NADA; entra(E_CALIBRANDO, t); }
        break;
      case E_TESTE:                        // roda a 300 rpm por 3 s e volta a zero (segure o satélite)
        alvo_roda = (t - t_estado < 1.6f + 3.0f) ? 300.0f * RPM : 0.0f;
        if (t - t_estado > 4.6f && fabsf(wr_cmd) < 0.5f) { wr_cmd = 0; entra(E_PRONTO, t); motor_quer = false; }
        break;
      case E_SOL:
      case E_PRONTO:
      default:
        alvo_roda = 0;
        if (est == E_PRONTO && !motor_quer) wr_cmd = 0;   // depois de PARAR a roda está solta
        break;
    }

    // ---- Roda ---------------------------------------------------------------------
    if (est != E_PRONTO || motor_quer) {
      wr_cmd = rampa(wr_cmd, alvo_roda, passo);
      w_roda_eixo = SENTIDO * wr_cmd;
      motor_quer = fabsf(wr_cmd) > 0.3f || fabsf(alvo_roda) > 0.3f;
    }
    if (est == E_PRONTO && fabsf(wr_cmd) < 0.3f) motor_quer = false;

    if (++n_taxa >= 100) { fctl = n_taxa * 1000.0f / (millis() - t_taxa); n_taxa = 0; t_taxa = millis(); }

    uint16_t fl = flags_lat;
    if (fabsf(wr_cmd) >= W_RODA_MAX - 0.5f) fl |= F_RODA_MAX;
    if (motor_quer) fl |= F_MOTOR_LIGADO;
    if (!driver_ok) fl |= F_DRIVER_FALHOU;
    float wr_med_corpo = SENTIDO * wr_medida;
    if (enc_ok && fabsf(wr_cmd) > 20.0f && fabsf(wr_med_corpo - wr_cmd) > 0.3f * fabsf(wr_cmd)) fl |= F_PERDEU_PASSO;

    portENTER_CRITICAL(&mux);
    D.est = est; D.sub = sub; D.opc = opc; D.alvo = alvo; D.flags = fl | (D.sv ? F_SOL_VALIDO : 0);
    D.w = w; D.yaw = yaw; D.wr_cmd = wr_cmd; D.wr_med = wr_med_corpo; D.uq = motor.enabled ? motor.voltage_limit : 0;
    D.tm = (t0 >= 0 && (est == E_DETUMBLING || est == E_CONCLUIDO)) ? t - t0 : -1;
    D.treq = treq; D.w0 = w0; D.bias = bias; D.temp = temp; D.imu_ok = imu_ok;
    D.enc_ok = enc_ok; D.fctl = fctl; D.ffoc = ffoc;
    portEXIT_CRITICAL(&mux);
  }
}

// =========================================================================
//  PAINÉIS SOLARES (lógica do SolarVector de 2025) - núcleo 0, 20 Hz
// =========================================================================
static void mede_sol() {
  float v[4], n4[4], soma = 0;
  for (int i = 0; i < 4; i++) {
    uint32_t s = 0;
    for (int k = 0; k < 16; k++) s += analogReadMilliVolts(PINO_PAINEL[i]);
    v[i] = s / 16.0f / 1000.0f;
    soma += v[i];
    n4[i] = constrain(v[i] / V_MAX_PAINEL[i], 0.0f, 1.0f);
  }
  // delta_x = +X - (-X); delta_y = +Y - (-Y)  (ordem do vetor: +X, -X, +Y, -Y)
  float dx = constrain(n4[0] - n4[1], -1.0f, 1.0f);
  float dy = constrain(n4[2] - n4[3], -1.0f, 1.0f);
  float az = atan2f(dy, dx) * GRAUS;
  if (az < 0) az += 360.0f;
  portENTER_CRITICAL(&mux);
  for (int i = 0; i < 4; i++) { D.painel[i] = v[i]; D.n[i] = n4[i]; }
  D.az = az; D.si = soma / (V_MAX_PAINEL[0] + V_MAX_PAINEL[2]); D.sv = soma > LUZ_MINIMA;
  portEXIT_CRITICAL(&mux);
}

// =========================================================================
//  PAINEL WEB (núcleo 0)
// =========================================================================
static void salva_ajustes() {
  nvs.begin("aocs_ol", false);
  nvs.putFloat("ganho", GANHO); nvs.putFloat("wmax", W_RODA_MAX);
  nvs.putFloat("rampa", RAMPA); nvs.putFloat("sentido", SENTIDO);
  nvs.end();
}
static void carrega_ajustes() {
  nvs.begin("aocs_ol", true);
  GANHO = nvs.getFloat("ganho", GANHO); W_RODA_MAX = nvs.getFloat("wmax", W_RODA_MAX);
  RAMPA = nvs.getFloat("rampa", RAMPA); SENTIDO = nvs.getFloat("sentido", SENTIDO);
  nvs.end();
}

static String js(float x, int d = 2) { return isfinite(x) ? String(x, d) : String("null"); }

static void rota_status() {
  Dados d;
  le_dados(d);
  String s;
  s.reserve(900);
  s += "{\"est\":" + String(d.est) + ",\"sub\":" + d.sub + ",\"opc\":" + d.opc + ",\"alvo\":" + d.alvo;
  s += ",\"fl\":" + String(d.flags);
  s += ",\"w\":" + js(d.w / RPM, 2) + ",\"yaw\":" + js(d.yaw * GRAUS, 1);
  s += ",\"wr\":" + js(d.wr_cmd / RPM, 0) + ",\"wrm\":" + js(d.wr_med / RPM, 0) + ",\"wlim\":" + js(W_RODA_MAX / RPM, 0);
  s += ",\"uq\":" + js(d.uq, 2);
  s += ",\"tm\":" + js(d.tm, 2) + ",\"treq\":" + js(d.treq, 2) + ",\"w0\":" + js(d.w0 / RPM, 1);
  s += ",\"az\":" + js(d.az, 1) + ",\"si\":" + js(d.si, 2) + ",\"sv\":" + String(d.sv ? 1 : 0);
  s += ",\"p\":[" + js(d.painel[0]) + "," + js(d.painel[1]) + "," + js(d.painel[2]) + "," + js(d.painel[3]) + "]";
  s += ",\"n\":[" + js(d.n[0], 3) + "," + js(d.n[1], 3) + "," + js(d.n[2], 3) + "," + js(d.n[3], 3) + "]";
  s += ",\"temp\":" + js(d.temp, 1) + ",\"bias\":" + js(d.bias, 5);
  s += ",\"imu\":" + String(d.imu_ok ? 1 : 0) + ",\"enc\":" + String(d.enc_ok ? 1 : 0) + ",\"drv\":" + String(driver_ok ? 1 : 0);
  s += ",\"fctl\":" + js(d.fctl, 0) + ",\"ffoc\":" + js(d.ffoc, 0);
  s += ",\"fw\":\"" FW_VERSAO "\",\"up\":" + String(millis() / 1000) + ",\"cmd\":1";
  s += ",\"ganho\":" + js(GANHO, 2) + ",\"wmax\":" + js(W_RODA_MAX, 0) + ",\"rampa\":" + js(RAMPA, 1) + ",\"sent\":" + js(SENTIDO, 0);
  s += ",\"walvo\":" + js(W_ALVO_RPM, 0) + ",\"wtol\":" + js(W_TOL_RPM, 0);
  s += ",\"evn\":" + String(n_evento) + ",\"ev\":\"" + String((const char *)msg_evento) + "\"}";
  servidor.sendHeader("Cache-Control", "no-store");
  servidor.send(200, "application/json", s);
}

static void responde(bool ok, const char *msg) {
  String s = String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + msg + "\"}";
  servidor.send(ok ? 200 : 409, "application/json", s);
}

static void rota_cmd() {
  String c = servidor.arg("c");
  Dados d;
  le_dados(d);
  if      (c == "sol")      pedido = P_SOL;
  else if (c == "det60")    pedido = P_DET60;
  else if (c == "detlivre") pedido = P_DETLIVRE;
  else if (c == "parar")    pedido = P_PARAR;
  else if (c == "teste") {
    if (fabsf(d.wr_cmd) > 0.5f || d.est == E_DETUMBLING || d.est == E_CONCLUIDO) { responde(false, "pare a roda antes"); return; }
    pedido = P_TESTE;
  } else if (c == "sentido") {
    if (d.est == E_DETUMBLING || d.est == E_CONCLUIDO || fabsf(d.wr_cmd) > 1.0f) {
      responde(false, "pare a roda antes de trocar o sentido"); return;
    }
    SENTIDO = -SENTIDO; salva_ajustes();
  } else if (c == "ajustes") {
    float g = servidor.arg("g").toFloat(), m = servidor.arg("m").toFloat(), r = servidor.arg("r").toFloat();
    if (!(g > 0 && g <= 50 && m >= 10 && m <= 160 && r >= 1 && r <= 100)) {
      responde(false, "valores fora da faixa (ganho 0-50, max 10-160 rad/s, rampa 1-100)"); return;
    }
    GANHO = g; W_RODA_MAX = m; RAMPA = r; salva_ajustes();
  } else { responde(false, "comando desconhecido"); return; }
  Serial.printf("# painel: %s\n", c.c_str());
  responde(true, "ok");
}

static void tarefa_rede(void *) {
  uint32_t t_sol = 0;
  for (;;) {
    servidor.handleClient();
    if (millis() - t_sol >= 50) { t_sol = millis(); mede_sol(); }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// =========================================================================
//  SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PINO_LED, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  Serial.println(F("\n# ===== CUBETSCHECK AOCS " FW_VERSAO " (logica 2025 + painel) ====="));
  carrega_ajustes();

  bool imu = false;
  for (int k = 0; k < 3 && !imu; k++) { imu = imu_inicia(); if (!imu) { imu_recupera_barramento(); delay(100); } }
  Serial.printf("# BMI088: %s\n", imu ? "ok" : "FALHOU");

  for (int i = 0; i < 4; i++) { pinMode(PINO_PAINEL[i], INPUT); analogSetPinAttenuation(PINO_PAINEL[i], ADC_11db); }

  // Encoder: só para mostrar a velocidade real da roda
  encoder.init();
  encoder.update();

  // Motor em malha aberta (como em 2025): sem sensor, sem alinhamento
  driver.voltage_power_supply = V_FONTE;
  driver.voltage_limit        = V_FONTE;
  driver_ok = driver.init();
  if (driver_ok) {
    motor.linkDriver(&driver);
    motor.foc_modulation    = FOCModulationType::SpaceVectorPWM;
    motor.controller        = MotionControlType::velocity_openloop;
    motor.torque_controller = TorqueControlType::voltage;
    motor.voltage_limit     = I_OL * R_FASE;
    motor.velocity_limit    = 200.0f;
    motor.init();
    motor.disable();
  }
  Serial.printf("# Driver DRV8313: %s | ganho %.1f, max %.0f rad/s, rampa %.0f rad/s2, sentido %+.0f\n",
                driver_ok ? "ok" : "FALHOU", GANHO, W_RODA_MAX, RAMPA, SENTIDO);

  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NOME, AP_SENHA);
  servidor.on("/", []() { servidor.send_P(200, "text/html; charset=utf-8", PAGINA_HTML); });
  servidor.on("/status.json", rota_status);
  servidor.on("/cmd", HTTP_POST, rota_cmd);
  servidor.begin();
  Serial.printf("# Wi-Fi \"%s\" (senha %s) -> http://%s\n", AP_NOME, AP_SENHA, WiFi.softAPIP().toString().c_str());
  Serial.println(F("# Serial: sol | det60 | detlivre | parar | teste | sentido | status   (BOOT 1,5 s = parar)"));

  xTaskCreatePinnedToCore(tarefa_controle, "controle", 6144, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(tarefa_rede,     "rede",     6144, nullptr, 2, nullptr, 0);
}

// =========================================================================
//  LOOP (núcleo 1): motor em malha aberta + encoder + Serial/BOOT/LED
// =========================================================================
void loop() {
  static uint32_t t_vel = millis(), t_conta = millis(), n = 0, botao = 0, t_led = 0;
  static float a_ant = 0;
  static bool primeira = true;
  static String linha;

  executa_motor();
  n++;

  // Velocidade real da roda pelo encoder (a cada 50 ms)
  if (millis() - t_vel >= 50) {
    encoder.update();
    float a = encoder.getAngle();
    float dts = (millis() - t_vel) / 1000.0f;
    if (!primeira) wr_medida = 0.6f * wr_medida + 0.4f * (a - a_ant) / dts;
    static float a_min = 1e9, a_max = -1e9;     // encoder vivo = leitura muda ou é != 0
    a_min = fminf(a_min, a); a_max = fmaxf(a_max, a);
    enc_ok = (a_max - a_min) > 0.001f || fabsf(a) > 0.001f;
    primeira = false; a_ant = a; t_vel = millis();
  }
  if (millis() - t_conta >= 1000) { ffoc = n * 1000.0f / (millis() - t_conta); n = 0; t_conta = millis(); }

  // Serial
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (linha.length() < 30) linha += c; continue; }
    linha.trim();
    if (linha == "sol") pedido = P_SOL;
    else if (linha == "det60") pedido = P_DET60;
    else if (linha == "detlivre") pedido = P_DETLIVRE;
    else if (linha == "parar") pedido = P_PARAR;
    else if (linha == "teste") pedido = P_TESTE;
    else if (linha == "sentido") { SENTIDO = -SENTIDO; salva_ajustes(); Serial.printf("# sentido %+.0f\n", SENTIDO); }
    else if (linha == "status") {
      Dados d; le_dados(d);
      Serial.printf("# est %u | sat %.1f rpm | roda cmd %.0f rpm, medida %.0f rpm | Uq %.2f V | az %.1f | fl 0x%04X\n",
                    d.est, d.w / RPM, d.wr_cmd / RPM, d.wr_med / RPM, d.uq, d.az, d.flags);
    }
    linha = "";
  }
  // BOOT 1,5 s = PARAR
  if (digitalRead(PINO_BOTAO) == LOW) {
    if (botao == 0) botao = millis();
    if (botao != 1 && millis() - botao > 1500) { pedido = P_PARAR; botao = 1; Serial.println(F("# BOOT: PARAR")); }
  } else botao = 0;
  // LED: pisca rápido com o motor ligado
  if (millis() - t_led > 50) { t_led = millis(); digitalWrite(PINO_LED, motor_quer ? (millis() / 100) % 2 : (millis() / 1000) % 2); }
}
