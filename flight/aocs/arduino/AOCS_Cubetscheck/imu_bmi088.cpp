// =========================================================================
//  IMU_BMI088.CPP - IMPLEMENTAÇÃO (base: esp32_bmi088.ino do gêmeo v0.x)
// =========================================================================
#include "imu_bmi088.h"
#include "config_pinos.h"
#include <Wire.h>

// ----- Registradores do acelerômetro --------------------------------------
static const uint8_t ACC_END_A = 0x18, ACC_END_B = 0x19;
static const uint8_t ACC_CHIP_ID = 0x00, ACC_ID_ESPERADO = 0x1E;
static const uint8_t ACC_DADOS = 0x12, ACC_TEMP = 0x22;
static const uint8_t ACC_CONF = 0x40, ACC_RANGE = 0x41;
static const uint8_t ACC_PWR_CONF = 0x7C, ACC_PWR_CTRL = 0x7D, ACC_SOFTRESET = 0x7E;
static const float   ACC_FUNDO_ESCALA = 3.0f;        // [g] com ACC_RANGE = 0x00

// ----- Registradores do giroscópio ----------------------------------------
static const uint8_t GYR_END_A = 0x68, GYR_END_B = 0x69;
static const uint8_t GYR_CHIP_ID = 0x00, GYR_ID_ESPERADO = 0x0F;
static const uint8_t GYR_DADOS = 0x02, GYR_RANGE = 0x0F, GYR_BANDWIDTH = 0x10;
static const uint8_t GYR_LPM1 = 0x11, GYR_SOFTRESET = 0x14;
// GYR_RANGE: 0x00 = +-2000 graus/s
static const uint8_t GYR_RANGE_VALOR = 0x00;
static const float   GYR_FUNDO_ESCALA = 2000.0f;     // [graus/s]
// GYR_BANDWIDTH: 0x03 = ODR 400 Hz, filtro 47 Hz
// (0x02 = 1000 Hz/116 Hz; 0x04 = 200 Hz/23 Hz). [F4:AJUSTE se houver
//  muita vibração da roda no sinal, teste 0x04]
static const uint8_t GYR_BANDA_VALOR = 0x03;

static const uint8_t CMD_SOFTRESET = 0xB6;
static const float   G0 = 9.80665f;

static uint8_t acc_end = 0;
static uint8_t gyr_end = 0;

// ----- I2C básico -----------------------------------------------------------
static bool i2c_escreve(uint8_t end, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(end);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool i2c_le(uint8_t end, uint8_t reg, uint8_t *buf, int n) {
  Wire.beginTransmission(end);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)end, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static uint8_t i2c_procura(uint8_t a, uint8_t b, uint8_t reg_id, uint8_t id) {
  uint8_t cand[2] = {a, b};
  for (int i = 0; i < 2; i++) {
    uint8_t lido = 0;
    if (i2c_le(cand[i], reg_id, &lido, 1) && lido == id) return cand[i];
  }
  return 0;
}

// ----- Inicialização ----------------------------------------------------------
static bool acc_inicia() {
  acc_end = i2c_procura(ACC_END_A, ACC_END_B, ACC_CHIP_ID, ACC_ID_ESPERADO);
  if (!acc_end) return false;
  i2c_escreve(acc_end, ACC_SOFTRESET, CMD_SOFTRESET);
  delay(5);
  // Após o reset o acelerômetro fica SUSPENSO: ativar
  i2c_escreve(acc_end, ACC_PWR_CONF, 0x00);   // modo ativo
  delay(5);
  i2c_escreve(acc_end, ACC_PWR_CTRL, 0x04);   // liga
  delay(50);
  i2c_escreve(acc_end, ACC_CONF, 0xA8);       // filtro normal, 100 Hz
  i2c_escreve(acc_end, ACC_RANGE, 0x00);      // +-3 g
  delay(2);
  return true;
}

static bool gyr_inicia() {
  gyr_end = i2c_procura(GYR_END_A, GYR_END_B, GYR_CHIP_ID, GYR_ID_ESPERADO);
  if (!gyr_end) return false;
  i2c_escreve(gyr_end, GYR_SOFTRESET, CMD_SOFTRESET);
  delay(30);
  i2c_escreve(gyr_end, GYR_RANGE, GYR_RANGE_VALOR);
  i2c_escreve(gyr_end, GYR_BANDWIDTH, GYR_BANDA_VALOR);
  i2c_escreve(gyr_end, GYR_LPM1, 0x00);       // modo normal
  delay(2);
  return true;
}

bool imu_inicia() {
  Wire.begin(PINO_SDA, PINO_SCL);
  Wire.setClock(I2C_FREQ_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  delay(20);
  bool a = acc_inicia();
  bool g = gyr_inicia();
  return a && g;
}

// ----- Leitura -----------------------------------------------------------------
static inline int16_t junta(uint8_t lsb, uint8_t msb) {
  return (int16_t)((msb << 8) | lsb);
}

bool imu_le_giro(float g[3]) {
  if (!gyr_end) return false;
  uint8_t raw[6];
  if (!i2c_le(gyr_end, GYR_DADOS, raw, 6)) return false;
  const float k = GYR_FUNDO_ESCALA / 32768.0f * (float)(PI / 180.0);
  for (int i = 0; i < 3; i++) g[i] = junta(raw[2*i], raw[2*i+1]) * k;
  return true;
}

bool imu_le_acel(float a[3]) {
  if (!acc_end) return false;
  uint8_t raw[6];
  if (!i2c_le(acc_end, ACC_DADOS, raw, 6)) return false;
  const float k = ACC_FUNDO_ESCALA / 32768.0f * G0;
  for (int i = 0; i < 3; i++) a[i] = junta(raw[2*i], raw[2*i+1]) * k;
  return true;
}

bool imu_le_temp(float *t) {
  if (!acc_end) return false;
  uint8_t raw[2];
  if (!i2c_le(acc_end, ACC_TEMP, raw, 2)) return false;
  int16_t v = (int16_t)((raw[0] << 3) | (raw[1] >> 5));
  if (v > 1023) v -= 2048;
  *t = v * 0.125f + 23.0f;
  return true;
}

uint8_t imu_endereco_acel() { return acc_end; }
uint8_t imu_endereco_giro() { return gyr_end; }

// ----- Recuperação do barramento ---------------------------------------------
// Se um escravo (BMI088 ou OBC) parar no meio de uma leitura segurando o
// SDA em nível baixo, o mestre não consegue mais gerar START. A saída
// padrão é gerar até 9 pulsos de clock (o escravo termina de enviar o
// byte e solta o SDA) e então uma condição de STOP.
void imu_recupera_barramento() {
  Wire.end();
  pinMode(PINO_SDA, INPUT_PULLUP);
  pinMode(PINO_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(PINO_SCL, HIGH);
  delayMicroseconds(5);
  for (int i = 0; i < 9 && digitalRead(PINO_SDA) == LOW; i++) {
    digitalWrite(PINO_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PINO_SCL, HIGH);
    delayMicroseconds(5);
  }
  // STOP: SDA sobe com SCL alto
  pinMode(PINO_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PINO_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PINO_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PINO_SDA, HIGH);
  delayMicroseconds(5);
  Wire.begin(PINO_SDA, PINO_SCL);
  Wire.setClock(I2C_FREQ_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
}

void imu_scan_i2c(Stream &s) {
  s.print(F("# I2C respondendo em:"));
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      s.printf(" 0x%02X", a);
      n++;
    }
  }
  s.println(n ? "" : " (nenhum)");
}
