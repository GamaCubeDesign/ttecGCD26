/*
 * The adsbd core on a virtual clock: SBS lines in; the onboard record,
 * IPC_TRACKS and IPC_STAT out (HLR-ADS-07, HLR-ADS-08, HLR-SW-02).
 */

#include "test_util.h"
#include "core.h"
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_tm.h"
#include "ndjson.h"

#include <stdio.h>
#include <string.h>

#define MS      1000000ull
#define T0      (5000 * MS)          /* anything but zero */
#define MAX_IPC 256
#define MAX_REC 64

static struct {
    bool    ipc_fail;
    int     n_ipc;
    uint8_t ipc[MAX_IPC][GAMA_FRAME_MAX_TOTAL];
    size_t  ipc_len[MAX_IPC];
    int     n_rec;
    char    rec[MAX_REC][ADSBD_NDJSON_MAX];
    int     n_log;
    char    log[256][32];
    char    last_fields[512];
} H;

static int h_ipc(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (H.ipc_fail) {
        return -1;
    }
    if (H.n_ipc < MAX_IPC) {
        memcpy(H.ipc[H.n_ipc], f, n);
        H.ipc_len[H.n_ipc] = n;
    }
    H.n_ipc++;
    return 0;
}

static void h_rec(void *ctx, const char *line, size_t n)
{
    (void)ctx;
    if (H.n_rec < MAX_REC && n < ADSBD_NDJSON_MAX) {
        memcpy(H.rec[H.n_rec], line, n);
        H.rec[H.n_rec][n] = '\0';
    }
    H.n_rec++;
}

static void h_log(void *ctx, uint64_t now_ms, const char *ev, const char *fields)
{
    (void)ctx; (void)now_ms;
    if (H.n_log < 256) {
        snprintf(H.log[H.n_log], sizeof(H.log[0]), "%s", ev);
    }
    snprintf(H.last_fields, sizeof(H.last_fields), "%s", fields);
    H.n_log++;
}

static const adsbd_ops_t OPS = { .ctx = NULL, .ipc_send = h_ipc, .record = h_rec, .log = h_log };
static adsbd_core_t C;
static adsbd_params_t P;

static void reset(int64_t offset_ns)
{
    memset(&H, 0, sizeof(H));
    adsbd_params_default(&P);
    adsbd_init(&C, &P, &OPS, T0, offset_ns);
}

static void line(uint64_t now, const char *l)
{
    adsbd_on_line(&C, now, l, strlen(l));
}

static void position(uint64_t now, uint32_t icao, double lat)
{
    char l[160];
    snprintf(l, sizeof(l), "MSG,3,1,1,%06X,1,,,,,,30000,,,%.6f,-47.000000,,,0,,0,0",
             (unsigned)icao, lat);
    line(now, l);
}

static int count_log(const char *ev)
{
    int n = 0;
    for (int i = 0; i < H.n_log && i < 256; i++) {
        n += strcmp(H.log[i], ev) == 0;
    }
    return n;
}

static int count_ipc(int from, uint8_t type)
{
    int n = 0;
    for (int i = from; i < H.n_ipc && i < MAX_IPC; i++) {
        n += H.ipc[i][1] == type;
    }
    return n;
}

static int find_ipc(int from, uint8_t type)
{
    for (int i = from; i < H.n_ipc && i < MAX_IPC; i++) {
        if (H.ipc[i][1] == type) {
            return i;
        }
    }
    return -1;
}

static gama_ipc_stat_t stat_at(int i)
{
    gama_frame_t f;
    gama_ipc_stat_t s;
    memset(&s, 0, sizeof(s));
    if (i >= 0 && gama_frame_decode(H.ipc[i], H.ipc_len[i], &f) > 0) {
        gama_ipc_stat_decode(f.payload, f.len, &s);
    }
    return s;
}

/* The IPC_TIME_SET ttcd would send. */
static void time_set(uint64_t now, uint32_t unix_s, uint32_t nsec)
{
    uint8_t p[GAMA_IPC_TIME_LEN], f[GAMA_FRAME_MAX_TOTAL];
    gama_ipc_time_t t = { .unix_s = unix_s, .nsec = nsec };
    gama_ipc_time_encode(p, sizeof(p), &t);
    int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_TIME_SET, 0, p, GAMA_IPC_TIME_LEN);
    adsbd_on_ipc_frame(&C, now, f, (size_t)n);
}

int main(void)
{
    const int64_t WALL0 = 1789900000LL * 1000000000LL - (int64_t)T0;  /* wall(T0) */

    TEST_GROUP("core: every message recorded with its arrival; nothing to ttcd while unlinked");
    {
        reset(WALL0);
        position(T0 + 100 * MS, 0xE48DF5, -23.559616);
        line(T0 + 200 * MS, "MSG,4,1,1,E48DF5,1,,,,,,,451.7,128.4,,,-1216,,,,,");
        CHECK_EQ_INT(H.n_rec, 2);
        CHECK(strstr(H.rec[0], "\"rx_epoch_ns\":1789900000100000000,") != NULL);
        CHECK(strstr(H.rec[1], "\"rx_epoch_ns\":1789900000200000000,") != NULL);
        adsbd_on_tick(&C, T0 + 1000 * MS);
        CHECK_EQ_INT(H.n_ipc, 0);
        CHECK_EQ_INT(C.n.lines, 2);
        CHECK_EQ_INT(C.n.decoded, 2);
    }

    TEST_GROUP("core: once linked, a snapshot and the counters go at once, then every second");
    {
        adsbd_on_ipc_link(&C, T0 + 1500 * MS, true);
        CHECK_EQ_INT(adsbd_next_deadline(&C), T0 + 1500 * MS);
        adsbd_on_tick(&C, T0 + 1500 * MS);
        CHECK_EQ_INT(count_ipc(0, GAMA_FRAME_IPC_TRACKS), 1);
        CHECK_EQ_INT(count_ipc(0, GAMA_FRAME_IPC_STAT), 1);

        gama_frame_t f;
        gama_ipc_tracks_hdr_t h;
        size_t n = 0;
        int i = find_ipc(0, GAMA_FRAME_IPC_TRACKS);
        CHECK(gama_frame_decode(H.ipc[i], H.ipc_len[i], &f) > 0);
        CHECK(gama_ipc_tracks_decode(f.payload, f.len, &h, &n) > 0);
        CHECK_EQ_INT(h.epoch_ms, (uint32_t)((T0 + 1500 * MS) / MS));
        CHECK_EQ_INT(h.index, 0);
        CHECK_EQ_INT(h.count, 1);
        CHECK_EQ_INT(n, 1);
        gama_track_t t;
        gama_track_decode(f.payload + GAMA_IPC_TRACKS_HEADER_LEN, GAMA_TRACK_WIRE_LEN, &t);
        CHECK_EQ_INT(t.icao, 0xE48DF5);
        CHECK_EQ_INT(t.age_ds, 14);                       /* position at +100 ms */
        CHECK_EQ_INT(t.flags, GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE |
                              GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE);
        CHECK_NEAR(t.latitude, -23.559616, 1.0 / GAMA_LAT_SCALE);

        gama_ipc_stat_t s = stat_at(find_ipc(0, GAMA_FRAME_IPC_STAT));
        CHECK_EQ_INT(s.msgs_received, 2);
        CHECK_EQ_INT(s.msgs_decoded, 2);
        CHECK_EQ_INT(s.aircraft_tracked, 1);
        CHECK_EQ_INT(s.dump1090_restarts, 0);

        int from = H.n_ipc;
        adsbd_on_tick(&C, T0 + 2000 * MS);
        CHECK_EQ_INT(H.n_ipc, from);                      /* not yet */
        adsbd_on_tick(&C, T0 + 2500 * MS);
        CHECK_EQ_INT(count_ipc(from, GAMA_FRAME_IPC_TRACKS), 1);
        CHECK_EQ_INT(count_ipc(from, GAMA_FRAME_IPC_STAT), 1);
    }

    TEST_GROUP("core: an empty table still sends a snapshot: one frame, no records");
    {
        reset(WALL0);
        adsbd_on_ipc_link(&C, T0, true);
        adsbd_on_tick(&C, T0);
        gama_frame_t f;
        gama_ipc_tracks_hdr_t h;
        size_t n = 99;
        int i = find_ipc(0, GAMA_FRAME_IPC_TRACKS);
        CHECK(i >= 0);
        CHECK(gama_frame_decode(H.ipc[i], H.ipc_len[i], &f) > 0);
        CHECK(gama_ipc_tracks_decode(f.payload, f.len, &h, &n) > 0);
        CHECK_EQ_INT(h.count, 1);
        CHECK_EQ_INT(n, 0);
    }

    TEST_GROUP("core: 30 aircraft go as two frames of one epoch; six are counted out");
    {
        reset(WALL0);
        for (uint32_t a = 0; a < 30; a++) {
            position(T0 + a * MS, 0xB00000 + a, -15.0 - 0.01 * a);
        }
        adsbd_on_ipc_link(&C, T0 + 100 * MS, true);
        adsbd_on_tick(&C, T0 + 100 * MS);
        CHECK_EQ_INT(count_ipc(0, GAMA_FRAME_IPC_TRACKS), 2);
        uint32_t epochs[2] = { 0, 1 };
        size_t records = 0;
        for (int k = 0, i = -1; k < 2; k++) {
            i = find_ipc(i + 1, GAMA_FRAME_IPC_TRACKS);
            gama_frame_t f;
            gama_ipc_tracks_hdr_t h;
            size_t n = 0;
            gama_frame_decode(H.ipc[i], H.ipc_len[i], &f);
            gama_ipc_tracks_decode(f.payload, f.len, &h, &n);
            CHECK_EQ_INT(h.index, k);
            CHECK_EQ_INT(h.count, 2);
            epochs[k] = h.epoch_ms;
            records += n;
        }
        CHECK_EQ_INT(epochs[0], epochs[1]);
        CHECK_EQ_INT(records, 24);
        CHECK_EQ_INT(C.n.left_out, 6);
    }

    TEST_GROUP("core: the ground's time through ttcd re-anchors the record, and says by how much");
    {
        reset(0);                                   /* the system clock: wrong */
        position(T0 + 100 * MS, 0xE48DF5, -23.5);
        CHECK(strstr(H.rec[0], "\"rx_epoch_ns\":5100000000,") != NULL);
        time_set(T0 + 2000 * MS, 1789900000u, 0u);
        CHECK(C.time_synced);
        CHECK_EQ_INT(C.n.anchors, 1);
        CHECK_EQ_INT(count_log("time_anchor"), 1);
        CHECK(strstr(H.last_fields, "\"step_ms\":1789899993000") != NULL);
        position(T0 + 3000 * MS, 0xE48DF5, -23.5);
        CHECK(strstr(H.rec[1], "\"rx_epoch_ns\":1789900001000000000,") != NULL);
        CHECK_EQ_INT(adsbd_wall_ns(&C, T0 + 2000 * MS), 1789900000000000000ull);
    }

    TEST_GROUP("core: malformed lines are counted, the first twenty named, none recorded");
    {
        reset(WALL0);
        for (int k = 0; k < 30; k++) {
            line(T0, "MSG,3,1,1,NOTHEX,1,,,,,,30000,,,-15.0,-47.0,,,0,,0,0");
        }
        adsbd_on_overlong_line(&C, T0);
        line(T0, "STA,,5,179,400AE7,10103,,,,,RM");
        line(T0, "MSG,3,1,1,~2A3B4C,1,,,,,,5000,,,-15.8,-47.9,,,0,,0,0");
        line(T0, "");
        CHECK_EQ_INT(C.n.rejected, 31);
        CHECK_EQ_INT(C.n.ignored, 1);
        CHECK_EQ_INT(C.n.non_icao, 1);
        CHECK_EQ_INT(C.n.lines, 33);                     /* the empty one is not a line */
        CHECK_EQ_INT(C.n.decoded, 0);
        CHECK_EQ_INT(H.n_rec, 0);
        CHECK_EQ_INT(count_log("sbs_reject"), (int)ADSBD_REJECT_LOG_MAX);
    }

    TEST_GROUP("core: frames ttcd's queue refuses are counted, never retried in a loop");
    {
        reset(WALL0);
        H.ipc_fail = true;
        adsbd_on_ipc_link(&C, T0, true);
        adsbd_on_tick(&C, T0);
        CHECK_EQ_INT(C.n.ipc_tx_drops, 2);               /* the snapshot and the STAT */
        CHECK_EQ_INT(count_log("ipc_drop"), 2);
    }

    TEST_GROUP("core: anything but a sensible TIME_SET from ttcd is counted and ignored");
    {
        reset(WALL0);
        uint8_t junk[3] = { 1, 2, 3 };
        adsbd_on_ipc_frame(&C, T0, junk, sizeof(junk));
        uint8_t f[GAMA_FRAME_MAX_TOTAL];
        int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_TRACKS, 0, NULL, 0);
        adsbd_on_ipc_frame(&C, T0, f, (size_t)n);
        time_set(T0, 1789900000u, 1000000000u);          /* nsec out of range */
        CHECK_EQ_INT(C.n.ipc_rx_bad, 3);
        CHECK(!C.time_synced);
        CHECK_EQ_INT(count_log("ipc_unexpected"), 3);
    }

    TEST_GROUP("core: dump1090 restarts reach IPC_STAT (TM_STAT on the ground)");
    {
        reset(WALL0);
        adsbd_on_dump1090_restart(&C, T0);
        adsbd_on_dump1090_restart(&C, T0);
        adsbd_on_ipc_link(&C, T0, true);
        adsbd_on_tick(&C, T0);
        CHECK_EQ_INT(stat_at(find_ipc(0, GAMA_FRAME_IPC_STAT)).dump1090_restarts, 2);
    }

    TEST_GROUP("core: an aircraft silent for a minute leaves the table and the snapshot");
    {
        reset(WALL0);
        position(T0, 0xE48DF5, -23.5);
        adsbd_on_ipc_link(&C, T0, true);
        adsbd_on_tick(&C, T0 + 60001 * MS);
        CHECK_EQ_INT(C.n.expired, 1);
        CHECK_EQ_INT(C.table.count, 0);
        gama_ipc_stat_t s = stat_at(find_ipc(0, GAMA_FRAME_IPC_STAT));
        CHECK_EQ_INT(s.aircraft_tracked, 0);
    }

    TEST_GROUP("core: the counters reach the event log every minute");
    {
        reset(WALL0);
        position(T0, 0xE48DF5, -23.5);
        adsbd_on_tick(&C, T0 + 59999 * MS);
        CHECK_EQ_INT(count_log("stats"), 0);
        adsbd_on_tick(&C, T0 + 60000 * MS);
        CHECK_EQ_INT(count_log("stats"), 1);
        CHECK(strstr(H.last_fields, "\"decoded\":1,") != NULL);
        CHECK(strstr(H.last_fields, "\"time_synced\":false") != NULL);
    }

    TEST_SUMMARY("test_adsbd_core");
}
