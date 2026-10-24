/*
 * The obcd core on a virtual clock: every edge of the CDR's Figure 44, the
 * events each mode refuses, the burn-wire, the deadlines, the thermostat
 * and the telemetry it sends ttcd (OBC-5, OBC-6, OBC-7; PLANO D1-D6).
 */

#include "test_util.h"
#include "core.h"
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_tc.h"
#include "gama_tm.h"

#include <stdio.h>
#include <string.h>

#define MAX_IPC 512
#define MAX_LOG 512
#define T0      100000u

static struct {
    uint64_t now;
    int      n_ipc;
    uint8_t  ipc[MAX_IPC][GAMA_FRAME_MAX_TOTAL];
    size_t   ipc_len[MAX_IPC];
    bool     ipc_fail;
    int      n_log;
    char     log[MAX_LOG][32];
    char     log_fields[MAX_LOG][400];
    bool     burnwire, heater;
    int      burnwire_writes, heater_writes;
    bool     outputs_absent;
} H;

static int h_ipc(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (H.ipc_fail) {
        return -1;
    }
    /* A ring: a 600 s mission sends more frames than it holds, and the
     * tests look at the latest ones. */
    memcpy(H.ipc[H.n_ipc % MAX_IPC], f, n);
    H.ipc_len[H.n_ipc % MAX_IPC] = n;
    H.n_ipc++;
    return 0;
}

static void h_log(void *ctx, uint64_t now, const char *ev, const char *fields)
{
    (void)ctx; (void)now;
    if (H.n_log < MAX_LOG) {
        snprintf(H.log[H.n_log], sizeof(H.log[0]), "%s", ev);
        snprintf(H.log_fields[H.n_log], sizeof(H.log_fields[0]), "%s", fields);
    }
    H.n_log++;
}

static int h_burnwire(void *ctx, bool on)
{
    (void)ctx;
    if (H.outputs_absent) { return -1; }
    H.burnwire = on;
    H.burnwire_writes++;
    return 0;
}

static int h_heater(void *ctx, bool on)
{
    (void)ctx;
    if (H.outputs_absent) { return -1; }
    H.heater = on;
    H.heater_writes++;
    return 0;
}

static const obcd_ops_t OPS = { .ctx = NULL, .ipc_send = h_ipc, .log = h_log,
                                .burnwire_set = h_burnwire, .heater_set = h_heater };
static obcd_core_t C;
static obcd_params_t P;

static void boot(void)
{
    memset(&H, 0, sizeof(H));
    H.now = T0;
    H.burnwire = H.heater = true;             /* whatever a previous run left */
    obcd_params_default(&P);
    obcd_init(&C, &P, &OPS, H.now);
    obcd_on_ipc_link(&C, H.now, true);
}

/* Runs the core's timers up to `until`, as the shell's timerfd would. */
static void run_to(uint64_t until)
{
    for (int guard = 0; guard < 100000; guard++) {
        uint64_t d = obcd_next_deadline(&C);
        if (d > until) {
            break;
        }
        H.now = d > H.now ? d : H.now;
        obcd_on_tick(&C, H.now);
    }
    H.now = until;
    obcd_on_tick(&C, H.now);
}

static void event_from_ttcd(uint8_t ev)
{
    uint8_t f[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_TC_EVENT, 0, &ev, 1);
    obcd_on_ipc_frame(&C, H.now, f, (size_t)n);
}

static int count_log(const char *ev)
{
    int n = 0;
    for (int i = 0; i < H.n_log && i < MAX_LOG; i++) {
        n += strcmp(H.log[i], ev) == 0;
    }
    return n;
}

static const char *last_fields(const char *ev)
{
    for (int i = (H.n_log < MAX_LOG ? H.n_log : MAX_LOG) - 1; i >= 0; i--) {
        if (strcmp(H.log[i], ev) == 0) { return H.log_fields[i]; }
    }
    return "";
}

/* The payload of the last IPC frame of this type, or NULL. */
static const uint8_t *last_ipc(uint8_t type, uint8_t *len)
{
    int oldest = H.n_ipc > MAX_IPC ? H.n_ipc - MAX_IPC : 0;
    for (int k = H.n_ipc - 1; k >= oldest; k--) {
        int i = k % MAX_IPC;
        gama_frame_t f;
        if (gama_frame_decode(H.ipc[i], H.ipc_len[i], &f) > 0 && f.type == type) {
            if (len != NULL) { *len = f.len; }
            return H.ipc[i] + GAMA_FRAME_HEADER_LEN;
        }
    }
    return NULL;
}

static uint8_t reported_mode(void)
{
    const uint8_t *p = last_ipc(GAMA_FRAME_IPC_MODE, NULL);
    return p != NULL ? p[0] : 0xEE;
}

static gama_ipc_telemetry_t last_telemetry(void)
{
    gama_ipc_telemetry_t t;
    memset(&t, 0, sizeof(t));
    uint8_t len = 0;
    const uint8_t *p = last_ipc(GAMA_FRAME_IPC_TELEMETRY, &len);
    if (p != NULL) {
        gama_ipc_telemetry_decode(p, len, &t);
    }
    return t;
}

int main(void)
{
    TEST_GROUP("obcd: boot in PRE_TEST, outputs off, ttcd told the mode at once (D5, D3)");
    {
        boot();
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
        CHECK(!H.burnwire);
        CHECK(!H.heater);
        CHECK_EQ_INT(reported_mode(), GAMA_OBC_ST_PRE_TEST);
        CHECK(last_ipc(GAMA_FRAME_IPC_TELEMETRY, NULL) != NULL);
        CHECK_EQ_INT(count_log("boot"), 1);
    }

    TEST_GROUP("obcd: Figure 44 as a table: every valid edge, and nothing else");
    {
        const uint8_t modes[] = {
            GAMA_OBC_ST_PRE_TEST, GAMA_OBC_ST_BASIC_INTERMEDIATE, GAMA_OBC_ST_ADVANCED_AOCS,
            GAMA_OBC_ST_MISSION_ADSB, GAMA_OBC_ST_MISSION_DOWNLINK, GAMA_OBC_ST_ENV_SURVIVAL,
        };
        int valid = 0;
        for (size_t m = 0; m < sizeof(modes); m++) {
            for (uint8_t ev = 0; ev <= 8; ev++) {
                valid += obcd_transition(modes[m], ev) != GAMA_OBC_ST_UNKNOWN;
            }
        }
        /* 5 out of PRE_TEST, 2 out of MISSION_ADSB, 4 EV_TASK_DONE back. */
        CHECK_EQ_INT(valid, 11);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_PRE_TEST, GAMA_OBC_EV_TC_MISSION_ADSB),
                     GAMA_OBC_ST_MISSION_ADSB);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_MISSION_ADSB, GAMA_OBC_EV_ADSB_TIMEOUT),
                     GAMA_OBC_ST_MISSION_DOWNLINK);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_MISSION_ADSB, GAMA_OBC_EV_TC_DOWNLINK),
                     GAMA_OBC_ST_MISSION_DOWNLINK);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_PRE_TEST, GAMA_OBC_EV_TC_DOWNLINK),
                     GAMA_OBC_ST_MISSION_DOWNLINK);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_MISSION_DOWNLINK, GAMA_OBC_EV_TASK_DONE),
                     GAMA_OBC_ST_PRE_TEST);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_ENV_SURVIVAL, GAMA_OBC_EV_TASK_DONE),
                     GAMA_OBC_ST_PRE_TEST);
        /* D1: no mode is reached except from PRE_TEST. */
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_ADVANCED_AOCS, GAMA_OBC_EV_TC_MISSION_ADSB),
                     GAMA_OBC_ST_UNKNOWN);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_MISSION_ADSB, GAMA_OBC_EV_TASK_DONE),
                     GAMA_OBC_ST_UNKNOWN);
        CHECK_EQ_INT(obcd_transition(GAMA_OBC_ST_PRE_TEST, GAMA_OBC_EV_TASK_DONE),
                     GAMA_OBC_ST_UNKNOWN);
    }

    TEST_GROUP("obcd: an event not valid in the mode is refused, counted and logged");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TASK_DONE);                 /* nothing to finish */
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
        CHECK_EQ_INT(C.n.refused, 1);
        CHECK(strstr(last_fields("event_refused"), "\"event\":\"TASK_DONE\"") != NULL);
        event_from_ttcd(GAMA_OBC_EV_TC_MISSION_ADSB);
        event_from_ttcd(GAMA_OBC_EV_TC_AOCS);                   /* D1: not from here */
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_MISSION_ADSB);
        CHECK_EQ_INT(C.n.refused, 2);
        event_from_ttcd(0);                                     /* EV_NONE */
        event_from_ttcd(42);                                    /* not an event */
        CHECK_EQ_INT(C.n.refused, 4);
    }

    TEST_GROUP("obcd: MISSION_ADSB ends by itself after 600 s (OBC-6), then downlink, then PRE_TEST");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TC_MISSION_ADSB);
        CHECK_EQ_INT(reported_mode(), GAMA_OBC_ST_MISSION_ADSB);
        uint64_t entered = H.now;
        run_to(entered + 599999);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_MISSION_ADSB);
        run_to(entered + 600000);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_MISSION_DOWNLINK);
        CHECK_EQ_INT(reported_mode(), GAMA_OBC_ST_MISSION_DOWNLINK);
        CHECK(strstr(last_fields("mode"), "\"event\":\"ADSB_TIMEOUT\",\"by\":\"timer\"") != NULL);
        /* ttcd says the roster and statistics have left (D2). */
        event_from_ttcd(GAMA_OBC_EV_TASK_DONE);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
        CHECK_EQ_INT(reported_mode(), GAMA_OBC_ST_PRE_TEST);
        CHECK_EQ_INT(C.n.transitions, 3);
        CHECK_EQ_INT(count_log("mode"), 3);                    /* OBC-5 */
    }

    TEST_GROUP("obcd: the ground ends MISSION_ADSB early; the timer then does nothing");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TC_MISSION_ADSB);
        run_to(H.now + 120000);
        event_from_ttcd(GAMA_OBC_EV_TC_DOWNLINK);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_MISSION_DOWNLINK);
        run_to(H.now + 600000);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_MISSION_DOWNLINK);   /* waits for ttcd */
        CHECK_EQ_INT(C.n.refused, 0);
    }

    TEST_GROUP("obcd: BASIC_INTERMEDIATE fires the burn-wire once for 5 s, then EV_TASK_DONE (D3)");
    {
        boot();
        int writes = H.burnwire_writes;
        event_from_ttcd(GAMA_OBC_EV_TC_BASIC_INTER);
        CHECK(H.burnwire);
        CHECK_EQ_INT(H.burnwire_writes, writes + 1);
        run_to(H.now + 4999);
        CHECK(H.burnwire);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_BASIC_INTERMEDIATE);
        run_to(H.now + 1);
        CHECK(!H.burnwire);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
        CHECK(strstr(last_fields("mode"), "\"by\":\"burnwire\"") != NULL);
        CHECK_EQ_INT(H.burnwire_writes, writes + 2);           /* on once, off once */
        run_to(H.now + 60000);
        CHECK(!H.burnwire);                                    /* and never again */
    }

    TEST_GROUP("obcd: leaving BASIC_INTERMEDIATE early turns the burn-wire off at once");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TC_BASIC_INTER);
        run_to(H.now + 1000);
        CHECK(H.burnwire);
        event_from_ttcd(GAMA_OBC_EV_TASK_DONE);                /* the ground ends it */
        CHECK(!H.burnwire);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
        run_to(H.now + 10000);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);            /* no second TASK_DONE */
        CHECK_EQ_INT(C.n.refused, 0);
    }

    TEST_GROUP("obcd: with no burn-wire line the mode still ends, and the log says it was not driven");
    {
        memset(&H, 0, sizeof(H));
        H.now = T0;
        H.outputs_absent = true;
        obcd_params_default(&P);
        obcd_init(&C, &P, &OPS, H.now);
        obcd_on_ipc_link(&C, H.now, true);
        event_from_ttcd(GAMA_OBC_EV_TC_BASIC_INTER);
        CHECK(strstr(last_fields("burnwire"), "\"on\":true,\"why\":\"deploy\",\"driven\":false") != NULL);
        run_to(H.now + 5000);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
        CHECK(C.n.output_failures >= 2);
    }

    TEST_GROUP("obcd: ADVANCED_AOCS ends at its deadline even if the ESP32 never answers");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TC_AOCS);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_ADVANCED_AOCS);
        run_to(H.now + P.aocs_timeout - 1);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_ADVANCED_AOCS);
        run_to(H.now + 1);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
        CHECK(strstr(last_fields("mode"), "\"by\":\"aocs_deadline\"") != NULL);
        /* An EV_TASK_DONE from the ESP32 (PLANO 2.4) ends it sooner. */
        event_from_ttcd(GAMA_OBC_EV_TC_AOCS);
        obcd_post_event(&C, H.now + 10, GAMA_OBC_EV_TASK_DONE, "aocs");
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
    }

    TEST_GROUP("obcd: ENV_SURVIVAL is entered and left by telecommand only");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TC_SURVIVAL);
        run_to(H.now + 3600000);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_ENV_SURVIVAL);         /* no timer ends it */
        event_from_ttcd(GAMA_OBC_EV_TASK_DONE);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_PRE_TEST);
    }

    TEST_GROUP("obcd: thermostat on below 10 degC, off above 12, holding between (D4)");
    {
        boot();
        obcd_on_battery_temp(&C, H.now, true, 1500);
        CHECK(!H.heater);
        obcd_on_battery_temp(&C, H.now, true, 999);
        CHECK(H.heater);
        obcd_on_battery_temp(&C, H.now, true, 1100);           /* between: holds */
        CHECK(H.heater);
        obcd_on_battery_temp(&C, H.now, true, 1200);           /* not above yet */
        CHECK(H.heater);
        obcd_on_battery_temp(&C, H.now, true, 1201);
        CHECK(!H.heater);
        obcd_on_battery_temp(&C, H.now, true, 1100);           /* between, from above */
        CHECK(!H.heater);
        CHECK_EQ_INT(count_log("heater"), 3);                  /* boot, on, off */
    }

    TEST_GROUP("obcd: in every mode, including MISSION_ADSB and ENV_SURVIVAL");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TC_SURVIVAL);
        obcd_on_battery_temp(&C, H.now, true, -500);
        CHECK(H.heater);
        event_from_ttcd(GAMA_OBC_EV_TASK_DONE);
        CHECK(H.heater);                                       /* modes do not touch it */
    }

    TEST_GROUP("obcd: a failed or silent sensor turns the heaters off, logged once (D4)");
    {
        boot();
        obcd_on_battery_temp(&C, H.now, true, 500);
        CHECK(H.heater);
        int faults = count_log("temp_fault");
        obcd_on_battery_temp(&C, H.now + 1000, false, 0);       /* one failure: last value holds */
        CHECK(H.heater);
        /* No good reading for longer than temp_stale: off. */
        run_to(H.now + P.temp_stale + 1);
        CHECK(!H.heater);
        CHECK_EQ_INT(count_log("temp_fault"), faults + 1);
        run_to(H.now + 30000);
        CHECK_EQ_INT(count_log("temp_fault"), faults + 1);     /* once per fault */
        CHECK_EQ_INT(last_telemetry().temp_bat_ccel, GAMA_NO_DATA_I16);
        obcd_on_battery_temp(&C, H.now, true, 700);            /* back */
        CHECK(H.heater);
        CHECK_EQ_INT(count_log("temp_ok"), 1);
    }

    TEST_GROUP("obcd: IPC_TELEMETRY every second, with 'no data' for what was not measured (D6)");
    {
        boot();
        int before = H.n_ipc;
        run_to(H.now + 10000);
        int tel = 0;
        for (int i = before; i < H.n_ipc; i++) {
            tel += H.ipc[i % MAX_IPC][1] == GAMA_FRAME_IPC_TELEMETRY;
        }
        CHECK_EQ_INT(tel, 10);
        gama_ipc_telemetry_t t = last_telemetry();
        CHECK_EQ_INT(t.battery_mv, GAMA_NO_DATA_U16);
        CHECK_EQ_INT(t.current_ma, GAMA_NO_DATA_I16);
        CHECK_EQ_INT(t.temp_bat_ccel, GAMA_NO_DATA_I16);
        CHECK_EQ_INT(t.temp_ext_ccel, GAMA_NO_DATA_I16);
        CHECK_EQ_INT(t.roll_cdeg, GAMA_NO_DATA_I16);

        obcd_on_power(&C, H.now, true, 7900, 850);
        obcd_on_battery_temp(&C, H.now, true, 2312);
        run_to(H.now + 1000);
        t = last_telemetry();
        CHECK_EQ_INT(t.battery_mv, 7900);
        CHECK_EQ_INT(t.current_ma, 850);
        CHECK_EQ_INT(t.temp_bat_ccel, 2312);                   /* HLR-EPS-04 */
        CHECK_EQ_INT(t.yaw_cdeg, GAMA_NO_DATA_I16);            /* AOCS: PLANO 2.4 */

        /* The INA219 stops answering: after power_stale, no data again. */
        obcd_on_power(&C, H.now, false, 0, 0);
        run_to(H.now + P.power_stale + 1000);
        t = last_telemetry();
        CHECK_EQ_INT(t.battery_mv, GAMA_NO_DATA_U16);
        CHECK_EQ_INT(count_log("power_fault"), 1);
    }

    TEST_GROUP("obcd: with ttcd away nothing is sent; reconnected, it hears the mode at once");
    {
        boot();
        event_from_ttcd(GAMA_OBC_EV_TC_MISSION_ADSB);
        obcd_on_ipc_link(&C, H.now, false);
        int before = H.n_ipc;
        run_to(H.now + 5000);
        CHECK_EQ_INT(H.n_ipc, before);
        obcd_on_ipc_link(&C, H.now, true);
        CHECK_EQ_INT(reported_mode(), GAMA_OBC_ST_MISSION_ADSB);
        CHECK_EQ_INT(C.mode, GAMA_OBC_ST_MISSION_ADSB);       /* the mission goes on */
    }

    TEST_GROUP("obcd: frames that make no sense are counted; a time anchor is logged");
    {
        boot();
        uint8_t junk[3] = { 1, 2, 3 };
        obcd_on_ipc_frame(&C, H.now, junk, sizeof(junk));
        uint8_t f[GAMA_FRAME_MAX_TOTAL];
        int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_STAT, 0, NULL, 0);
        obcd_on_ipc_frame(&C, H.now, f, (size_t)n);
        CHECK_EQ_INT(C.n.ipc_rx_bad, 2);
        uint8_t tp[GAMA_IPC_TIME_LEN];
        gama_ipc_time_t t = { .unix_s = 1792800000u, .nsec = 5u };
        gama_ipc_time_encode(tp, sizeof(tp), &t);
        n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_TIME_SET, 0, tp, GAMA_IPC_TIME_LEN);
        obcd_on_ipc_frame(&C, H.now, f, (size_t)n);
        CHECK(C.time_synced);
        CHECK_EQ_INT(count_log("time_anchor"), 1);
        CHECK_EQ_INT(C.n.ipc_rx_bad, 2);
    }

    TEST_GROUP("obcd: a refused send to ttcd is counted, not retried in a loop");
    {
        boot();
        H.ipc_fail = true;
        run_to(H.now + 3000);
        CHECK_EQ_INT(C.n.ipc_tx_drops, 3);
        H.ipc_fail = false;
    }

    TEST_SUMMARY("test_obcd_core");
}
