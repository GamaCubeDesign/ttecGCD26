/*
 * main.c — the obcd shell: the clock, the connection to ttcd, the GPIO
 * lines and the sensor threads (ADR-0016).
 *
 *   obcd -c /etc/gama/obcd.conf [-o key=value]... [--check-config]
 *
 * Every decision lives in the core (core.h). The main loop waits in exactly
 * one place, epoll_wait, and never on a device (AGENTS.md, hard rule 3).
 * The two sensors are read by threads of their own and their readings reach
 * the loop through a pipe:
 *   - the DS18B20, whose read blocks for its ~750 ms conversion (D9);
 *   - the INA219, whose I2C transfers take about a millisecond — unless the
 *     bus is held by the AOCS ESP32, which shares it on the boards as
 *     fabricated. A stuck bus stalls that thread, not the mode transitions
 *     or the thermostat.
 *
 * What it keeps alive: the connection to ttcd, retried every retry_ms, so
 * either can start first or restart (ADR-0005); systemd keeps obcd itself
 * alive (Restart=always), and its watchdog sees the loop turning (D8).
 */

#define _GNU_SOURCE
#include "config.h"
#include "core.h"
#include "ds18b20.h"
#include "hw_linux.h"
#include "ina219.h"
#include "ipc.h"
#include "gama_frame.h"
#include "gama_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef TTEC_VERSION
#define TTEC_VERSION "unknown"
#endif

/* One reading, from a sensor thread to the loop. Smaller than PIPE_BUF, so
 * every write is atomic and the reader never sees half of one. */
enum { READING_POWER = 1, READING_TEMP = 2 };
typedef struct {
    uint8_t  kind;
    uint8_t  ok;
    uint16_t battery_mv;
    int16_t  value;        /* current_ma, or the temperature in ccel */
    int16_t  why;          /* failure: errno for the INA219, ds18b20_result_t */
} reading_t;

static struct {
    obcd_config_t cfg;
    obcd_core_t   core;
    int           ep, timer_fd, signal_fd;
    int           readings[2];          /* pipe: [0] the loop, [1] the threads */

    int           ipc_fd;
    bool          ipc_complained;
    uint64_t      ipc_retry_at;

    int           burnwire_fd, heater_fd;

    FILE         *log;
    uint64_t      log_bytes;
    bool          log_full;

    /* what the loop last said about each sensor, to log changes only */
    int           power_state, temp_state;   /* -1 unknown, 0 failing, 1 ok */

    pthread_t     power_thread, temp_thread;
    bool          power_started, temp_started;
    atomic_bool   stop_threads;

    int           notify_fd;
    struct sockaddr_un notify_addr;
    socklen_t     notify_len;
    uint64_t      last_watchdog;

    bool          exiting;
    int           exit_code;
} O;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- the event log: one JSON object per line, flushed, stamped with the
 * absolute monotonic clock like ttcd's and adsbd's, so the three line up
 * (D7, ADR-0009). ---- */

static void write_log(uint64_t now, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void write_log(uint64_t now, const char *event, const char *fmt, ...)
{
    if (O.log == NULL || O.log_full) {
        return;
    }
    char fields[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fields, sizeof(fields), fmt, ap);
    va_end(ap);
    int n = fprintf(O.log, "{\"mono_ms\":%" PRIu64 ",\"event\":\"%s\"%s%s}\n",
                    now, event, fields[0] != '\0' ? "," : "", fields);
    fflush(O.log);
    if (n > 0) {
        O.log_bytes += (uint64_t)n;
    }
    if (O.log_bytes > O.cfg.log_max_bytes) {
        fprintf(O.log, "{\"mono_ms\":%" PRIu64 ",\"event\":\"log_full\"}\n", now);
        fflush(O.log);
        O.log_full = true;
    }
}

/* ---- systemd: READY and the watchdog, without libsystemd (D8) ---- */

static void notify_open(void)
{
    O.notify_fd = -1;
    const char *path = getenv("NOTIFY_SOCKET");
    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(O.notify_addr.sun_path)) {
        return;
    }
    memset(&O.notify_addr, 0, sizeof(O.notify_addr));
    O.notify_addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    memcpy(O.notify_addr.sun_path, path, n);
    if (path[0] == '@') {
        O.notify_addr.sun_path[0] = '\0';      /* an abstract socket */
    }
    O.notify_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n);
    O.notify_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

static void notify(const char *what)
{
    if (O.notify_fd >= 0) {
        sendto(O.notify_fd, what, strlen(what), MSG_NOSIGNAL,
               (struct sockaddr *)&O.notify_addr, O.notify_len);
    }
}

/* ---- core operations ---- */

static int op_ipc_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (O.ipc_fd < 0) {
        return -1;
    }
    /* A closed connection is noticed by the epoll loop, not here. */
    return ipc_send(O.ipc_fd, frame, len) == IPC_OK ? 0 : -1;
}

static void op_log(void *ctx, uint64_t now, const char *event, const char *fields)
{
    (void)ctx;
    write_log(now, event, "%s", fields);
}

static int op_burnwire(void *ctx, bool on)
{
    (void)ctx;
    return hw_gpio_set(O.burnwire_fd, on);
}

static int op_heater(void *ctx, bool on)
{
    (void)ctx;
    return hw_gpio_set(O.heater_fd, on);
}

/* ---- the sensor threads: they may block; nothing else here does ---- */

static void deliver(const reading_t *r)
{
    ssize_t n;
    do {
        n = write(O.readings[1], r, sizeof(*r));
    } while (n < 0 && errno == EINTR);
}

/* Sleeps for ms in slices, so a stop request is seen within 100 ms. */
static void thread_sleep(uint32_t ms)
{
    while (ms > 0 && !atomic_load(&O.stop_threads)) {
        uint32_t slice = ms < 100u ? ms : 100u;
        struct timespec t = { .tv_sec = 0, .tv_nsec = (long)slice * 1000000L };
        nanosleep(&t, NULL);
        ms -= slice;
    }
}

static void *power_main(void *arg)
{
    (void)arg;
    hw_i2c_t h = { .fd = -1 };
    ina219_bus_t bus;
    while (!atomic_load(&O.stop_threads)) {
        reading_t r = { .kind = READING_POWER };
        if (h.fd < 0) {
            if (hw_i2c_open(&h, O.cfg.i2c_dev, O.cfg.ina219_addr, &bus) != 0) {
                r.why = (int16_t)errno;
            } else if (ina219_init(&bus) != 0) {
                r.why = (int16_t)errno;
                hw_i2c_close(&h);
            }
        }
        if (h.fd >= 0) {
            if (ina219_read(&bus, O.cfg.ina219_shunt_mohm, &r.battery_mv, &r.value) == 0) {
                r.ok = 1;
            } else {
                r.why = (int16_t)errno;
                hw_i2c_close(&h);              /* reopen and reconfigure next time */
            }
        }
        deliver(&r);
        thread_sleep(O.cfg.power_period_ms);
    }
    hw_i2c_close(&h);
    return NULL;
}

static void *temp_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&O.stop_threads)) {
        reading_t r = { .kind = READING_TEMP };
        int16_t ccel = 0;
        ds18b20_result_t res = ds18b20_read_file(O.cfg.w1_path, &ccel);
        r.ok = res == DS18B20_OK;
        r.value = ccel;
        r.why = (int16_t)res;
        deliver(&r);
        thread_sleep(O.cfg.temp_period_ms);
    }
    return NULL;
}

/* Logs a sensor only when it starts or stops working. */
static void sensor_state(int *state, uint64_t now, const char *which, const reading_t *r)
{
    int s = r->ok ? 1 : 0;
    if (s == *state) {
        return;
    }
    *state = s;
    if (r->ok) {
        write_log(now, "sensor", "\"which\":\"%s\",\"ok\":true", which);
    } else if (r->kind == READING_POWER) {
        write_log(now, "sensor", "\"which\":\"%s\",\"ok\":false,\"why\":\"%s\"",
                  which, strerror(r->why));
    } else {
        write_log(now, "sensor", "\"which\":\"%s\",\"ok\":false,\"why\":\"%s\"",
                  which, ds18b20_result_name((ds18b20_result_t)r->why));
    }
}

static void read_readings(void)
{
    reading_t r[16];
    for (;;) {
        ssize_t n = read(O.readings[0], r, sizeof(r));
        if (n <= 0) {
            return;
        }
        uint64_t now = now_ms();
        for (size_t i = 0; i < (size_t)n / sizeof(r[0]); i++) {
            if (r[i].kind == READING_POWER) {
                sensor_state(&O.power_state, now, "ina219", &r[i]);
                obcd_on_power(&O.core, now, r[i].ok != 0, r[i].battery_mv, r[i].value);
            } else if (r[i].kind == READING_TEMP) {
                sensor_state(&O.temp_state, now, "ds18b20", &r[i]);
                obcd_on_battery_temp(&O.core, now, r[i].ok != 0, r[i].value);
            }
        }
    }
}

/* ---- the connection to ttcd ---- */

static void ipc_close(uint64_t now, const char *why)
{
    epoll_ctl(O.ep, EPOLL_CTL_DEL, O.ipc_fd, NULL);
    close(O.ipc_fd);
    O.ipc_fd = -1;
    O.ipc_retry_at = now + O.cfg.retry_ms;
    write_log(now, "ipc_down", "\"why\":\"%s\"", why);
    obcd_on_ipc_link(&O.core, now, false);
}

static void ipc_try_connect(uint64_t now)
{
    int fd = ipc_client_connect(O.cfg.ipc_path);
    if (fd < 0) {
        if (!O.ipc_complained) {
            write_log(now, "ipc_connect_failed", "\"path\":\"%s\",\"why\":\"%s\"",
                      O.cfg.ipc_path, strerror(errno));
            O.ipc_complained = true;
        }
        O.ipc_retry_at = now + O.cfg.retry_ms;
        return;
    }
    uint8_t p[GAMA_IPC_HELLO_LEN], f[GAMA_FRAME_MAX_TOTAL];
    gama_ipc_hello_t h = { .role = GAMA_IPC_ROLE_OBC, .version = GAMA_IPC_VERSION };
    gama_ipc_hello_encode(p, sizeof(p), &h);
    int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_HELLO, 0, p, GAMA_IPC_HELLO_LEN);
    struct epoll_event e = { .events = EPOLLIN, .data.fd = fd };
    if (n < 0 || ipc_send(fd, f, (size_t)n) != IPC_OK || epoll_ctl(O.ep, EPOLL_CTL_ADD, fd, &e) != 0) {
        close(fd);
        O.ipc_retry_at = now + O.cfg.retry_ms;
        return;
    }
    O.ipc_fd = fd;
    O.ipc_complained = false;
    write_log(now, "ipc_up", "\"path\":\"%s\"", O.cfg.ipc_path);
    obcd_on_ipc_link(&O.core, now, true);
}

static void ipc_read(void)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL + 1];
    for (;;) {
        size_t len = 0;
        ipc_result_t r = ipc_recv(O.ipc_fd, buf, sizeof(buf), &len);
        uint64_t now = now_ms();
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
        obcd_on_ipc_frame(&O.core, now, buf, len);
    }
}

/* ---- the loop ---- */

static void arm_timer(uint64_t now)
{
    uint64_t d = obcd_next_deadline(&O.core);
    if (O.ipc_fd < 0 && O.ipc_retry_at < d) {
        d = O.ipc_retry_at;
    }
    if (d <= now) {
        d = now + 1;              /* a zero it_value would disarm the timer */
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(d / 1000u);
    its.it_value.tv_nsec = (long)(d % 1000u) * 1000000L;
    timerfd_settime(O.timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void service(uint64_t now)
{
    if (O.ipc_fd < 0 && now >= O.ipc_retry_at) {
        ipc_try_connect(now);
    }
    if (now >= obcd_next_deadline(&O.core)) {
        obcd_on_tick(&O.core, now);
    }
    /* The core wakes at least every telemetry period, so this reaches
     * systemd at least that often while the loop turns. */
    if (now - O.last_watchdog >= 1000u) {
        notify("WATCHDOG=1");
        O.last_watchdog = now;
    }
}

/* ---- startup ---- */

static void usage(void)
{
    fprintf(stderr,
            "usage: obcd [-c config] [-o key=value]... [--check-config]\n"
            "  -c FILE          configuration file (key = value lines)\n"
            "  -o KEY=VALUE     override one key; repeatable\n"
            "  --check-config   validate the configuration and exit\n");
}

static int configure(int argc, char **argv, bool *check_only)
{
    char err[300];
    obcd_config_default(&O.cfg);
    *check_only = false;
    /* The file first, then the overrides, whatever their order on the line. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            if (obcd_config_load(&O.cfg, argv[++i], err, sizeof(err)) != 0) {
                fprintf(stderr, "obcd: %s\n", err);
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
                fprintf(stderr, "obcd: -o expects key=value, got %s\n", kv);
                return -1;
            }
            *eq = '\0';
            if (obcd_config_set(&O.cfg, kv, eq + 1, err, sizeof(err)) != 0) {
                fprintf(stderr, "obcd: %s\n", err);
                return -1;
            }
        } else if (strcmp(argv[i], "--check-config") == 0) {
            *check_only = true;
        } else {
            usage();
            return -1;
        }
    }
    if (obcd_config_validate(&O.cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "obcd: %s\n", err);
        return -1;
    }
    return 0;
}

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

/* Requests one output line, low. A line that is not configured, or that
 * the kernel refuses, leaves that output absent: the core then logs every
 * attempt to drive it as not driven. */
static int open_output(uint64_t now, int32_t line, const char *name)
{
    if (line == OBCD_LINE_NONE) {
        write_log(now, "output", "\"which\":\"%s\",\"line\":null,\"note\":\"not configured\"", name);
        return -1;
    }
    int fd = hw_gpio_output(O.cfg.gpio_chip, (uint32_t)line, name);
    if (fd < 0) {
        write_log(now, "output", "\"which\":\"%s\",\"chip\":\"%s\",\"line\":%d,\"error\":\"%s\"",
                  name, O.cfg.gpio_chip, (int)line, strerror(errno));
    } else {
        write_log(now, "output", "\"which\":\"%s\",\"chip\":\"%s\",\"line\":%d",
                  name, O.cfg.gpio_chip, (int)line);
    }
    return fd;
}

static void start_threads(uint64_t now)
{
    atomic_store(&O.stop_threads, false);
    if (O.cfg.i2c_dev[0] != '\0') {
        O.power_started = pthread_create(&O.power_thread, NULL, power_main, NULL) == 0;
    } else {
        write_log(now, "sensor", "\"which\":\"ina219\",\"note\":\"not configured\"");
    }
    if (O.cfg.w1_path[0] != '\0') {
        O.temp_started = pthread_create(&O.temp_thread, NULL, temp_main, NULL) == 0;
    } else {
        write_log(now, "sensor", "\"which\":\"ds18b20\",\"note\":\"not configured\"");
    }
}

static void stop_threads(void)
{
    atomic_store(&O.stop_threads, true);
    /* A thread inside a read of the 1-Wire file finishes it first: at most
     * one conversion, ~750 ms. */
    if (O.power_started) {
        pthread_join(O.power_thread, NULL);
    }
    if (O.temp_started) {
        pthread_join(O.temp_thread, NULL);
    }
}

int main(int argc, char **argv)
{
    memset(&O, 0, sizeof(O));
    O.ipc_fd = O.burnwire_fd = O.heater_fd = -1;
    O.power_state = O.temp_state = -1;
    bool check_only;
    if (configure(argc, argv, &check_only) != 0) {
        return 2;
    }
    if (check_only) {
        printf("configuration valid: ipc_path=%s i2c_dev=%s w1_path=%s burnwire_line=%d heater_line=%d\n",
               O.cfg.ipc_path, O.cfg.i2c_dev[0] ? O.cfg.i2c_dev : "(none)",
               O.cfg.w1_path[0] ? O.cfg.w1_path : "(none)",
               (int)O.cfg.burnwire_line, (int)O.cfg.heater_line);
        return 0;
    }
    if (strcmp(O.cfg.log_path, "-") == 0) {
        O.log = stdout;
    } else if ((O.log = open_append(O.cfg.log_path, &O.log_bytes)) == NULL) {
        fprintf(stderr, "obcd: cannot open log %s: %s\n", O.cfg.log_path, strerror(errno));
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    /* Blocked before any thread starts, so the threads inherit the mask and
     * the signals reach the loop's signalfd only. */
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);
    O.signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    O.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    O.ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event es = { .events = EPOLLIN, .data.fd = O.signal_fd };
    struct epoll_event et = { .events = EPOLLIN, .data.fd = O.timer_fd };
    if (O.signal_fd < 0 || O.timer_fd < 0 || O.ep < 0 ||
        pipe2(O.readings, O_CLOEXEC) != 0 ||
        epoll_ctl(O.ep, EPOLL_CTL_ADD, O.signal_fd, &es) != 0 ||
        epoll_ctl(O.ep, EPOLL_CTL_ADD, O.timer_fd, &et) != 0) {
        fprintf(stderr, "obcd: event loop setup: %s\n", strerror(errno));
        return 1;
    }
    /* The loop's end does not block; the threads' end does, which is fine. */
    fcntl(O.readings[0], F_SETFL, O_NONBLOCK);
    struct epoll_event er = { .events = EPOLLIN, .data.fd = O.readings[0] };
    epoll_ctl(O.ep, EPOLL_CTL_ADD, O.readings[0], &er);

    uint64_t now = now_ms();
    write_log(now, "start",
              "\"version\":\"%s\",\"ipc_path\":\"%s\",\"i2c_dev\":\"%s\",\"ina219_addr\":%u,"
              "\"w1_path\":\"%s\",\"gpio_chip\":\"%s\"",
              TTEC_VERSION, O.cfg.ipc_path, O.cfg.i2c_dev, O.cfg.ina219_addr,
              O.cfg.w1_path, O.cfg.gpio_chip);
    O.burnwire_fd = open_output(now, O.cfg.burnwire_line, "burnwire");
    O.heater_fd = open_output(now, O.cfg.heater_line, "heater");

    obcd_ops_t ops = { .ctx = NULL, .ipc_send = op_ipc_send, .log = op_log,
                       .burnwire_set = op_burnwire, .heater_set = op_heater };
    obcd_init(&O.core, &O.cfg.p, &ops, now);
    start_threads(now);
    notify_open();
    notify("READY=1");
    O.ipc_retry_at = now;
    service(now);

    while (!O.exiting) {
        arm_timer(now_ms());
        struct epoll_event evs[8];
        int n = epoll_wait(O.ep, evs, 8, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            write_log(now_ms(), "epoll_error", "\"fatal\":true");
            O.exit_code = 1;
            break;
        }
        for (int i = 0; i < n && !O.exiting; i++) {
            int fd = evs[i].data.fd;
            if (fd == O.signal_fd) {
                struct signalfd_siginfo si;
                while (read(O.signal_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    write_log(now_ms(), "stop", "\"signal\":%" PRIu32, si.ssi_signo);
                    O.exiting = true;
                }
            } else if (fd == O.timer_fd) {
                uint64_t expirations;
                if (read(O.timer_fd, &expirations, sizeof(expirations)) < 0) {
                    /* spurious wake-up; service() below still runs */
                }
            } else if (fd == O.readings[0]) {
                read_readings();
            } else if (fd == O.ipc_fd) {
                ipc_read();
            }
        }
        if (!O.exiting) {
            service(now_ms());
        }
    }

    now = now_ms();
    notify("STOPPING=1");
    /* Outputs off before letting go of them (D3: the burn-wire never stays
     * on past obcd). */
    hw_gpio_set(O.burnwire_fd, false);
    hw_gpio_set(O.heater_fd, false);
    stop_threads();
    if (O.ipc_fd >= 0) {
        close(O.ipc_fd);
    }
    write_log(now, "exit", "\"code\":%d,\"transitions\":%" PRIu32 ",\"refused\":%" PRIu32,
              O.exit_code, O.core.n.transitions, O.core.n.refused);
    if (O.log != NULL && O.log != stdout) {
        fclose(O.log);
    }
    return O.exit_code;
}
