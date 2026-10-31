/*
 * The real obcd binary, with no OBC board.
 *
 * This test plays ttcd (the IPC server obcd connects to) and the battery's
 * DS18B20 (a w1_slave file it rewrites). The INA219's bus and the GPIO chip
 * do not exist, as on a development PC, so those paths are exercised as
 * failures: no data, outputs not driven, both in the log. What the unit
 * tests cannot reach is here: the epoll loop, the sensor thread and its
 * pipe, the reconnection, the configuration file and the shutdown. Real
 * time; ~6 s.
 *
 *   test_obcd_integration /path/to/obcd
 */

#define _GNU_SOURCE
#include "test_util.h"
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_tc.h"
#include "gama_tm.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char dir[64], sock_path[128], log_path[128], conf_path[128], w1_path[128];
static const char *obcd_bin;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void pause_ms(int ms)
{
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

static void write_w1(long milli, const char *crc)
{
    FILE *f = fopen(w1_path, "w");
    fprintf(f, "72 01 4b 46 7f ff 0e 10 57 : crc=57 %s\n72 01 4b 46 7f ff 0e 10 57 t=%ld\n",
            crc, milli);
    fclose(f);
}

static bool readable(int fd, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, ms) > 0;
}

/* What the fake ttcd has heard. */
static struct {
    int hellos, modes, telemetry;
    gama_ipc_hello_t hello;
    uint8_t mode;
    gama_ipc_telemetry_t tel;
} S;

static void listen_obcd(int fd, int ms, bool (*done)(void))
{
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end && !(done != NULL && done())) {
        if (!readable(fd, 50)) {
            continue;
        }
        uint8_t buf[GAMA_FRAME_MAX_TOTAL + 1];
        size_t len;
        while (ipc_recv(fd, buf, sizeof(buf), &len) == IPC_OK) {
            gama_frame_t f;
            if (gama_frame_decode(buf, len, &f) < 0) {
                continue;
            }
            if (f.type == GAMA_FRAME_IPC_HELLO) {
                S.hellos++;
                gama_ipc_hello_decode(f.payload, f.len, &S.hello);
            } else if (f.type == GAMA_FRAME_IPC_MODE && f.len == 1) {
                S.modes++;
                S.mode = f.payload[0];
            } else if (f.type == GAMA_FRAME_IPC_TELEMETRY) {
                S.telemetry++;
                gama_ipc_telemetry_decode(f.payload, f.len, &S.tel);
            }
        }
    }
}

static uint8_t want_mode;
static bool mode_is(void) { return S.mode == want_mode; }
static int16_t want_temp;
static bool temp_is(void) { return S.tel.temp_bat_ccel == want_temp; }

static void send_event(int fd, uint8_t ev)
{
    uint8_t f[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_TC_EVENT, 0, &ev, 1);
    if (n < 0 || ipc_send(fd, f, (size_t)n) != IPC_OK) {
        printf("  could not send IPC_TC_EVENT\n");
    }
}

static bool wait_mode(int fd, uint8_t mode, int ms)
{
    want_mode = mode;
    listen_obcd(fd, ms, mode_is);
    return S.mode == mode;
}

static int count_in_log(const char *needle)
{
    FILE *f = fopen(log_path, "r");
    int n = 0;
    char line[1024];
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        n += strstr(line, needle) != NULL;
    }
    if (f != NULL) { fclose(f); }
    return n;
}

static int accept_within(int lfd, int ms)
{
    if (!readable(lfd, ms)) {
        return -1;
    }
    int fd = ipc_server_accept(lfd);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    return fd;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/obcd\n", argv[0]);
        return 2;
    }
    obcd_bin = argv[1];
    signal(SIGPIPE, SIG_IGN);
    snprintf(dir, sizeof(dir), "/tmp/obcd-it-XXXXXX");
    if (mkdtemp(dir) == NULL) { perror("mkdtemp"); return 1; }
    snprintf(sock_path, sizeof(sock_path), "%s/ttec.sock", dir);
    snprintf(log_path, sizeof(log_path), "%s/obcd.jsonl", dir);
    snprintf(conf_path, sizeof(conf_path), "%s/obcd.conf", dir);
    snprintf(w1_path, sizeof(w1_path), "%s/w1_slave", dir);

    FILE *cf = fopen(conf_path, "w");
    fprintf(cf,
            "# integration test: no OBC board; ttcd and the DS18B20 played by the test\n"
            "ipc_path = %s\nlog_path = %s\nretry_ms = 200\n"
            "w1_path = %s\ntemp_period_ms = 800\ntemp_stale_ms = 2500\n"
            "i2c_dev = %s/no-i2c\npower_period_ms = 500\n"
            "gpio_chip = %s/no-gpiochip\nburnwire_line = 22\n"
            "burnwire_ms = 300\n",
            sock_path, log_path, w1_path, dir, dir);
    fclose(cf);
    write_w1(8500, "YES");                               /* 8.5 degC: cold */

    int lfd = ipc_server_open(sock_path, 4);
    CHECK(lfd >= 0);

    pid_t pid = fork();
    if (pid == 0) {
        execl(obcd_bin, "obcd", "-c", conf_path, (char *)NULL);
        _exit(127);
    }

    int fd = -1;
    TEST_GROUP("obcd integration: connects as the OBC, in PRE_TEST, telemetry every second");
    {
        fd = accept_within(lfd, 3000);
        CHECK(fd >= 0);
        listen_obcd(fd, 2500, NULL);
        CHECK_EQ_INT(S.hellos, 1);
        CHECK_EQ_INT(S.hello.role, GAMA_IPC_ROLE_OBC);
        CHECK_EQ_INT(S.hello.version, GAMA_IPC_VERSION);
        CHECK(S.modes >= 1);
        CHECK_EQ_INT(S.mode, GAMA_OBC_ST_PRE_TEST);
        CHECK(S.telemetry >= 2);
        CHECK_EQ_INT(S.tel.temp_bat_ccel, 850);               /* from the w1_slave file */
        CHECK_EQ_INT(S.tel.battery_mv, GAMA_NO_DATA_U16);     /* no I2C bus here */
        CHECK_EQ_INT(S.tel.roll_cdeg, GAMA_NO_DATA_I16);
    }

    TEST_GROUP("obcd integration: absent hardware is in the log, and nothing pretends");
    {
        CHECK_EQ_INT(count_in_log("\"event\":\"output\",\"which\":\"burnwire\""), 1);
        CHECK_EQ_INT(count_in_log("\"which\":\"heater\",\"line\":null"), 1);
        CHECK_EQ_INT(count_in_log("\"event\":\"sensor\",\"which\":\"ina219\",\"ok\":false"), 1);
        CHECK_EQ_INT(count_in_log("\"event\":\"sensor\",\"which\":\"ds18b20\",\"ok\":true"), 1);
        /* 8.5 degC: the thermostat asks for heat, with no line to drive (D4). */
        CHECK_EQ_INT(count_in_log("\"event\":\"heater\",\"on\":true,\"why\":\"cold\""), 1);
    }

    TEST_GROUP("obcd integration: the thermostat follows the file; a CRC failure goes stale, then off");
    {
        write_w1(12500, "YES");
        want_temp = 1250;
        listen_obcd(fd, 2500, temp_is);
        CHECK_EQ_INT(S.tel.temp_bat_ccel, 1250);
        CHECK_EQ_INT(count_in_log("\"event\":\"heater\",\"on\":false,\"why\":\"warm\""), 1);
        write_w1(5000, "NO");
        want_temp = GAMA_NO_DATA_I16;
        listen_obcd(fd, 4000, temp_is);
        CHECK_EQ_INT(S.tel.temp_bat_ccel, GAMA_NO_DATA_I16);
        CHECK_EQ_INT(count_in_log("\"event\":\"temp_fault\""), 1);
        CHECK_EQ_INT(count_in_log("\"which\":\"ds18b20\",\"ok\":false,\"why\":\"CRC\""), 1);
    }

    TEST_GROUP("obcd integration: SET_MODE events walk Figure 44; an invalid one is refused");
    {
        send_event(fd, GAMA_OBC_EV_TC_MISSION_ADSB);
        CHECK(wait_mode(fd, GAMA_OBC_ST_MISSION_ADSB, 1000));
        send_event(fd, GAMA_OBC_EV_TC_AOCS);                   /* D1: not from here */
        listen_obcd(fd, 300, NULL);
        CHECK_EQ_INT(S.mode, GAMA_OBC_ST_MISSION_ADSB);
        CHECK_EQ_INT(count_in_log("\"event\":\"event_refused\",\"event\":\"TC_AOCS\""), 1);
        send_event(fd, GAMA_OBC_EV_TC_DOWNLINK);
        CHECK(wait_mode(fd, GAMA_OBC_ST_MISSION_DOWNLINK, 1000));
        send_event(fd, GAMA_OBC_EV_TASK_DONE);                 /* as ttcd posts it (D2) */
        CHECK(wait_mode(fd, GAMA_OBC_ST_PRE_TEST, 1000));
        CHECK_EQ_INT(count_in_log("\"event\":\"mode\""), 3);   /* OBC-5 */
    }

    TEST_GROUP("obcd integration: BASIC_INTERMEDIATE ends by itself after the burn-wire pulse");
    {
        send_event(fd, GAMA_OBC_EV_TC_BASIC_INTER);
        CHECK(wait_mode(fd, GAMA_OBC_ST_BASIC_INTERMEDIATE, 1000));
        CHECK(wait_mode(fd, GAMA_OBC_ST_PRE_TEST, 1500));
        CHECK_EQ_INT(count_in_log("\"by\":\"burnwire\""), 1);
        CHECK_EQ_INT(count_in_log("\"event\":\"burnwire\",\"on\":true,\"why\":\"deploy\",\"driven\":false"), 1);
    }

    TEST_GROUP("obcd integration: ttcd restarting is survived: obcd reconnects, says hello and the mode");
    {
        send_event(fd, GAMA_OBC_EV_TC_SURVIVAL);
        CHECK(wait_mode(fd, GAMA_OBC_ST_ENV_SURVIVAL, 1000));
        close(fd);
        memset(&S, 0, sizeof(S));
        fd = accept_within(lfd, 3000);
        CHECK(fd >= 0);
        if (fd >= 0) {
            want_mode = GAMA_OBC_ST_ENV_SURVIVAL;
            listen_obcd(fd, 1500, mode_is);
        }
        CHECK_EQ_INT(S.hellos, 1);
        CHECK_EQ_INT(S.mode, GAMA_OBC_ST_ENV_SURVIVAL);      /* the mode survived */
    }

    TEST_GROUP("obcd integration: SIGTERM stops it cleanly, the story in its log");
    {
        kill(pid, SIGTERM);
        int status = -1;
        pid_t w = 0;
        for (int i = 0; i < 150 && w == 0; i++) {
            w = waitpid(pid, &status, WNOHANG);
            if (w == 0) { pause_ms(20); }
        }
        if (w != pid) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        CHECK(w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK_EQ_INT(count_in_log("\"event\":\"stop\""), 1);
        CHECK_EQ_INT(count_in_log("\"event\":\"exit\",\"code\":0"), 1);
    }

    if (fd >= 0) { close(fd); }
    close(lfd);
    unlink(sock_path);
    unlink(log_path);
    unlink(conf_path);
    unlink(w1_path);
    rmdir(dir);
    TEST_SUMMARY("test_obcd_integration");
}
