/*
 * gs_cli — ground station for the bench.
 *
 * Runs the ground station's console (ground/console/gs_console.h: the real
 * ground side of the link, its commands and its JSON lines) on a radio: a
 * second RA-02 on a Raspberry Pi, or the UDP radio on a development machine.
 * Reads one command per line from standard input and waits for each to
 * finish before reading the next, so a file of commands is a repeatable bench
 * procedure. Writes one JSON object per line to standard output: every frame
 * received — with its raw bytes in hex, RSSI and SNR, so any analysis can be
 * redone from the log (HLR-SW-02) — every acknowledgement, loss, rate change
 * and contact change, and a summary at the end. The commands are listed in
 * gs_console.h; the ESP32 ground station takes the same ones.
 *
 *   gs_cli --radio sx1278 --spi /dev/spidev0.1 --reset 16 --dio0 26 --power 2
 *   gs_cli --radio udp --port 47002 --peer-port 47001
 *
 * With --auto-time on, every time contact with the satellite is (re)gained
 * the ground sends its wall clock (SET_TIME) by itself, as operations do
 * (ADR-0009). The bench steps leave it off, so their measurements see only
 * the telecommands their scripts send.
 */

#define _GNU_SOURCE
#include "gs_console.h"
#include "radio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static radio_t      R;
static gs_console_t C;
static bool         stdin_open = true;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- the console's operations ---- */

static int c_tx(void *c, const uint8_t *f, size_t n) { (void)c; return R.transmit(&R, f, n); }
static int c_prof(void *c, uint8_t p) { (void)c; return R.set_profile(&R, p); }
static int c_power(void *c, int8_t dbm) { (void)c; return R.set_power(&R, dbm); }

static int c_wall(void *c, uint64_t *ms)
{
    (void)c;
    struct timespec w;
    clock_gettime(CLOCK_REALTIME, &w);
    *ms = (uint64_t)w.tv_sec * 1000u + (uint64_t)w.tv_nsec / 1000000u;
    return 0;
}

static void c_write(void *c, const char *line, size_t len)
{
    (void)c;
    fwrite(line, 1, len, stdout);
    fflush(stdout);
}

/* Reads and runs lines while the console can take them. */
static void read_commands(void)
{
    static char buf[4096];
    static size_t have;
    while (stdin_open && gs_console_wants_line(&C)) {
        char *nl = memchr(buf, '\n', have);
        if (nl != NULL) {
            char line[GS_CONSOLE_INPUT_MAX];
            size_t len = (size_t)(nl - buf);
            if (len >= sizeof(line)) {
                len = sizeof(line) - 1u;     /* an absurdly long line is cut */
            }
            memcpy(line, buf, len);
            line[len] = '\0';
            size_t used = (size_t)(nl - buf) + 1u;
            memmove(buf, buf + used, have - used);
            have -= used;
            gs_console_line(&C, mono_ms(), line);
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
                gs_console_line(&C, mono_ms(), buf);
            }
            gs_console_eof(&C);
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
            "  both:   --profile safe|nominal|fast (nominal)\n"
            "          --auto-time on|off (off): SET_TIME whenever contact comes up\n");
}

int main(int argc, char **argv)
{
    const char *radio = NULL, *spi = "/dev/spidev0.1", *chip = "/dev/gpiochip0",
               *bind_ip = "127.0.0.1", *peer = "127.0.0.1", *lbt = "header",
               *profile_name = "nominal", *auto_time = "off";
    uint32_t reset = 16, dio0 = 26;
    int power = 2, port = 47002, peer_port = 47001;
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
        else if (strcmp(k, "--auto-time") == 0) { auto_time = v; }
        else if (strcmp(k, "--profile") == 0) {
            /* Anything but safe or fast has always meant nominal here. */
            profile_name = strcmp(v, "safe") == 0 || strcmp(v, "fast") == 0 ? v : "nominal";
        } else { usage(); return 2; }
    }
    if (radio == NULL) { usage(); return 2; }

    gs_console_ops_t ops = { .radio_tx = c_tx, .radio_set_profile = c_prof,
                             .radio_set_power = c_power, .wall_ms = c_wall,
                             .write_line = c_write };
    gs_console_init(&C, &ops, radio);
    const char *why = "";
    if (gs_console_set(&C, mono_ms(), "auto_time", auto_time, &why) != 0) {
        usage();
        return 2;
    }
    gs_console_set(&C, mono_ms(), "profile", profile_name, &why);
    uint8_t profile = C.params.initial_profile;

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
    gs_console_begin(&C, mono_ms());

    int ep = epoll_create1(EPOLL_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int fl = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    struct epoll_event e1 = { .events = EPOLLIN, .data.fd = R.fd };
    struct epoll_event e2 = { .events = EPOLLIN, .data.fd = tfd };
    struct epoll_event e3 = { .events = EPOLLIN, .data.fd = STDIN_FILENO };
    epoll_ctl(ep, EPOLL_CTL_ADD, R.fd, &e1);
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &e2);
    /* A regular file cannot be added to epoll; it is always readable, so
     * read_commands() then simply runs whenever the console is idle. */
    bool stdin_polled = epoll_ctl(ep, EPOLL_CTL_ADD, STDIN_FILENO, &e3) == 0;

    uint64_t last_poll = mono_ms();
    read_commands();
    while (!gs_console_finished(&C)) {
        uint64_t now = mono_ms();
        uint64_t d = gs_console_next_deadline(&C);
        if (last_poll + 250u < d) { d = last_poll + 250u; }
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
                if (!stdin_open) {
                    /* At end of input a pipe stays readable: stop watching it,
                     * or every pass would wake up for nothing. */
                    epoll_ctl(ep, EPOLL_CTL_DEL, STDIN_FILENO, NULL);
                } else {
                    read_commands();
                }
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
            if (ev.kind == RADIO_EV_RX) {
                gs_console_on_rx(&C, mono_ms(), ev.buf, ev.len, ev.rssi, ev.snr);
            } else if (ev.kind == RADIO_EV_RX_ERROR) {
                gs_console_on_rx_error(&C, mono_ms());
            } else if (ev.kind == RADIO_EV_TX_DONE) {
                gs_console_on_tx_done(&C, mono_ms());
            }
        }
        gs_console_on_tick(&C, mono_ms());
        if (stdin_open && (!stdin_polled || gs_console_wants_line(&C))) {
            read_commands();
        }
    }

    gs_console_summary(&C, mono_ms());
    R.close(&R);
    return 0;
}
