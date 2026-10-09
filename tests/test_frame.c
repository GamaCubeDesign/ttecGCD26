/* Framing layer: round-trips, boundary sizes, and every rejection path.
 *
 * The rejection tests matter as much as the happy path. A frame that fails
 * validation must be *reported*, because HLR-ADS-08 requires us to quantify
 * the data loss rate -- a decoder that silently dropped bad frames would
 * make the loss invisible. */

#include "test_util.h"
#include "gama_frame.h"

#include <stdint.h>

int main(void)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL];
    gama_frame_t f;

    TEST_GROUP("frame: round-trip with a payload");
    {
        const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TM_HK,
                                  0x1234, payload, sizeof(payload));
        CHECK_EQ_INT(n, GAMA_FRAME_OVERHEAD + sizeof(payload));

        int d = gama_frame_decode(buf, (size_t)n, &f);
        CHECK_EQ_INT(d, n);
        CHECK_EQ_INT(f.version, GAMA_FRAME_VERSION);
        CHECK_EQ_INT(f.type, GAMA_FRAME_TM_HK);
        CHECK_EQ_INT(f.seq, 0x1234);
        CHECK_EQ_INT(f.len, sizeof(payload));
        CHECK_MEM_EQ(f.payload, payload, sizeof(payload));
    }

    TEST_GROUP("frame: zero-length payload");
    {
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC_ACK,
                                  0, NULL, 0);
        CHECK_EQ_INT(n, GAMA_FRAME_OVERHEAD);

        int d = gama_frame_decode(buf, (size_t)n, &f);
        CHECK_EQ_INT(d, n);
        CHECK_EQ_INT(f.len, 0);
        CHECK(f.payload == NULL);
    }

    TEST_GROUP("frame: maximum payload fits the SX1278 FIFO");
    {
        uint8_t payload[GAMA_FRAME_MAX_PAYLOAD];
        for (size_t i = 0; i < sizeof(payload); i++) {
            payload[i] = (uint8_t)i;
        }
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_BULK_DATA,
                                  0xFFFF, payload, GAMA_FRAME_MAX_PAYLOAD);
        CHECK_EQ_INT(n, GAMA_FRAME_MAX_TOTAL);

        int d = gama_frame_decode(buf, (size_t)n, &f);
        CHECK_EQ_INT(d, n);
        CHECK_EQ_INT(f.seq, 0xFFFF);
        CHECK_MEM_EQ(f.payload, payload, GAMA_FRAME_MAX_PAYLOAD);
    }

    TEST_GROUP("frame: sequence wraps without losing a value");
    {
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_BEACON,
                                  65535, NULL, 0);
        CHECK(gama_frame_decode(buf, (size_t)n, &f) > 0);
        CHECK_EQ_INT(f.seq, 65535);

        n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_BEACON, 0, NULL, 0);
        CHECK(gama_frame_decode(buf, (size_t)n, &f) > 0);
        CHECK_EQ_INT(f.seq, 0);
    }

    TEST_GROUP("frame: header layout is little-endian as specified");
    {
        const uint8_t payload[] = { 0xAA };
        gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC, 0xBEEF, payload, 1);
        CHECK_EQ_INT(buf[0], GAMA_FRAME_VERSION);
        CHECK_EQ_INT(buf[1], GAMA_FRAME_TC);
        CHECK_EQ_INT(buf[2], 0xEF); /* sequence low byte first */
        CHECK_EQ_INT(buf[3], 0xBE);
        CHECK_EQ_INT(buf[4], 1);
        CHECK_EQ_INT(buf[5], 0xAA);
    }

    TEST_GROUP("frame: encoder refuses rather than truncates");
    {
        const uint8_t payload[] = { 1, 2, 3, 4 };
        CHECK_EQ_INT(gama_frame_encode(buf, 5, GAMA_FRAME_TC, 0, payload, 4),
                     GAMA_FRAME_ERR_ARG);
        CHECK_EQ_INT(gama_frame_encode(NULL, 64, GAMA_FRAME_TC, 0, payload, 4),
                     GAMA_FRAME_ERR_ARG);
        CHECK_EQ_INT(gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC, 0, NULL, 4),
                     GAMA_FRAME_ERR_ARG);
    }

    TEST_GROUP("frame: corrupted payload fails the CRC");
    {
        const uint8_t payload[] = { 0x10, 0x20, 0x30 };
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TM_TRACKS,
                                  7, payload, sizeof(payload));
        buf[6] ^= 0x01;
        CHECK_EQ_INT(gama_frame_decode(buf, (size_t)n, &f), GAMA_FRAME_ERR_CRC);
    }

    TEST_GROUP("frame: corrupted header fails the CRC");
    {
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC, 42, NULL, 0);
        buf[2] ^= 0x01; /* flip a sequence bit */
        CHECK_EQ_INT(gama_frame_decode(buf, (size_t)n, &f), GAMA_FRAME_ERR_CRC);
    }

    TEST_GROUP("frame: unknown version is rejected before anything else");
    {
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC, 1, NULL, 0);
        buf[0] = GAMA_FRAME_VERSION + 1;
        CHECK_EQ_INT(gama_frame_decode(buf, (size_t)n, &f), GAMA_FRAME_ERR_VERSION);
    }

    TEST_GROUP("frame: truncation is reported, not read past");
    {
        const uint8_t payload[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TM_STAT,
                                  1, payload, sizeof(payload));

        /* Shorter than the fixed header. */
        CHECK_EQ_INT(gama_frame_decode(buf, 0, &f), GAMA_FRAME_ERR_SHORT);
        CHECK_EQ_INT(gama_frame_decode(buf, GAMA_FRAME_HEADER_LEN - 1, &f),
                     GAMA_FRAME_ERR_SHORT);

        /* Header present, but the payload the header promises is not. */
        for (size_t cut = GAMA_FRAME_HEADER_LEN; cut < (size_t)n; cut++) {
            CHECK_EQ_INT(gama_frame_decode(buf, cut, &f), GAMA_FRAME_ERR_SHORT);
        }
    }

    TEST_GROUP("frame: an over-long declared length is rejected");
    {
        /* Hand-build a header claiming more payload than a frame can hold.
         * Reached only through corruption, but it must not be believed. */
        buf[0] = GAMA_FRAME_VERSION;
        buf[1] = GAMA_FRAME_TC;
        buf[2] = 0;
        buf[3] = 0;
        buf[4] = (uint8_t)(GAMA_FRAME_MAX_PAYLOAD + 1);
        CHECK_EQ_INT(gama_frame_decode(buf, sizeof(buf), &f), GAMA_FRAME_ERR_LEN);

        CHECK_EQ_INT(gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC, 0, buf,
                                       (uint8_t)(GAMA_FRAME_MAX_PAYLOAD + 1)),
                     GAMA_FRAME_ERR_LEN);
    }

    TEST_GROUP("frame: NULL arguments are rejected");
    {
        CHECK_EQ_INT(gama_frame_decode(NULL, 16, &f), GAMA_FRAME_ERR_ARG);
        CHECK_EQ_INT(gama_frame_decode(buf, 16, NULL), GAMA_FRAME_ERR_ARG);
    }

    TEST_GROUP("frame: internal message types are barred from the radio");
    {
        CHECK(gama_frame_is_ipc_only(GAMA_FRAME_IPC_TC_EVENT));
        CHECK(gama_frame_is_ipc_only(GAMA_FRAME_IPC_TELEMETRY));
        CHECK(gama_frame_is_ipc_only(GAMA_FRAME_IPC_HELLO));
        CHECK(!gama_frame_is_ipc_only(GAMA_FRAME_TC));
        CHECK(!gama_frame_is_ipc_only(GAMA_FRAME_TM_TRACKS));
        CHECK(!gama_frame_is_ipc_only(GAMA_FRAME_BULK_DATA));
        CHECK(!gama_frame_is_ipc_only(GAMA_FRAME_BEACON));
    }

    TEST_GROUP("frame: every type and result has a name");
    {
        CHECK_STR_EQ(gama_frame_type_name(GAMA_FRAME_TM_TRACKS), "TM_TRACKS");
        CHECK_STR_EQ(gama_frame_type_name(0xEE), "UNKNOWN");
        CHECK_STR_EQ(gama_frame_result_name(GAMA_FRAME_ERR_CRC), "CRC");
        CHECK_STR_EQ(gama_frame_result_name(-99), "UNKNOWN");
    }

    TEST_SUMMARY("test_frame");
}
