// =========================================================================
//  CONFIG_PINOS.H - PINOS E CONSTANTES FIXAS DE HARDWARE
// =========================================================================
// Descrição:
//     Pinos da placa AOCS conforme o esquemático 2026 e constantes que
//     dependem só do hardware (não mudam com os testes de bancada).
//     Valores que DEPENDEM de bancada ficam em parametros.cpp, nunca aqui.
//
// Projeto:     Cubetscheck - Gama CubeDesign (UnB)
// Subsistema:  AOCS
// Evento:      V CubeDesign - Buenos Aires - 24/11/2026
//
// ARQUIVO COMPARTILHADO: a mesma cópia é usada em todos os sketches.
// Se alterar aqui, copie para os outros (ou rode ferramentas/sincroniza).
// =========================================================================
#pragma once
#include <stdint.h>

// ----- I2C: BMI088 (e barramento do satélite) ----------------------------
// No esquemático os rótulos SDA/SCL do barramento estão trocados; na placa
// fabricada estão corretos (confirmado pela equipe em 02/11).
constexpr int PINO_SDA = 21;
constexpr int PINO_SCL = 22;
constexpr uint32_t I2C_FREQ_HZ   = 400000;   // [Hz] modo rápido
constexpr uint16_t I2C_TIMEOUT_MS = 5;       // [ms] limite de cada transação

// ----- SPI: encoder AS5048A (VSPI padrão da ESP32) ------------------------
constexpr int PINO_SPI_SCK  = 18;
constexpr int PINO_SPI_MISO = 19;
constexpr int PINO_SPI_MOSI = 23;
constexpr int PINO_AS5048_CS = 5;

// ----- Driver SimpleFOC Mini v1.0 (DRV8313) -------------------------------
// nFT, nSP e nRT não estão ligados: nSP e nRT têm pull-up na placa.
// O 3V3 do módulo é SAÍDA do regulador interno e fica desconectado.
constexpr int PINO_IN1 = 25;
constexpr int PINO_IN2 = 26;
constexpr int PINO_IN3 = 27;
constexpr int PINO_EN  = 15;   // strapping pin: tem pull-up durante o boot

// ----- Painéis solares (ADC1, funcionam com Wi-Fi ligado) -----------------
// Ordem usada em TODO o firmware e na página: +X, -X, +Y, -Y
constexpr int PINO_PAINEL[4] = {34, 35, 32, 33};
constexpr int PAINEL_XP = 0, PAINEL_XM = 1, PAINEL_YP = 2, PAINEL_YM = 3;

// ----- Interface com o operador --------------------------------------------
constexpr int PINO_LED   = 2;   // LED azul da DevKit
constexpr int PINO_BOTAO = 0;   // botão BOOT da DevKit (ativo em nível baixo)

// ----- Motor iPower GBM2804H-100T (12N14P, 10 ohm) ------------------------
constexpr int PARES_DE_POLOS = 7;            // 12N14P

// ----- Rede -----------------------------------------------------------------
#define AP_NOME   "Cubetscheck-ADCS"
#define AP_SENHA  "cubesat2026"

// ----- Taxas ---------------------------------------------------------------
constexpr uint32_t TAXA_CONTROLE_HZ   = 400; // [Hz] laço de controle (giro a 400 Hz)
constexpr uint32_t TAXA_SOL_HZ        = 20;  // [Hz] leitura dos painéis

// ----- Conversões ----------------------------------------------------------
constexpr float RPM_PARA_RADS = 0.10471975512f;  // 2*pi/60
constexpr float RADS_PARA_RPM = 9.54929658551f;  // 60/(2*pi)

// Versão do firmware (aparece no Serial e na página)
#define FW_VERSAO "2026.11-OL"
