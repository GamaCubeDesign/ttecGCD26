/*
 * gen_vectors — prints the golden wire vectors used by tests/test_vectors.c.
 *
 * Run this only when the protocol version is deliberately bumped, then paste
 * the output into the test and review the diff byte by byte. The vectors are
 * checked in as literals on purpose: a test that regenerated its own
 * expectation would happily follow an accidental layout change.
 *
 *   cmake --build build --target gen_vectors && ./build/tools/gen_vectors/gen_vectors
 */

#include <stdio.h>
#include <string.h>

#include "gama_frame.h"
#include "gama_tc.h"
#include "gama_tm.h"
#include "gama_bytes.h"
#include "gama_ipc.h"

static void dump(const char *name, const uint8_t *b, size_t n)
{
    printf("    /* %s (%zu bytes) */\n    static const uint8_t %s[] = {\n        ",
           name, n, name);
    for (size_t i = 0; i < n; i++) {
        printf("0x%02X,", b[i]);
        if ((i + 1) % 12 == 0 && i + 1 < n) {
            printf("\n        ");
        } else if (i + 1 < n) {
            printf(" ");
        }
    }
    printf("\n    };\n\n");
}

int main(void)
{
    uint8_t frame[GAMA_FRAME_MAX_TOTAL];
    uint8_t payload[GAMA_FRAME_MAX_PAYLOAD];
    int n;

    /* --- a track record, on its own --- */
    gama_track_t t = {
        .icao              = 0xABCDEFu,
        .latitude          = 1.0,      /* exactly 93206 in i24 units  */
        .longitude         = -1.0,     /* exactly -46603 in i24 units */
        .altitude_ft       = 25000,    /* exactly 1000 in 25-ft units */
        .ground_speed_kt   = 450.0,    /* exactly 4500 in 0.1-kt units */
        .track_deg         = 90.0,     /* exactly 9000 in 0.01-deg units */
        .vertical_rate_fpm = -1024,
        .age_ds            = 4321,
        .flags             = GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE |
                             GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE,
    };
    n = gama_track_encode(payload, sizeof(payload), &t);
    dump("vec_track", payload, (size_t)n);

    /* --- a full TM_TRACKS frame carrying that one record --- */
    n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TM_TRACKS,
                          0x0102, payload, (uint8_t)GAMA_TRACK_WIRE_LEN);
    dump("vec_frame_tracks", frame, (size_t)n);

    /* --- a roster record --- */
    gama_roster_t r = { .icao = 0x123456u, .callsign = "GOL1234" };
    n = gama_roster_encode(payload, sizeof(payload), &r);
    dump("vec_roster", payload, (size_t)n);

    /* --- housekeeping --- */
    gama_hk_t hk = {
        .battery_mv = 8200, .current_ma = -250,
        .temp_ext_ccel = -1050, .temp_soc_ccel = 4875,
        .roll_cdeg = 100, .pitch_cdeg = -200, .yaw_cdeg = 9000,
        .adsb_msgs = 0x00BC614Eu, .obc_mode = GAMA_OBC_ST_MISSION_ADSB,
        .link_state = GAMA_LINK_STREAM, .uptime_s = 0x00015180u,
        .flags = GAMA_HK_F_OBC_LINKED | GAMA_HK_F_ADSB_LINKED |
                 GAMA_HK_F_TIME_SYNCED | GAMA_HK_F_DUMP1090_UP,
    };
    n = gama_hk_encode(payload, sizeof(payload), &hk);
    dump("vec_hk", payload, (size_t)n);

    /* --- mission statistics --- */
    gama_stat_t st = {
        .msgs_received = 60000, .msgs_decoded = 57600,
        .aircraft_tracked = 20, .tm_frames_sent = 286,
        .tc_frames_rx = 40, .tc_frames_bad = 2,
        .latency_p95_ms = 5300, .dump1090_restarts = 0,
    };
    n = gama_stat_encode(payload, sizeof(payload), &st);
    dump("vec_stat", payload, (size_t)n);

    /* --- a SET_RATE telecommand frame --- */
    payload[0] = GAMA_TC_SET_RATE;
    payload[1] = GAMA_RATE_FAST;
    n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TC, 0x0001, payload, 2);
    dump("vec_frame_tc_set_rate", frame, (size_t)n);

    /* --- the acknowledgement it produces --- */
    payload[0] = GAMA_TC_SET_RATE;
    payload[1] = GAMA_ACK_OK;
    gama_put_u16(payload + 2, 0x0001);
    n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TC_ACK, 0x0001,
                          payload, GAMA_TC_ACK_WIRE_LEN);
    dump("vec_frame_tc_ack", frame, (size_t)n);

    /* --- IPC payloads: the OBC<->TT&C interface contract --- */
    gama_ipc_hello_t hello = { .role = GAMA_IPC_ROLE_OBC, .version = GAMA_IPC_VERSION };
    n = gama_ipc_hello_encode(payload, sizeof(payload), &hello);
    dump("vec_ipc_hello", payload, (size_t)n);

    gama_ipc_time_t tm = { .unix_s = 0x12345678u, .nsec = 0x0A0B0C0Du };
    n = gama_ipc_time_encode(payload, sizeof(payload), &tm);
    dump("vec_ipc_time", payload, (size_t)n);

    gama_ipc_telemetry_t tel = {
        .battery_mv = 8200, .current_ma = -250, .temp_bat_ccel = -150,
        .temp_ext_ccel = 2345, .roll_cdeg = 100, .pitch_cdeg = -200, .yaw_cdeg = 9000,
    };
    n = gama_ipc_telemetry_encode(payload, sizeof(payload), &tel);
    dump("vec_ipc_telemetry", payload, (size_t)n);

    gama_ipc_stat_t ist = { .msgs_received = 60000, .msgs_decoded = 57600,
                            .aircraft_tracked = 20, .dump1090_restarts = 1 };
    n = gama_ipc_stat_encode(payload, sizeof(payload), &ist);
    dump("vec_ipc_stat", payload, (size_t)n);

    /* --- a complete IPC frame: telecommand event for the OBC --- */
    payload[0] = GAMA_OBC_EV_TC_MISSION_ADSB;
    n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_IPC_TC_EVENT, 0x0005,
                          payload, GAMA_IPC_TC_EVENT_LEN);
    dump("vec_frame_ipc_tc_event", frame, (size_t)n);

    /* --- an empty beacon: the smallest legal frame --- */
    n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_BEACON, 0x00FF, NULL, 0);
    dump("vec_frame_beacon", frame, (size_t)n);

    return 0;
}
