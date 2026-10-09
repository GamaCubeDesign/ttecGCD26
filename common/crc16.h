/*
 * crc16.h — CRC-16/CCITT-FALSE
 *
 * Poly 0x1021, init 0xFFFF, no input/output reflection, final XOR 0x0000.
 * Check value for the ASCII string "123456789" is 0x29B1.
 *
 * Used as the application-layer integrity check of every GAMA frame
 * (common/gama_frame.h). The LoRa PHY already carries its own CRC, and the
 * SX1278 does report a PHY CRC failure (PayloadCrcError in RegIrqFlags; the
 * driver counts it, flight/radio/sx1278.c). The application CRC is still
 * needed, for three reasons:
 *
 *   - frames cross hops the PHY CRC never covers: the IPC sockets between
 *     the flight processes, and the UART between the ESP32 and the ground
 *     computer;
 *   - it does not depend on the modem being configured with its CRC on;
 *   - one integrity check on every transport means one place where a
 *     corrupted frame is rejected and counted (HLR-ADS-08 loss rate).
 *
 * An earlier version of this comment said the SX1278 drops PHY CRC failures
 * silently. It does not.
 *
 * This file is shared verbatim between the Raspberry Pi flight software and
 * the ESP32 ground station firmware. Keep it free of platform headers.
 */

#ifndef GAMA_CRC16_H
#define GAMA_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Seed value for an incremental computation. */
#define GAMA_CRC16_INIT 0xFFFFu

/* Folds len bytes of data into a running CRC. Pass GAMA_CRC16_INIT as crc to
 * start a new computation; pass the previous return value to continue one. */
uint16_t gama_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

/* One-shot convenience wrapper over gama_crc16_update(). */
uint16_t gama_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_CRC16_H */
