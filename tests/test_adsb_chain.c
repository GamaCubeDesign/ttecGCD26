/*
 * The ADS-B chain, end to end, with no hardware: SBS lines -> the real adsbd
 * -> the real ttcd -> the UDP radio -> the ground's gs_link.
 *
 * Also the two properties the architecture promises that only the processes
 * together can show:
 *   - the radio outlives the payload: adsbd killed, ttcd keeps answering
 *     and sending housekeeping (ADR-0005);
 *   - a payload that restarts gets the ground's time back from ttcd at
 *     once, without another SET_TIME (ADR-0009).
 *
 * The ground's clock here runs a day ahead of this machine's, so a record
 * stamped with it cannot be mistaken for one stamped by the system clock.
 * Real time; ~8 s.
 *
 *   test_adsb_chain /path/to/ttcd /path/to/adsbd
 */

#define _GNU_SOURCE
#include "test_util.h"
#include "gama_frame.h"
#include "gama_gs_link.h"
#include "gama_ipc.h"
#include "gama_tc.h"
#include "gama_tm.h"
#include "radio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GROUND_AHEAD_S 86400u
#define AIRCRAFT       3

static char dir[64], sock_path[128], ttcd_conf[128], ttcd_log[128];
static char adsbd_conf[128], adsbd_log[128], rec_path[128], obc_log[128], obc_conf[128];
static const char *ttcd_bin, *adsbd_bin, *obc_bin;
static radio_t R;
static gs_link_t G;
static int ep = -1, tfd = -1, sbs_lfd = -1, sbs_fd = -1;
static pid_t ttcd_pid, adsbd_pid, obc_pid;

static const uint32_t ICAO[AIRCRAFT] = { 0xE48DF5, 0xE49608, 0xE4A669 };
static const double LAT0[AIRCRAFT] = { -15.70, -15.90, -16.10 };
static const char *const CALLSIGN[AIRCRAFT] = { "TAM3054", "GLO1234", "AZU4321" };
static int feeds;
static uint64_t next_feed;

static struct {
    uint32_t hk;
    gama_hk_t last_hk;
    bool seen[AIRCRAFT];
    gama_track_t track[AIRCRAFT];
    struct { uint32_t id; uint8_t status; bool done; } acks[16];
    int n_acks;
    int roster_frames, stat_frames;
    bool named[AIRCRAFT];                /* callsign heard in TM_ROSTER */
} S;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t wall_ns(void)
{
    struct timespec w;
    clock_gettime(CLOCK_REALTIME, &w);
    return (uint64_t)w.tv_sec * 1000000000u + (uint64_t)w.tv_nsec;
}

static uint16_t free_port(int type)
{
    int s = socket(AF_INET, type, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &l);
    close(s);
    return ntohs(a.sin_port);
}

static pid_t spawn(const char *bin, const char *conf)
{
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        execl(bin, bin, "-c", conf, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int stop(pid_t pid)
{
    kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 250; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        struct timespec t = { 0, 20000000L };
        nanosleep(&t, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

static int count_in(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) { return 0; }
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        n += strstr(line, needle) != NULL;
    }
    fclose(f);
    return n;
}

/* rx_epoch_ns of the record's last line. */
static uint64_t last_stamp(void)
{
    FILE *f = fopen(rec_path, "r");
    if (f == NULL) { return 0; }
    char line[512], last[512] = "";
    while (fgets(line, sizeof(line), f) != NULL) {
        snprintf(last, sizeof(last), "%s", line);
    }
    fclose(f);
    const char *s = strstr(last, "\"rx_epoch_ns\":");
    return s != NULL ? strtoull(s + 14, NULL, 10) : 0;
}

/* ---- the ground ---- */

static int g_tx(void *c, const uint8_t *f, size_t n) { (void)c; return R.transmit(&R, f, n); }
static int g_prof(void *c, uint8_t p) { (void)c; return R.set_profile(&R, p); }

static int g_wall(void *c, uint32_t *s, uint32_t *ns)
{
    (void)c;
    struct timespec w;
    clock_gettime(CLOCK_REALTIME, &w);
    *s = (uint32_t)w.tv_sec + GROUND_AHEAD_S;
    *ns = (uint32_t)w.tv_nsec;
    return 0;
}

static void g_event(void *c, uint64_t now, const gs_event_t *e)
{
    (void)c; (void)now;
    if (e->kind == GS_EV_ACK) {
        for (int i = 0; i < S.n_acks; i++) {
            if (S.acks[i].id == e->id) {
                S.acks[i].status = e->status;
                S.acks[i].done = true;
            }
        }
    } else if (e->kind == GS_EV_FRAME && e->type == GAMA_FRAME_TM_HK) {
        S.hk++;
        gama_hk_decode(e->payload, e->len, &S.last_hk);
    } else if (e->kind == GS_EV_FRAME && e->type == GAMA_FRAME_TM_STAT) {
        S.stat_frames++;
    } else if (e->kind == GS_EV_FRAME && e->type == GAMA_FRAME_TM_ROSTER) {
        S.roster_frames++;
        for (size_t r = 0; r + GAMA_ROSTER_WIRE_LEN <= e->len; r += GAMA_ROSTER_WIRE_LEN) {
            gama_roster_t ro;
            gama_roster_decode(e->payload + r, GAMA_ROSTER_WIRE_LEN, &ro);
            for (int a = 0; a < AIRCRAFT; a++) {
                if (ro.icao == ICAO[a] && strcmp(ro.callsign, CALLSIGN[a]) == 0) {
                    S.named[a] = true;
                }
            }
        }
    } else if (e->kind == GS_EV_FRAME && e->type == GAMA_FRAME_TM_TRACKS) {
        for (size_t r = 0; r + GAMA_TRACK_WIRE_LEN <= e->len; r += GAMA_TRACK_WIRE_LEN) {
            gama_track_t t;
            gama_track_decode(e->payload + r, GAMA_TRACK_WIRE_LEN, &t);
            for (int a = 0; a < AIRCRAFT; a++) {
                if (t.icao == ICAO[a]) {
                    S.seen[a] = true;
                    S.track[a] = t;
                }
            }
        }
    }
}

static int submit(uint8_t cmd, const uint8_t *args, uint8_t n)
{
    S.acks[S.n_acks].id = gs_submit(&G, now_ms(), cmd, args, n);
    S.acks[S.n_acks].done = false;
    return S.n_acks++;
}

/* ---- dump1090, played by this test ---- */

static void feed(void)
{
    if (sbs_fd < 0) {
        return;
    }
    char text[1024];
    size_t pos = 0;
    for (int a = 0; a < AIRCRAFT; a++) {
        pos += (size_t)snprintf(text + pos, sizeof(text) - pos,
                                "MSG,1,1,1,%06X,1,,,,,%s,,,,,,,,,,,\r\n"
                                "MSG,3,1,1,%06X,1,,,,,,%d,,,%.6f,-47.900000,,,0,,0,0\r\n"
                                "MSG,4,1,1,%06X,1,,,,,,,420.0,180.0,,,-640,,,,,\r\n",
                                (unsigned)ICAO[a], CALLSIGN[a],
                                (unsigned)ICAO[a], 20000 + 1000 * a, LAT0[a] - 0.0005 * feeds,
                                (unsigned)ICAO[a]);
    }
    feeds++;
    if (write(sbs_fd, text, pos) != (ssize_t)pos) {
        close(sbs_fd);                               /* adsbd went away */
        epoll_ctl(ep, EPOLL_CTL_DEL, sbs_fd, NULL);
        sbs_fd = -1;
    }
}

/* Runs the ground, and dump1090's feed, for up to `ms` or until *cond. */
static void run(uint64_t ms, const bool *cond)
{
    uint64_t end = now_ms() + ms;
    while (now_ms() < end && !(cond != NULL && *cond)) {
        uint64_t now = now_ms();
        uint64_t d = gs_next_deadline(&G);
        if (next_feed < d) { d = next_feed; }
        if (end < d) { d = end; }
        if (d <= now) { d = now + 1; }
        struct itimerspec its = { .it_value = { .tv_sec = (time_t)(d / 1000u),
                                                .tv_nsec = (long)(d % 1000u) * 1000000L } };
        timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
        struct epoll_event evs[8];
        int n = epoll_wait(ep, evs, 8, -1);
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == R.fd) {
                radio_event_t ev;
                while (R.next_event(&R, &ev) == 1) {
                    if (ev.kind == RADIO_EV_RX) {
                        gs_on_rx(&G, now_ms(), ev.buf, ev.len, ev.rssi, ev.snr);
                    } else if (ev.kind == RADIO_EV_TX_DONE) {
                        gs_on_tx_done(&G, now_ms());
                    }
                }
            } else if (fd == sbs_lfd) {
                int c = accept4(sbs_lfd, NULL, NULL, SOCK_CLOEXEC);
                if (c >= 0) {
                    if (sbs_fd >= 0) {
                        close(sbs_fd);
                    }
                    sbs_fd = c;
                }
            } else if (fd == tfd) {
                uint64_t x;
                if (read(tfd, &x, sizeof(x)) < 0) { /* spurious */ }
            }
        }
        gs_on_tick(&G, now_ms());
        if (now_ms() >= next_feed) {
            feed();
            next_feed = now_ms() + 500;
        }
    }
}

static uint8_t set_mode(uint8_t ev)
{
    int k = submit(GAMA_TC_SET_MODE, &ev, 1);
    run(3000, &S.acks[k].done);
    return S.acks[k].done ? S.acks[k].status : 0xFF;
}

/* Asks for housekeeping and waits for it to arrive. */
static bool fresh_hk(void)
{
    uint32_t before = S.hk;
    int k = submit(GAMA_TC_REQ_HK, NULL, 0);
    run(3000, &S.acks[k].done);
    for (int i = 0; i < 30 && S.hk == before; i++) {
        run(100, NULL);
    }
    return S.hk > before;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s /path/to/ttcd /path/to/adsbd /path/to/obcd\n", argv[0]);
        return 2;
    }
    ttcd_bin = argv[1];
    adsbd_bin = argv[2];
    obc_bin = argv[3];
    signal(SIGPIPE, SIG_IGN);
    snprintf(dir, sizeof(dir), "/tmp/adsb-chain-XXXXXX");
    if (mkdtemp(dir) == NULL) { perror("mkdtemp"); return 1; }
    snprintf(sock_path, sizeof(sock_path), "%s/ttec.sock", dir);
    snprintf(ttcd_conf, sizeof(ttcd_conf), "%s/ttcd.conf", dir);
    snprintf(ttcd_log, sizeof(ttcd_log), "%s/ttcd.jsonl", dir);
    snprintf(adsbd_conf, sizeof(adsbd_conf), "%s/adsbd.conf", dir);
    snprintf(adsbd_log, sizeof(adsbd_log), "%s/adsbd.jsonl", dir);
    snprintf(rec_path, sizeof(rec_path), "%s/adsb.ndjson", dir);
    snprintf(obc_log, sizeof(obc_log), "%s/obcd.jsonl", dir);
    snprintf(obc_conf, sizeof(obc_conf), "%s/obcd.conf", dir);

    uint16_t sat_port = free_port(SOCK_DGRAM), gnd_port = free_port(SOCK_DGRAM);
    sbs_lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(sbs_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(sbs_lfd, (struct sockaddr *)&a, sizeof(a));
    listen(sbs_lfd, 4);
    socklen_t al = sizeof(a);
    getsockname(sbs_lfd, (struct sockaddr *)&a, &al);

    FILE *cf = fopen(ttcd_conf, "w");
    fprintf(cf, "radio = udp\nudp_port = %u\nudp_peer_port = %u\nipc_path = %s\n"
                "log_path = %s\nhk_period_ms = 1000\n",
            sat_port, gnd_port, sock_path, ttcd_log);
    fclose(cf);
    cf = fopen(adsbd_conf, "w");
    fprintf(cf, "ipc_path = %s\nsbs_port = %u\nndjson_path = %s\nlog_path = %s\n"
                "dump1090_cmd =\nretry_ms = 200\ndecoder = mode\n",
            sock_path, ntohs(a.sin_port), rec_path, adsbd_log);
    fclose(cf);

    ttcd_pid = spawn(ttcd_bin, ttcd_conf);
    struct stat st;
    for (int i = 0; i < 250 && stat(sock_path, &st) != 0; i++) {
        struct timespec t = { 0, 20000000L };
        nanosleep(&t, NULL);
    }
    adsbd_pid = spawn(adsbd_bin, adsbd_conf);
    /* The real obcd, with no OBC board: no sensors, no outputs. */
    cf = fopen(obc_conf, "w");
    fprintf(cf, "ipc_path = %s\nlog_path = %s\nretry_ms = 200\n", sock_path, obc_log);
    fclose(cf);
    obc_pid = spawn(obc_bin, obc_conf);

    const char *why = "";
    CHECK_EQ_INT(radio_open_udp(&R, "127.0.0.1", gnd_port, "127.0.0.1", sat_port,
                                GAMA_RATE_NOMINAL, &why), 0);
    gs_params_t gp;
    gs_params_default(&gp);
    gs_ops_t go = { .radio_tx = g_tx, .radio_set_profile = g_prof, .event = g_event,
                    .wall_time = g_wall };
    gs_init(&G, &gp, &go, now_ms());
    ep = epoll_create1(0);
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct epoll_event e1 = { .events = EPOLLIN, .data.fd = R.fd };
    struct epoll_event e2 = { .events = EPOLLIN, .data.fd = tfd };
    struct epoll_event e3 = { .events = EPOLLIN, .data.fd = sbs_lfd };
    epoll_ctl(ep, EPOLL_CTL_ADD, R.fd, &e1);
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &e2);
    epoll_ctl(ep, EPOLL_CTL_ADD, sbs_lfd, &e3);
    next_feed = now_ms();

    TEST_GROUP("chain: in PRE_TEST nothing streams, and adsbd reads no SBS");
    {
        bool pre_test = false;
        for (int i = 0; i < 5 && !pre_test; i++) {
            pre_test = fresh_hk() && (S.last_hk.flags & GAMA_HK_F_OBC_LINKED) &&
                       S.last_hk.obc_mode == GAMA_OBC_ST_PRE_TEST;
        }
        CHECK(pre_test);
        run(2000, NULL);
        CHECK(!S.seen[0] && !S.seen[1] && !S.seen[2]);
        CHECK(sbs_fd < 0);
        CHECK(!(S.last_hk.flags & GAMA_HK_F_DUMP1090_UP));
    }

    TEST_GROUP("chain: SET_MODE MISSION_ADSB alone puts the aircraft on the air (TTC-07)");
    {
        CHECK_EQ_INT(set_mode(GAMA_OBC_EV_TC_MISSION_ADSB), GAMA_ACK_OK);
        bool all = false;
        for (int i = 0; i < 120 && !all; i++) {
            run(100, NULL);
            all = S.seen[0] && S.seen[1] && S.seen[2];
        }
        CHECK(all);
        for (int ac = 0; ac < AIRCRAFT; ac++) {
            const gama_track_t *t = &S.track[ac];
            CHECK(t->flags & GAMA_TRACK_F_POSITION);
            CHECK(t->flags & GAMA_TRACK_F_VELOCITY);
            CHECK(t->latitude < LAT0[ac] + 0.001 && t->latitude > LAT0[ac] - 0.05);
            CHECK_EQ_INT(t->altitude_ft, 20000 + 1000 * ac);
            /* HLR-ADS-08: from arrival on board to the start of transmission. */
            CHECK(t->age_ds < 50);
        }
    }

    TEST_GROUP("chain: the callsigns adsbd heard reach the ground on REQ_ROSTER");
    {
        int k = submit(GAMA_TC_REQ_ROSTER, NULL, 0);
        run(3000, &S.acks[k].done);
        CHECK(S.acks[k].done);
        CHECK_EQ_INT(S.acks[k].status, GAMA_ACK_OK);
        for (int i = 0; i < 30 && S.roster_frames == 0; i++) {
            run(100, NULL);
        }
        CHECK_EQ_INT(S.roster_frames, 1);
        for (int ac = 0; ac < AIRCRAFT; ac++) {
            CHECK(S.named[ac]);
            S.named[ac] = false;                         /* for MISSION_DOWNLINK's */
        }
    }

    TEST_GROUP("chain: housekeeping says dump1090 is delivering");
    {
        uint32_t before = S.hk;
        int k = submit(GAMA_TC_REQ_HK, NULL, 0);
        run(3000, &S.acks[k].done);
        CHECK(S.acks[k].done);
        for (int i = 0; i < 30 && S.hk == before; i++) {
            run(100, NULL);
        }
        CHECK(S.hk > before);
        CHECK(S.last_hk.flags & GAMA_HK_F_ADSB_LINKED);
        CHECK(S.last_hk.flags & GAMA_HK_F_DUMP1090_UP);
        CHECK(S.last_hk.adsb_msgs > 0 && S.last_hk.adsb_msgs != GAMA_NO_DATA_U32);
        CHECK_EQ_INT(S.last_hk.battery_mv, GAMA_NO_DATA_U16);          /* no OBC here */
    }

    TEST_GROUP("chain: the ground's SET_TIME reaches adsbd's record through ttcd");
    {
        uint8_t zeros[GAMA_IPC_TIME_LEN] = { 0 };        /* stamped at transmission */
        int k = submit(GAMA_TC_SET_TIME, zeros, GAMA_IPC_TIME_LEN);
        run(3000, &S.acks[k].done);
        CHECK(S.acks[k].done);
        run(1200, NULL);
        CHECK_EQ_INT(count_in(adsbd_log, "\"event\":\"time_anchor\""), 1);
        uint64_t ground_now = wall_ns() + (uint64_t)GROUND_AHEAD_S * 1000000000u;
        uint64_t stamp = last_stamp();
        CHECK(stamp + 3000000000u > ground_now && stamp < ground_now + 1000000000u);
    }

    TEST_GROUP("chain: adsbd killed, the radio carries on (ADR-0005)");
    {
        kill(adsbd_pid, SIGKILL);
        waitpid(adsbd_pid, NULL, 0);
        uint32_t hk_before = S.hk;
        int k = submit(GAMA_TC_PING, NULL, 0);
        run(3000, NULL);
        CHECK(S.acks[k].done);
        CHECK_EQ_INT(S.acks[k].status, GAMA_ACK_OK);
        CHECK(S.hk >= hk_before + 2);
        CHECK(!(S.last_hk.flags & GAMA_HK_F_ADSB_LINKED));
    }

    TEST_GROUP("chain: a restarted adsbd is re-anchored by ttcd, with no new SET_TIME");
    {
        adsbd_pid = spawn(adsbd_bin, adsbd_conf);
        bool anchored = false;
        for (int i = 0; i < 40 && !anchored; i++) {
            run(100, NULL);
            anchored = count_in(adsbd_log, "\"event\":\"time_anchor\"") == 2;
        }
        CHECK(anchored);
        run(1500, NULL);
        uint64_t ground_now = wall_ns() + (uint64_t)GROUND_AHEAD_S * 1000000000u;
        uint64_t stamp = last_stamp();
        CHECK(stamp + 3000000000u > ground_now && stamp < ground_now + 1000000000u);
        CHECK(S.last_hk.flags & GAMA_HK_F_ADSB_LINKED);
    }

    TEST_GROUP("chain: MISSION_DOWNLINK sends the roster and statistics, then the OBC is back in PRE_TEST");
    {
        int rosters = S.roster_frames, stats = S.stat_frames;
        CHECK_EQ_INT(set_mode(GAMA_OBC_EV_TC_DOWNLINK), GAMA_ACK_OK);
        for (int i = 0; i < 50 && S.roster_frames == rosters; i++) {
            run(100, NULL);
        }
        CHECK(S.stat_frames > stats);
        CHECK_EQ_INT(S.roster_frames, rosters + 1);
        for (int ac = 0; ac < AIRCRAFT; ac++) {
            CHECK(S.named[ac]);
        }
        CHECK_EQ_INT(count_in(ttcd_log, "\"event\":\"downlink_done\",\"task_done_sent\":true"), 1);
        bool back = false;
        for (int i = 0; i < 5 && !back; i++) {
            back = fresh_hk() && S.last_hk.obc_mode == GAMA_OBC_ST_PRE_TEST;
        }
        CHECK(back);
        CHECK_EQ_INT(count_in(obc_log, "\"to\":\"PRE_TEST\",\"event\":\"TASK_DONE\",\"by\":\"ttcd\""), 1);
        CHECK(count_in(adsbd_log, "\"event\":\"decoder\",\"on\":false") >= 1);
        CHECK(!(S.last_hk.flags & GAMA_HK_F_DUMP1090_UP));
        CHECK_EQ_INT(S.last_hk.link_state, GAMA_LINK_IDLE);
    }

    TEST_GROUP("chain: the three processes stop cleanly");
    {
        CHECK_EQ_INT(stop(obc_pid), 0);
        CHECK_EQ_INT(stop(adsbd_pid), 0);
        CHECK_EQ_INT(stop(ttcd_pid), 0);
    }

    R.close(&R);
    if (sbs_fd >= 0) { close(sbs_fd); }
    close(sbs_lfd);
    unlink(ttcd_conf);
    unlink(ttcd_log);
    unlink(adsbd_conf);
    unlink(adsbd_log);
    unlink(rec_path);
    unlink(obc_log);
    unlink(obc_conf);
    rmdir(dir);
    TEST_SUMMARY("test_adsb_chain");
}
