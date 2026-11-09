/*
 * main.c — the obcd shell: the clock, the connection to ttcd, the GPIO
 * lines and the sensor threads (ADR-0016).
 *
 *   obcd -c /etc/gama/obcd.conf [-o key=value]... [--check-config]
 *
 * Every decision lives in the core (core.h). The main loop waits in exactly
 * one place, epoll_wait, and never on a device (AGENTS.md, hard rule 3).
 * Two threads do the waiting, and their results reach the loop through a
 * pipe:
 *   - the DS18B20's, whose read blocks for its ~750 ms conversion (D9);
 *   - the I2C thread, the only owner of the shared bus (ADR-0017): the
 *     INA219's readings, and the AOCS ESP32's START and STATUS frames, on
 *     the timeline of flight/obcd/i2c_sched.h, which lends the bus to the
 *     ESP32 for its task. A stuck bus stalls that thread, not the mode
 *     transitions or the thermostat. The loop sends it STARTs through a
 *     second pipe.
 *
 * What it keeps alive: the connection to ttcd, retried every retry_ms, so
 * either can start first or restart (ADR-0005); systemd keeps obcd itself
 * alive (Restart=always), and its watchdog sees the loop turning (D8).
 */

#define _GNU_SOURCE
#include "config.h"
#include "core.h"
#include "ds18b20.h"
#include "gama_aocs.h"
#include "hw_linux.h"
#include "i2c_sched.h"
#include "ina219.h"
#include "ipc.h"
#include "gama_frame.h"
#include "gama_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <poll.h>
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

/* One message, from a thread to the loop. Smaller than PIPE_BUF, so every
 * write is atomic and the reader never sees half of one. */
enum { READING_POWER = 1, READING_TEMP = 2, READING_AOCS_STARTED = 3, READING_AOCS_RESULT = 4 };
typedef struct {
    uint8_t  kind;
    uint8_t  ok;           /* AOCS_STARTED: obcd_aocs_outcome_t; RESULT: valid */
    uint16_t battery_mv;
    int16_t  value;        /* current_ma, or the temperature in ccel */
    int16_t  why;          /* failure: errno for the INA219, ds18b20_result_t */
    uint64_t lease_until;  /* AOCS_STARTED */
    uint8_t  status[GAMA_AOCS_STATUS_LEN];   /* AOCS_RESULT */
} reading_t;

/* One START, from the loop to the I2C thread. */
typedef struct {
    uint8_t  frame[GAMA_AOCS_COMMAND_LEN];
    uint8_t  seq;
    uint16_t max_task_s;
} job_t;

static struct {
    obcd_config_t cfg;
    obcd_core_t   core;
    int           ep, timer_fd, signal_fd;
    int           readings[2];          /* pipe: [0] the loop, [1] the threads */
    int           jobs[2];              /* pipe: [0] the I2C thread, [1] the loop */

    int           ipc_fd;
    bool          ipc_complained;
    uint64_t      ipc_retry_at;

    int           burnwire_fd, heater_fd;

    FILE         *log;
    uint64_t      log_bytes;
    bool          log_full;

    /* what the loop last said about each sensor, to log changes only */
    int           power_state, temp_state;   /* -1 unknown, 0 failing, 1 ok */

    pthread_t     i2c_thread, temp_thread;
    bool          i2c_started, temp_started;
    atomic_bool   stop_threads;
    uint64_t      resumed_lease;        /* a lease left on disk; 0 none */

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

/* Hands a START to the I2C thread, without waiting: a full queue is a
 * failure the core reports, never a stall. */
static int op_aocs_start(void *ctx, const uint8_t *frame, size_t len, uint8_t seq,
                         uint16_t max_task_s)
{
    (void)ctx;
    if (!O.i2c_started || len != GAMA_AOCS_COMMAND_LEN) {
        return -1;
    }
    job_t j = { .seq = seq, .max_task_s = max_task_s };
    memcpy(j.frame, frame, len);
    return write(O.jobs[1], &j, sizeof(j)) == (ssize_t)sizeof(j) ? 0 : -1;
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

/* ---- the lease's end on disk (ADR-0017, ICD §5) ----
 *
 * Written as soon as the START is out, before the ESP32 answers, and removed
 * when the START is refused or the lease's result is read: an obcd that
 * starts while it is there keeps off the bus until the lease is over. It is
 * kept on the SD card because a reboot of the Pi does not reset the ESP32;
 * the monotonic clock does not survive one, so a file from another boot
 * counts as what was left of the lease when it was written, from now. */

static void boot_id(char *out, size_t cap)
{
    out[0] = '\0';
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "re");
    if (f != NULL) {
        if (fgets(out, (int)cap, f) == NULL) {
            out[0] = '\0';
        }
        out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

/* 0, or the errno of what failed. sync: on the card, not only in the page
 * cache, which can take tens of ms — never inside the STATUS window. */
static int lease_save(uint64_t now, uint64_t lease_until, bool sync)
{
    const char *path = O.cfg.i2c_lease_path;
    if (path[0] == '\0') {
        return 0;
    }
    char id[64], tmp[sizeof(O.cfg.i2c_lease_path) + 4];
    boot_id(id, sizeof(id));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return errno;
    }
    int e = 0;
    if (dprintf(fd, "boot_id %s\nlease_until_mono_ms %" PRIu64 "\nlease_left_ms %" PRIu64 "\n",
                id[0] != '\0' ? id : "-", lease_until,
                lease_until > now ? lease_until - now : 0u) < 0 ||
        (sync && fsync(fd) != 0)) {
        e = errno;
    }
    close(fd);
    /* Renamed into place: a crash leaves the old file or the new one, never
     * half of one. */
    if (e == 0 && rename(tmp, path) != 0) {
        e = errno;
    }
    if (e == 0 && sync) {
        char d[sizeof(O.cfg.i2c_lease_path)];
        snprintf(d, sizeof(d), "%s", path);
        int dfd = open(dirname(d), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0 || fsync(dfd) != 0) {
            e = errno;
        }
        if (dfd >= 0) {
            close(dfd);
        }
    }
    return e;
}

static void lease_drop(void)
{
    if (O.cfg.i2c_lease_path[0] != '\0') {
        unlink(O.cfg.i2c_lease_path);
    }
}

/* When a lease left on disk ends, in this boot's clock; 0 if there is none. */
static uint64_t lease_load(uint64_t now)
{
    FILE *f = O.cfg.i2c_lease_path[0] != '\0' ? fopen(O.cfg.i2c_lease_path, "re") : NULL;
    if (f == NULL) {
        return 0;
    }
    char stored[64], id[64];
    uint64_t until = 0, left = 0;
    int n = fscanf(f, "boot_id %63s lease_until_mono_ms %" SCNu64 " lease_left_ms %" SCNu64,
                   stored, &until, &left);
    fclose(f);
    if (n != 3) {
        /* A lease was started, and the file no longer says how long: the
         * longest this configuration asks for. */
        return now + GAMA_AOCS_T_TAKEOVER_MS + (uint64_t)obcd_aocs_max_task_s(&O.cfg.p) * 1000u +
               GAMA_AOCS_T_RESUME_MARGIN_MS;
    }
    boot_id(id, sizeof(id));
    if (id[0] != '\0' && strcmp(id, stored) == 0) {
        return until;                  /* this boot: the same clock */
    }
    /* Another boot: the clock started again, some time after the file was
     * written, so what was left then is the most that can be left now. */
    return now + left;
}

/* ---- the I2C thread: the only owner of the shared bus (ADR-0017) ---- */

static i2c_result_t as_result(int r)
{
    return r == 0 ? I2C_OK : (r == -EREMOTEIO || r == -ENXIO) ? I2C_NACK : I2C_ERROR;
}

/* -errno of a driver call that failed; -EIO if it said nothing (the
 * INA219's overflow flag). */
static int failure(void)
{
    return errno != 0 ? -errno : -EIO;
}

/* One transaction the scheduler asked for. The INA219's readings, and its
 * failures, go to the loop. */
static i2c_result_t transfer(hw_i2c_t *h, ina219_bus_t *bus, const i2c_sched_t *s,
                             i2c_op_t op, uint8_t *status)
{
    int r = 0;
    errno = 0;
    if (h->fd < 0 && hw_i2c_open(h, O.cfg.i2c_dev, O.cfg.ina219_addr, bus) != 0) {
        r = failure();
        if (op == I2C_OP_AOCS_START) {
            return I2C_NACK;          /* nothing went on the wire: nobody took it */
        }
    }
    reading_t power = { .kind = READING_POWER };
    if (r == 0) {
        switch (op) {
        case I2C_OP_INA219_INIT:
            r = ina219_init(bus) == 0 ? 0 : failure();
            break;
        case I2C_OP_INA219_READ:
            r = ina219_read(bus, O.cfg.ina219_shunt_mohm, &power.battery_mv, &power.value) == 0
                    ? 0 : failure();
            break;
        case I2C_OP_AOCS_START:
            r = hw_i2c_write(h->fd, O.cfg.aocs_addr, s->start, GAMA_AOCS_COMMAND_LEN);
            break;
        case I2C_OP_AOCS_STATUS:
            r = hw_i2c_read(h->fd, O.cfg.aocs_addr, status, GAMA_AOCS_STATUS_LEN);
            break;
        case I2C_OP_NONE:
        default:
            break;
        }
    }
    if (op == I2C_OP_INA219_INIT || op == I2C_OP_INA219_READ) {
        if (op == I2C_OP_INA219_READ || r != 0) {
            power.ok = r == 0;
            power.why = (int16_t)-r;
            deliver(&power);
        }
        if (r != 0) {
            hw_i2c_close(h);          /* reopened next time; the scheduler reconfigures it */
        }
    }
    return as_result(r);
}

/* Tells the loop what became of a START, or of a lease. */
static void report(const i2c_sched_t *s, uint64_t now, i2c_event_t ev, bool valid,
                   const uint8_t *status)
{
    reading_t m = { .kind = READING_AOCS_STARTED };
    switch (ev) {
    case I2C_EV_AOCS_LENT:
        /* Now on the card too, the STATUS window over: from here a reboot of
         * the Pi finds it. */
        m.ok = OBCD_AOCS_LENT;
        m.lease_until = s->lease_until;
        m.why = (int16_t)lease_save(now, s->lease_until, true);
        break;
    case I2C_EV_AOCS_REFUSED:
        lease_drop();
        m.ok = OBCD_AOCS_REFUSED;
        break;
    case I2C_EV_AOCS_ABSENT:
        /* After a START that failed but may have gone out, the file was
         * written already: nobody took it, so nothing is kept. */
        lease_drop();
        m.ok = OBCD_AOCS_ABSENT;
        break;
    case I2C_EV_AOCS_RESULT:
        lease_drop();
        m.kind = READING_AOCS_RESULT;
        m.ok = valid;
        if (status != NULL) {
            memcpy(m.status, status, sizeof(m.status));
        }
        break;
    case I2C_EV_NONE:
    default:
        return;
    }
    deliver(&m);
}

/* Waits for a START from the loop: until `until`, and 100 ms at most, so a
 * stop request is seen. */
static void wait_for_job(i2c_sched_t *s, uint64_t now, uint64_t until)
{
    uint64_t ms = until > now ? until - now : 0;
    struct pollfd p = { .fd = O.jobs[0], .events = POLLIN };
    job_t j;
    if (poll(&p, 1, (int)(ms < 100u ? ms : 100u)) > 0 &&
        read(O.jobs[0], &j, sizeof(j)) == (ssize_t)sizeof(j)) {
        i2c_sched_request_start(s, j.frame, j.seq, j.max_task_s);
    }
}

static void *i2c_main(void *arg)
{
    (void)arg;
    hw_i2c_t h = { .fd = -1 };
    ina219_bus_t bus;
    i2c_sched_t s;
    i2c_sched_params_t sp = { .power_period_ms = O.cfg.power_period_ms };
    uint64_t now = now_ms();
    i2c_sched_init(&s, &sp, now);
    i2c_sched_resume_lease(&s, now, O.resumed_lease);     /* nothing if it is over */

    while (!atomic_load(&O.stop_threads)) {
        i2c_op_t op;
        uint64_t wake;
        now = now_ms();
        report(&s, now, i2c_sched_next(&s, now, &op, &wake), false, NULL);
        if (op == I2C_OP_NONE) {
            wait_for_job(&s, now, wake);
            continue;
        }
        uint8_t status[GAMA_AOCS_STATUS_LEN] = { 0 };
        i2c_result_t r = transfer(&h, &bus, &s, op, status);
        bool valid = false;
        now = now_ms();
        i2c_event_t ev = i2c_sched_done(&s, now, op, r, status, &valid);
        if (op == I2C_OP_AOCS_START && s.phase == I2C_PH_STARTING) {
            /* Before the ESP32 answers, so an obcd that dies now does not
             * leave the next one free to talk. Not synced yet: that waits
             * for the end of the STATUS window. */
            lease_save(now, s.lease_until, false);
        }
        report(&s, now, ev, valid, status);
    }
    hw_i2c_close(&h);
    return NULL;
}

/* The DS18B20's reading: from w1_path, or with "auto" from the one DS18B20
 * the kernel lists, looked up at every reading — the kernel finds the chip
 * some seconds after boot, and a replaced chip has another serial. */
static ds18b20_result_t read_battery_temp(int16_t *ccel)
{
    if (strcmp(O.cfg.w1_path, "auto") != 0) {
        return ds18b20_read_file(O.cfg.w1_path, ccel);
    }
    char path[sizeof(O.cfg.w1_devices_dir) + 64];
    int n = ds18b20_find(O.cfg.w1_devices_dir, path, sizeof(path));
    return n == 1 ? ds18b20_read_file(path, ccel)
         : n == 0 ? DS18B20_ERR_NOT_FOUND
         : n > 1  ? DS18B20_ERR_AMBIGUOUS
                  : DS18B20_ERR_IO;
}

static void *temp_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&O.stop_threads)) {
        reading_t r = { .kind = READING_TEMP };
        int16_t ccel = 0;
        ds18b20_result_t res = read_battery_temp(&ccel);
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
            } else if (r[i].kind == READING_AOCS_STARTED) {
                if (r[i].why != 0) {
                    write_log(now, "lease_file", "\"path\":\"%s\",\"saved\":false,\"why\":\"%s\"",
                              O.cfg.i2c_lease_path, strerror(r[i].why));
                }
                obcd_on_aocs_started(&O.core, now, (obcd_aocs_outcome_t)r[i].ok,
                                     r[i].lease_until);
            } else if (r[i].kind == READING_AOCS_RESULT) {
                obcd_on_aocs_result(&O.core, now, r[i].ok != 0, r[i].status);
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
        /* A lease a previous obcd left: the core knows from the start, and
         * the thread keeps off the bus until it ends (ADR-0017). */
        O.resumed_lease = lease_load(now);
        if (O.resumed_lease > now) {
            write_log(now, "lease_resumed", "\"path\":\"%s\",\"lease_ms\":%" PRIu64,
                      O.cfg.i2c_lease_path, O.resumed_lease - now);
            obcd_on_aocs_started(&O.core, now, OBCD_AOCS_LENT, O.resumed_lease);
        } else {
            lease_drop();
        }
        O.i2c_started = pthread_create(&O.i2c_thread, NULL, i2c_main, NULL) == 0;
    } else {
        write_log(now, "sensor", "\"which\":\"ina219\",\"note\":\"not configured\"");
    }
    if (O.cfg.w1_path[0] != '\0') {
        if (strcmp(O.cfg.w1_path, "auto") == 0) {
            /* What the thread will find now; it looks again at every reading. */
            char path[sizeof(O.cfg.w1_devices_dir) + 64];
            int n = ds18b20_find(O.cfg.w1_devices_dir, path, sizeof(path));
            if (n == 1) {
                write_log(now, "sensor", "\"which\":\"ds18b20\",\"found\":1,\"path\":\"%s\"", path);
            } else {
                write_log(now, "sensor", "\"which\":\"ds18b20\",\"found\":%d,\"dir\":\"%s\"",
                          n, O.cfg.w1_devices_dir);
            }
        }
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
    if (O.i2c_started) {
        pthread_join(O.i2c_thread, NULL);
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
        printf("configuration valid: ipc_path=%s i2c_dev=%s aocs_addr=0x%02x w1_path=%s "
               "burnwire_line=%d heater_line=%d\n",
               O.cfg.ipc_path, O.cfg.i2c_dev[0] ? O.cfg.i2c_dev : "(none)", O.cfg.aocs_addr,
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
        pipe2(O.readings, O_CLOEXEC) != 0 || pipe2(O.jobs, O_CLOEXEC) != 0 ||
        epoll_ctl(O.ep, EPOLL_CTL_ADD, O.signal_fd, &es) != 0 ||
        epoll_ctl(O.ep, EPOLL_CTL_ADD, O.timer_fd, &et) != 0) {
        fprintf(stderr, "obcd: event loop setup: %s\n", strerror(errno));
        return 1;
    }
    /* The loop's ends do not block; the threads' ends do, which is fine. */
    fcntl(O.readings[0], F_SETFL, O_NONBLOCK);
    fcntl(O.jobs[1], F_SETFL, O_NONBLOCK);
    struct epoll_event er = { .events = EPOLLIN, .data.fd = O.readings[0] };
    epoll_ctl(O.ep, EPOLL_CTL_ADD, O.readings[0], &er);

    uint64_t now = now_ms();
    write_log(now, "start",
              "\"version\":\"%s\",\"ipc_path\":\"%s\",\"i2c_dev\":\"%s\",\"ina219_addr\":%u,"
              "\"aocs_addr\":%u,\"i2c_lease_path\":\"%s\",\"w1_path\":\"%s\",\"gpio_chip\":\"%s\"",
              TTEC_VERSION, O.cfg.ipc_path, O.cfg.i2c_dev, O.cfg.ina219_addr, O.cfg.aocs_addr,
              O.cfg.i2c_lease_path, O.cfg.w1_path, O.cfg.gpio_chip);
    O.burnwire_fd = open_output(now, O.cfg.burnwire_line, "burnwire");
    O.heater_fd = open_output(now, O.cfg.heater_line, "heater");

    obcd_ops_t ops = { .ctx = NULL, .ipc_send = op_ipc_send, .log = op_log,
                       .burnwire_set = op_burnwire, .heater_set = op_heater,
                       .aocs_start = op_aocs_start };
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
