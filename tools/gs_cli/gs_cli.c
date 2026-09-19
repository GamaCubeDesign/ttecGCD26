/*
 * gs_cli — ground station for the bench.
 *
 * Runs the real ground side of the link (common/gama_gs_link.c) on a radio:
 * a second RA-02 on a Raspberry Pi, or the UDP radio on a development
 * machine. Reads one command per line from standard input and waits for each
 * to finish before reading the next, so a file of commands is a repeatable
 * bench procedure. Writes one JSON object per line to standard output: every
 * frame received — with its raw bytes in hex, RSSI and SNR, so any analysis
 * can be redone from the log (HLR-SW-02) — every acknowledgement, loss, rate
 * change and contact change, and a summary at the end.
 *
 *   gs_cli --radio sx1278 --spi /dev/spidev0.1 --reset 16 --dio0 26 --power 2
 *   gs_cli --radio udp --port 47002 --peer-port 47001
 *
 * Commands:
 *   ping [N]              N sequential PINGs (default 1), latency of each
 *   per N                 N PINGs, then uplink and downlink loss from TM_STAT
 *   hk | stat             request housekeeping / statistics
 *   rate safe|nominal|fast
 *   power DBM             satellite transmit power, 2..20
 *   mode EVENT            OBC event: basic_inter aocs mission_adsb downlink
 *                         survival task_done adsb_timeout, or its number
 *   stream SECONDS        start streaming track snapshots at this period
 *   stop                  stop streaming
 *   time                  send this machine's wall clock (SET_TIME)
 *   shutdown confirm      stop ttcd (it will not restart by itself)
 *   wait SECONDS          just listen
 *   deafen SECONDS        ignore everything received for this long, then
 *                         carry on — provokes a lost ACK on purpose:
 *                         `deafen 3` then `rate fast` exercises the revert
 *   quit
 */

#define _GNU_SOURCE
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_gs_link.h"
#include "gama_ipc.h"
#include "gama_lora.h"
#include "gama_tc.h"
#include "gama_tm.h"
#include "radio.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static radio_t   R;
static gs_link_t G;
static int       ep, tfd;
static bool      stdin_open = true;
static uint64_t  deaf_until;         /* frames received before this are ignored */
static uint32_t  deaf_dropped;

/* The activity the current command is waiting on. */
static struct {
    enum { IDLE, AWAIT_ACK, PINGING, WAITING } kind;
    uint32_t id;               /* AWAIT_ACK: the telecommand            */
    int      remaining;        /* PINGING: PINGs still to send          */
    int      total, acked, failed;
    uint64_t lat_sum, lat_max, lat_min;
    uint32_t attempts;
    bool     per;              /* PINGING as part of a PER measurement  */
    uint64_t until;            /* WAITING                               */
} A;

/* PER bookkeeping: the satellite's view before and after. */
static struct {
    int      phase;            /* 0 none, 1 await first STAT, 2 pinging, 3 await last STAT */
    uint32_t sat_rx0, gs_sent0, dl_frames0, dl_lost0;
    bool     stat_seen;
    gama_stat_t stat;
} PER;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    struct timespec w;
    clock_gettime(CLOCK_REALTIME, &w);
    printf("{\"mono_ms\":%" PRIu64 ",\"wall_ms\":%" PRIu64 ",",
           mono_ms(), (uint64_t)w.tv_sec * 1000u + (uint64_t)w.tv_nsec / 1000000u);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("}\n");
    fflush(stdout);
}

/* ---- radio glue ---- */

static int g_tx(void *c, const uint8_t *f, size_t n) { (void)c; return R.transmit(&R, f, n); }
static int g_prof(void *c, uint8_t p) { (void)c; return R.set_profile(&R, p); }

static int g_wall(void *c, uint32_t *s, uint32_t *ns)
{
    (void)c;
    struct timespec w;
    clock_gettime(CLOCK_REALTIME, &w);
    *s = (uint32_t)w.tv_sec;
    *ns = (uint32_t)w.tv_nsec;
    return 0;
}

static void print_frame(const gs_event_t *e)
{
    char hex[2 * GAMA_FRAME_MAX_PAYLOAD + 1];
    for (uint8_t i = 0; i < e->len; i++) {
        snprintf(hex + 2 * i, 3, "%02x", e->payload[i]);
    }
    hex[2 * e->len] = '\0';

    if (e->type == GAMA_FRAME_TM_HK) {
        gama_hk_t h;
        if (gama_hk_decode(e->payload, e->len, &h) > 0) {
            out("\"ev\":\"hk\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"battery_mv\":%u,"
                "\"current_ma\":%d,\"temp_ext_ccel\":%d,\"temp_soc_ccel\":%d,"
                "\"roll_cdeg\":%d,\"pitch_cdeg\":%d,\"yaw_cdeg\":%d,\"adsb_msgs\":%" PRIu32 ","
                "\"obc_mode\":\"%s\",\"link\":\"%s\",\"uptime_s\":%" PRIu32 ",\"flags\":%u,"
                "\"payload\":\"%s\"",
                e->seq, e->rssi, e->snr, h.battery_mv, h.current_ma, h.temp_ext_ccel,
                h.temp_soc_ccel, h.roll_cdeg, h.pitch_cdeg, h.yaw_cdeg, h.adsb_msgs,
                gama_obc_state_name(h.obc_mode), gama_link_state_name(h.link_state),
                h.uptime_s, h.flags, hex);
            return;
        }
    } else if (e->type == GAMA_FRAME_TM_STAT) {
        gama_stat_t s;
        if (gama_stat_decode(e->payload, e->len, &s) > 0) {
            PER.stat = s;
            PER.stat_seen = true;
            out("\"ev\":\"stat\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"msgs_received\":%" PRIu32 ","
                "\"msgs_decoded\":%" PRIu32 ",\"aircraft\":%u,\"tm_frames_sent\":%" PRIu32 ","
                "\"tc_frames_rx\":%" PRIu32 ",\"tc_frames_bad\":%" PRIu32 ","
                "\"latency_p95_ms\":%u,\"dump1090_restarts\":%u,\"payload\":\"%s\"",
                e->seq, e->rssi, e->snr, s.msgs_received, s.msgs_decoded, s.aircraft_tracked,
                s.tm_frames_sent, s.tc_frames_rx, s.tc_frames_bad, s.latency_p95_ms,
                s.dump1090_restarts, hex);
            return;
        }
    } else if (e->type == GAMA_FRAME_TM_TRACKS && e->len % GAMA_TRACK_WIRE_LEN == 0) {
        /* The update time of each record in this machine's clock: arrival,
         * minus the frame's time on air, minus the record's age (ADR-0007). */
        uint32_t toa_ms = (gama_lora_toa_us(gama_lora_profile(G.profile),
                                            (uint8_t)(GAMA_FRAME_OVERHEAD + e->len)) + 500u) / 1000u;
        char recs[1600];
        size_t pos = 0;
        for (size_t r = 0; r < e->len / GAMA_TRACK_WIRE_LEN && pos < sizeof(recs) - 160; r++) {
            gama_track_t t;
            gama_track_decode(e->payload + r * GAMA_TRACK_WIRE_LEN, GAMA_TRACK_WIRE_LEN, &t);
            pos += (size_t)snprintf(recs + pos, sizeof(recs) - pos,
                                    "%s{\"icao\":\"%06" PRIX32 "\",\"lat\":%.5f,\"lon\":%.5f,"
                                    "\"alt_ft\":%" PRId32 ",\"gs_kt\":%.1f,\"trk\":%.2f,"
                                    "\"vr_fpm\":%" PRId32 ",\"age_ds\":%u,\"flags\":%u}",
                                    r ? "," : "", t.icao, t.latitude, t.longitude, t.altitude_ft,
                                    t.ground_speed_kt, t.track_deg, t.vertical_rate_fpm,
                                    t.age_ds, t.flags);
        }
        out("\"ev\":\"tracks\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"toa_ms\":%u,\"n\":%zu,"
            "\"records\":[%s],\"payload\":\"%s\"",
            e->seq, e->rssi, e->snr, toa_ms, (size_t)(e->len / GAMA_TRACK_WIRE_LEN), recs, hex);
        return;
    }
    out("\"ev\":\"frame\",\"type\":\"%s\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"payload\":\"%s\"",
        gama_frame_type_name(e->type), e->seq, e->rssi, e->snr, hex);
}

static void next_ping(void);
static void per_finish(void);

static void g_event(void *c, uint64_t now, const gs_event_t *e)
{
    (void)c; (void)now;
    switch (e->kind) {
    case GS_EV_ACK:
        out("\"ev\":\"ack\",\"id\":%" PRIu32 ",\"cmd\":\"%s\",\"status\":\"%s\","
            "\"latency_ms\":%" PRIu32 ",\"attempts\":%u",
            e->id, gama_tc_name(e->cmd), gama_ack_status_name(e->status),
            e->latency_ms, e->attempts);
        if (e->id != 0 && A.kind == AWAIT_ACK && e->id == A.id) {
            A.kind = IDLE;
        } else if (e->id != 0 && A.kind == PINGING && e->id == A.id) {
            A.acked++;
            A.attempts += e->attempts;
            A.lat_sum += e->latency_ms;
            if (e->latency_ms > A.lat_max) { A.lat_max = e->latency_ms; }
            if (e->latency_ms < A.lat_min) { A.lat_min = e->latency_ms; }
            next_ping();
        }
        break;
    case GS_EV_TC_FAILED:
        out("\"ev\":\"tc_failed\",\"id\":%" PRIu32 ",\"cmd\":\"%s\",\"attempts\":%u",
            e->id, gama_tc_name(e->cmd), e->attempts);
        if (A.kind == AWAIT_ACK && e->id == A.id) {
            A.kind = IDLE;
        } else if (A.kind == PINGING && e->id == A.id) {
            A.failed++;
            next_ping();
        }
        break;
    case GS_EV_FRAME:
        print_frame(e);
        break;
    case GS_EV_LOSS:
        out("\"ev\":\"loss\",\"lost\":%" PRIu32, e->lost);
        break;
    case GS_EV_RATE:
        out("\"ev\":\"rate\",\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\"",
            gama_rate_profile_name(e->from), gama_rate_profile_name(e->to), e->reason);
        break;
    case GS_EV_CONTACT:
        out("\"ev\":\"contact\",\"up\":%s", e->up ? "true" : "false");
        break;
    default:
        break;
    }
}

/* ---- commands ---- */

static bool submit(uint8_t cmd, const uint8_t *args, uint8_t n)
{
    uint32_t id = gs_submit(&G, mono_ms(), cmd, args, n);
    if (id == 0) {
        out("\"ev\":\"error\",\"what\":\"could not queue %s\"", gama_tc_name(cmd));
        return false;
    }
    A.kind = AWAIT_ACK;
    A.id = id;
    return true;
}

static void next_ping(void)
{
    if (A.remaining == 0) {
        uint64_t mean = A.acked ? A.lat_sum / (uint64_t)A.acked : 0;
        out("\"ev\":\"ping_summary\",\"sent\":%d,\"acked\":%d,\"failed\":%d,"
            "\"attempts\":%" PRIu32 ",\"latency_min_ms\":%" PRIu64 ",\"latency_mean_ms\":%" PRIu64
            ",\"latency_max_ms\":%" PRIu64,
            A.total, A.acked, A.failed, A.attempts, A.acked ? A.lat_min : 0, mean, A.lat_max);
        A.kind = IDLE;
        if (A.per) {
            PER.phase = 3;
            PER.stat_seen = false;
            submit(GAMA_TC_REQ_STAT, NULL, 0);
            A.kind = WAITING;                 /* until the STAT arrives */
            A.until = mono_ms() + 30000u;
        }
        return;
    }
    A.remaining--;
    A.id = gs_submit(&G, mono_ms(), GAMA_TC_PING, NULL, 0);
}

static void start_pings(int n, bool per)
{
    memset(&A, 0, sizeof(A));
    A.kind = PINGING;
    A.remaining = n;
    A.total = n;
    A.lat_min = UINT64_MAX;
    A.per = per;
    next_ping();
}

static void per_start(int n)
{
    memset(&PER, 0, sizeof(PER));
    PER.phase = 1;
    PER.gs_sent0 = G.n.tc_sent;
    PER.dl_frames0 = G.n.dl_frames;
    PER.dl_lost0 = G.n.dl_lost;
    submit(GAMA_TC_REQ_STAT, NULL, 0);
    A.kind = WAITING;
    A.until = mono_ms() + 30000u;
    A.total = n;                              /* remembered for phase 2 */
}

/* Called while WAITING in a PER measurement, whenever a STAT has arrived. */
static void per_step(void)
{
    if (PER.phase == 1 && PER.stat_seen) {
        PER.sat_rx0 = PER.stat.tc_frames_rx;
        PER.gs_sent0 = G.n.tc_sent;
        PER.dl_frames0 = G.n.dl_frames;
        PER.dl_lost0 = G.n.dl_lost;
        PER.phase = 2;
        start_pings(A.total, true);
    } else if (PER.phase == 3 && PER.stat_seen) {
        per_finish();
    }
}

static void per_finish(void)
{
    /* Uplink: telecommand frames the satellite received, over those we sent
     * (the final REQ_STAT is among the sent, and counted by the satellite
     * before it answers, so both sides include it). Downlink: frames missing
     * from the satellite's sequence, over all it sent us. */
    uint32_t sent = G.n.tc_sent - PER.gs_sent0;
    uint32_t got  = PER.stat.tc_frames_rx - PER.sat_rx0;
    uint32_t dl_n = (G.n.dl_frames - PER.dl_frames0) + (G.n.dl_lost - PER.dl_lost0);
    uint32_t dl_l = G.n.dl_lost - PER.dl_lost0;
    out("\"ev\":\"per\",\"profile\":\"%s\",\"uplink_sent\":%" PRIu32 ",\"uplink_received\":%" PRIu32
        ",\"uplink_per\":%.4f,\"downlink_frames\":%" PRIu32 ",\"downlink_lost\":%" PRIu32
        ",\"downlink_per\":%.4f",
        gama_rate_profile_name(G.profile), sent, got,
        sent ? 1.0 - (double)got / sent : 0.0, dl_n, dl_l, dl_n ? (double)dl_l / dl_n : 0.0);
    PER.phase = 0;
    A.kind = IDLE;
}

static int parse_event(const char *s)
{
    static const struct { const char *name; int ev; } names[] = {
        { "basic_inter", GAMA_OBC_EV_TC_BASIC_INTER }, { "aocs", GAMA_OBC_EV_TC_AOCS },
        { "mission_adsb", GAMA_OBC_EV_TC_MISSION_ADSB }, { "downlink", GAMA_OBC_EV_TC_DOWNLINK },
        { "survival", GAMA_OBC_EV_TC_SURVIVAL }, { "task_done", GAMA_OBC_EV_TASK_DONE },
        { "adsb_timeout", GAMA_OBC_EV_ADSB_TIMEOUT },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(s, names[i].name) == 0) { return names[i].ev; }
    }
    char *end;
    long v = strtol(s, &end, 10);
    return (*s != '\0' && *end == '\0' && v >= 1 && v <= 7) ? (int)v : -1;
}

static void run_command(char *line)
{
    char *cmd = strtok(line, " \t\r\n");
    char *arg = strtok(NULL, " \t\r\n");
    if (cmd == NULL || cmd[0] == '#') {
        return;
    }
    out("\"ev\":\"command\",\"text\":\"%s%s%s\"", cmd, arg ? " " : "", arg ? arg : "");

    if (strcmp(cmd, "ping") == 0) {
        int n = arg ? atoi(arg) : 1;
        start_pings(n > 0 ? n : 1, false);
    } else if (strcmp(cmd, "per") == 0 && arg != NULL && atoi(arg) > 0) {
        per_start(atoi(arg));
    } else if (strcmp(cmd, "hk") == 0) {
        submit(GAMA_TC_REQ_HK, NULL, 0);
    } else if (strcmp(cmd, "stat") == 0) {
        submit(GAMA_TC_REQ_STAT, NULL, 0);
    } else if (strcmp(cmd, "rate") == 0 && arg != NULL) {
        uint8_t p = strcmp(arg, "safe") == 0 ? GAMA_RATE_SAFE
                  : strcmp(arg, "nominal") == 0 ? GAMA_RATE_NOMINAL
                  : strcmp(arg, "fast") == 0 ? GAMA_RATE_FAST : 0xFF;
        if (p == 0xFF) { out("\"ev\":\"error\",\"what\":\"rate safe|nominal|fast\""); return; }
        submit(GAMA_TC_SET_RATE, &p, 1);
    } else if (strcmp(cmd, "power") == 0 && arg != NULL) {
        int8_t dbm = (int8_t)atoi(arg);
        submit(GAMA_TC_SET_TX_POWER, (const uint8_t *)&dbm, 1);
    } else if (strcmp(cmd, "mode") == 0 && arg != NULL) {
        int ev = parse_event(arg);
        if (ev < 0) { out("\"ev\":\"error\",\"what\":\"unknown OBC event\""); return; }
        uint8_t e = (uint8_t)ev;
        submit(GAMA_TC_SET_MODE, &e, 1);
    } else if (strcmp(cmd, "stream") == 0 && arg != NULL) {
        uint8_t a[2];
        gama_put_u16(a, (uint16_t)(atof(arg) * 10.0));
        submit(GAMA_TC_STREAM_START, a, 2);
    } else if (strcmp(cmd, "stop") == 0) {
        submit(GAMA_TC_STREAM_STOP, NULL, 0);
    } else if (strcmp(cmd, "time") == 0) {
        uint8_t a[GAMA_IPC_TIME_LEN] = { 0 };   /* stamped at transmission */
        submit(GAMA_TC_SET_TIME, a, GAMA_IPC_TIME_LEN);
    } else if (strcmp(cmd, "shutdown") == 0) {
        if (arg == NULL || strcmp(arg, "confirm") != 0) {
            out("\"ev\":\"error\",\"what\":\"type: shutdown confirm\"");
            return;
        }
        uint8_t a[2];
        gama_put_u16(a, GAMA_TC_SHUTDOWN_MAGIC);
        submit(GAMA_TC_SHUTDOWN, a, 2);
    } else if (strcmp(cmd, "deafen") == 0 && arg != NULL) {
        deaf_until = mono_ms() + (uint64_t)(atof(arg) * 1000.0);   /* returns at once */
    } else if (strcmp(cmd, "wait") == 0 && arg != NULL) {
        A.kind = WAITING;
        A.until = mono_ms() + (uint64_t)(atof(arg) * 1000.0);
    } else if (strcmp(cmd, "quit") == 0) {
        stdin_open = false;
    } else {
        out("\"ev\":\"error\",\"what\":\"unknown command\"");
    }
}

/* Reads and runs lines while no command is in progress. */
static void read_commands(void)
{
    static char buf[4096];
    static size_t have;
    while (A.kind == IDLE && stdin_open) {
        char *nl = memchr(buf, '\n', have);
        if (nl != NULL) {
            char line[512];
            size_t len = (size_t)(nl - buf);
            if (len >= sizeof(line)) {
                len = sizeof(line) - 1u;     /* an absurdly long line is cut */
            }
            memcpy(line, buf, len);
            line[len] = '\0';
            size_t used = (size_t)(nl - buf) + 1u;
            memmove(buf, buf + used, have - used);
            have -= used;
            run_command(line);
            continue;
        }
        ssize_t n = read(STDIN_FILENO, buf + have, sizeof(buf) - 1 - have);
        if (n > 0) {
            have += (size_t)n;
        } else if (n == 0) {
            stdin_open = false;
            if (have > 0) {           /* a last line without a newline */
                buf[have] = '\0';
                have = 0;
                run_command(buf);
            }
        } else {
            return;                   /* EAGAIN: wait for epoll */
        }
    }
}

static void usage(void)
{
    fprintf(stderr,
            "usage: gs_cli --radio sx1278|udp [options] < commands\n"
            "  sx1278: --spi DEV (/dev/spidev0.1) --chip DEV (/dev/gpiochip0)\n"
            "          --reset N (16) --dio0 N (26) --power DBM (2) --lbt header|preamble\n"
            "  udp:    --bind IP (127.0.0.1) --port N (47002) --peer IP --peer-port N (47001)\n"
            "  both:   --profile safe|nominal|fast (nominal)\n");
}

int main(int argc, char **argv)
{
    const char *radio = NULL, *spi = "/dev/spidev0.1", *chip = "/dev/gpiochip0",
               *bind_ip = "127.0.0.1", *peer = "127.0.0.1", *lbt = "header";
    uint32_t reset = 16, dio0 = 26;
    int power = 2, port = 47002, peer_port = 47001;
    uint8_t profile = GAMA_RATE_NOMINAL;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if      (strcmp(k, "--radio") == 0)     { radio = v; }
        else if (strcmp(k, "--spi") == 0)       { spi = v; }
        else if (strcmp(k, "--chip") == 0)      { chip = v; }
        else if (strcmp(k, "--reset") == 0)     { reset = (uint32_t)atoi(v); }
        else if (strcmp(k, "--dio0") == 0)      { dio0 = (uint32_t)atoi(v); }
        else if (strcmp(k, "--power") == 0)     { power = atoi(v); }
        else if (strcmp(k, "--lbt") == 0)       { lbt = v; }
        else if (strcmp(k, "--bind") == 0)      { bind_ip = v; }
        else if (strcmp(k, "--port") == 0)      { port = atoi(v); }
        else if (strcmp(k, "--peer") == 0)      { peer = v; }
        else if (strcmp(k, "--peer-port") == 0) { peer_port = atoi(v); }
        else if (strcmp(k, "--profile") == 0) {
            profile = strcmp(v, "safe") == 0 ? GAMA_RATE_SAFE
                    : strcmp(v, "fast") == 0 ? GAMA_RATE_FAST : GAMA_RATE_NOMINAL;
        } else { usage(); return 2; }
    }
    if (radio == NULL) { usage(); return 2; }

    const char *why = "";
    int r;
    if (strcmp(radio, "udp") == 0) {
        r = radio_open_udp(&R, bind_ip, (uint16_t)port, peer, (uint16_t)peer_port, profile, &why);
    } else {
        sx1278_linux_cfg_t hw = { .spi_dev = spi, .spi_hz = 1000000u, .gpio_chip = chip,
                                  .reset_line = reset, .dio0_line = dio0 };
        r = radio_open_sx1278(&R, &hw, profile, (int8_t)power,
                              strcmp(lbt, "preamble") == 0 ? SX_LBT_PREAMBLE : SX_LBT_HEADER,
                              &why);
    }
    if (r != 0) {
        fprintf(stderr, "gs_cli: radio: %s\n", why);
        return 1;
    }

    gs_params_t gp;
    gs_params_default(&gp);
    gp.initial_profile = profile;
    gs_ops_t go = { .radio_tx = g_tx, .radio_set_profile = g_prof,
                    .event = g_event, .wall_time = g_wall };
    gs_init(&G, &gp, &go, mono_ms());
    out("\"ev\":\"start\",\"radio\":\"%s\",\"profile\":\"%s\"", radio,
        gama_rate_profile_name(profile));

    ep = epoll_create1(EPOLL_CLOEXEC);
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int fl = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    struct epoll_event e1 = { .events = EPOLLIN, .data.fd = R.fd };
    struct epoll_event e2 = { .events = EPOLLIN, .data.fd = tfd };
    struct epoll_event e3 = { .events = EPOLLIN, .data.fd = STDIN_FILENO };
    epoll_ctl(ep, EPOLL_CTL_ADD, R.fd, &e1);
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &e2);
    /* A regular file cannot be added to epoll; it is always readable, so
     * read_commands() then simply runs whenever the tool is idle. */
    bool stdin_polled = epoll_ctl(ep, EPOLL_CTL_ADD, STDIN_FILENO, &e3) == 0;

    uint64_t last_poll = mono_ms();
    read_commands();
    while (stdin_open || A.kind != IDLE) {
        uint64_t now = mono_ms();
        uint64_t d = gs_next_deadline(&G);
        if (last_poll + 250u < d) { d = last_poll + 250u; }
        if (A.kind == WAITING && A.until < d) { d = A.until; }
        if (d <= now) { d = now + 1u; }
        struct itimerspec its = { .it_value = { .tv_sec = (time_t)(d / 1000u),
                                                .tv_nsec = (long)(d % 1000u) * 1000000L } };
        timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);

        struct epoll_event evs[4];
        int n = epoll_wait(ep, evs, 4, -1);
        if (n < 0 && errno != EINTR) {
            break;
        }
        for (int i = 0; i < n; i++) {
            if (evs[i].data.fd == STDIN_FILENO) {
                read_commands();
            } else if (evs[i].data.fd == tfd) {
                uint64_t x;
                if (read(tfd, &x, sizeof(x)) < 0) { /* spurious */ }
            }
        }
        /* The radio every time round: its descriptor, or the periodic poll
         * that covers a missed interrupt edge. */
        last_poll = mono_ms();
        radio_event_t ev;
        while (R.next_event(&R, &ev) == 1) {
            if (ev.kind == RADIO_EV_RX && mono_ms() < deaf_until) {
                deaf_dropped++;
                out("\"ev\":\"deaf_drop\",\"type\":\"%s\"", gama_frame_type_name(ev.buf[1]));
            } else if (ev.kind == RADIO_EV_RX) {
                gs_on_rx(&G, mono_ms(), ev.buf, ev.len, ev.rssi, ev.snr);
            } else if (ev.kind == RADIO_EV_RX_ERROR) {
                gs_on_rx_error(&G, mono_ms());
                out("\"ev\":\"rx_crc_error\"");
            } else if (ev.kind == RADIO_EV_TX_DONE) {
                gs_on_tx_done(&G, mono_ms());
            }
        }
        gs_on_tick(&G, mono_ms());

        if (A.kind == WAITING) {
            if (PER.phase != 0) {
                per_step();
            }
            if (A.kind == WAITING && mono_ms() >= A.until) {
                if (PER.phase != 0) {
                    out("\"ev\":\"error\",\"what\":\"no TM_STAT answer; PER incomplete\"");
                    PER.phase = 0;
                }
                A.kind = IDLE;
            }
        }
        if (A.kind == IDLE && (!stdin_polled || stdin_open)) {
            read_commands();
        }
    }

    out("\"ev\":\"summary\",\"dl_frames\":%" PRIu32 ",\"dl_lost\":%" PRIu32 ",\"dl_bad\":%" PRIu32
        ",\"tc_sent\":%" PRIu32 ",\"tc_acked\":%" PRIu32 ",\"tc_failed\":%" PRIu32
        ",\"lbt_defers\":%" PRIu32,
        G.n.dl_frames, G.n.dl_lost, G.n.dl_bad, G.n.tc_sent, G.n.tc_acked, G.n.tc_failed,
        G.n.lbt_defers);
    R.close(&R);
    return 0;
}
