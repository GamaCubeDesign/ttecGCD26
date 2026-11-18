/*
 * avr.c — see avr.h.
 */

#include "avr.h"
#include "gama_tm.h"

#define AVR_LONG_BYTES  14u   /* 112-bit message */
#define AVR_SHORT_BYTES  7u   /*  56-bit message */
#define AVR_STAMP_HEX   12u   /* '@' timestamp, 48 bits */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

uint32_t avr_crc24(const uint8_t *msg, size_t n)
{
    /* Bitwise, MSB first: 11 bytes per identification message, every ~5 s
     * per aircraft — no table needed. Run over the whole message, the
     * remainder of an intact one is 0. */
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)msg[i] << 16;
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if (crc & 0x1000000u) {
                crc ^= 0x1FFF409u;
            }
        }
    }
    return crc & 0xFFFFFFu;
}

uint8_t avr_class(char set, uint8_t category)
{
    if (set == 'A') {
        switch (category) {
        case 1: case 2:                 return GAMA_TRACK_CLASS_LIGHT;
        case 3: case 4: case 5: case 6: return GAMA_TRACK_CLASS_LARGE;
        case 7:                         return GAMA_TRACK_CLASS_ROTOR;
        default:                        return GAMA_TRACK_CLASS_UNKNOWN;
        }
    }
    if (set == 'B' && category == 1) {  /* glider */
        return GAMA_TRACK_CLASS_LIGHT;
    }
    return GAMA_TRACK_CLASS_UNKNOWN;
}

avr_result_t avr_parse(const char *line, size_t len, avr_msg_t *out)
{
    if (line == NULL || out == NULL) {
        return AVR_ERR_FORMAT;
    }
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    if (len < 2 || line[len - 1] != ';') {
        return AVR_ERR_FORMAT;
    }
    size_t start;
    if (line[0] == '*') {
        start = 1;
    } else if (line[0] == '@' && len > 1 + AVR_STAMP_HEX) {
        for (size_t i = 1; i <= AVR_STAMP_HEX; i++) {
            if (hexval(line[i]) < 0) {
                return AVR_ERR_FORMAT;
            }
        }
        start = 1 + AVR_STAMP_HEX;
    } else {
        return AVR_ERR_FORMAT;
    }
    size_t hex = len - 1 - start;
    if (hex != 2u * AVR_LONG_BYTES && hex != 2u * AVR_SHORT_BYTES) {
        return AVR_ERR_FORMAT;
    }
    uint8_t msg[AVR_LONG_BYTES];
    size_t bytes = hex / 2u;
    for (size_t i = 0; i < bytes; i++) {
        int hi = hexval(line[start + 2 * i]);
        int lo = hexval(line[start + 2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return AVR_ERR_FORMAT;
        }
        msg[i] = (uint8_t)(hi << 4 | lo);
    }
    if (bytes != AVR_LONG_BYTES) {
        return AVR_NOT_IDENT;               /* 56-bit: surveillance replies   */
    }
    unsigned df = msg[0] >> 3;
    if (df != 17u && df != 18u) {
        return AVR_NOT_IDENT;
    }
    unsigned tc = msg[4] >> 3;
    if (tc < 1u || tc > 4u) {
        return AVR_NOT_IDENT;               /* position, velocity, status...  */
    }
    /* The CRC only for identification messages: dump1090 already checked
     * every message it forwards, so this is a guard against a corrupted
     * line, and the other messages are not used. */
    if (avr_crc24(msg, AVR_LONG_BYTES) != 0u) {
        return AVR_ERR_CRC;
    }
    if (df == 18u && (msg[0] & 0x07u) != 0u) {
        return AVR_NON_ICAO;                /* CF 1..7: anonymous, TIS-B, ADS-R */
    }
    out->icao = (uint32_t)msg[1] << 16 | (uint32_t)msg[2] << 8 | msg[3];
    out->set = (char)('A' + (4 - (int)tc));
    out->category = (uint8_t)(msg[4] & 0x07u);
    out->cls = avr_class(out->set, out->category);
    return AVR_OK;
}

const char *avr_result_name(int result)
{
    switch (result) {
    case AVR_OK:         return "ok";
    case AVR_NOT_IDENT:  return "not_ident";
    case AVR_NON_ICAO:   return "non_icao";
    case AVR_ERR_FORMAT: return "format";
    case AVR_ERR_CRC:    return "crc";
    default:             return "unknown";
    }
}
