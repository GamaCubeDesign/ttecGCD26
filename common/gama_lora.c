#include "gama_lora.h"
#include "gama_tc.h"

#include <stddef.h>

/* ADR-0004. Every field is spelled out, including the ones that are the same
 * across profiles, so that the table is the whole truth. */
static const gama_lora_phy_t PROFILES[GAMA_RATE_COUNT] = {
    [GAMA_RATE_SAFE] = {
        .sf = 12, .bw_hz = 125000u, .cr = 4,
        .preamble_len = 8, .crc_on = true, .implicit_header = false,
    },
    [GAMA_RATE_NOMINAL] = {
        .sf = 9, .bw_hz = 125000u, .cr = 1,
        .preamble_len = 8, .crc_on = true, .implicit_header = false,
    },
    [GAMA_RATE_FAST] = {
        .sf = 7, .bw_hz = 125000u, .cr = 1,
        .preamble_len = 8, .crc_on = true, .implicit_header = false,
    },
};

const gama_lora_phy_t *gama_lora_profile(uint8_t profile)
{
    if (profile >= GAMA_RATE_COUNT) {
        return NULL;
    }
    return &PROFILES[profile];
}

uint32_t gama_lora_symbol_us(const gama_lora_phy_t *phy)
{
    /* Exact for the bandwidths we use: 1e6 / 125 kHz = 8 us per chip. */
    return (uint32_t)(((uint64_t)1u << phy->sf) * 1000000u / phy->bw_hz);
}

bool gama_lora_ldro(const gama_lora_phy_t *phy)
{
    return gama_lora_symbol_us(phy) > 16000u;
}

uint32_t gama_lora_toa_us(const gama_lora_phy_t *phy, uint8_t payload_len)
{
    const uint32_t tsym = gama_lora_symbol_us(phy);
    const int32_t  de   = gama_lora_ldro(phy) ? 1 : 0;

    /* (preamble + 4.25) symbols, kept in integers as (4 * preamble + 17) / 4.
     * Exact because every symbol duration we use is a multiple of 4 us. */
    const uint32_t preamble_us = (4u * phy->preamble_len + 17u) * tsym / 4u;

    const int32_t numerator = 8 * (int32_t)payload_len - 4 * (int32_t)phy->sf
                              + 28
                              + (phy->crc_on ? 16 : 0)
                              - (phy->implicit_header ? 20 : 0);
    const int32_t denominator = 4 * ((int32_t)phy->sf - 2 * de);

    /* ceil(numerator / denominator), clamped at zero as the datasheet's
     * max(..., 0) requires. The denominator is always positive (sf >= 7). */
    const int32_t blocks = numerator > 0
                               ? (numerator + denominator - 1) / denominator
                               : 0;
    const uint32_t payload_symbols = 8u + (uint32_t)blocks * (phy->cr + 4u);

    return preamble_us + payload_symbols * tsym;
}

uint32_t gama_lora_header_us(const gama_lora_phy_t *phy)
{
    /* Preamble (programmed length + 4.25) plus the 8-symbol header block. */
    return (4u * phy->preamble_len + 17u + 4u * 8u) * gama_lora_symbol_us(phy) / 4u;
}
