/*
 * The real adsbd binary, with no SDR and no ttcd.
 *
 * This test plays both of adsbd's peers: dump1090 (a TCP server writing SBS
 * lines, as dump1090-fa does on port 30003) and ttcd (the IPC server adsbd
 * connects to). Everything the unit tests cannot reach is exercised here:
 * the epoll loop, both reconnections, the record file, the event log, the
 * configuration file and the supervision of a child process. Real time; ~4 s.
 *
 *   test_adsbd_integration /path/to/adsbd
 */

#define _GNU_SOURCE
#include "test_util.h"
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_tm.h"
#include "ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char dir[64], sock_path[128], rec_path[128], log_path[128], conf_path[128];
static const char *adsbd_bin;
static uint16_t sbs_port;

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

/* dump1090's SBS port: a TCP listener on loopback, on any free port. */
static int sbs_listen(void)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    listen(s, 4);
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &l);
    sbs_port = ntohs(a.sin_port);
    return s;
}

static bool readable(int fd, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, ms) > 0;
}

static int accept_within(int lfd, int ms)
{
    return readable(lfd, ms) ? accept4(lfd, NULL, NULL, SOCK_CLOEXEC) : -1;
}

static void say(int fd, const char *text)
{
    size_t n = strlen(text);
    if (write(fd, text, n) != (ssize_t)n) {
        printf("  short write to adsbd\n");
    }
}

static void write_conf(const char *dump1090_cmd)
{
    FILE *cf = fopen(conf_path, "w");
    fprintf(cf,
            "# integration test: no SDR, no ttcd — both played by the test\n"
            "ipc_path = %s\nsbs_port = %u\n"
            "ndjson_path = %s\nlog_path = %s\n"
            "dump1090_cmd = %s\n"
            "retry_ms = 200\nrestart_min_ms = 100\nrestart_max_ms = 400\n",
            sock_path, sbs_port, rec_path, log_path, dump1090_cmd);
    fclose(cf);
}

/* Runs adsbd with extra arguments to completion; returns its exit status. */
static int run_adsbd(const char *a1, const char *a2, const char *a3)
{
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        execl(adsbd_bin, "adsbd", "-c", conf_path, a1, a2, a3, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static pid_t start_adsbd(const char *dump1090_cmd)
{
    write_conf(dump1090_cmd);
    pid_t pid = fork();
    if (pid == 0) {
        execl(adsbd_bin, "adsbd", "-c", conf_path, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int stop_adsbd(pid_t pid)
{
    kill(pid, SIGTERM);
    int status = 0;
    pid_t w = 0;
    for (int i = 0; i < 250 && w == 0; i++) {
        w = waitpid(pid, &status, WNOHANG);
        if (w == 0) { pause_ms(20); }
    }
    if (w != pid) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* What the fake ttcd has heard. */
typedef struct {
    int hellos, tracks, stats;
    gama_ipc_hello_t hello;
    gama_ipc_stat_t stat;
    gama_track_t rec[GAMA_IPC_TRACKS_MAX * GAMA_IPC_TRACKS_MAX_FRAMES];
    size_t nrec;                        /* of the latest snapshot */
} heard_t;

static const gama_track_t *heard_track(const heard_t *h, uint32_t icao)
{
    for (size_t i = 0; i < h->nrec; i++) {
        if (h->rec[i].icao == icao) { return &h->rec[i]; }
    }
    return NULL;
}

/* Reads everything adsbd sends for up to `ms`, or until done() holds. */
static void listen_ttcd(int fd, int ms, heard_t *h, bool (*done)(const heard_t *))
{
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end && !(done != NULL && done(h))) {
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
                h->hellos++;
                gama_ipc_hello_decode(f.payload, f.len, &h->hello);
            } else if (f.type == GAMA_FRAME_IPC_STAT) {
                h->stats++;
                gama_ipc_stat_decode(f.payload, f.len, &h->stat);
            } else if (f.type == GAMA_FRAME_IPC_TRACKS) {
                gama_ipc_tracks_hdr_t hd;
                size_t n = 0;
                if (gama_ipc_tracks_decode(f.payload, f.len, &hd, &n) < 0) {
                    continue;
                }
                if (hd.index == 0) {
                    h->nrec = 0;
                }
                for (size_t r = 0; r < n; r++) {
                    gama_track_decode(f.payload + GAMA_IPC_TRACKS_HEADER_LEN + r * GAMA_TRACK_WIRE_LEN,
                                      GAMA_TRACK_WIRE_LEN, &h->rec[h->nrec++]);
                }
                h->tracks++;
            }
        }
    }
}

static bool has_hello(const heard_t *h)   { return h->hellos > 0; }
static bool has_both(const heard_t *h)    { return heard_track(h, 0xE48DF5) && heard_track(h, 0xE49608); }
static bool has_restarts(const heard_t *h) { return h->stat.dump1090_restarts >= 3; }

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

static bool wait_for_log(const char *needle, int ms)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) {
        if (file_contains(log_path, needle)) { return true; }
        pause_ms(20);
    }
    return false;
}

/* The last line of the record, and how many there are. */
static int read_record(char *last, size_t cap, int *on_ground_lines, int *well_formed)
{
    FILE *f = fopen(rec_path, "r");
    int lines = 0;
    *on_ground_lines = 0;
    *well_formed = 0;
    char line[512];
    last[0] = '\0';
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        lines++;
        size_t n = strlen(line);
        *well_formed += strncmp(line, "{\"icao\":\"", 9) == 0 && n > 2 &&
                        line[n - 2] == '}' && line[n - 1] == '\n';
        *on_ground_lines += strstr(line, "\"on_ground\":1}") != NULL;
        snprintf(last, cap, "%s", line);
    }
    if (f != NULL) { fclose(f); }
    return lines;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/adsbd\n", argv[0]);
        return 2;
    }
    adsbd_bin = argv[1];
    signal(SIGPIPE, SIG_IGN);
    snprintf(dir, sizeof(dir), "/tmp/adsbd-it-XXXXXX");
    if (mkdtemp(dir) == NULL) { perror("mkdtemp"); return 1; }
    snprintf(sock_path, sizeof(sock_path), "%s/ttec.sock", dir);
    snprintf(rec_path, sizeof(rec_path), "%s/adsb.ndjson", dir);
    snprintf(log_path, sizeof(log_path), "%s/adsbd.jsonl", dir);
    snprintf(conf_path, sizeof(conf_path), "%s/adsbd.conf", dir);

    int sbs_lfd = sbs_listen();
    int ttcd_lfd = ipc_server_open(sock_path, 4);
    CHECK(ttcd_lfd >= 0);

    TEST_GROUP("integration: the configuration is checked without starting anything");
    {
        write_conf("");
        CHECK_EQ_INT(run_adsbd("--check-config", NULL, NULL), 0);
        CHECK_EQ_INT(run_adsbd("-o", "snapshot_max_aircraft=200", NULL), 2);
        CHECK_EQ_INT(run_adsbd("-o", "dump1090_cmd=dump1090-fa", NULL), 2);   /* no PATH */
    }

    /* dump1090 is played by this test: adsbd starts nothing. */
    pid_t main_pid = start_adsbd("");
    int ipc = -1, sbs = -1;
    heard_t h;
    memset(&h, 0, sizeof(h));

    TEST_GROUP("integration: adsbd reaches ttcd and dump1090, whichever comes first");
    {
        ipc = accept_within(ttcd_lfd, 3000);
        CHECK(ipc >= 0);
        if (ipc >= 0) {
            fcntl(ipc, F_SETFL, O_NONBLOCK);
            listen_ttcd(ipc, 2000, &h, has_hello);
        }
        CHECK_EQ_INT(h.hellos, 1);
        CHECK_EQ_INT(h.hello.role, GAMA_IPC_ROLE_ADSBD);
        CHECK_EQ_INT(h.hello.version, GAMA_IPC_VERSION);
        sbs = accept_within(sbs_lfd, 3000);
        CHECK(sbs >= 0);
    }

    TEST_GROUP("integration: SBS lines become tracks, counters and a record");
    {
        say(sbs, "MSG,3,1,1,E48DF5,1,2026/09/28,12:00:00.000,2026/09/28,12:00:00.000,"
                 ",37000,,,-23.559616,-46.658908,,,0,,0,0\r\n"
                 "MSG,4,1,1,E48DF5,1,2026/09/28,12:00:00.500,2026/09/28,12:00:00.500,"
                 ",,451.7,128.4,,,-1216,,,,,\r\n"
                 "MSG,1,1,1,E48DF5,1,,,,,TAM3054 ,,,,,,,,,,,\r\n"
                 "MSG,3,1,1,~2A3B4C,1,,,,,,5000,,,-15.8,-47.9,,,0,,0,0\r\n"
                 "hello, this is not SBS\r\n"
                 "MSG,2,1,1,E49608,1,,,,,,,12.0,275.3,-15.870000,-47.9");
        pause_ms(60);                                     /* a line split across reads */
        say(sbs, "20000,,,,,,-1\r\n");
        listen_ttcd(ipc, 3000, &h, has_both);
        const gama_track_t *a = heard_track(&h, 0xE48DF5);
        const gama_track_t *b = heard_track(&h, 0xE49608);
        CHECK(a != NULL && b != NULL);
        if (a != NULL && b != NULL) {
            CHECK_EQ_INT(a->flags, GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE |
                                   GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE);
            CHECK_NEAR(a->latitude, -23.559616, 1.0 / GAMA_LAT_SCALE);
            CHECK_EQ_INT(a->altitude_ft, 37000);
            CHECK(a->age_ds < 30);
            CHECK_EQ_INT(b->flags, GAMA_TRACK_F_POSITION | GAMA_TRACK_F_VELOCITY |
                                   GAMA_TRACK_F_ON_GROUND);
            CHECK_NEAR(b->longitude, -47.92, 1.0 / GAMA_LON_SCALE);
        }
        listen_ttcd(ipc, 1200, &h, NULL);                 /* one more IPC_STAT */
        CHECK_EQ_INT(h.stat.msgs_received, 6);
        CHECK_EQ_INT(h.stat.msgs_decoded, 4);
        CHECK_EQ_INT(h.stat.aircraft_tracked, 2);
    }

    TEST_GROUP("integration: the ground's time, through ttcd, stamps what follows");
    {
        uint8_t p[GAMA_IPC_TIME_LEN], f[GAMA_FRAME_MAX_TOTAL];
        gama_ipc_time_t t = { .unix_s = 1800000000u, .nsec = 0u };
        gama_ipc_time_encode(p, sizeof(p), &t);
        int n = gama_frame_encode(f, sizeof(f), GAMA_FRAME_IPC_TIME_SET, 0, p, GAMA_IPC_TIME_LEN);
        CHECK_EQ_INT(ipc_send(ipc, f, (size_t)n), IPC_OK);
        CHECK(wait_for_log("\"event\":\"time_anchor\"", 2000));
        say(sbs, "MSG,3,1,1,E48DF5,1,,,,,,36000,,,-23.55,-46.65,,,0,,0,0\n");
        pause_ms(300);
        char last[512];
        int on_ground = 0, well = 0;
        int lines = read_record(last, sizeof(last), &on_ground, &well);
        CHECK_EQ_INT(lines, 5);                           /* 4 accepted + 1 after */
        CHECK_EQ_INT(well, lines);
        CHECK_EQ_INT(on_ground, 1);                       /* E49608, from SBS -1 */
        unsigned long long stamp = 0;
        const char *s = strstr(last, "\"rx_epoch_ns\":");
        if (s != NULL) { stamp = strtoull(s + 14, NULL, 10); }
        CHECK(stamp >= 1800000000000000000ull && stamp < 1800000005000000000ull);
    }

    TEST_GROUP("integration: ttcd restarting is survived: adsbd reconnects and says hello");
    {
        close(ipc);
        memset(&h, 0, sizeof(h));
        ipc = accept_within(ttcd_lfd, 3000);
        CHECK(ipc >= 0);
        if (ipc >= 0) {
            fcntl(ipc, F_SETFL, O_NONBLOCK);
            listen_ttcd(ipc, 2000, &h, has_hello);
        }
        CHECK_EQ_INT(h.hellos, 1);
    }

    TEST_GROUP("integration: dump1090 restarting is survived: adsbd connects again");
    {
        close(sbs);
        sbs = accept_within(sbs_lfd, 3000);
        CHECK(sbs >= 0);
        CHECK(wait_for_log("\"event\":\"sbs_down\"", 1000));
    }

    TEST_GROUP("integration: SIGTERM stops adsbd cleanly, the story in its log");
    {
        CHECK_EQ_INT(stop_adsbd(main_pid), 0);
        CHECK(file_contains(log_path, "\"event\":\"start\""));
        CHECK(file_contains(log_path, "\"event\":\"sbs_up\""));
        CHECK(file_contains(log_path, "\"event\":\"ipc_up\""));
        CHECK(file_contains(log_path, "\"event\":\"ipc_down\""));
        CHECK(file_contains(log_path, "\"event\":\"sbs_reject\",\"why\":\"ERR_FORMAT\""));
        CHECK(file_contains(log_path, "\"event\":\"stop\""));
        CHECK(file_contains(log_path, "\"event\":\"exit\""));
        close(sbs);
        close(ipc);
    }

    TEST_GROUP("integration: a dump1090 that keeps dying is restarted, and counted");
    {
        pid_t pid = start_adsbd("/bin/sh -c exit");
        memset(&h, 0, sizeof(h));
        ipc = accept_within(ttcd_lfd, 3000);
        CHECK(ipc >= 0);
        if (ipc >= 0) {
            fcntl(ipc, F_SETFL, O_NONBLOCK);
            listen_ttcd(ipc, 4000, &h, has_restarts);
        }
        CHECK(h.stat.dump1090_restarts >= 3);
        CHECK(file_contains(log_path, "\"event\":\"dump1090_restart_in\""));
        CHECK_EQ_INT(stop_adsbd(pid), 0);
        close(ipc);
    }

    TEST_GROUP("integration: stopping adsbd stops its dump1090 too");
    {
        unlink(log_path);
        pid_t pid = start_adsbd("/bin/sleep 30");
        CHECK(wait_for_log("\"event\":\"dump1090_start\"", 3000));
        int dpid = -1;
        FILE *f = fopen(log_path, "r");
        char line[512];
        const char *key = "\"event\":\"dump1090_start\",\"pid\":";
        while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
            const char *s = strstr(line, key);
            if (s != NULL) { dpid = atoi(s + strlen(key)); }
        }
        if (f != NULL) { fclose(f); }
        CHECK(dpid > 0);
        CHECK(dpid > 0 && kill(dpid, 0) == 0);            /* running */
        CHECK_EQ_INT(stop_adsbd(pid), 0);
        CHECK(dpid > 0 && kill(dpid, 0) != 0 && errno == ESRCH);   /* and gone */
        int ipc2 = accept_within(ttcd_lfd, 10);           /* drain its connection */
        if (ipc2 >= 0) { close(ipc2); }
    }

    close(sbs_lfd);
    close(ttcd_lfd);
    unlink(sock_path);
    unlink(rec_path);
    unlink(log_path);
    unlink(conf_path);
    rmdir(dir);
    TEST_SUMMARY("test_adsbd_integration");
}
