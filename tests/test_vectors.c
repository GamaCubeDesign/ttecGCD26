/*
 * Golden wire vectors.
 *
 * This is the load-bearing test of the whole codec. Every other test checks
 * that encode and decode agree with each other, which they would continue to
 * do after an accidental change to the layout. These vectors pin the actual
 * bytes, so any change to field order, width, scaling or endianness fails
 * here -- including a change made on only one side of the link.
 *
 * The same vectors are compiled into the ESP32 firmware test build. Passing
 * on both targets is what demonstrates that the Raspberry Pi and the ground
 * station really do produce identical bytes, which is the claim that
 * docs/adr/0002-flight-software-language.md rests on.
 *
 * Regenerate with tools/gen_vectors after a deliberate protocol version
 * bump, and review the diff byte by byte. Never adjust a vector to make a
 * failing test pass.
 */

#include "test_util.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_tc.h"
#include "gama_tm.h"

int main(void)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL];

    TEST_GROUP("vectors: track record layout");
    {
        /* icao=0xABCDEF lat=+1deg lon=-1deg alt=25000ft gs=450kt trk=90deg
         * vrate=-1024fpm age=4321ds flags=POS|ALT|VEL|VRATE
         *
         * Field by field:
         *   EF CD AB  icao 0xABCDEF, little-endian u24
         *   16 6C 01  latitude  1.0 * 93206 = 93206 = 0x016C16
         *   F5 49 FF  longitude -1.0 * 46603 = -46603 = 0xFF49F5 in i24
         *   E8 03     altitude 25000 / 25 = 1000
         *   94 11     ground speed 450.0 * 10 = 4500
         *   28 23     track 90.00 * 100 = 9000
         *   00 FC     vertical rate -1024
         *   E1 10     age 4321 deciseconds
         *   0F        flags, bits 0..3 set
         */
        static const uint8_t vec_track[] = {
            0xEF, 0xCD, 0xAB, 0x16, 0x6C, 0x01, 0xF5, 0x49, 0xFF, 0xE8, 0x03, 0x94,
            0x11, 0x28, 0x23, 0x00, 0xFC, 0xE1, 0x10, 0x0F,
        };

        gama_track_t t = {
            .icao              = 0xABCDEFu,
            .latitude          = 1.0,
            .longitude         = -1.0,
            .altitude_ft       = 25000,
            .ground_speed_kt   = 450.0,
            .track_deg         = 90.0,
            .vertical_rate_fpm = -1024,
            .age_ds            = 4321,
            .flags             = GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE |
                                 GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE,
        };

        CHECK_EQ_INT(gama_track_encode(buf, sizeof(buf), &t), sizeof(vec_track));
        CHECK_MEM_EQ(buf, vec_track, sizeof(vec_track));

        /* And the reverse: the bytes must decode to the values above. */
        gama_track_t back;
        CHECK_EQ_INT(gama_track_decode(vec_track, sizeof(vec_track), &back),
                     GAMA_TRACK_WIRE_LEN);
        CHECK_EQ_INT(back.icao, 0xABCDEFu);
        CHECK_NEAR(back.latitude, 1.0, 1e-9);
        CHECK_NEAR(back.longitude, -1.0, 1e-9);
        CHECK_EQ_INT(back.altitude_ft, 25000);
        CHECK_NEAR(back.ground_speed_kt, 450.0, 1e-9);
        CHECK_NEAR(back.track_deg, 90.0, 1e-9);
        CHECK_EQ_INT(back.vertical_rate_fpm, -1024);
        CHECK_EQ_INT(back.age_ds, 4321);
    }

    TEST_GROUP("vectors: TM_TRACKS frame, header through CRC");
    {
        /* 01        protocol version 1
         * 11        type TM_TRACKS
         * 02 01     sequence 0x0102, little-endian
         * 14        payload length 20
         * ...       the track record above
         * 7E B4     CRC-16/CCITT-FALSE over the preceding 25 bytes
         */
        static const uint8_t vec[] = {
            0x01, 0x11, 0x02, 0x01, 0x14, 0xEF, 0xCD, 0xAB, 0x16, 0x6C, 0x01, 0xF5,
            0x49, 0xFF, 0xE8, 0x03, 0x94, 0x11, 0x28, 0x23, 0x00, 0xFC, 0xE1, 0x10,
            0x0F, 0x7E, 0xB4,
        };

        gama_track_t t = {
            .icao = 0xABCDEFu, .latitude = 1.0, .longitude = -1.0,
            .altitude_ft = 25000, .ground_speed_kt = 450.0, .track_deg = 90.0,
            .vertical_rate_fpm = -1024, .age_ds = 4321,
            .flags = GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE |
                     GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE,
        };
        uint8_t payload[GAMA_TRACK_WIRE_LEN];
        gama_track_encode(payload, sizeof(payload), &t);

        int n = gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TM_TRACKS,
                                  0x0102, payload, GAMA_TRACK_WIRE_LEN);
        CHECK_EQ_INT(n, sizeof(vec));
        CHECK_MEM_EQ(buf, vec, sizeof(vec));

        gama_frame_t f;
        CHECK_EQ_INT(gama_frame_decode(vec, sizeof(vec), &f), sizeof(vec));
        CHECK_EQ_INT(f.type, GAMA_FRAME_TM_TRACKS);
        CHECK_EQ_INT(f.seq, 0x0102);
        CHECK_EQ_INT(f.len, GAMA_TRACK_WIRE_LEN);
    }

    TEST_GROUP("vectors: roster record layout");
    {
        /* 56 34 12                 icao 0x123456
         * 47 4F 4C 31 32 33 34 20  "GOL1234" padded to 8 with a space
         */
        static const uint8_t vec[] = {
            0x56, 0x34, 0x12, 0x47, 0x4F, 0x4C, 0x31, 0x32, 0x33, 0x34, 0x20,
        };
        gama_roster_t r = { .icao = 0x123456u, .callsign = "GOL1234" };

        CHECK_EQ_INT(gama_roster_encode(buf, sizeof(buf), &r), sizeof(vec));
        CHECK_MEM_EQ(buf, vec, sizeof(vec));

        gama_roster_t back;
        gama_roster_decode(vec, sizeof(vec), &back);
        CHECK_EQ_INT(back.icao, 0x123456u);
        CHECK_STR_EQ(back.callsign, "GOL1234");
    }

    TEST_GROUP("vectors: housekeeping layout");
    {
        /* 08 20  battery 8200 mV       06 FF  current -250 mA
         * E6 FB  ext temp -10.50 C     0B 13  SoC temp 48.75 C
         * 64 00  roll 1.00 deg         38 FF  pitch -2.00 deg
         * 28 23  yaw 90.00 deg         4E 61 BC 00  12345678 ADS-B messages
         * 03     OBC mode MISSION_ADSB 02     link state STREAM
         * 80 51 01 00  uptime 86400 s  0F     all four status flags set
         */
        static const uint8_t vec[] = {
            0x08, 0x20, 0x06, 0xFF, 0xE6, 0xFB, 0x0B, 0x13, 0x64, 0x00, 0x38, 0xFF,
            0x28, 0x23, 0x4E, 0x61, 0xBC, 0x00, 0x03, 0x02, 0x80, 0x51, 0x01, 0x00,
            0x0F,
        };
        gama_hk_t hk = {
            .battery_mv = 8200, .current_ma = -250,
            .temp_ext_ccel = -1050, .temp_soc_ccel = 4875,
            .roll_cdeg = 100, .pitch_cdeg = -200, .yaw_cdeg = 9000,
            .adsb_msgs = 12345678u, .obc_mode = GAMA_OBC_ST_MISSION_ADSB,
            .link_state = GAMA_LINK_STREAM, .uptime_s = 86400u,
            .flags = GAMA_HK_F_OBC_LINKED | GAMA_HK_F_ADSB_LINKED |
                     GAMA_HK_F_TIME_SYNCED | GAMA_HK_F_DUMP1090_UP,
        };

        CHECK_EQ_INT(gama_hk_encode(buf, sizeof(buf), &hk), sizeof(vec));
        CHECK_MEM_EQ(buf, vec, sizeof(vec));
    }

    TEST_GROUP("vectors: mission statistics layout");
    {
        /* 60 EA 00 00  60000 received   00 E1 00 00  57600 decoded
         * 14 00        20 aircraft      1E 01 00 00  286 TM frames sent
         * 28 00 00 00  40 TC received   02 00 00 00  2 TC rejected
         * B4 14        p95 latency 5300 ms          00 00  no restarts
         */
        static const uint8_t vec[] = {
            0x60, 0xEA, 0x00, 0x00, 0x00, 0xE1, 0x00, 0x00, 0x14, 0x00, 0x1E, 0x01,
            0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xB4, 0x14,
            0x00, 0x00,
        };
        gama_stat_t s = {
            .msgs_received = 60000, .msgs_decoded = 57600,
            .aircraft_tracked = 20, .tm_frames_sent = 286,
            .tc_frames_rx = 40, .tc_frames_bad = 2,
            .latency_p95_ms = 5300, .dump1090_restarts = 0,
        };

        CHECK_EQ_INT(gama_stat_encode(buf, sizeof(buf), &s), sizeof(vec));
        CHECK_MEM_EQ(buf, vec, sizeof(vec));
    }

    TEST_GROUP("vectors: SET_RATE telecommand and its acknowledgement");
    {
        /* 01 01 01 00 02 | 20 02 | 9A EE
         * version, TC, sequence 1, 2 argument bytes, SET_RATE(FAST), CRC */
        static const uint8_t vec_tc[] = {
            0x01, 0x01, 0x01, 0x00, 0x02, 0x20, 0x02, 0x9A, 0xEE,
        };
        uint8_t args[2] = { GAMA_TC_SET_RATE, GAMA_RATE_FAST };
        CHECK_EQ_INT(gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC,
                                       0x0001, args, 2), sizeof(vec_tc));
        CHECK_MEM_EQ(buf, vec_tc, sizeof(vec_tc));

        /* 01 02 01 00 04 | 20 00 01 00 | EF 49
         * TC_ACK for SET_RATE, status OK, echoing sequence 1 */
        static const uint8_t vec_ack[] = {
            0x01, 0x02, 0x01, 0x00, 0x04, 0x20, 0x00, 0x01, 0x00, 0xEF, 0x49,
        };
        uint8_t ack[GAMA_TC_ACK_WIRE_LEN];
        ack[0] = GAMA_TC_SET_RATE;
        ack[1] = GAMA_ACK_OK;
        gama_put_u16(ack + 2, 0x0001);
        CHECK_EQ_INT(gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_TC_ACK,
                                       0x0001, ack, GAMA_TC_ACK_WIRE_LEN),
                     sizeof(vec_ack));
        CHECK_MEM_EQ(buf, vec_ack, sizeof(vec_ack));
    }

    TEST_GROUP("vectors: beacon is the smallest legal frame");
    {
        /* 01 7F FF 00 00 A5 E2 -- seven bytes, which is the framing overhead. */
        static const uint8_t vec[] = { 0x01, 0x7F, 0xFF, 0x00, 0x00, 0xA5, 0xE2 };

        CHECK_EQ_INT(gama_frame_encode(buf, sizeof(buf), GAMA_FRAME_BEACON,
                                       0x00FF, NULL, 0), sizeof(vec));
        CHECK_MEM_EQ(buf, vec, sizeof(vec));
        CHECK_EQ_INT(sizeof(vec), GAMA_FRAME_OVERHEAD);
    }

    TEST_GROUP("vectors: record sizes match the data budget");
    {
        /* docs/budgets/data-budget.md sizes the mission on these numbers.
         * If a record grows, the budget and the ADR must be revisited. */
        CHECK_EQ_INT(GAMA_TRACK_WIRE_LEN,  20);
        CHECK_EQ_INT(GAMA_ROSTER_WIRE_LEN, 11);
        CHECK_EQ_INT(GAMA_HK_WIRE_LEN,     25);
        CHECK_EQ_INT(GAMA_STAT_WIRE_LEN,   26);
        CHECK_EQ_INT(GAMA_FRAME_OVERHEAD,   7);
        CHECK_EQ_INT(GAMA_FRAME_MAX_PAYLOAD, 248);

        /* A 20-aircraft snapshot is two frames of 12 + 8 records. */
        CHECK_EQ_INT(GAMA_FRAME_OVERHEAD + 12 * GAMA_TRACK_WIRE_LEN, 247);
        CHECK_EQ_INT(GAMA_FRAME_OVERHEAD +  8 * GAMA_TRACK_WIRE_LEN, 167);
    }

    TEST_SUMMARY("test_vectors");
}
