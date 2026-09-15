/*
 * crc16.h — CRC-16/CCITT-FALSE
 *
 * Poly 0x1021, init 0xFFFF, no input/output reflection, final XOR 0x0000.
 * Check value for the ASCII string "123456789" is 0x29B1.
 *
 * Used as the application-layer integrity check of the OTA frame
 * (see docs/icd/ota-protocol-icd.md). The LoRa PHY already carries its own
 * CRC, but a PHY CRC failure makes the radio drop the packet silently: the
 * receiver cannot tell a corrupted frame from one that never arrived. The
 * application CRC exists so corruption can be *counted*, which is the
 * evidence required by HLR-ADS-08 (maximum acceptable data loss rate).
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
