/*
 * ina219.h — the INA219 current and voltage monitor, register logic only
 * (OBC-7, HLR-EPS-04).
 *
 * Talks to an abstract bus, like the SX1278 driver: the Linux bus is i2c-dev
 * (main.c, ADR-0012), the tests use an emulated chip. Which I2C bus the chip
 * sits on — the shared one with the AOCS ESP32, or the Pi's own I2C0 if the
 * PCB team accepts the proposal of 2026-10-24 — is only the device path in
 * the configuration.
 *
 * Kept from ../obc/src/drivers/ina219.c: the configuration word 0x399F
 * (32 V range, +/-320 mV shunt range, 12-bit, continuous) and the 4 mV bus
 * voltage step. Changed: the current is computed from the shunt voltage
 * (10 uV per bit) and the shunt resistance in the configuration, instead of
 * a calibration register that has to be rewritten after every brown-out of
 * the chip; and a failed read is reported as a failure, never as 0 V.
 *
 * What it measures (hardware finding A9): it sits after the battery's BMS,
 * so it sees the satellite's load, never the charging current.
 */

#ifndef OBCD_INA219_H
#define OBCD_INA219_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    /* Big-endian 16-bit registers, as the INA219 has them. 0 on success. */
    int (*write_reg16)(void *ctx, uint8_t reg, uint16_t value);
    int (*read_reg16)(void *ctx, uint8_t reg, uint16_t *value);
} ina219_bus_t;

#define INA219_REG_CONFIG 0x00u
#define INA219_REG_SHUNT  0x01u
#define INA219_REG_BUS    0x02u

#define INA219_CONFIG_VALUE 0x399Fu
#define INA219_DEFAULT_ADDR 0x40u

/* Writes the configuration. 0, or -1 if the bus failed. */
int ina219_init(const ina219_bus_t *bus);

/* One reading. Returns 0 and fills the outputs, or -1: the bus failed, or
 * the chip reports an overflow (the math overflow flag, bit 0 of the bus
 * register), in which case the current is out of range and not reported.
 * shunt_mohm: the shunt resistance in milliohms, > 0. Current is positive
 * into the load. */
int ina219_read(const ina219_bus_t *bus, uint32_t shunt_mohm,
                uint16_t *battery_mv, int16_t *current_ma);

#ifdef __cplusplus
}
#endif

#endif /* OBCD_INA219_H */
