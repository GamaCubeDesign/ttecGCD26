#include "crc16.h"

/* Bitwise implementation. A 256-entry table would be ~4x faster but costs
 * 512 bytes of .rodata; at our frame sizes (<= 255 bytes) and packet rates
 * (< 1 Hz) the difference is far below the noise floor of the radio's
 * time-on-air, so we keep the smaller, obviously-correct version. */
uint16_t gama_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    /* The casts are explicit because integer promotion widens every
     * intermediate to int; the project builds with -Wconversion so that a
     * narrowing that was not intended cannot pass unnoticed. */
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ ((unsigned int)data[i] << 8));
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u)
                      ? (uint16_t)(((unsigned int)crc << 1) ^ 0x1021u)
                      : (uint16_t)((unsigned int)crc << 1);
        }
    }
    return crc;
}

uint16_t gama_crc16(const uint8_t *data, size_t len)
{
    return gama_crc16_update(GAMA_CRC16_INIT, data, len);
}
