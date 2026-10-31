/*
 * The ground console (ground/console/gs_console.h): the lines the bench
 * scripts and ground/host/tracks_to_ndjson.py read byte for byte, whichever
 * ground station prints them (tools/gs_cli or the ESP32).
 *
 * A fake radio: frames the console transmits are captured, frames from the
 * satellite are built with the shared codec and handed in. The clock is
 * virtual, the wall clock fixed, so whole lines can be compared exactly.
 */

#include "test_util.h"
#include "gs_console.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_tc.h"
#include "gama_tm.h"

#include <stdio.h>

#define MAX_LINES 64
#define WALL_MS   1793000000000ull

static struct {
    char     lines[MAX_LINES][GS_CONSOLE_LINE_MAX + 1];
    int      n_lines;
    uint8_t  tx[GAMA_FRAME_MAX_TOTAL];
    size_t   tx_len;
    int      n_tx;
    bool     has_wall;
    int8_t   power;
    bool     lbt_preamble;
    uint16_t sat_seq;
} H;

static int f_tx(void *c, const uint8_t *f, size_t n)
{
    (void)c;
    memcpy(H.tx, f, n);
    H.tx_len = n;
    H.n_tx++;
    return GS_TX_STARTED;
}
static int f_prof(void *c, uint8_t p) { (void)c; (void)p; return 0; }
static int f_power(void *c, int8_t dbm) { (void)c; H.power = dbm; return 0; }
static int f_lbt(void *c, bool pre) { (void)c; H.lbt_preamble = pre; return 0; }
static int f_wall(void *c, uint64_t *ms)
{
    (void)c;
    if (!H.has_wall) { return -1; }
    *ms = WALL_MS;
    return 0;
}
static void f_write(void *c, const char *line, size_t len)
{
    (void)c;
    if (H.n_lines < MAX_LINES && len <= GS_CONSOLE_LINE_MAX) {
        memcpy(H.lines[H.n_lines], line, len);
        H.lines[H.n_lines][len] = '\0';
    }
    H.n_lines++;
}

static gs_console_t C;
static const gs_console_ops_t OPS = { .radio_tx = f_tx, .radio_set_profile = f_prof,
                                      .radio_set_power = f_power, .radio_set_lbt = f_lbt,
                                      .wall_ms = f_wall, .write_line = f_write };

static void fresh(bool wall)
{
    memset(&H, 0, sizeof(H));
    H.has_wall = wall;
    gs_console_init(&C, &OPS, "test");
}

static void line(uint64_t now, const char *text)
{
    char buf[GS_CONSOLE_INPUT_MAX];
    snprintf(buf, sizeof(buf), "%s", text);
    gs_console_line(&C, now, buf);
}

/* A frame from the satellite, with consecutive sequence numbers. */
static void from_sat(uint64_t now, uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t f[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(f, sizeof(f), type, ++H.sat_seq, payload, len);
    gs_console_on_rx(&C, now, f, (size_t)n, -60, 9);
}

/* The satellite acknowledges the telecommand last transmitted. */
static void ack_last(uint64_t now)
{
    gs_console_on_tx_done(&C, now);
    gama_frame_t f;
    gama_frame_decode(H.tx, H.tx_len, &f);
    uint8_t a[GAMA_TC_ACK_WIRE_LEN] = { f.payload[0], GAMA_ACK_OK };
    gama_put_u16(a + 2, f.seq);
    from_sat(now, GAMA_FRAME_TC_ACK, a, sizeof(a));
}

static bool printed(const char *want)
{
    for (int i = 0; i < H.n_lines && i < MAX_LINES; i++) {
        if (strcmp(H.lines[i], want) == 0) { return true; }
    }
    printf("  missing: %s", want);
    return false;
}

static const char *last_line(void)
{
    return H.n_lines > 0 ? H.lines[(H.n_lines < MAX_LINES ? H.n_lines : MAX_LINES) - 1] : "";
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle)) { n++; }
    return n;
}

int main(void)
{
    TEST_GROUP("console: the start line, as gs_cli has always printed it");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        CHECK(printed("{\"mono_ms\":1000,\"wall_ms\":1793000000000,\"ev\":\"start\","
                      "\"radio\":\"test\",\"profile\":\"NOMINAL\"}\n"));
    }

    TEST_GROUP("console: a telecommand: echo, frame, ack — and the next line waits for it");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        CHECK(gs_console_wants_line(&C));
        line(2000, "hk");
        CHECK(printed("{\"mono_ms\":2000,\"wall_ms\":1793000000000,\"ev\":\"command\",\"text\":\"hk\"}\n"));
        CHECK_EQ_INT(H.n_tx, 1);
        CHECK_EQ_INT(H.tx[1], GAMA_FRAME_TC);
        CHECK_EQ_INT(H.tx[5], GAMA_TC_REQ_HK);
        CHECK(!gs_console_wants_line(&C));                  /* waits for its ACK */
        ack_last(2300);
        CHECK(printed("{\"mono_ms\":2300,\"wall_ms\":1793000000000,\"ev\":\"ack\",\"id\":1,"
                      "\"cmd\":\"REQ_HK\",\"status\":\"OK\",\"latency_ms\":300,\"attempts\":1}\n"));
        CHECK(gs_console_wants_line(&C));
    }

    TEST_GROUP("console: housekeeping, byte for byte");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        gama_hk_t hk = {
            .battery_mv = 8200, .current_ma = -250, .temp_bat_ccel = -150,
            .temp_ext_ccel = -1050, .roll_cdeg = 100, .pitch_cdeg = -200,
            .yaw_cdeg = 9000, .temp_soc_ccel = 4875, .adsb_msgs = 12345678u,
            .obc_mode = GAMA_OBC_ST_MISSION_ADSB, .link_state = GAMA_LINK_STREAM,
            .uptime_s = 86400u, .flags = 0x0F,
        };
        uint8_t p[GAMA_HK_WIRE_LEN];
        gama_hk_encode(p, sizeof(p), &hk);
        from_sat(5000, GAMA_FRAME_TM_HK, p, sizeof(p));
        CHECK(printed("{\"mono_ms\":5000,\"wall_ms\":1793000000000,\"ev\":\"contact\",\"up\":true}\n"));
        CHECK(printed("{\"mono_ms\":5000,\"wall_ms\":1793000000000,\"ev\":\"hk\",\"seq\":1,"
                      "\"rssi\":-60,\"snr\":9,\"battery_mv\":8200,\"current_ma\":-250,"
                      "\"temp_bat_ccel\":-150,\"temp_ext_ccel\":-1050,\"temp_soc_ccel\":4875,"
                      "\"roll_cdeg\":100,\"pitch_cdeg\":-200,\"yaw_cdeg\":9000,"
                      "\"adsb_msgs\":12345678,\"obc_mode\":\"MISSION_ADSB\",\"link\":\"STREAM\","
                      "\"uptime_s\":86400,\"flags\":15,"
                      "\"payload\":\"082006ff6affe6fb640038ff28230b134e61bc000302805101000f\"}\n"));
    }

    TEST_GROUP("console: twelve track records at their widest all reach the line");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        uint8_t p[12 * GAMA_TRACK_WIRE_LEN];
        for (int i = 0; i < 12; i++) {
            gama_track_t t = { .icao = 0xE48000u + (uint32_t)i, .latitude = -89.12345,
                               .longitude = -179.12345, .altitude_ft = -819175,
                               .ground_speed_kt = 6553.4, .track_deg = 359.99,
                               .vertical_rate_fpm = -32768, .age_ds = 65535, .flags = 0xFF };
            gama_track_encode(p + (size_t)i * GAMA_TRACK_WIRE_LEN, GAMA_TRACK_WIRE_LEN, &t);
        }
        from_sat(6000, GAMA_FRAME_TM_TRACKS, p, sizeof(p));
        const char *l = last_line();
        CHECK(strstr(l, "\"ev\":\"tracks\",\"seq\":1,\"rssi\":-60,\"snr\":9,\"toa_ms\":") != NULL);
        CHECK(strstr(l, ",\"n\":12,\"records\":[{\"icao\":\"E48000\",") != NULL);
        CHECK_EQ_INT(count(l, "\"icao\":"), 12);              /* gs_cli printed 11 */
        CHECK(strstr(l, "\"icao\":\"E4800B\",") != NULL);
        CHECK(strstr(l, "\"vr_fpm\":-32768,\"age_ds\":65535,\"flags\":255}],\"payload\":\"") != NULL);
        CHECK_EQ_INT(C.lines_too_long, 0);
        size_t n = strlen(l);
        CHECK(n > 2 && l[n - 2] == '}' && l[n - 1] == '\n');
    }

    TEST_GROUP("console: what arrives raw — a callsign, an input line — is escaped as JSON");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        gama_roster_t r = { .icao = 0x123456u, .callsign = "A\"B\\C" };
        r.callsign[5] = (char)0x01;
        uint8_t p[GAMA_ROSTER_WIRE_LEN];
        gama_roster_encode(p, sizeof(p), &r);
        from_sat(7000, GAMA_FRAME_TM_ROSTER, p, sizeof(p));
        CHECK(strstr(last_line(),
                     "\"aircraft\":[{\"icao\":\"123456\",\"callsign\":\"A\\\"B\\\\C\\u0001\"}]") != NULL);
        line(7100, "mode \"x\x7f");
        CHECK(printed("{\"mono_ms\":7100,\"wall_ms\":1793000000000,\"ev\":\"command\","
                      "\"text\":\"mode \\\"x\\u007f\"}\n"));
        CHECK(printed("{\"mono_ms\":7100,\"wall_ms\":1793000000000,\"ev\":\"error\","
                      "\"what\":\"unknown OBC event\"}\n"));
    }

    TEST_GROUP("console: before begin only set and begin; settings reach the radio");
    {
        fresh(false);
        line(100, "# a comment, ignored");
        line(100, "hk");
        CHECK(strstr(last_line(), "\"ev\":\"error\",\"what\":\"begin first") != NULL);
        line(110, "set power 2");
        CHECK(printed("{\"mono_ms\":110,\"wall_ms\":0,\"ev\":\"set\",\"key\":\"power\",\"value\":\"2\"}\n"));
        CHECK_EQ_INT(H.power, 2);
        line(120, "set power 30");
        CHECK(strstr(last_line(), "\"what\":\"power: 2..20 dBm\"") != NULL);
        line(130, "set lbt preamble");
        CHECK(H.lbt_preamble);
        line(140, "set profile fast");
        line(150, "set clock 1793000000150");
        line(160, "begin");
        CHECK(printed("{\"mono_ms\":160,\"wall_ms\":1793000000160,\"ev\":\"start\","
                      "\"radio\":\"test\",\"profile\":\"FAST\"}\n"));
        line(170, "set profile safe");                         /* too late */
        CHECK(strstr(last_line(), "only clock can be set after begin") != NULL);
        CHECK_EQ_INT(H.n_tx, 0);
    }

    TEST_GROUP("console: no wall clock, no SET_TIME — it would anchor the satellite in 1970");
    {
        fresh(false);
        gs_console_begin(&C, 1000);
        line(1100, "time");
        CHECK(strstr(last_line(), "\"ev\":\"error\",\"what\":\"no wall clock") != NULL);
        CHECK_EQ_INT(H.n_tx, 0);
        CHECK(gs_console_wants_line(&C));
        line(1200, "set clock 1793000000000");
        line(1300, "time");
        CHECK_EQ_INT(H.n_tx, 1);
        gama_frame_t f;
        gama_frame_decode(H.tx, H.tx_len, &f);
        CHECK_EQ_INT(f.payload[0], GAMA_TC_SET_TIME);
        /* Stamped at transmission: 1793000000000 + (1300 - 1200) ms. */
        CHECK_EQ_INT(gama_get_u32(f.payload + 1), 1793000000u);
        CHECK_EQ_INT(gama_get_u32(f.payload + 5), 100000000u);
    }

    TEST_GROUP("console: auto_time without a clock says so instead of sending");
    {
        fresh(false);
        const char *why = "";
        CHECK_EQ_INT(gs_console_set(&C, 0, "auto_time", "on", &why), 0);
        gs_console_begin(&C, 1000);
        uint8_t hkp[GAMA_HK_WIRE_LEN] = { 0 };
        from_sat(1500, GAMA_FRAME_TM_HK, hkp, sizeof(hkp));
        CHECK(printed("{\"mono_ms\":1500,\"wall_ms\":0,\"ev\":\"error\",\"what\":"
                      "\"no wall clock: SET_TIME not sent (set clock first)\"}\n"));
        CHECK_EQ_INT(H.n_tx, 0);
    }

    TEST_GROUP("console: bad numbers are refused, not cast");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        line(1100, "wait -5");
        CHECK(strstr(last_line(), "\"what\":\"wait SECONDS\"") != NULL);
        line(1200, "deafen 1e30");
        CHECK(strstr(last_line(), "\"what\":\"deafen SECONDS\"") != NULL);
        line(1300, "stream abc");
        CHECK(strstr(last_line(), "\"what\":\"stream SECONDS\"") != NULL);
        line(1400, "ground_power 25");
        CHECK(strstr(last_line(), "\"what\":\"ground_power: 2..20 dBm\"") != NULL);
        line(1500, "ground_power 14");
        CHECK_EQ_INT(H.power, 14);
        CHECK(gs_console_wants_line(&C));
        CHECK_EQ_INT(H.n_tx, 0);
    }

    TEST_GROUP("console: wait, quit and the summary");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        line(1000, "wait 2");
        CHECK(!gs_console_wants_line(&C));
        CHECK_EQ_INT(gs_console_next_deadline(&C) <= 3000, 1);
        gs_console_on_tick(&C, 2999);
        CHECK(!gs_console_wants_line(&C));
        gs_console_on_tick(&C, 3000);
        CHECK(gs_console_wants_line(&C));
        CHECK(!gs_console_finished(&C));
        line(3100, "quit");
        CHECK(gs_console_finished(&C));
        gs_console_summary(&C, 3200);
        CHECK(printed("{\"mono_ms\":3200,\"wall_ms\":1793000000000,\"ev\":\"summary\","
                      "\"dl_frames\":0,\"dl_lost\":0,\"dl_bad\":0,\"tc_sent\":0,\"tc_acked\":0,"
                      "\"tc_failed\":0,\"lbt_defers\":0}\n"));
    }

    TEST_GROUP("console: end of input finishes the command in progress first");
    {
        fresh(true);
        gs_console_begin(&C, 1000);
        line(1000, "stat");
        gs_console_eof(&C);
        CHECK(!gs_console_finished(&C));
        ack_last(1200);
        CHECK(gs_console_finished(&C));
    }

    TEST_SUMMARY("test_gs_console");
}
