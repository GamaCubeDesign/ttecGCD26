/* Telemetry records: round-trips, quantisation error bounds, clamping and
 * the degenerate inputs the SBS parser can realistically produce. */

#include "test_util.h"
#include "gama_tm.h"
#include "gama_frame.h"
#include "gama_tc.h"

#include <math.h>
#include <string.h>

int main(void)
{
    uint8_t buf[64];

    TEST_GROUP("track: round-trip stays inside the documented resolution");
    {
        gama_track_t in = {
            .icao              = 0xE48DF5u,
            .latitude          = -23.559616,  /* Sao Jose dos Campos     */
            .longitude         = -46.658908,
            .altitude_ft       = 37000,
            .ground_speed_kt   = 451.7,
            .track_deg         = 128.44,
            .vertical_rate_fpm = -1216,
            .age_ds            = 3471,
            .flags             = GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE |
                                 GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE,
        };
        gama_track_t out;

        CHECK_EQ_INT(gama_track_encode(buf, sizeof(buf), &in), GAMA_TRACK_WIRE_LEN);
        CHECK_EQ_INT(gama_track_decode(buf, sizeof(buf), &out), GAMA_TRACK_WIRE_LEN);

        CHECK_EQ_INT(out.icao, in.icao);
        /* Half a quantisation step is the worst case for round-to-nearest. */
        CHECK_NEAR(out.latitude,  in.latitude,  0.5 / GAMA_LAT_SCALE);
        CHECK_NEAR(out.longitude, in.longitude, 0.5 / GAMA_LON_SCALE);
        CHECK_NEAR(out.altitude_ft, in.altitude_ft, 25.0);
        CHECK_NEAR(out.ground_speed_kt, in.ground_speed_kt, 0.05);
        CHECK_NEAR(out.track_deg, in.track_deg, 0.005);
        CHECK_EQ_INT(out.vertical_rate_fpm, in.vertical_rate_fpm);
        CHECK_EQ_INT(out.age_ds, in.age_ds);
        CHECK_EQ_INT(out.flags, in.flags);
    }

    TEST_GROUP("track: quantisation error stays under the advertised metres");
    {
        /* The data budget claims ~1.2 m latitude and ~2.4 m longitude. Check
         * the claim holds across the whole range, not just near the origin. */
        const double deg_to_m = 111320.0;
        for (double lat = -90.0; lat <= 90.0; lat += 7.5) {
            for (double lon = -180.0; lon <= 180.0; lon += 15.0) {
                gama_track_t in = { .latitude = lat, .longitude = lon };
                gama_track_t out;
                gama_track_encode(buf, sizeof(buf), &in);
                gama_track_decode(buf, sizeof(buf), &out);
                CHECK(fabs(out.latitude  - lat) * deg_to_m < 1.2);
                CHECK(fabs(out.longitude - lon) * deg_to_m < 2.4);
            }
        }
    }

    TEST_GROUP("track: heading normalises into [0, 360)");
    {
        struct { double in; double want; } cases[] = {
            { 0.0,    0.0   }, { 359.99, 359.99 }, { 360.0, 0.0 },
            { -1.0,   359.0 }, { 720.0,  0.0    }, { 180.5, 180.5 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            gama_track_t in = { .track_deg = cases[i].in };
            gama_track_t out;
            gama_track_encode(buf, sizeof(buf), &in);
            gama_track_decode(buf, sizeof(buf), &out);
            CHECK_NEAR(out.track_deg, cases[i].want, 0.005);
        }
    }

    TEST_GROUP("track: out-of-range values saturate instead of wrapping");
    {
        /* A wrapped latitude would place the aircraft on the far side of the
         * planet while looking entirely plausible. Saturation is visible. */
        gama_track_t in = {
            .latitude          = 1000.0,
            .longitude         = -1000.0,
            .altitude_ft       = 50000000,
            .ground_speed_kt   = 1e9,
            .vertical_rate_fpm = -9999999,
        };
        gama_track_t out;
        gama_track_encode(buf, sizeof(buf), &in);
        gama_track_decode(buf, sizeof(buf), &out);

        CHECK(out.latitude  > 0.0 && out.latitude  <= 90.001);
        CHECK(out.longitude < 0.0 && out.longitude >= -180.001);
        CHECK(out.altitude_ft > 0);
        CHECK(out.ground_speed_kt > 0.0 && out.ground_speed_kt <= 6553.5);
        CHECK(out.vertical_rate_fpm < 0);
    }

    TEST_GROUP("track: NaN encodes to a defined value, never undefined behaviour");
    {
        /* atof() on a malformed SBS field yields 0.0, but a division
         * elsewhere in the pipeline could still produce NaN. It must not
         * reach an out-of-range cast. */
        gama_track_t in = {
            .latitude        = NAN,
            .longitude       = NAN,
            .ground_speed_kt = NAN,
            .track_deg       = NAN,
        };
        gama_track_t out;
        CHECK_EQ_INT(gama_track_encode(buf, sizeof(buf), &in), GAMA_TRACK_WIRE_LEN);
        gama_track_decode(buf, sizeof(buf), &out);
        CHECK(!isnan(out.latitude));
        CHECK(!isnan(out.longitude));
        CHECK(!isnan(out.ground_speed_kt));
        CHECK(!isnan(out.track_deg));
    }

    TEST_GROUP("track: a partially-known aircraft keeps its validity bits");
    {
        /* ADS-B sends position, velocity and identity in separate messages,
         * so a fresh track is normally incomplete. */
        gama_track_t in = {
            .icao        = 0x000001u,
            .altitude_ft = 1200,
            .flags       = GAMA_TRACK_F_ALTITUDE | GAMA_TRACK_F_ON_GROUND,
        };
        gama_track_t out;
        gama_track_encode(buf, sizeof(buf), &in);
        gama_track_decode(buf, sizeof(buf), &out);
        CHECK(!(out.flags & GAMA_TRACK_F_POSITION));
        CHECK(out.flags & GAMA_TRACK_F_ALTITUDE);
        CHECK(out.flags & GAMA_TRACK_F_ON_GROUND);
    }

    TEST_GROUP("track: ICAO keeps all 24 bits");
    {
        gama_track_t in = { .icao = 0xFFFFFFu };
        gama_track_t out;
        gama_track_encode(buf, sizeof(buf), &in);
        gama_track_decode(buf, sizeof(buf), &out);
        CHECK_EQ_INT(out.icao, 0xFFFFFFu);
    }

    TEST_GROUP("track: undersized buffers are refused");
    {
        gama_track_t t = { .icao = 1 };
        CHECK_EQ_INT(gama_track_encode(buf, GAMA_TRACK_WIRE_LEN - 1, &t),
                     GAMA_FRAME_ERR_ARG);
        CHECK_EQ_INT(gama_track_decode(buf, GAMA_TRACK_WIRE_LEN - 1, &t),
                     GAMA_FRAME_ERR_SHORT);
        CHECK_EQ_INT(gama_track_encode(NULL, 64, &t), GAMA_FRAME_ERR_ARG);
        CHECK_EQ_INT(gama_track_decode(buf, 64, NULL), GAMA_FRAME_ERR_ARG);
    }

    TEST_GROUP("track: twelve records fit one frame payload");
    {
        CHECK_EQ_INT(GAMA_TRACKS_PER_FRAME, 12);
        CHECK(GAMA_TRACKS_PER_FRAME * GAMA_TRACK_WIRE_LEN <= GAMA_FRAME_MAX_PAYLOAD);
        /* 20 aircraft therefore need exactly two frames. */
        CHECK_EQ_INT((20 + GAMA_TRACKS_PER_FRAME - 1) / GAMA_TRACKS_PER_FRAME, 2);
    }

    TEST_GROUP("roster: callsign is padded on the wire and stripped on arrival");
    {
        gama_roster_t in = { .icao = 0xABCDEFu, .callsign = "TAM3054" };
        gama_roster_t out;

        CHECK_EQ_INT(gama_roster_encode(buf, sizeof(buf), &in), GAMA_ROSTER_WIRE_LEN);
        CHECK_EQ_INT(buf[3 + 7], ' '); /* padded to the full 8 characters */

        CHECK_EQ_INT(gama_roster_decode(buf, sizeof(buf), &out), GAMA_ROSTER_WIRE_LEN);
        CHECK_EQ_INT(out.icao, in.icao);
        CHECK_STR_EQ(out.callsign, "TAM3054");
    }

    TEST_GROUP("roster: a full-width callsign is not truncated");
    {
        gama_roster_t in = { .icao = 1, .callsign = "ABCDEFGH" };
        gama_roster_t out;
        gama_roster_encode(buf, sizeof(buf), &in);
        gama_roster_decode(buf, sizeof(buf), &out);
        CHECK_STR_EQ(out.callsign, "ABCDEFGH");
    }

    TEST_GROUP("roster: an empty callsign survives the round trip");
    {
        gama_roster_t in = { .icao = 2, .callsign = "" };
        gama_roster_t out;
        gama_roster_encode(buf, sizeof(buf), &in);
        gama_roster_decode(buf, sizeof(buf), &out);
        CHECK_STR_EQ(out.callsign, "");
    }

    TEST_GROUP("hk: round-trip preserves every field");
    {
        gama_hk_t in = {
            .battery_mv = 8214, .current_ma = -342,
            .temp_ext_ccel = -1150, .temp_soc_ccel = 6725,
            .roll_cdeg = 1234, .pitch_cdeg = -5678, .yaw_cdeg = 18000,
            .adsb_msgs = 4294967295u, .obc_mode = GAMA_OBC_ST_MISSION_ADSB,
            .link_state = GAMA_LINK_STREAM, .uptime_s = 86399,
            .flags = GAMA_HK_F_OBC_LINKED | GAMA_HK_F_TIME_SYNCED,
        };
        gama_hk_t out;

        CHECK_EQ_INT(gama_hk_encode(buf, sizeof(buf), &in), GAMA_HK_WIRE_LEN);
        CHECK_EQ_INT(gama_hk_decode(buf, sizeof(buf), &out), GAMA_HK_WIRE_LEN);
        /* Field by field, never memcmp of the struct: its padding bytes are
         * not written by the decoder and their value is unspecified (on the
         * Pi under UBSan they held stack garbage). */
        CHECK_EQ_INT(out.battery_mv,    in.battery_mv);
        CHECK_EQ_INT(out.current_ma,    in.current_ma);
        CHECK_EQ_INT(out.temp_ext_ccel, in.temp_ext_ccel);
        CHECK_EQ_INT(out.temp_soc_ccel, in.temp_soc_ccel);
        CHECK_EQ_INT(out.roll_cdeg,     in.roll_cdeg);
        CHECK_EQ_INT(out.pitch_cdeg,    in.pitch_cdeg);
        CHECK_EQ_INT(out.yaw_cdeg,      in.yaw_cdeg);
        CHECK_EQ_INT(out.adsb_msgs,     in.adsb_msgs);
        CHECK_EQ_INT(out.obc_mode,      in.obc_mode);
        CHECK_EQ_INT(out.link_state,    in.link_state);
        CHECK_EQ_INT(out.uptime_s,      in.uptime_s);
        CHECK_EQ_INT(out.flags,         in.flags);
    }

    TEST_GROUP("hk: negative current reads back as charging");
    {
        gama_hk_t in = { .current_ma = -32768 };
        gama_hk_t out;
        gama_hk_encode(buf, sizeof(buf), &in);
        gama_hk_decode(buf, sizeof(buf), &out);
        CHECK_EQ_INT(out.current_ma, -32768);
    }

    TEST_GROUP("stat: round-trip preserves every counter");
    {
        gama_stat_t in = {
            .msgs_received = 61234, .msgs_decoded = 59876,
            .aircraft_tracked = 20, .tm_frames_sent = 287,
            .tc_frames_rx = 41, .tc_frames_bad = 3,
            .latency_p95_ms = 5312, .dump1090_restarts = 1,
        };
        gama_stat_t out;

        CHECK_EQ_INT(gama_stat_encode(buf, sizeof(buf), &in), GAMA_STAT_WIRE_LEN);
        CHECK_EQ_INT(gama_stat_decode(buf, sizeof(buf), &out), GAMA_STAT_WIRE_LEN);
        /* Field by field: see the hk group above. */
        CHECK_EQ_INT(out.msgs_received,     in.msgs_received);
        CHECK_EQ_INT(out.msgs_decoded,      in.msgs_decoded);
        CHECK_EQ_INT(out.aircraft_tracked,  in.aircraft_tracked);
        CHECK_EQ_INT(out.tm_frames_sent,    in.tm_frames_sent);
        CHECK_EQ_INT(out.tc_frames_rx,      in.tc_frames_rx);
        CHECK_EQ_INT(out.tc_frames_bad,     in.tc_frames_bad);
        CHECK_EQ_INT(out.latency_p95_ms,    in.latency_p95_ms);
        CHECK_EQ_INT(out.dump1090_restarts, in.dump1090_restarts);
    }

    TEST_GROUP("records: every payload fits a single frame");
    {
        CHECK(GAMA_HK_WIRE_LEN     <= GAMA_FRAME_MAX_PAYLOAD);
        CHECK(GAMA_STAT_WIRE_LEN   <= GAMA_FRAME_MAX_PAYLOAD);
        CHECK(GAMA_ROSTER_WIRE_LEN <= GAMA_FRAME_MAX_PAYLOAD);
        CHECK(GAMA_TRACK_WIRE_LEN  <= GAMA_FRAME_MAX_PAYLOAD);
    }

    TEST_SUMMARY("test_tm");
}
