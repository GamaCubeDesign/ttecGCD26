/*
 * AVR raw messages (ADR-0023): the emitter category from the ADS-B
 * identification message, the CRC-24, and every kind of line that is not
 * one, refused or counted by its reason (HLR-ADS-08).
 */

#include "test_util.h"
#include "avr.h"
#include "gama_tm.h"

#include <stdio.h>
#include <string.h>

static avr_result_t parse(const char *line, avr_msg_t *m)
{
    return avr_parse(line, strlen(line), m);
}

/* An identification message for icao with type code tc and category ca,
 * its parity computed, as an AVR line. The callsign bits stay those of the
 * classic example; only the category is under test. */
static const char *ident(uint8_t df_byte, uint32_t icao, unsigned tc, unsigned ca)
{
    static char line[40];
    uint8_t msg[14] = { df_byte, (uint8_t)(icao >> 16), (uint8_t)(icao >> 8), (uint8_t)icao,
                        (uint8_t)(tc << 3 | ca), 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0 };
    uint32_t pi = avr_crc24(msg, 11);
    msg[11] = (uint8_t)(pi >> 16);
    msg[12] = (uint8_t)(pi >> 8);
    msg[13] = (uint8_t)pi;
    char *p = line;
    *p++ = '*';
    for (size_t i = 0; i < sizeof(msg); i++) {
        p += snprintf(p, 3, "%02X", msg[i]);
    }
    *p++ = ';';
    *p = '\0';
    return line;
}

int main(void)
{
    avr_msg_t m;

    TEST_GROUP("avr: the classic identification message (KLM1023, 4840D6)");
    {
        /* The textbook example of ADS-B decoding: DF17, ICAO 4840D6, type
         * code 4, category 0. Its parity is known from outside this code,
         * which is what pins avr_crc24 itself. */
        CHECK_EQ_INT(parse("*8D4840D6202CC371C32CE0576098;", &m), AVR_OK);
        CHECK_EQ_INT(m.icao, 0x4840D6u);
        CHECK_EQ_INT(m.set, 'A');
        CHECK_EQ_INT(m.category, 0);
        CHECK_EQ_INT(m.cls, GAMA_TRACK_CLASS_UNKNOWN);
        const uint8_t raw[14] = { 0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3,
                                  0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98 };
        CHECK_EQ_INT(avr_crc24(raw, 11), 0x576098u);
        CHECK_EQ_INT(avr_crc24(raw, 14), 0u);
        /* Lower case, a trailing '\r' and the MLAT timestamp form. */
        CHECK_EQ_INT(parse("*8d4840d6202cc371c32ce0576098;\r", &m), AVR_OK);
        CHECK_EQ_INT(parse("@0123456789AB8D4840D6202CC371C32CE0576098;", &m), AVR_OK);
        CHECK_EQ_INT(m.icao, 0x4840D6u);
    }

    TEST_GROUP("avr: every category maps to its class (ADR-0023)");
    {
        static const struct { unsigned tc, ca; char set; uint8_t cls; } cases[] = {
            { 4, 1, 'A', GAMA_TRACK_CLASS_LIGHT }, { 4, 2, 'A', GAMA_TRACK_CLASS_LIGHT },
            { 4, 3, 'A', GAMA_TRACK_CLASS_LARGE }, { 4, 4, 'A', GAMA_TRACK_CLASS_LARGE },
            { 4, 5, 'A', GAMA_TRACK_CLASS_LARGE }, { 4, 6, 'A', GAMA_TRACK_CLASS_LARGE },
            { 4, 7, 'A', GAMA_TRACK_CLASS_ROTOR }, { 4, 0, 'A', GAMA_TRACK_CLASS_UNKNOWN },
            { 3, 1, 'B', GAMA_TRACK_CLASS_LIGHT }, { 3, 2, 'B', GAMA_TRACK_CLASS_UNKNOWN },
            { 3, 7, 'B', GAMA_TRACK_CLASS_UNKNOWN }, { 2, 1, 'C', GAMA_TRACK_CLASS_UNKNOWN },
            { 1, 3, 'D', GAMA_TRACK_CLASS_UNKNOWN },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            CHECK_EQ_INT(parse(ident(0x8D, 0xE0B244u, cases[i].tc, cases[i].ca), &m), AVR_OK);
            CHECK_EQ_INT(m.icao, 0xE0B244u);
            CHECK_EQ_INT(m.set, cases[i].set);
            CHECK_EQ_INT(m.category, cases[i].ca);
            CHECK_EQ_INT(m.cls, cases[i].cls);
        }
    }

    TEST_GROUP("avr: DF18 counts only with an ICAO address (CF 0)");
    {
        CHECK_EQ_INT(parse(ident(0x90, 0xABCDEFu, 4, 7), &m), AVR_OK);   /* DF18 CF0 */
        CHECK_EQ_INT(m.cls, GAMA_TRACK_CLASS_ROTOR);
        CHECK_EQ_INT(parse(ident(0x91, 0xABCDEFu, 4, 3), &m), AVR_NON_ICAO);
        CHECK_EQ_INT(parse(ident(0x92, 0xABCDEFu, 4, 3), &m), AVR_NON_ICAO);
    }

    TEST_GROUP("avr: other messages are counted as not identification");
    {
        /* Airborne position (TC 11) and velocity (TC 19), DF17. */
        CHECK_EQ_INT(parse("*8D40621D58C382D690C8AC2863A7;", &m), AVR_NOT_IDENT);
        CHECK_EQ_INT(parse("*8D485020994409940838175B284F;", &m), AVR_NOT_IDENT);
        /* A 56-bit all-call reply and a 112-bit Comm-B (DF20). */
        CHECK_EQ_INT(parse("*5D4840D6E3A0BC;", &m), AVR_NOT_IDENT);
        CHECK_EQ_INT(parse("*A0001838CA3E51F0A8000047BC4B;", &m), AVR_NOT_IDENT);
    }

    TEST_GROUP("avr: a corrupted identification fails its CRC");
    {
        CHECK_EQ_INT(parse("*8D4840D6202CC371C32CE0576099;", &m), AVR_ERR_CRC);
        CHECK_EQ_INT(parse("*8D4840D6232CC371C32CE0576098;", &m), AVR_ERR_CRC);  /* A3 forged */
    }

    TEST_GROUP("avr: malformed lines are refused");
    {
        static const char *bad[] = {
            "", ";", "*;", "8D4840D6202CC371C32CE0576098;",      /* no '*'        */
            "*8D4840D6202CC371C32CE0576098",                      /* no ';'        */
            "*8D4840D6202CC371C32CE057609;",                      /* 27 digits     */
            "*8D4840D6202CC371C32CE05760981;",                    /* 29 digits     */
            "*8D4840D6202CC371C32CE057609G;",                     /* not hex       */
            "@0123456789A8D4840D6202CC371C32CE0576098;",          /* short stamp   */
            "MSG,3,1,1,4840D6,1,,,,,,,,,,,,,,,,0",                /* an SBS line   */
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            CHECK_EQ_INT(parse(bad[i], &m), AVR_ERR_FORMAT);
        }
        CHECK_EQ_INT(avr_parse(NULL, 0, &m), AVR_ERR_FORMAT);
        CHECK_STR_EQ(avr_result_name(AVR_ERR_CRC), "crc");
        CHECK_STR_EQ(avr_result_name(AVR_NOT_IDENT), "not_ident");
    }

    TEST_SUMMARY("test_avr");
}
