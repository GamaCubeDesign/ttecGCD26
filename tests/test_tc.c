/* Telecommand catalogue: argument lengths, value stability, and name lookup.
 *
 * The wire-value test is the point of this file. These numbers are also
 * implemented by hand in the obc repository; if anyone renumbers an enum
 * here, this test fails before the two repositories disagree in flight. */

#include "test_util.h"
#include "gama_tc.h"

int main(void)
{
    TEST_GROUP("tc: argument lengths are declared for every command");
    {
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_PING),         0);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_SET_TIME),     8);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_SET_MODE),     1);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_SET_RATE),     1);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_SET_TX_POWER), 1);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_STREAM_START), 2);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_STREAM_STOP),  0);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_REQ_HK),       0);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_REQ_STAT),     0);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_REQ_ROSTER),   0);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_BULK_START),   6);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_BULK_ABORT),   0);
        CHECK_EQ_INT(gama_tc_arg_len(GAMA_TC_SHUTDOWN),     2);
    }

    TEST_GROUP("tc: an unknown identifier is rejected, not defaulted to zero");
    {
        /* Returning 0 would let a corrupted command id dispatch as a valid
         * no-argument command. -1 forces the caller to reject it. */
        CHECK_EQ_INT(gama_tc_arg_len(0x00), -1);
        CHECK_EQ_INT(gama_tc_arg_len(0x99), -1);
        CHECK_EQ_INT(gama_tc_arg_len(0xFF), -1);
    }

    TEST_GROUP("tc: wire values are pinned (the obc repo mirrors these)");
    {
        CHECK_EQ_INT(GAMA_TC_PING,         0x01);
        CHECK_EQ_INT(GAMA_TC_SET_TIME,     0x02);
        CHECK_EQ_INT(GAMA_TC_SET_MODE,     0x10);
        CHECK_EQ_INT(GAMA_TC_SET_RATE,     0x20);
        CHECK_EQ_INT(GAMA_TC_SHUTDOWN,     0x70);
        CHECK_EQ_INT(GAMA_TC_SHUTDOWN_MAGIC, 0xA5A5);

        CHECK_EQ_INT(GAMA_OBC_EV_NONE,            0);
        CHECK_EQ_INT(GAMA_OBC_EV_TC_BASIC_INTER,  1);
        CHECK_EQ_INT(GAMA_OBC_EV_TC_AOCS,         2);
        CHECK_EQ_INT(GAMA_OBC_EV_TC_MISSION_ADSB, 3);
        CHECK_EQ_INT(GAMA_OBC_EV_TC_DOWNLINK,     4);
        CHECK_EQ_INT(GAMA_OBC_EV_TC_SURVIVAL,     5);
        CHECK_EQ_INT(GAMA_OBC_EV_TASK_DONE,       6);
        CHECK_EQ_INT(GAMA_OBC_EV_ADSB_TIMEOUT,    7);

        CHECK_EQ_INT(GAMA_OBC_ST_PRE_TEST,         0);
        CHECK_EQ_INT(GAMA_OBC_ST_MISSION_ADSB,     3);
        CHECK_EQ_INT(GAMA_OBC_ST_MISSION_DOWNLINK, 4);
        CHECK_EQ_INT(GAMA_OBC_ST_ENV_SURVIVAL,     5);
        CHECK_EQ_INT(GAMA_OBC_ST_UNKNOWN,          0xFF);

        CHECK_EQ_INT(GAMA_RATE_SAFE,    0);
        CHECK_EQ_INT(GAMA_RATE_NOMINAL, 1);
        CHECK_EQ_INT(GAMA_RATE_FAST,    2);
        CHECK_EQ_INT(GAMA_RATE_COUNT,   3);
    }

    TEST_GROUP("tc: names are defined and never NULL");
    {
        CHECK_STR_EQ(gama_tc_name(GAMA_TC_SET_RATE), "SET_RATE");
        CHECK_STR_EQ(gama_tc_name(0x99), "UNKNOWN");
        CHECK_STR_EQ(gama_ack_status_name(GAMA_ACK_BAD_ARGS), "BAD_ARGS");
        CHECK_STR_EQ(gama_ack_status_name(200), "UNKNOWN");
        CHECK_STR_EQ(gama_rate_profile_name(GAMA_RATE_NOMINAL), "NOMINAL");
        CHECK_STR_EQ(gama_rate_profile_name(9), "UNKNOWN");
        CHECK_STR_EQ(gama_obc_state_name(GAMA_OBC_ST_MISSION_ADSB), "MISSION_ADSB");
        CHECK_STR_EQ(gama_obc_state_name(99), "UNKNOWN");
        CHECK_STR_EQ(gama_link_state_name(GAMA_LINK_BULK), "BULK");
        CHECK_STR_EQ(gama_link_state_name(99), "UNKNOWN");
    }

    TEST_SUMMARY("test_tc");
}
