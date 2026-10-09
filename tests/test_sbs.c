/*
 * SBS-1 parsing (HLR-ADS-02) and the onboard record line (HLR-ADS-07,
 * HLR-SW-02), byte for byte.
 *
 * The lines follow dump1090-fa's net_io.c: 22 fields, the four booleans at
 * the end written -1 for true, 0 for false, empty for unknown.
 */

#include "test_util.h"
#include "ndjson.h"
#include "sbs.h"

#include <string.h>

static sbs_result_t parse(const char *line, sbs_msg_t *m)
{
    return sbs_parse(line, strlen(line), m);
}

/* The record line for an SBS line, at a fixed arrival time. */
static const char *record(const char *sbs)
{
    static char buf[ADSBD_NDJSON_MAX];
    sbs_msg_t m;
    if (parse(sbs, &m) != SBS_OK || adsbd_ndjson_format(buf, sizeof(buf), &m,
                                                        1789900000123456789ull) < 0) {
        return "(failed)";
    }
    return buf;
}

int main(void)
{
    sbs_msg_t m;

    TEST_GROUP("sbs: airborne position (MSG,3)");
    {
        CHECK_EQ_INT(parse("MSG,3,1,1,E48DF5,1,2026/09/28,12:00:00.000,2026/09/28,12:00:00.000,"
                           ",37000,,,-23.559616,-46.658908,,,0,,0,0", &m), SBS_OK);
        CHECK_EQ_INT(m.type, 3);
        CHECK_EQ_INT(m.icao, 0xE48DF5);
        CHECK_EQ_INT(m.have, SBS_HAVE_ALTITUDE | SBS_HAVE_POSITION | SBS_HAVE_GROUND);
        CHECK_EQ_INT(m.altitude_ft, 37000);
        CHECK_NEAR(m.lat, -23.559616, 1e-9);
        CHECK_NEAR(m.lon, -46.658908, 1e-9);
        CHECK_EQ_INT(m.on_ground, 0);
    }

    TEST_GROUP("sbs: an empty on-ground field is unknown (a transponder with CA 6)");
    {
        CHECK_EQ_INT(parse("MSG,4,1,1,E48DF5,1,2026/09/28,12:00:00.500,2026/09/28,12:00:00.500,"
                           ",,451.7,128.4,,,-1216,,,,,", &m), SBS_OK);
        CHECK_EQ_INT(m.have, SBS_HAVE_SPEED | SBS_HAVE_TRACK | SBS_HAVE_VRATE);
        CHECK_NEAR(m.ground_speed_kt, 451.7, 1e-9);
        CHECK_NEAR(m.track_deg, 128.4, 1e-9);
        CHECK_EQ_INT(m.vertical_rate_fpm, -1216);
        CHECK_EQ_INT(m.on_ground, -1);
    }

    TEST_GROUP("sbs: identification (MSG,1), callsign padding removed");
    {
        CHECK_EQ_INT(parse("MSG,1,1,1,E48DF5,1,2026/09/28,12:00:01.000,2026/09/28,12:00:01.000,"
                           "TAM3054 ,,,,,,,,,,,", &m), SBS_OK);
        CHECK_EQ_INT(m.have, SBS_HAVE_CALLSIGN);
        CHECK_STR_EQ(m.callsign, "TAM3054");
    }

    TEST_GROUP("sbs: on the ground is -1, the BaseStation TRUE (the prototype's bug)");
    {
        CHECK_EQ_INT(parse("MSG,2,1,1,E49608,1,2026/09/28,12:00:02.000,2026/09/28,12:00:02.000,"
                           ",,12.0,275.3,-15.870000,-47.920000,,,,,,-1", &m), SBS_OK);
        CHECK_EQ_INT(m.on_ground, 1);
        CHECK(m.have & SBS_HAVE_GROUND);
        CHECK(m.have & SBS_HAVE_POSITION);
        CHECK_EQ_INT(parse("MSG,8,1,1,E49608,1,,,,,,,,,,,,,,,,1", &m), SBS_OK);
        CHECK_EQ_INT(m.on_ground, 1);                    /* "1" is accepted too */
        CHECK_EQ_INT(parse("MSG,8,1,1,E49608,1,,,,,,,,,,,,,,,,0", &m), SBS_OK);
        CHECK_EQ_INT(m.on_ground, 0);
    }

    TEST_GROUP("sbs: altitude and squawk replies (MSG,5 and MSG,6)");
    {
        CHECK_EQ_INT(parse("MSG,5,1,1,E48DF5,1,,,,,,36975,,,,,,,0,,0,0", &m), SBS_OK);
        CHECK_EQ_INT(m.altitude_ft, 36975);
        CHECK_EQ_INT(parse("MSG,6,1,1,E48DF5,1,,,,,,36975,,,,,,7700,-1,-1,0,0", &m), SBS_OK);
        CHECK(m.have & SBS_HAVE_SQUAWK);
        CHECK_STR_EQ(m.squawk, "7700");
    }

    TEST_GROUP("sbs: CRLF, lowercase hex, and a MSG cut after the address");
    {
        CHECK_EQ_INT(parse("MSG,3,1,1,e48df5,1,,,,,,37000,,,-23.5,-46.6,,,0,,0,0\r", &m), SBS_OK);
        CHECK_EQ_INT(m.icao, 0xE48DF5);
        CHECK_EQ_INT(m.on_ground, 0);
        CHECK_EQ_INT(parse("MSG,7,1,1,E48DF5", &m), SBS_OK);
        CHECK_EQ_INT(m.have, 0);
    }

    TEST_GROUP("sbs: other BaseStation lines are ignored, not rejected");
    {
        CHECK_EQ_INT(parse("STA,,5,179,400AE7,10103,2008/11/28,14:58:51.153,2008/11/28,14:58:51.153,RM", &m),
                     SBS_IGNORED);
        CHECK_EQ_INT(parse("ID,,,,400AE7,,,,,,RYR1427", &m), SBS_IGNORED);
        CHECK_EQ_INT(parse("CLK,,,,,,,,,,", &m), SBS_IGNORED);
    }

    TEST_GROUP("sbs: a non-ICAO address is recognised and skipped");
    {
        CHECK_EQ_INT(parse("MSG,3,1,1,~2A3B4C,1,,,,,,5000,,,-15.8,-47.9,,,0,,0,0", &m), SBS_NON_ICAO);
    }

    TEST_GROUP("sbs: malformed lines are rejected, each with its reason");
    {
        CHECK_EQ_INT(parse("", &m), SBS_ERR_FORMAT);
        CHECK_EQ_INT(parse("garbage", &m), SBS_ERR_FORMAT);
        CHECK_EQ_INT(parse("MSG,3", &m), SBS_ERR_FORMAT);
        CHECK_EQ_INT(parse("MSG,9,1,1,E48DF5,1,,,,,,,,,,,,,,,,", &m), SBS_ERR_TYPE);
        CHECK_EQ_INT(parse("MSG,,1,1,E48DF5,1,,,,,,,,,,,,,,,,", &m), SBS_ERR_TYPE);
        CHECK_EQ_INT(parse("MSG,3,1,1,E48DF,1,,,,,,,,,,,,,,,,", &m), SBS_ERR_ICAO);
        CHECK_EQ_INT(parse("MSG,3,1,1,E48DFG,1,,,,,,,,,,,,,,,,", &m), SBS_ERR_ICAO);
        CHECK_EQ_INT(parse("MSG,3,1,1,E48DF5,1,,,,,,37000,,,91.0,-46.6,,,0,,0,0", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,3,1,1,E48DF5,1,,,,,,37000,,,-23.5,,,,0,,0,0", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,3,1,1,E48DF5,1,,,,,,3.7e4,,,-23.5,-46.6,,,0,,0,0", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,4,1,1,E48DF5,1,,,,,,,nan,128.4,,,0,,,,,", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,4,1,1,E48DF5,1,,,,,,,451.7,361.0,,,0,,,,,", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,1,1,1,E48DF5,1,,,,,tam3054,,,,,,,,,,,", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,1,1,1,E48DF5,1,,,,,TAM\"3054,,,,,,,,,,,", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,1,1,1,E48DF5,1,,,,,TOOLONGCS,,,,,,,,,,,", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,6,1,1,E48DF5,1,,,,,,,,,,,,77,,,,", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(parse("MSG,8,1,1,E49608,1,,,,,,,,,,,,,,,,2", &m), SBS_ERR_VALUE);
        CHECK_EQ_INT(sbs_parse(NULL, 0, &m), SBS_ERR_FORMAT);
    }

    TEST_GROUP("ndjson: the three lines the data budget is sized from, byte for byte");
    {
        /* tools/analysis/lora_budget.py, ndjson_line_bytes(): if one of these
         * changes, the 160.4 B per line and the 7.34 MiB per mission quoted
         * in ADR-0003 and data-budget.md change with it. */
        CHECK_STR_EQ(record("MSG,3,1,1,E48DF5,1,,,,,,37000,,,-23.559616,-46.658908,,,,,,0"),
                     "{\"icao\":\"E48DF5\",\"rx_epoch_ns\":1789900000123456789,"
                     "\"transmission_type\":3,\"callsign\":\"\",\"altitude_ft\":37000,"
                     "\"lat\":-23.559616,\"lon\":-46.658908,\"on_ground\":0}\n");
        CHECK_STR_EQ(record("MSG,4,1,1,E48DF5,1,,,,,,,451.7,128.4,,,-1216,,,,,0"),
                     "{\"icao\":\"E48DF5\",\"rx_epoch_ns\":1789900000123456789,"
                     "\"transmission_type\":4,\"callsign\":\"\",\"ground_speed_kt\":451.7,"
                     "\"track_deg\":128.4,\"vertical_rate_fpm\":-1216,\"on_ground\":0}\n");
        CHECK_STR_EQ(record("MSG,1,1,1,E48DF5,1,,,,,TAM3054 ,,,,,,,,,,,0"),
                     "{\"icao\":\"E48DF5\",\"rx_epoch_ns\":1789900000123456789,"
                     "\"transmission_type\":1,\"callsign\":\"TAM3054\",\"on_ground\":0}\n");
    }

    TEST_GROUP("ndjson: on the ground is 1, unknown is -1, squawk is kept");
    {
        CHECK_STR_EQ(record("MSG,2,1,1,E49608,1,,,,,,,12.0,275.3,-15.870000,-47.920000,,,,,,-1"),
                     "{\"icao\":\"E49608\",\"rx_epoch_ns\":1789900000123456789,"
                     "\"transmission_type\":2,\"callsign\":\"\",\"ground_speed_kt\":12.0,"
                     "\"track_deg\":275.3,\"lat\":-15.870000,\"lon\":-47.920000,\"on_ground\":1}\n");
        CHECK_STR_EQ(record("MSG,6,1,1,E48DF5,1,,,,,,,,,,,,7700,-1,-1,0,"),
                     "{\"icao\":\"E48DF5\",\"rx_epoch_ns\":1789900000123456789,"
                     "\"transmission_type\":6,\"callsign\":\"\",\"squawk\":\"7700\",\"on_ground\":-1}\n");
    }

    TEST_GROUP("ndjson: a buffer too small fails instead of truncating");
    {
        char small[40];
        CHECK_EQ_INT(parse("MSG,3,1,1,E48DF5,1,,,,,,37000,,,-23.5,-46.6,,,0,,0,0", &m), SBS_OK);
        CHECK_EQ_INT(adsbd_ndjson_format(small, sizeof(small), &m, 1), -1);
    }

    TEST_SUMMARY("test_sbs");
}
