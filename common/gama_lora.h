/*
 * gama_lora.h — LoRa physical-layer parameters shared by both ends of the link.
 *
 * The three rate profiles of ADR-0004 are defined here, once. The satellite
 * and the ground station must agree on every field of a profile or they
 * cannot hear each other, and a disagreement would only show up as silence on
 * the day of the test. Compiling the same table into both ends is what makes
 * that disagreement impossible (ADR-0002).
 *
 * Time-on-air follows the SX1276/77/78/79 datasheet (Semtech, rev. 7),
 * §4.1.1.7, in integer microseconds. tools/analysis/lora_budget.py implements
 * the same formula independently; tests/test_lora.c checks the two agree.
 *
 * Shared verbatim between the Raspberry Pi and the ESP32. No floating point,
 * no allocation, no platform headers.
 */

#ifndef GAMA_LORA_H
#define GAMA_LORA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed for the mission: the RA-02 front end is matched for 433 MHz, and the
 * private-network sync word keeps us off LoRaWAN traffic (ADR-0004). */
#define GAMA_LORA_FREQ_HZ    433000000u
#define GAMA_LORA_SYNC_WORD  0x12u

/* The SX1278 FIFO is 256 bytes, but its payload length register is 8 bits and
 * a packet of 255 bytes is the largest the modem accepts. */
#define GAMA_LORA_MAX_PAYLOAD 255u

typedef struct {
    uint8_t  sf;              /* spreading factor, 7..12                  */
    uint32_t bw_hz;           /* bandwidth                                */
    uint8_t  cr;              /* coding rate denominator minus 4: 1 = 4/5 */
    uint16_t preamble_len;    /* programmed preamble, in symbols          */
    bool     crc_on;          /* PHY payload CRC                          */
    bool     implicit_header; /* false: explicit header carries the length */
} gama_lora_phy_t;

/*
 * The PHY settings for a gama_rate_profile_t value (common/gama_tc.h).
 *
 * Returns NULL for an index that is not a defined profile. Callers must treat
 * NULL as a rejected request, never fall back to a default: a silently
 * substituted profile is the desynchronisation this table exists to prevent.
 */
const gama_lora_phy_t *gama_lora_profile(uint8_t profile);

/* Duration of one symbol, 2^SF / BW, in microseconds. */
uint32_t gama_lora_symbol_us(const gama_lora_phy_t *phy);

/* Whether LowDataRateOptimize must be set. The datasheet mandates it when a
 * symbol lasts longer than 16 ms: SF11 and SF12 at 125 kHz. */
bool gama_lora_ldro(const gama_lora_phy_t *phy);

/* Time on air of one packet carrying payload_len bytes, in microseconds. */
uint32_t gama_lora_toa_us(const gama_lora_phy_t *phy, uint8_t payload_len);

/*
 * Time from the start of a transmission until a receiver has decoded the
 * explicit header: preamble plus the eight header symbols.
 *
 * This is the earliest moment a receiver can know a packet is arriving, so it
 * bounds how long a node must stay silent after a transmission for the other
 * side's reply to be detectable (ADR-0007).
 */
uint32_t gama_lora_header_us(const gama_lora_phy_t *phy);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_LORA_H */
