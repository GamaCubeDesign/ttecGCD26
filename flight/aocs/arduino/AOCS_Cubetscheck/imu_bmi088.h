// =========================================================================
//  IMU_BMI088.H - DRIVER DO BMI088 (I2C) COM RECUPERAÇÃO DO BARRAMENTO
// =========================================================================
// O BMI088 são dois sensores num chip:
//     acelerômetro -> 0x18 ou 0x19 (pino SDO1)
//     giroscópio   -> 0x68 ou 0x69 (pino SDO2)
// O driver testa os dois endereços de cada um.
//
// Configuração (diferente do gêmeo v0.x, que usava +-500 graus/s a 100 Hz):
//     giroscópio   : +-2000 graus/s, ODR 400 Hz, filtro 47 Hz
//                    (giro manual pode passar de 60 rpm = 360 graus/s)
//     acelerômetro : +-3 g, 100 Hz
//
// Robustez (o barramento é compartilhado com o OBC):
//     - toda transação tem timeout (I2C_TIMEOUT_MS);
//     - imu_recupera_barramento() gera 9 pulsos de clock + STOP para
//       soltar um escravo que ficou segurando o SDA, e reinicia o Wire.
//
// Eixos: os do CHIP. A conversão para o corpo usa P.sinal_giro (eixo Z
// do chip para cima, confirmado pela equipe).
//
// IMPORTANTE: o Wire não pode ser usado por duas tarefas ao mesmo tempo.
// No firmware final, só a tarefa de controle (núcleo 1) chama estas funções.
//
// ARQUIVO COMPARTILHADO: a mesma cópia é usada em todos os sketches.
// =========================================================================
#pragma once
#include <Arduino.h>

bool imu_inicia();                    // procura, reinicia e configura (true = os dois ok)
bool imu_le_giro(float g[3]);         // [rad/s] eixos do chip
bool imu_le_acel(float a[3]);         // [m/s^2] eixos do chip
bool imu_le_temp(float *t);           // [graus C]
uint8_t imu_endereco_acel();          // 0 = não encontrado
uint8_t imu_endereco_giro();          // 0 = não encontrado
void imu_recupera_barramento();       // destrava o I2C e reinicia o Wire
void imu_scan_i2c(Stream &s);         // lista os endereços que respondem
