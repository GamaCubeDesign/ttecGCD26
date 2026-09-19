/* IPC payload codec: round-trips, exact-length rejection, and the track age
 * rewrite ttcd applies between IPC_TRACKS and TM_TRACKS. */

#include "test_util.h"
#include "gama_ipc.h"
#include "gama_frame.h"
#include "gama_tm.h"

int main(void)
{
    uint8_t buf[GAMA_FRAME_MAX_PAYLOAD];

    TEST_GROUP("ipc: hello round-trip");
    {
        gama_ipc_hello_t in = { .role = GAMA_IPC_ROLE_OBC, .version = GAMA_IPC_VERSION }, out;
        CHECK_EQ_INT(gama_ipc_hello_encode(buf, sizeof(buf), &in), GAMA_IPC_HELLO_LEN);
        CHECK_EQ_INT(gama_ipc_hello_decode(buf, GAMA_IPC_HELLO_LEN, &out), GAMA_IPC_HELLO_LEN);
        CHECK_EQ_INT(out.role, GAMA_IPC_ROLE_OBC);
        CHECK_EQ_INT(out.version, GAMA_IPC_VERSION);
    }

    TEST_GROUP("ipc: time round-trip keeps full 32-bit ranges");
    {
        gama_ipc_time_t in = { .unix_s = 0xFEDCBA98u, .nsec = 999999999u }, out;
        CHECK_EQ_INT(gama_ipc_time_encode(buf, sizeof(buf), &in), GAMA_IPC_TIME_LEN);
        CHECK_EQ_INT(gama_ipc_time_decode(buf, GAMA_IPC_TIME_LEN, &out), GAMA_IPC_TIME_LEN);
        CHECK_EQ_INT(out.unix_s, 0xFEDCBA98u);
        CHECK_EQ_INT(out.nsec, 999999999u);
    }

    TEST_GROUP("ipc: telemetry round-trip, including negative values");
    {
        gama_ipc_telemetry_t in = {
            .battery_mv = 8123, .current_ma = -432, .temp_bat_ccel = -150,
            .temp_ext_ccel = 2345, .roll_cdeg = -17999, .pitch_cdeg = 4500,
            .yaw_cdeg = 32767,
        }, out;
        CHECK_EQ_INT(gama_ipc_telemetry_encode(buf, sizeof(buf), &in), GAMA_IPC_TELEMETRY_LEN);
        CHECK_EQ_INT(gama_ipc_telemetry_decode(buf, GAMA_IPC_TELEMETRY_LEN, &out),
                     GAMA_IPC_TELEMETRY_LEN);
        CHECK_MEM_EQ(&out, &in, sizeof(in));
    }

    TEST_GROUP("ipc: stat round-trip");
    {
        gama_ipc_stat_t in = { .msgs_received = 61234, .msgs_decoded = 60001,
                               .aircraft_tracked = 20, .dump1090_restarts = 3 }, out;
        CHECK_EQ_INT(gama_ipc_stat_encode(buf, sizeof(buf), &in), GAMA_IPC_STAT_LEN);
        CHECK_EQ_INT(gama_ipc_stat_decode(buf, GAMA_IPC_STAT_LEN, &out), GAMA_IPC_STAT_LEN);
        CHECK_MEM_EQ(&out, &in, sizeof(in));
    }

    TEST_GROUP("ipc: decoders require the exact length");
    {
        /* A payload of the wrong size is a protocol error between two of our
         * own processes. Parsing it leniently would hide the bug. */
        gama_ipc_hello_t h; gama_ipc_time_t t; gama_ipc_telemetry_t tm; gama_ipc_stat_t st;
        CHECK_EQ_INT(gama_ipc_hello_decode(buf, 1, &h), GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_hello_decode(buf, 3, &h), GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_time_decode(buf, 7, &t), GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_telemetry_decode(buf, 13, &tm), GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_telemetry_decode(buf, 15, &tm), GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_stat_decode(buf, 11, &st), GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_hello_decode(NULL, 2, &h), GAMA_FRAME_ERR_ARG);
        CHECK_EQ_INT(gama_ipc_hello_encode(buf, 1, &h), GAMA_FRAME_ERR_ARG);
    }

    TEST_GROUP("ipc: tracks payload is a header and whole records");
    {
        uint32_t epoch; size_t n;
        size_t len = GAMA_IPC_TRACKS_HEADER_LEN;
        CHECK_EQ_INT(gama_ipc_tracks_header_encode(buf, sizeof(buf), 0x01020304u), 4);
        CHECK_EQ_INT(gama_ipc_tracks_decode(buf, len, &epoch, &n), 4);
        CHECK_EQ_INT(epoch, 0x01020304u);
        CHECK_EQ_INT(n, 0);

        CHECK_EQ_INT(gama_ipc_tracks_decode(buf, len + 3 * GAMA_TRACK_WIRE_LEN, &epoch, &n), 4);
        CHECK_EQ_INT(n, 3);

        /* The maximum: twelve records, 244 bytes, still one frame. */
        CHECK_EQ_INT(gama_ipc_tracks_decode(buf, len + 12 * GAMA_TRACK_WIRE_LEN, &epoch, &n), 4);
        CHECK_EQ_INT(n, 12);
        CHECK(len + 12 * GAMA_TRACK_WIRE_LEN <= GAMA_FRAME_MAX_PAYLOAD);

        CHECK_EQ_INT(gama_ipc_tracks_decode(buf, len + 13 * GAMA_TRACK_WIRE_LEN, &epoch, &n),
                     GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_tracks_decode(buf, len + GAMA_TRACK_WIRE_LEN + 1, &epoch, &n),
                     GAMA_FRAME_ERR_LEN);
        CHECK_EQ_INT(gama_ipc_tracks_decode(buf, 3, &epoch, &n), GAMA_FRAME_ERR_SHORT);
    }

    TEST_GROUP("track age: rewritten in place, other fields untouched");
    {
        gama_track_t t = { .icao = 0xABCDEFu, .latitude = 10.0, .longitude = -20.0,
                           .altitude_ft = 30000, .ground_speed_kt = 420.0,
                           .track_deg = 45.0, .vertical_rate_fpm = 64,
                           .age_ds = 12, .flags = GAMA_TRACK_F_POSITION };
        uint8_t rec[GAMA_TRACK_WIRE_LEN], before[GAMA_TRACK_WIRE_LEN];
        gama_track_encode(rec, sizeof(rec), &t);
        memcpy(before, rec, sizeof(rec));

        CHECK_EQ_INT(gama_track_age_add(rec, 30), 42);

        gama_track_t back;
        gama_track_decode(rec, sizeof(rec), &back);
        CHECK_EQ_INT(back.age_ds, 42);
        /* Every byte except the two age bytes is unchanged. */
        CHECK_MEM_EQ(rec, before, 17);
        CHECK_EQ_INT(rec[19], before[19]);
    }

    TEST_GROUP("track age: saturates instead of wrapping");
    {
        /* A wrapped age would make a stale aircraft look freshly updated. */
        gama_track_t t = { .age_ds = 65000 };
        uint8_t rec[GAMA_TRACK_WIRE_LEN];
        gama_track_encode(rec, sizeof(rec), &t);
        CHECK_EQ_INT(gama_track_age_add(rec, 1000), 65535);
        CHECK_EQ_INT(gama_track_age_add(rec, 0xFFFFFFFFu), 65535);

        t.age_ds = 0;
        gama_track_encode(rec, sizeof(rec), &t);
        CHECK_EQ_INT(gama_track_age_add(rec, 0), 0);
    }

    TEST_GROUP("ipc: every role has a name");
    {
        CHECK_STR_EQ(gama_ipc_role_name(GAMA_IPC_ROLE_ADSBD), "adsbd");
        CHECK_STR_EQ(gama_ipc_role_name(GAMA_IPC_ROLE_OBC), "obc");
        CHECK_STR_EQ(gama_ipc_role_name(GAMA_IPC_ROLE_TOOL), "tool");
        CHECK_STR_EQ(gama_ipc_role_name(99), "none");
    }

    TEST_SUMMARY("test_ipc_codec");
}
