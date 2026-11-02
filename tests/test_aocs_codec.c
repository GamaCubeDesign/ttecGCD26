/*
 * The OBC <-> AOCS I2C frames (common/gama_aocs.h, docs/icd/obc-aocs-i2c.md),
 * DRAFT version 1.
 *
 * The vectors pin the bytes: they were built field by field in Python and
 * checksummed with binascii.crc_hqx(data, 0xFFFF), an implementation of
 * CRC-16/CCITT-FALSE independent of common/crc16.c. The AOCS ESP32 firmware
 * runs this same file (ground/esp32-tests) to show both ends agree.
 */

#include "test_util.h"
#include "gama_aocs.h"
#include "gama_frame.h"
#include "gama_tm.h"

int main(void)
{
    uint8_t buf[64];

    /* START: seq 0x2C, detumble + azimuth, at most 120 s, azimuth 90.00 deg. */
    static const uint8_t vec_start[] = {
        0x47, 0x01, 0x01, 0x2C, 0x02, 0x03, 0x78, 0x00, 0x28, 0x23, 0x00, 0x00,
        0x00, 0x00, 0x42, 0x6E,
    };
    /* PING: seq 7; every task field zero. */
    static const uint8_t vec_ping[] = {
        0x47, 0x01, 0x01, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x3D, 0xF2,
    };
    /* STATUS: answers seq 0x2C, DONE, no error, roll 1.50, pitch -2.75,
     * yaw 90.12 deg, residual rate 0.35 deg/s, the task took 97 s. */
    static const uint8_t vec_status[] = {
        0x47, 0x01, 0x02, 0x2C, 0x03, 0x00, 0x96, 0x00, 0xED, 0xFE, 0x34, 0x23,
        0x23, 0x00, 0x61, 0x00, 0x00, 0x00, 0xA8, 0x4F,
    };

    TEST_GROUP("aocs: both frames fit the ESP32's 32-byte I2C FIFO");
    {
        CHECK_EQ_INT(GAMA_AOCS_COMMAND_LEN, 16);
        CHECK_EQ_INT(GAMA_AOCS_STATUS_LEN, 20);
        CHECK(GAMA_AOCS_STATUS_LEN <= 32);
    }

    TEST_GROUP("aocs: START, byte for byte");
    {
        gama_aocs_command_t c = { .seq = 0x2C, .cmd = GAMA_AOCS_CMD_START,
                                  .tasks = GAMA_AOCS_TASK_DETUMBLE | GAMA_AOCS_TASK_AZIMUTH,
                                  .max_task_s = 120, .azimuth_cdeg = 9000 };
        CHECK_EQ_INT(gama_aocs_command_encode(buf, sizeof(buf), &c), sizeof(vec_start));
        CHECK_MEM_EQ(buf, vec_start, sizeof(vec_start));
        gama_aocs_command_t back;
        CHECK_EQ_INT(gama_aocs_command_decode(vec_start, sizeof(vec_start), &back),
                     GAMA_AOCS_COMMAND_LEN);
        CHECK_EQ_INT(back.seq, 0x2C);
        CHECK_EQ_INT(back.cmd, GAMA_AOCS_CMD_START);
        CHECK_EQ_INT(back.tasks, 3);
        CHECK_EQ_INT(back.max_task_s, 120);
        CHECK_EQ_INT(back.azimuth_cdeg, 9000);
    }

    TEST_GROUP("aocs: PING, byte for byte");
    {
        gama_aocs_command_t c = { .seq = 7, .cmd = GAMA_AOCS_CMD_PING };
        CHECK_EQ_INT(gama_aocs_command_encode(buf, sizeof(buf), &c), sizeof(vec_ping));
        CHECK_MEM_EQ(buf, vec_ping, sizeof(vec_ping));
    }

    TEST_GROUP("aocs: STATUS, byte for byte");
    {
        gama_aocs_status_t s = { .seq = 0x2C, .state = GAMA_AOCS_ST_DONE,
                                 .error = GAMA_AOCS_ERR_NONE, .roll_cdeg = 150,
                                 .pitch_cdeg = -275, .yaw_cdeg = 9012,
                                 .rate_cdps = 35, .task_s = 97 };
        CHECK_EQ_INT(gama_aocs_status_encode(buf, sizeof(buf), &s), sizeof(vec_status));
        CHECK_MEM_EQ(buf, vec_status, sizeof(vec_status));
        gama_aocs_status_t back;
        CHECK_EQ_INT(gama_aocs_status_decode(vec_status, sizeof(vec_status), &back),
                     GAMA_AOCS_STATUS_LEN);
        CHECK_EQ_INT(back.state, GAMA_AOCS_ST_DONE);
        CHECK_EQ_INT(back.pitch_cdeg, -275);
        CHECK_EQ_INT(back.yaw_cdeg, 9012);
        CHECK_EQ_INT(back.task_s, 97);
        CHECK_STR_EQ(gama_aocs_state_name(back.state), "DONE");
    }

    TEST_GROUP("aocs: attitude not measured travels as 'no data'");
    {
        gama_aocs_status_t s = { .seq = 1, .state = GAMA_AOCS_ST_FAILED,
                                 .error = GAMA_AOCS_ERR_SENSOR,
                                 .roll_cdeg = GAMA_NO_DATA_I16, .pitch_cdeg = GAMA_NO_DATA_I16,
                                 .yaw_cdeg = GAMA_NO_DATA_I16, .rate_cdps = GAMA_NO_DATA_U16 };
        gama_aocs_status_encode(buf, sizeof(buf), &s);
        gama_aocs_status_t back;
        CHECK_EQ_INT(gama_aocs_status_decode(buf, GAMA_AOCS_STATUS_LEN, &back),
                     GAMA_AOCS_STATUS_LEN);
        CHECK_EQ_INT(back.roll_cdeg, GAMA_NO_DATA_I16);
        CHECK_EQ_INT(back.rate_cdps, GAMA_NO_DATA_U16);
        CHECK_EQ_INT(back.error, GAMA_AOCS_ERR_SENSOR);
    }

    TEST_GROUP("aocs: a lease is always bounded: START without a sane max_task_s is refused");
    {
        gama_aocs_command_t c = { .seq = 1, .cmd = GAMA_AOCS_CMD_START, .max_task_s = 0 };
        CHECK_EQ_INT(gama_aocs_command_encode(buf, sizeof(buf), &c), GAMA_FRAME_ERR_ARG);
        c.max_task_s = GAMA_AOCS_MAX_TASK_S + 1u;
        CHECK_EQ_INT(gama_aocs_command_encode(buf, sizeof(buf), &c), GAMA_FRAME_ERR_ARG);
        c.max_task_s = GAMA_AOCS_MAX_TASK_S;
        CHECK_EQ_INT(gama_aocs_command_encode(buf, sizeof(buf), &c), GAMA_AOCS_COMMAND_LEN);
    }

    TEST_GROUP("aocs: a damaged frame is refused, and says how");
    {
        gama_aocs_status_t s;
        uint8_t bad[GAMA_AOCS_STATUS_LEN];
        memcpy(bad, vec_status, sizeof(bad));
        bad[10] ^= 0x01;                                   /* one bit on the bus */
        CHECK_EQ_INT(gama_aocs_status_decode(bad, sizeof(bad), &s), GAMA_FRAME_ERR_CRC);
        memcpy(bad, vec_status, sizeof(bad));
        bad[0] = 0xFF;                                     /* nobody home: bus idles high */
        CHECK_EQ_INT(gama_aocs_status_decode(bad, sizeof(bad), &s), GAMA_FRAME_ERR_VERSION);
        CHECK_EQ_INT(gama_aocs_status_decode(vec_status, 19, &s), GAMA_FRAME_ERR_LEN);
        /* A COMMAND read where a STATUS was expected: the type tells them apart. */
        uint8_t as_status[GAMA_AOCS_STATUS_LEN] = { 0 };
        memcpy(as_status, vec_start, sizeof(vec_start));
        CHECK_EQ_INT(gama_aocs_status_decode(as_status, sizeof(as_status), &s),
                     GAMA_FRAME_ERR_VERSION);
        gama_aocs_command_t c;
        CHECK_EQ_INT(gama_aocs_command_decode(NULL, 16, &c), GAMA_FRAME_ERR_ARG);
        CHECK_EQ_INT(gama_aocs_command_encode(buf, 15, &c), GAMA_FRAME_ERR_ARG);
    }

    TEST_SUMMARY("test_aocs_codec");
}
