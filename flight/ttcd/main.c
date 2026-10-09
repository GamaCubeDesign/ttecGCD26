/*
 * main.c — the ttcd shell: file descriptors, the epoll loop and the clock.
 *
 * Every protocol decision lives in the core (core.h). This file only
 * translates between the operating system and it (ADR-0005): it owns the IPC
 * socket, the radio, one timer and one signal descriptor, and it waits in
 * exactly one place, epoll_wait. Nothing here sleeps.
 *
 *   ttcd -c /etc/gama/ttcd.conf [-o key=value]... [--check-config]
 */

#define _GNU_SOURCE
#include "config.h"
#include "core.h"
#include "ipc.h"
#include "radio.h"
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_tc.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#ifndef TTEC_VERSION
#define TTEC_VERSION "unknown"
#endif

/* Poll the radio at least this often even without an interrupt: an edge on
 * DIO0 can be missed, and a missed one would otherwise leave us deaf. */
#define RADIO_POLL_MS 250u
#define MAX_PEERS     6

typedef struct {
    int     fd;
    uint8_t role;        /* GAMA_IPC_ROLE_NONE until its HELLO */
} peer_t;

static struct {
    ttcd_config_t cfg;
    ttcd_core_t   core;
    radio_t       radio;
    int           ep, listen_fd, timer_fd, signal_fd;
    peer_t        peers[MAX_PEERS];
    FILE         *log;
    uint64_t      log_bytes;
    bool          log_full;
    uint64_t      last_radio_poll;
    bool          exiting;
    int           exit_code;
} T;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- log: one JSON object per line, flushed, like obc/docs/log_schema.md,
 * but stamped with the absolute monotonic clock so it lines up with adsbd's
 * snapshot epochs and with any other process on this board. ---- */

static void write_log(uint64_t now, const char *event, const char *fields)
{
    if (T.log == NULL || T.log_full) {
        return;
    }
    int n = fprintf(T.log, "{\"mono_ms\":%" PRIu64 ",\"event\":\"%s\"%s%s}\n",
                    now, event, fields[0] != '\0' ? "," : "", fields);
    fflush(T.log);
    if (n > 0) {
        T.log_bytes += (uint64_t)n;
    }
    if (T.log_bytes > T.cfg.log_max_bytes) {
        /* A full filesystem would take the radio down with it; stop logging
         * instead, and say so in the last line. */
        fprintf(T.log, "{\"mono_ms\":%" PRIu64 ",\"event\":\"log_full\"}\n", now);
        fflush(T.log);
        T.log_full = true;
    }
}

/* ---- core operations ---- */

static int op_radio_tx(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return T.radio.transmit(&T.radio, frame, len);   /* same 0 / 1 / -1 */
}

static int op_set_profile(void *ctx, uint8_t p) { (void)ctx; return T.radio.set_profile(&T.radio, p); }
static int op_set_power(void *ctx, int8_t dbm)  { (void)ctx; return T.radio.set_power(&T.radio, dbm); }

static int op_ipc_send(void *ctx, uint8_t role, const uint8_t *frame, size_t len)
{
    (void)ctx;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (T.peers[i].fd >= 0 && T.peers[i].role == role) {
            return ipc_send(T.peers[i].fd, frame, len) == IPC_OK ? 0 : -1;
        }
    }
    return -1;
}

static void op_log(void *ctx, uint64_t now, const char *event, const char *fields)
{
    (void)ctx;
    write_log(now, event, fields);
}

static int op_soc_temp(void *ctx, int16_t *ccel)
{
    (void)ctx;
    if (T.cfg.soc_temp_path[0] == '\0') {
        return -1;
    }
    FILE *f = fopen(T.cfg.soc_temp_path, "r");
    if (f == NULL) {
        return -1;
    }
    long mdeg;
    int ok = fscanf(f, "%ld", &mdeg) == 1;
    fclose(f);
    if (!ok) {
        return -1;
    }
    *ccel = (int16_t)(mdeg / 10);
    return 0;
}

static void op_exit(void *ctx, int code)
{
    (void)ctx;
    T.exiting = true;
    T.exit_code = code;
}

/* ---- epoll plumbing ---- */

static int watch(int fd)
{
    struct epoll_event e = { .events = EPOLLIN, .data.fd = fd };
    return epoll_ctl(T.ep, EPOLL_CTL_ADD, fd, &e);
}

static void arm_timer(uint64_t now)
{
    uint64_t d = ttcd_next_deadline(&T.core);
    uint64_t poll_at = T.last_radio_poll + RADIO_POLL_MS;
    if (poll_at < d) {
        d = poll_at;
    }
    if (d <= now) {
        d = now + 1u;          /* a zero it_value would disarm the timer */
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(d / 1000u);
    its.it_value.tv_nsec = (long)(d % 1000u) * 1000000L;
    timerfd_settime(T.timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void drain_radio(void)
{
    T.last_radio_poll = now_ms();
    radio_event_t ev;
    for (int i = 0; i < 16; i++) {
        int r = T.radio.next_event(&T.radio, &ev);
        if (r < 0) {
            write_log(now_ms(), "radio_error", "\"what\":\"next_event\"");
            return;
        }
        if (r == 0) {
            return;
        }
        uint64_t now = now_ms();
        switch (ev.kind) {
        case RADIO_EV_RX:
            ttcd_on_radio_rx(&T.core, now, ev.buf, ev.len, ev.rssi, ev.snr);
            break;
        case RADIO_EV_RX_ERROR:
            ttcd_on_radio_rx_error(&T.core, now);
            break;
        case RADIO_EV_TX_DONE:
            ttcd_on_radio_tx_done(&T.core, now);
            break;
        case RADIO_EV_NONE:
        default:
            break;
        }
    }
}

static void drop_peer(peer_t *p, const char *why)
{
    uint64_t now = now_ms();
    char fields[96];
    snprintf(fields, sizeof(fields), "\"role\":\"%s\",\"why\":\"%s\"",
             gama_ipc_role_name(p->role), why);
    write_log(now, "ipc_drop_peer", fields);
    if (p->role != GAMA_IPC_ROLE_NONE) {
        ttcd_on_ipc_peer(&T.core, now, p->role, false);
    }
    epoll_ctl(T.ep, EPOLL_CTL_DEL, p->fd, NULL);
    close(p->fd);
    p->fd = -1;
    p->role = GAMA_IPC_ROLE_NONE;
}

static bool role_taken(uint8_t role)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (T.peers[i].fd >= 0 && T.peers[i].role == role) {
            return true;
        }
    }
    return false;
}

static void accept_peers(void)
{
    for (;;) {
        int fd = ipc_server_accept(T.listen_fd);
        if (fd < 0) {
            return;
        }
        int slot = -1;
        for (int i = 0; i < MAX_PEERS && slot < 0; i++) {
            if (T.peers[i].fd < 0) {
                slot = i;
            }
        }
        if (slot < 0 || watch(fd) != 0) {
            write_log(now_ms(), "ipc_reject", "\"why\":\"no free slot\"");
            close(fd);
            continue;
        }
        T.peers[slot].fd = fd;
        T.peers[slot].role = GAMA_IPC_ROLE_NONE;
    }
}

/* The first frame on a connection must be a HELLO with our IPC version. */
static bool accept_hello(peer_t *p, const uint8_t *buf, size_t len)
{
    gama_frame_t f;
    gama_ipc_hello_t h;
    if (gama_frame_decode(buf, len, &f) < 0 || f.type != GAMA_FRAME_IPC_HELLO ||
        gama_ipc_hello_decode(f.payload, f.len, &h) < 0) {
        drop_peer(p, "first frame was not a HELLO");
        return false;
    }
    if (h.version != GAMA_IPC_VERSION) {
        drop_peer(p, "IPC version mismatch");
        return false;
    }
    if (h.role != GAMA_IPC_ROLE_ADSBD && h.role != GAMA_IPC_ROLE_OBC &&
        h.role != GAMA_IPC_ROLE_TOOL) {
        drop_peer(p, "unknown role");
        return false;
    }
    if (h.role != GAMA_IPC_ROLE_TOOL && role_taken(h.role)) {
        /* A second adsbd or OBC is a misconfiguration; the client retries,
         * and succeeds once the old connection's end has been seen. */
        drop_peer(p, "role already connected");
        return false;
    }
    p->role = h.role;
    ttcd_on_ipc_peer(&T.core, now_ms(), h.role, true);
    return true;
}

static void read_peer(peer_t *p)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL + 1];
    for (;;) {
        size_t len = 0;
        ipc_result_t r = ipc_recv(p->fd, buf, sizeof(buf), &len);
        if (r == IPC_EMPTY) {
            return;
        }
        if (r == IPC_TRUNCATED) {
            write_log(now_ms(), "ipc_truncated", "\"ignored\":true");
            continue;
        }
        if (r != IPC_OK) {
            drop_peer(p, r == IPC_CLOSED ? "closed" : "error");
            return;
        }
        if (p->role == GAMA_IPC_ROLE_NONE) {
            if (!accept_hello(p, buf, len)) {
                return;
            }
            continue;
        }
        ttcd_on_ipc_frame(&T.core, now_ms(), p->role, buf, len);
    }
}

/* ---- startup ---- */

static void usage(void)
{
    fprintf(stderr,
            "usage: ttcd [-c config] [-o key=value]... [--check-config]\n"
            "  -c FILE          configuration file (key = value lines)\n"
            "  -o KEY=VALUE     override one key; repeatable\n"
            "  --check-config   validate the configuration and exit\n");
}

static int configure(int argc, char **argv, bool *check_only)
{
    char err[300];
    ttcd_config_default(&T.cfg);
    *check_only = false;

    /* The file first, then the overrides, whatever their order on the line. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            if (ttcd_config_load(&T.cfg, argv[++i], err, sizeof(err)) != 0) {
                fprintf(stderr, "ttcd: %s\n", err);
                return -1;
            }
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            i++;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            char kv[400];
            snprintf(kv, sizeof(kv), "%s", argv[++i]);
            char *eq = strchr(kv, '=');
            if (eq == NULL) {
                fprintf(stderr, "ttcd: -o expects key=value, got %s\n", kv);
                return -1;
            }
            *eq = '\0';
            if (ttcd_config_set(&T.cfg, kv, eq + 1, err, sizeof(err)) != 0) {
                fprintf(stderr, "ttcd: %s\n", err);
                return -1;
            }
        } else if (strcmp(argv[i], "--check-config") == 0) {
            *check_only = true;
        } else {
            usage();
            return -1;
        }
    }
    if (ttcd_config_validate(&T.cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "ttcd: %s\n", err);
        return -1;
    }
    return 0;
}

static int open_radio(const char **why)
{
    const ttcd_config_t *c = &T.cfg;
    if (strcmp(c->radio, "udp") == 0) {
        return radio_open_udp(&T.radio, c->udp_bind, c->udp_port, c->udp_peer,
                              c->udp_peer_port, c->p.initial_profile, why);
    }
    sx1278_linux_cfg_t hw = {
        .spi_dev = c->spi_dev, .spi_hz = c->spi_hz, .gpio_chip = c->gpio_chip,
        .reset_line = c->reset_line, .dio0_line = c->dio0_line,
    };
    sx1278_lbt_t lbt = strcmp(c->lbt, "preamble") == 0 ? SX_LBT_PREAMBLE : SX_LBT_HEADER;
    return radio_open_sx1278(&T.radio, &hw, c->p.initial_profile, c->p.tx_power_dbm,
                             lbt, why);
}

int main(int argc, char **argv)
{
    memset(&T, 0, sizeof(T));
    for (int i = 0; i < MAX_PEERS; i++) {
        T.peers[i].fd = -1;
    }
    bool check_only;
    if (configure(argc, argv, &check_only) != 0) {
        return 2;
    }
    if (check_only) {
        printf("configuration valid: radio=%s ipc_path=%s log_path=%s profile=%s\n",
               T.cfg.radio, T.cfg.ipc_path, T.cfg.log_path,
               gama_rate_profile_name(T.cfg.p.initial_profile));
        return 0;
    }

    if (strcmp(T.cfg.log_path, "-") == 0) {
        T.log = stdout;
    } else if ((T.log = fopen(T.cfg.log_path, "a")) == NULL) {
        fprintf(stderr, "ttcd: cannot open log %s: %s\n", T.cfg.log_path, strerror(errno));
        return 1;
    }

    /* Wall-clock time appears once, here, only to anchor the log to a date:
     * the Pi has no RTC, so it may be wrong until the ground sends SET_TIME. */
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    char fields[256];
    snprintf(fields, sizeof(fields),
             "\"version\":\"%s\",\"radio\":\"%s\",\"ipc_path\":\"%s\","
             "\"wall_s\":%lld,\"wall_note\":\"unsynchronised until SET_TIME\"",
             TTEC_VERSION, T.cfg.radio, T.cfg.ipc_path, (long long)wall.tv_sec);
    write_log(now_ms(), "start", fields);

    const char *why = "unknown";
    if (open_radio(&why) != 0) {
        snprintf(fields, sizeof(fields), "\"why\":\"%s\"", why);
        write_log(now_ms(), "radio_open_failed", fields);
        fprintf(stderr, "ttcd: radio: %s\n", why);
        return 1;
    }

    T.listen_fd = ipc_server_open(T.cfg.ipc_path, 8);
    if (T.listen_fd < 0) {
        snprintf(fields, sizeof(fields), "\"why\":\"%s\"", strerror(errno));
        write_log(now_ms(), "ipc_open_failed", fields);
        fprintf(stderr, "ttcd: ipc %s: %s\n", T.cfg.ipc_path, strerror(errno));
        T.radio.close(&T.radio);
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);
    T.signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    T.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    T.ep = epoll_create1(EPOLL_CLOEXEC);
    if (T.signal_fd < 0 || T.timer_fd < 0 || T.ep < 0 ||
        watch(T.signal_fd) != 0 || watch(T.timer_fd) != 0 ||
        watch(T.listen_fd) != 0 || watch(T.radio.fd) != 0) {
        fprintf(stderr, "ttcd: event loop setup: %s\n", strerror(errno));
        return 1;
    }

    ttcd_ops_t ops = {
        .ctx = NULL, .radio_tx = op_radio_tx, .radio_set_profile = op_set_profile,
        .radio_set_power = op_set_power, .ipc_send = op_ipc_send, .log = op_log,
        .soc_temp = op_soc_temp, .exit_request = op_exit,
    };
    T.last_radio_poll = now_ms();
    ttcd_init(&T.core, &T.cfg.p, &ops, now_ms());

    while (!T.exiting) {
        arm_timer(now_ms());
        struct epoll_event evs[16];
        int n = epoll_wait(T.ep, evs, 16, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            write_log(now_ms(), "epoll_error", "\"fatal\":true");
            T.exit_code = 1;
            break;
        }
        for (int i = 0; i < n && !T.exiting; i++) {
            int fd = evs[i].data.fd;
            if (fd == T.signal_fd) {
                struct signalfd_siginfo si;
                if (read(T.signal_fd, &si, sizeof(si)) > 0) {
                    snprintf(fields, sizeof(fields), "\"signal\":%u", si.ssi_signo);
                    write_log(now_ms(), "stop", fields);
                    T.exiting = true;
                    T.exit_code = 0;
                }
            } else if (fd == T.listen_fd) {
                accept_peers();
            } else if (fd == T.radio.fd) {
                drain_radio();
            } else if (fd == T.timer_fd) {
                uint64_t expirations;
                if (read(T.timer_fd, &expirations, sizeof(expirations)) < 0) {
                    /* spurious wake-up; the deadline check below still runs */
                }
                if (now_ms() >= T.last_radio_poll + RADIO_POLL_MS) {
                    drain_radio();
                }
                ttcd_on_tick(&T.core, now_ms());
            } else {
                for (int k = 0; k < MAX_PEERS; k++) {
                    if (T.peers[k].fd == fd) {
                        read_peer(&T.peers[k]);
                        break;
                    }
                }
            }
        }
    }

    for (int k = 0; k < MAX_PEERS; k++) {
        if (T.peers[k].fd >= 0) {
            close(T.peers[k].fd);
        }
    }
    close(T.listen_fd);
    unlink(T.cfg.ipc_path);
    T.radio.close(&T.radio);
    if (T.log != NULL && T.log != stdout) {
        fclose(T.log);
    }
    return T.exit_code;
}
