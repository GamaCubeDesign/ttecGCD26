/* Verifies the CRC is the standard CRC-16/CCITT-FALSE and not one of the
 * several near-identical variants. Getting this wrong would only show up as
 * the two ends of the radio link silently rejecting every frame. */

#include "test_util.h"
#include "crc16.h"

int main(void)
{
    TEST_GROUP("crc16: published check value");
    {
        /* The canonical check value for CRC-16/CCITT-FALSE. If this passes,
         * the polynomial, init value, reflection and final XOR are all the
         * variant we documented. */
        const uint8_t v[] = "123456789";
        CHECK_EQ_INT(gama_crc16(v, 9), 0x29B1);
    }

    TEST_GROUP("crc16: empty input returns the seed untouched");
    {
        CHECK_EQ_INT(gama_crc16(NULL, 0), GAMA_CRC16_INIT);
    }

    TEST_GROUP("crc16: incremental equals one-shot");
    {
        const uint8_t v[] = "123456789";
        uint16_t c = gama_crc16_update(GAMA_CRC16_INIT, v, 4);
        c = gama_crc16_update(c, v + 4, 5);
        CHECK_EQ_INT(c, gama_crc16(v, 9));
    }

    TEST_GROUP("crc16: single-bit flips are detected");
    {
        uint8_t v[32];
        for (size_t i = 0; i < sizeof(v); i++) {
            v[i] = (uint8_t)(i * 7u + 3u);
        }
        uint16_t base = gama_crc16(v, sizeof(v));

        for (size_t byte = 0; byte < sizeof(v); byte++) {
            for (int bit = 0; bit < 8; bit++) {
                v[byte] ^= (uint8_t)(1u << bit);
                CHECK(gama_crc16(v, sizeof(v)) != base);
                v[byte] ^= (uint8_t)(1u << bit);
            }
        }
    }

    TEST_GROUP("crc16: byte transposition is detected");
    {
        /* A plain checksum would miss this; a CRC must not. */
        const uint8_t a[] = { 0x01, 0x02, 0x03, 0x04 };
        const uint8_t b[] = { 0x01, 0x03, 0x02, 0x04 };
        CHECK(gama_crc16(a, 4) != gama_crc16(b, 4));
    }

    TEST_SUMMARY("test_crc16");
}
