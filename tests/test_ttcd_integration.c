/*
 * The real ttcd binary, end to end, with no hardware.
 *
 * ttcd runs as a child process with the UDP radio. This test plays both of
 * its peers: the ground station (the real gs_link, on the other end of the
 * UDP radio) and the OBC (an IPC client). Everything the unit tests cannot
 * reach is exercised here: the epoll loop, the timer, the IPC handshake, the
 * radio backend, the configuration file and the log. Real time; ~10 s.
 *
 *   test_ttcd_integration /path/to/ttcd
 */

#define _GNU_SOURCE
#include "test_util.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_gs_link.h"
#include "gama_ipc.h"
#include "gama_tc.h"
#include "gama_tm.h"
#include "ipc.h"
#include "radio.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char dir[64], sock_path[128], log_path[128], conf_path[128];
static radio_t R;
static gs_link_t G;
static int obc_fd = -1, ep = -1, tfd = -1;
static pid_t child;

static uint32_t hk_rx, obc_events;
static gama_hk_t last_hk;
static struct { uint32_t id; uint8_t status; bool done; } acks[16];
static int n_acks;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint16_t free_udp_port(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &l);
    close(s);
    return ntohs(a.sin_port);
}

/* ---- the ground station ---- */

static int g_tx(void *c, const uint8_t *f, size_t n) { (void)c; return R.transmit(&R, f, n); }
static int g_prof(void *c, uint8_t p) { (void)c; return R.set_profile(&R, p); }

static void g_event(void *c, uint64_t now, const gs_event_t *e)
{
    (void)c; (void)now;
    if (e->kind == GS_EV_ACK && e->id != 0) {
        for (int i = 0; i < n_acks; i++) {
            if (acks[i].id == e->id) {
                acks[i].status = e->status;
                acks[i].done = true;
            }
        }
    } else if (e->kind == GS_EV_FRAME && e->type == GAMA_FRAME_TM_HK) {
        hk_rx++;
        gama_hk_decode(e->payload, e->len, &last_hk);
    }
}

static int submit(uint8_t cmd, const uint8_t *args, uint8_t n)
{
    uint32_t id = gs_submit(&G, now_ms(), cmd, args, n);
    acks[n_acks].id = id;
    acks[n_acks].done = false;
    return n_acks++;
}

/* ---- the OBC ---- */

static void obc_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t f[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(f, sizeof(f), type, 0, payload, len);
    ipc_send(obc_fd, f, (size_t)n);
}

static void obc_read(void)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL + 1];
    size_t len;
    while (ipc_recv(obc_fd, buf, sizeof(buf), &len) == IPC_OK) {
        gama_frame_t f;
        if (gama_frame_decode(buf, len, &f) > 0 && f.type == GAMA_FRAME_IPC_TC_EVENT &&
            f.payload[0] == GAMA_OBC_EV_TC_MISSION_ADSB) {
            obc_events++;
        }
    }
}

/* Runs the ground's event loop for up to `ms`, or until *cond holds. */
static void run(uint64_t ms, const bool *cond)
{
    uint64_t end = now_ms() + ms;
    while (now_ms() < end && !(cond != NULL && *cond)) {
        uint64_t d = gs_next_deadline(&G);
        uint64_t now = now_ms();
        uint64_t wake = d < end ? d : end;
        if (wake <= now) { wake = now + 1; }
        struct itimerspec its = { .it_value = { .tv_sec = (time_t)(wake / 1000u),
                                                .tv_nsec = (long)(wake % 1000u) * 1000000L } };
        timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
        struct epoll_event evs[4];
        int n = epoll_wait(ep, evs, 4, -1);
        for (int i = 0; i < n; i++) {
            if (evs[i].data.fd == R.fd) {
                radio_event_t ev;
                while (R.next_event(&R, &ev) == 1) {
                    if (ev.kind == RADIO_EV_RX) {
                        gs_on_rx(&G, now_ms(), ev.buf, ev.len, ev.rssi, ev.snr);
                    } else if (ev.kind == RADIO_EV_TX_DONE) {
                        gs_on_tx_done(&G, now_ms());
                    }
                }
            } else if (evs[i].data.fd == obc_fd) {
                obc_read();
            } else if (evs[i].data.fd == tfd) {
                uint64_t x;
                if (read(tfd, &x, sizeof(x)) < 0) { /* spurious */ }
                gs_on_tick(&G, now_ms());
            }
        }
    }
}

static bool file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) { return false; }
    static char text[1 << 20];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/ttcd\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    snprintf(dir, sizeof(dir), "/tmp/ttec-it-XXXXXX");
    if (mkdtemp(dir) == NULL) { perror("mkdtemp"); return 1; }
    snprintf(sock_path, sizeof(sock_path), "%s/ttec.sock", dir);
    snprintf(log_path, sizeof(log_path), "%s/ttcd.jsonl", dir);
    snprintf(conf_path, sizeof(conf_path), "%s/ttcd.conf", dir);

    uint16_t sat_port = free_udp_port(), gnd_port = free_udp_port();
    FILE *cf = fopen(conf_path, "w");
    fprintf(cf,
            "# integration test: the UDP radio instead of the SX1278\n"
            "radio = udp\n"
            "udp_port = %u\nudp_peer_port = %u\n"
            "ipc_path = %s\nlog_path = %s\n"
            "hk_period_ms = 2000\n",
            sat_port, gnd_port, sock_path, log_path);
    fclose(cf);

    TEST_GROUP("integration: ttcd starts with the UDP radio and a config file");
    child = fork();
    if (child == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        execl(argv[1], "ttcd", "-c", conf_path, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 200 && obc_fd < 0; i++) {
        obc_fd = ipc_client_connect(sock_path);
        if (obc_fd < 0) { usleep(10000); }
    }
    CHECK(obc_fd >= 0);

    const char *why = "";
    CHECK_EQ_INT(radio_open_udp(&R, "127.0.0.1", gnd_port, "127.0.0.1", sat_port,
                                GAMA_RATE_NOMINAL, &why), 0);
    gs_params_t gp;
    gs_params_default(&gp);
    gs_ops_t go = { .radio_tx = g_tx, .radio_set_profile = g_prof, .event = g_event };
    gs_init(&G, &gp, &go, now_ms());

    ep = epoll_create1(0);
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct epoll_event e1 = { .events = EPOLLIN, .data.fd = R.fd };
    struct epoll_event e2 = { .events = EPOLLIN, .data.fd = obc_fd };
    struct epoll_event e3 = { .events = EPOLLIN, .data.fd = tfd };
    epoll_ctl(ep, EPOLL_CTL_ADD, R.fd, &e1);
    epoll_ctl(ep, EPOLL_CTL_ADD, obc_fd, &e2);
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &e3);

    TEST_GROUP("integration: the OBC handshakes and reports telemetry");
    {
        uint8_t p[GAMA_IPC_TELEMETRY_LEN];
        gama_ipc_hello_t h = { .role = GAMA_IPC_ROLE_OBC, .version = GAMA_IPC_VERSION };
        gama_ipc_hello_encode(p, sizeof(p), &h);
        obc_send(GAMA_FRAME_IPC_HELLO, p, GAMA_IPC_HELLO_LEN);
        uint8_t mode = GAMA_OBC_ST_PRE_TEST;
        obc_send(GAMA_FRAME_IPC_MODE, &mode, 1);
        gama_ipc_telemetry_t t = { .battery_mv = 7777, .current_ma = -123,
                                   .temp_bat_ccel = 1500, .temp_ext_ccel = 2000 };
        gama_ipc_telemetry_encode(p, sizeof(p), &t);
        obc_send(GAMA_FRAME_IPC_TELEMETRY, p, GAMA_IPC_TELEMETRY_LEN);

        uint32_t first = hk_rx;
        bool got = false;
        for (int i = 0; i < 40 && !got; i++) {
            run(100, NULL);
            got = hk_rx > first && last_hk.battery_mv == 7777;
        }
        CHECK(got);
        CHECK_EQ_INT(last_hk.obc_mode, GAMA_OBC_ST_PRE_TEST);
        CHECK(last_hk.flags & GAMA_HK_F_OBC_LINKED);
    }

    TEST_GROUP("integration: a PING is acknowledged over the radio");
    {
        int k = submit(GAMA_TC_PING, NULL, 0);
        run(3000, &acks[k].done);
        CHECK(acks[k].done);
        CHECK_EQ_INT(acks[k].status, GAMA_ACK_OK);
    }

    TEST_GROUP("integration: SET_MODE reaches the OBC as an IPC event");
    {
        uint8_t ev = GAMA_OBC_EV_TC_MISSION_ADSB;
        int k = submit(GAMA_TC_SET_MODE, &ev, 1);
        run(3000, &acks[k].done);
        CHECK(acks[k].done);
        CHECK_EQ_INT(acks[k].status, GAMA_ACK_OK);
        run(200, NULL);
        CHECK_EQ_INT(obc_events, 1);
    }

    TEST_GROUP("integration: both ends move to FAST and keep talking");
    {
        uint8_t fast = GAMA_RATE_FAST;
        int k = submit(GAMA_TC_SET_RATE, &fast, 1);
        run(3000, &acks[k].done);
        CHECK(acks[k].done);
        run(1000, NULL);                                 /* confirmation PING */
        CHECK_EQ_INT(G.profile, GAMA_RATE_FAST);
        CHECK(!G.rate_pending);
        int j = submit(GAMA_TC_PING, NULL, 0);
        run(2000, &acks[j].done);
        CHECK(acks[j].done);
    }

    TEST_GROUP("integration: SHUTDOWN is acknowledged, then ttcd exits with 64");
    {
        uint8_t magic[2];
        gama_put_u16(magic, GAMA_TC_SHUTDOWN_MAGIC);
        int k = submit(GAMA_TC_SHUTDOWN, magic, 2);
        run(3000, &acks[k].done);
        CHECK(acks[k].done);
        int status = 0;
        pid_t w = 0;
        for (int i = 0; i < 100 && w == 0; i++) {
            w = waitpid(child, &status, WNOHANG);
            if (w == 0) { usleep(20000); }
        }
        CHECK_EQ_INT(w, child);
        CHECK(WIFEXITED(status));
        CHECK_EQ_INT(WEXITSTATUS(status), 64);
        struct stat st;
        CHECK(stat(sock_path, &st) != 0);                /* socket removed */
    }

    TEST_GROUP("integration: the log is one JSON object per line, with the story");
    {
        FILE *f = fopen(log_path, "r");
        CHECK(f != NULL);
        char line[1024];
        int lines = 0, well_formed = 0;
        while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
            lines++;
            size_t n = strlen(line);
            well_formed += strncmp(line, "{\"mono_ms\":", 11) == 0 && n > 2 &&
                           line[n - 2] == '}' && line[n - 1] == '\n';
        }
        if (f != NULL) { fclose(f); }
        CHECK(lines > 10);
        CHECK_EQ_INT(well_formed, lines);
        CHECK(file_contains(log_path, "\"event\":\"start\""));
        CHECK(file_contains(log_path, "\"event\":\"boot\""));
        CHECK(file_contains(log_path, "\"event\":\"obc_telemetry\""));
        CHECK(file_contains(log_path, "\"cmd\":\"SET_MODE\",\"seq\""));
        CHECK(file_contains(log_path, "\"to\":\"FAST\",\"reason\":\"commanded\""));
        CHECK(file_contains(log_path, "\"event\":\"rate_commit\""));
        CHECK(file_contains(log_path, "\"event\":\"shutdown\""));
    }

    R.close(&R);
    close(obc_fd);
    unlink(log_path);
    unlink(conf_path);
    rmdir(dir);
    TEST_SUMMARY("test_ttcd_integration");
}
