/*
 * main.c — the adsbd shell: dump1090, the sockets, the files and the clock.
 *
 * Every decision about the data lives in the core (core.h). This file owns
 * the child process and the file descriptors, and waits in exactly one
 * place, epoll_wait. Nothing here sleeps (AGENTS.md, hard rule 2).
 *
 *   adsbd -c /etc/gama/adsbd.conf [-o key=value]... [--check-config]
 *
 * What it keeps alive (ADR-0005, ADR-0008):
 *   - dump1090-fa, started as a child with the configured command line and
 *     started again, with a doubling wait, whenever it dies. Each restart is
 *     counted and reaches the ground in TM_STAT;
 *   - the SBS connection to dump1090's port, retried every retry_ms, since
 *     dump1090 opens it only after it has found the SDR;
 *   - the connection to ttcd, which may start after us or restart under us.
 *     Neither side waits for the other: the payload keeps recording while
 *     the radio is away, and the radio keeps talking while the payload is.
 */

#define _GNU_SOURCE
#include "config.h"
#include "core.h"
#include "ipc.h"
#include "gama_frame.h"
#include "gama_ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef TTEC_VERSION
#define TTEC_VERSION "unknown"
#endif

#define NS_PER_MS     1000000u
#define SBS_BUF       4096u    /* dump1090's lines are ~150 bytes           */
#define MAX_ARGS      40
#define STOP_GRACE_MS 3000u    /* dump1090's time to exit before SIGKILL    */
#define RAN_LONG_MS   60000u   /* a run this long resets the restart backoff */

static struct {
    adsbd_config_t cfg;
    adsbd_core_t   core;
    int            ep, timer_fd, signal_fd;

    /* dump1090 */
    char     cmd_buf[400];
    char    *argv[MAX_ARGS];
    int      argc;
    pid_t    child;
    uint64_t child_started;
    uint64_t restart_at;          /* 0: nothing scheduled                   */
    uint32_t backoff_ms;

    /* the SBS connection */
    int      sbs_fd;
    bool     sbs_connecting;
    bool     sbs_complained;      /* one "cannot connect" per outage        */
    uint64_t sbs_retry_at;
    char     sbs_buf[SBS_BUF];
    size_t   sbs_have;
    bool     sbs_skipping;        /* inside a line longer than the buffer   */

    /* the connection to ttcd */
    int      ipc_fd;
    bool     ipc_complained;
    uint64_t ipc_retry_at;

    /* files */
    FILE    *rec;                 /* the onboard record, NDJSON             */
    uint64_t rec_bytes, rec_dropped;
    bool     rec_full;
    FILE    *log;
    uint64_t log_bytes;
    bool     log_full;

    bool     stopping;
    uint64_t stop_deadline;
    bool     exiting;
    int      exit_code;
} A;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ---- the event log: one JSON object per line, flushed, stamped with the
 * absolute monotonic clock like ttcd's, so the two line up. ---- */

static void write_log(uint64_t now, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void write_log(uint64_t now, const char *event, const char *fmt, ...)
{
    if (A.log == NULL || A.log_full) {
        return;
    }
    char fields[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fields, sizeof(fields), fmt, ap);
    va_end(ap);
    int n = fprintf(A.log, "{\"mono_ms\":%" PRIu64 ",\"event\":\"%s\"%s%s}\n",
                    now / NS_PER_MS, event, fields[0] != '\0' ? "," : "", fields);
    fflush(A.log);
    if (n > 0) {
        A.log_bytes += (uint64_t)n;
    }
    if (A.log_bytes > A.cfg.log_max_bytes) {
        fprintf(A.log, "{\"mono_ms\":%" PRIu64 ",\"event\":\"log_full\"}\n", now / NS_PER_MS);
        fflush(A.log);
        A.log_full = true;
    }
}

/* ---- core operations ---- */

static int op_ipc_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (A.ipc_fd < 0) {
        return -1;
    }
    /* A closed connection is noticed by the epoll loop, not here: the core
     * is mid-call, and must not be told about it re-entrantly. */
    return ipc_send(A.ipc_fd, frame, len) == IPC_OK ? 0 : -1;
}

static void op_record(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    if (A.rec == NULL || A.rec_full) {
        A.rec_dropped++;
        return;
    }
    /* Flushed per line, as the prototype did: a power cut loses at most the
     * line being written (HLR-SW-02). */
    fwrite(line, 1, len, A.rec);
    fflush(A.rec);
    A.rec_bytes += len;
    if (A.rec_bytes >= A.cfg.ndjson_max_bytes) {
        /* A full filesystem would take ttcd and the radio down with it
         * (data-budget.md §7): stop recording instead, and say so. */
        A.rec_full = true;
        write_log(now_ns(), "record_full", "\"bytes\":%" PRIu64, A.rec_bytes);
    }
}

static void op_log(void *ctx, uint64_t now_ms, const char *event, const char *fields)
{
    (void)ctx;
    write_log(now_ms * NS_PER_MS, event, "%s", fields);
}

/* ---- epoll plumbing ---- */

static int watch(int fd, uint32_t events)
{
    struct epoll_event e = { .events = events, .data.fd = fd };
    return epoll_ctl(A.ep, EPOLL_CTL_ADD, fd, &e);
}

static void rewatch(int fd, uint32_t events)
{
    struct epoll_event e = { .events = events, .data.fd = fd };
    epoll_ctl(A.ep, EPOLL_CTL_MOD, fd, &e);
}

static void arm_timer(uint64_t now)
{
    uint64_t d = adsbd_next_deadline(&A.core);
    if (A.sbs_fd < 0 && A.sbs_retry_at < d) {
        d = A.sbs_retry_at;
    }
    if (A.ipc_fd < 0 && A.ipc_retry_at < d) {
        d = A.ipc_retry_at;
    }
    if (A.restart_at != 0 && A.restart_at < d) {
        d = A.restart_at;
    }
    if (A.stopping && A.stop_deadline < d) {
        d = A.stop_deadline;
    }
    if (d <= now) {
        d = now + NS_PER_MS;      /* a zero it_value would disarm the timer */
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(d / 1000000000u);
    its.it_value.tv_nsec = (long)(d % 1000000000u);
    timerfd_settime(A.timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

/* ---- dump1090 ---- */

static void start_dump1090(uint64_t now)
{
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        write_log(now, "dump1090_fork_failed", "\"why\":\"%s\"", strerror(errno));
        A.restart_at = now + (uint64_t)A.cfg.restart_max_ms * NS_PER_MS;
        return;
    }
    if (pid == 0) {
        /* The child. It must not outlive adsbd holding the SDR, or the next
         * adsbd could not start its own. systemd kills the whole unit on a
         * stop; this covers adsbd dying on its own. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);             /* adsbd died before prctl took effect */
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        execv(A.argv[0], A.argv);
        _exit(127);
    }
    A.child = pid;
    A.child_started = now;
    A.restart_at = 0;
    write_log(now, "dump1090_start", "\"pid\":%d,\"path\":\"%s\"", (int)pid, A.argv[0]);
}

static void schedule_restart(uint64_t now)
{
    bool ran_long = now - A.child_started >= (uint64_t)RAN_LONG_MS * NS_PER_MS;
    if (A.backoff_ms == 0 || ran_long) {
        A.backoff_ms = A.cfg.restart_min_ms;
    } else {
        A.backoff_ms = A.backoff_ms * 2u > A.cfg.restart_max_ms ? A.cfg.restart_max_ms
                                                                : A.backoff_ms * 2u;
    }
    A.restart_at = now + (uint64_t)A.backoff_ms * NS_PER_MS;
}

static void reap(uint64_t now)
{
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        if (p != A.child) {
            continue;
        }
        A.child = -1;
        if (WIFSIGNALED(st)) {
            write_log(now, "dump1090_exit", "\"signal\":%d", WTERMSIG(st));
        } else {
            write_log(now, "dump1090_exit", "\"status\":%d", WEXITSTATUS(st));
        }
        if (A.stopping) {
            A.exiting = true;
        } else {
            schedule_restart(now);
            write_log(now, "dump1090_restart_in", "\"ms\":%" PRIu32, A.backoff_ms);
        }
    }
}

/* ---- the SBS connection ---- */

static void sbs_close(uint64_t now, const char *why)
{
    if (A.sbs_fd >= 0) {
        epoll_ctl(A.ep, EPOLL_CTL_DEL, A.sbs_fd, NULL);
        close(A.sbs_fd);
        if (!A.sbs_connecting) {
            write_log(now, "sbs_down", "\"why\":\"%s\"", why);
        } else if (!A.sbs_complained) {
            write_log(now, "sbs_connect_failed", "\"why\":\"%s\"", why);
            A.sbs_complained = true;
        }
    }
    A.sbs_fd = -1;
    A.sbs_connecting = false;
    A.sbs_have = 0;
    A.sbs_skipping = false;
    A.sbs_retry_at = now + (uint64_t)A.cfg.retry_ms * NS_PER_MS;
}

static void sbs_up(uint64_t now)
{
    A.sbs_connecting = false;
    A.sbs_complained = false;
    write_log(now, "sbs_up", "\"host\":\"%s\",\"port\":%u", A.cfg.sbs_host, A.cfg.sbs_port);
}

static void sbs_try_connect(uint64_t now)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(A.cfg.sbs_port);
    inet_pton(AF_INET, A.cfg.sbs_host, &a.sin_addr);     /* validated at start */

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        A.sbs_retry_at = now + (uint64_t)A.cfg.retry_ms * NS_PER_MS;
        return;
    }
    A.sbs_fd = fd;
    A.sbs_connecting = true;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
        watch(fd, EPOLLIN);
        sbs_up(now);
    } else if (errno == EINPROGRESS) {
        watch(fd, EPOLLOUT);      /* sbs_on_event() finishes the connection */
    } else {
        sbs_close(now, strerror(errno));
    }
}

static void sbs_read(void)
{
    for (;;) {
        if (A.sbs_have == SBS_BUF) {
            /* A full buffer and no newline: not an SBS line. Drop it, and
             * everything up to the next newline. */
            A.sbs_have = 0;
            A.sbs_skipping = true;
            adsbd_on_overlong_line(&A.core, now_ns());
        }
        ssize_t n = recv(A.sbs_fd, A.sbs_buf + A.sbs_have, SBS_BUF - A.sbs_have, 0);
        /* HLR-ADS-07: the arrival time, taken as the bytes come in. Every
         * line completed by this read arrived together. */
        uint64_t now = now_ns();
        if (n == 0) {
            sbs_close(now, "closed by dump1090");
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            sbs_close(now, strerror(errno));
            return;
        }
        size_t end = A.sbs_have + (size_t)n, start = 0;
        for (size_t i = A.sbs_have; i < end; i++) {
            if (A.sbs_buf[i] != '\n') {
                continue;
            }
            if (A.sbs_skipping) {
                A.sbs_skipping = false;       /* the tail of the long line */
            } else {
                adsbd_on_line(&A.core, now, A.sbs_buf + start, i - start);
            }
            start = i + 1;
        }
        if (A.sbs_skipping) {
            A.sbs_have = 0;
        } else {
            memmove(A.sbs_buf, A.sbs_buf + start, end - start);
            A.sbs_have = end - start;
        }
    }
}

static void sbs_on_event(uint32_t events)
{
    if (!A.sbs_connecting) {
        sbs_read();
        return;
    }
    uint64_t now = now_ns();
    int err = 0;
    socklen_t l = sizeof(err);
    if (!(events & EPOLLOUT) ||
        getsockopt(A.sbs_fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0 || err != 0) {
        sbs_close(now, err != 0 ? strerror(err) : "connect");
        return;
    }
    rewatch(A.sbs_fd, EPOLLIN);
    sbs_up(now);
}

/* ---- the connection to ttcd ---- */

static void ipc_close(uint64_t now, const char *why)
{
    epoll_ctl(A.ep, EPOLL_CTL_DEL, A.ipc_fd, NULL);
    close(A.ipc_fd);
    A.ipc_fd = -1;
    A.ipc_retry_at = now + (uint64_t)A.cfg.retry_ms * NS_PER_MS;
    write_log(now, "ipc_down", "\"why\":\"%s\"", why);
    adsbd_on_ipc_link(&A.core, now, false);
}

static void ipc_try_connect(uint64_t now)
{
    int fd = ipc_client_connect(A.cfg.ipc_path);
    if (fd < 0) {
        if (!A.ipc_complained) {
            write_log(now, "ipc_connect_failed", "\"path\":\"%s\",\"why\":\"%s\"",
                      A.cfg.ipc_path, strerror(errno));
            A.ipc_complained = true;
        }
        A.ipc_retry_at = now + (uint64_t)A.cfg.retry_ms * NS_PER_MS;
        return;
    }
    uint8_t p[GAMA_IPC_HELLO_LEN], f[GAMA_FRAME_MAX_TOTAL];
    gama_ipc_hello_t h = { .role = GAMA_IPC_ROLE_ADSBD, .version = GAMA_IPC_VERSION };
    gama_ipc_hello_encode(p, sizeof(p), &h);
    int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_HELLO, 0, p, GAMA_IPC_HELLO_LEN);
    if (n < 0 || ipc_send(fd, f, (size_t)n) != IPC_OK || watch(fd, EPOLLIN) != 0) {
        close(fd);
        A.ipc_retry_at = now + (uint64_t)A.cfg.retry_ms * NS_PER_MS;
        return;
    }
    A.ipc_fd = fd;
    A.ipc_complained = false;
    write_log(now, "ipc_up", "\"path\":\"%s\"", A.cfg.ipc_path);
    adsbd_on_ipc_link(&A.core, now, true);
}

static void ipc_read(void)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL + 1];
    for (;;) {
        size_t len = 0;
        ipc_result_t r = ipc_recv(A.ipc_fd, buf, sizeof(buf), &len);
        uint64_t now = now_ns();
        if (r == IPC_EMPTY) {
            return;
        }
        if (r == IPC_TRUNCATED) {
            write_log(now, "ipc_truncated", "\"ignored\":true");
            continue;
        }
        if (r != IPC_OK) {
            ipc_close(now, r == IPC_CLOSED ? "closed by ttcd" : "error");
            return;
        }
        adsbd_on_ipc_frame(&A.core, now, buf, len);
    }
}

/* ---- timers and shutdown ---- */

static void begin_stop(uint64_t now, uint32_t signo)
{
    if (A.stopping) {
        return;
    }
    A.stopping = true;
    A.restart_at = 0;
    write_log(now, "stop", "\"signal\":%" PRIu32, signo);
    if (A.child > 0) {
        kill(A.child, SIGTERM);
        A.stop_deadline = now + (uint64_t)STOP_GRACE_MS * NS_PER_MS;
    } else {
        A.exiting = true;
    }
}

static void service(uint64_t now)
{
    if (A.stopping) {
        if (A.child > 0 && now >= A.stop_deadline) {
            /* Shutdown only: the child ignored SIGTERM; this wait is for a
             * process the kernel is already reaping. */
            kill(A.child, SIGKILL);
            waitpid(A.child, NULL, 0);
            write_log(now, "dump1090_killed", "\"pid\":%d", (int)A.child);
            A.child = -1;
            A.exiting = true;
        }
    } else if (A.restart_at != 0 && now >= A.restart_at) {
        start_dump1090(now);
        adsbd_on_dump1090_restart(&A.core, now);
    }
    if (A.sbs_fd < 0 && now >= A.sbs_retry_at) {
        sbs_try_connect(now);
    }
    if (A.ipc_fd < 0 && now >= A.ipc_retry_at) {
        ipc_try_connect(now);
    }
    if (now >= adsbd_next_deadline(&A.core)) {
        adsbd_on_tick(&A.core, now);
    }
}

/* ---- startup ---- */

static void usage(void)
{
    fprintf(stderr,
            "usage: adsbd [-c config] [-o key=value]... [--check-config]\n"
            "  -c FILE          configuration file (key = value lines)\n"
            "  -o KEY=VALUE     override one key; repeatable\n"
            "  --check-config   validate the configuration and exit\n");
}

static int configure(int argc, char **argv, bool *check_only)
{
    char err[300];
    adsbd_config_default(&A.cfg);
    *check_only = false;

    /* The file first, then the overrides, whatever their order on the line. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            if (adsbd_config_load(&A.cfg, argv[++i], err, sizeof(err)) != 0) {
                fprintf(stderr, "adsbd: %s\n", err);
                return -1;
            }
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            i++;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            char kv[600];
            snprintf(kv, sizeof(kv), "%s", argv[++i]);
            char *eq = strchr(kv, '=');
            if (eq == NULL) {
                fprintf(stderr, "adsbd: -o expects key=value, got %s\n", kv);
                return -1;
            }
            *eq = '\0';
            if (adsbd_config_set(&A.cfg, kv, eq + 1, err, sizeof(err)) != 0) {
                fprintf(stderr, "adsbd: %s\n", err);
                return -1;
            }
        } else if (strcmp(argv[i], "--check-config") == 0) {
            *check_only = true;
        } else {
            usage();
            return -1;
        }
    }
    if (adsbd_config_validate(&A.cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "adsbd: %s\n", err);
        return -1;
    }
    struct in_addr probe;
    if (inet_pton(AF_INET, A.cfg.sbs_host, &probe) != 1) {
        fprintf(stderr, "adsbd: sbs_host must be an IPv4 address, got %s\n", A.cfg.sbs_host);
        return -1;
    }
    A.argc = adsbd_config_argv(&A.cfg, A.cmd_buf, sizeof(A.cmd_buf), A.argv, MAX_ARGS);
    return A.argc < 0 ? -1 : 0;
}

/* Opens a file for appending and counts what it already holds, so the size
 * cap covers the file, not just this run. */
static FILE *open_append(const char *path, uint64_t *bytes)
{
    FILE *f = fopen(path, "ae");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        long at = ftell(f);
        *bytes = at > 0 ? (uint64_t)at : 0u;
    }
    return f;
}

int main(int argc, char **argv)
{
    memset(&A, 0, sizeof(A));
    A.sbs_fd = -1;
    A.ipc_fd = -1;
    A.child = -1;
    bool check_only;
    if (configure(argc, argv, &check_only) != 0) {
        return 2;
    }
    if (check_only) {
        printf("configuration valid: sbs=%s:%u ipc_path=%s ndjson_path=%s dump1090=%s\n",
               A.cfg.sbs_host, A.cfg.sbs_port, A.cfg.ipc_path, A.cfg.ndjson_path,
               A.argc > 0 ? A.argv[0] : "(external: not started by adsbd)");
        return 0;
    }

    if (strcmp(A.cfg.log_path, "-") == 0) {
        A.log = stdout;
    } else if ((A.log = open_append(A.cfg.log_path, &A.log_bytes)) == NULL) {
        fprintf(stderr, "adsbd: cannot open log %s: %s\n", A.cfg.log_path, strerror(errno));
        return 1;
    }
    /* The record is the mission's data (HLR-SW-02): without it, adsbd does
     * not pretend to run. */
    if ((A.rec = open_append(A.cfg.ndjson_path, &A.rec_bytes)) == NULL) {
        fprintf(stderr, "adsbd: cannot open record %s: %s\n", A.cfg.ndjson_path, strerror(errno));
        return 1;
    }
    A.rec_full = A.rec_bytes >= A.cfg.ndjson_max_bytes;

    /* ADR-0009: the wall clock starts as the system's — possibly wrong on a
     * Pi with no RTC — until the ground's SET_TIME arrives through ttcd. */
    uint64_t now = now_ns();
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    int64_t offset = (int64_t)wall.tv_sec * 1000000000 + (int64_t)wall.tv_nsec - (int64_t)now;
    write_log(now, "start",
              "\"version\":\"%s\",\"sbs\":\"%s:%u\",\"ipc_path\":\"%s\",\"ndjson_path\":\"%s\","
              "\"record_bytes\":%" PRIu64 ",\"dump1090\":\"%s\",\"wall_s\":%lld,"
              "\"wall_note\":\"system clock until the ground's SET_TIME\"",
              TTEC_VERSION, A.cfg.sbs_host, A.cfg.sbs_port, A.cfg.ipc_path, A.cfg.ndjson_path,
              A.rec_bytes, A.argc > 0 ? A.argv[0] : "external", (long long)wall.tv_sec);
    if (A.rec_full) {
        write_log(now, "record_full", "\"bytes\":%" PRIu64 ",\"at_start\":true", A.rec_bytes);
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);
    A.signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    A.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    A.ep = epoll_create1(EPOLL_CLOEXEC);
    if (A.signal_fd < 0 || A.timer_fd < 0 || A.ep < 0 ||
        watch(A.signal_fd, EPOLLIN) != 0 || watch(A.timer_fd, EPOLLIN) != 0) {
        fprintf(stderr, "adsbd: event loop setup: %s\n", strerror(errno));
        return 1;
    }

    adsbd_ops_t ops = { .ctx = NULL, .ipc_send = op_ipc_send, .record = op_record,
                        .log = op_log };
    adsbd_init(&A.core, &A.cfg.p, &ops, now, offset);
    if (A.argc > 0) {
        start_dump1090(now);
    }
    A.sbs_retry_at = now;
    A.ipc_retry_at = now;
    service(now);

    while (!A.exiting) {
        arm_timer(now_ns());
        struct epoll_event evs[8];
        int n = epoll_wait(A.ep, evs, 8, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            write_log(now_ns(), "epoll_error", "\"fatal\":true");
            A.exit_code = 1;
            break;
        }
        for (int i = 0; i < n && !A.exiting; i++) {
            int fd = evs[i].data.fd;
            if (fd == A.signal_fd) {
                struct signalfd_siginfo si;
                while (read(A.signal_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    if (si.ssi_signo == SIGCHLD) {
                        reap(now_ns());
                    } else {
                        begin_stop(now_ns(), si.ssi_signo);
                    }
                }
            } else if (fd == A.timer_fd) {
                uint64_t expirations;
                if (read(A.timer_fd, &expirations, sizeof(expirations)) < 0) {
                    /* spurious wake-up; service() below still runs */
                }
            } else if (fd == A.sbs_fd) {
                sbs_on_event(evs[i].events);
            } else if (fd == A.ipc_fd) {
                ipc_read();
            }
        }
        if (!A.exiting) {
            service(now_ns());
        }
    }

    now = now_ns();
    if (A.sbs_fd >= 0) {
        close(A.sbs_fd);
    }
    if (A.ipc_fd >= 0) {
        close(A.ipc_fd);
    }
    write_log(now, "exit", "\"code\":%d,\"record_bytes\":%" PRIu64 ",\"record_dropped\":%" PRIu64,
              A.exit_code, A.rec_bytes, A.rec_dropped);
    fclose(A.rec);
    if (A.log != NULL && A.log != stdout) {
        fclose(A.log);
    }
    return A.exit_code;
}
