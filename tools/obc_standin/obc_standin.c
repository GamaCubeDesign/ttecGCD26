/*
 * obc_standin — a stand-in for the OBC, until flight/obcd/ exists (PLANO
 * etapa 2). Not flight software.
 *
 * It speaks the OBC's side of the IPC contract (common/gama_ipc.h) to ttcd,
 * so that the mode-driven behaviour of ttcd and adsbd (ADR-0014) can be run
 * end to end without the OBC: a SET_MODE from the ground reaches it as an
 * IPC_TC_EVENT, it changes mode and reports it with IPC_MODE, and ttcd and
 * adsbd follow.
 *
 * It follows every event at once, with none of the OBC's checks: the
 * transitions of the CDR's Figure 44, and the rule that every mode is
 * reached from PRE_TEST (PLANO D1), belong to obcd. What it does keep:
 *   - EV_TASK_DONE returns to PRE_TEST (the end of MISSION_DOWNLINK, D2);
 *   - MISSION_ADSB ends by itself after --adsb-timeout seconds, 600 by
 *     default, as the OBC's own EV_ADSB_TIMEOUT;
 *   - IPC_TELEMETRY every second (D6), every field "no data": a stand-in
 *     has no sensors, and must not report readings it does not have.
 *
 *   obc_standin --ipc /run/gama/ttec.sock [--adsb-timeout S]
 *
 * One JSON object per line on standard output. Reconnects every second
 * while ttcd is away, starting again in PRE_TEST (PLANO D5).
 */

#define _GNU_SOURCE
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_tc.h"
#include "gama_tm.h"
#include "ipc.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TELEMETRY_MS 1000u
#define RETRY_MS     1000u

static struct {
    const char *ipc_path;
    uint64_t adsb_timeout_ms;   /* 0: never */
    int      fd;
    uint16_t seq;
    uint8_t  mode;
    uint64_t mode_since;
    uint64_t next_telemetry;
    uint64_t retry_at;
} S;

static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void say(uint64_t now, const char *event, const char *fields)
{
    printf("{\"mono_ms\":%" PRIu64 ",\"event\":\"%s\"%s%s}\n",
           now, event, fields[0] != '\0' ? "," : "", fields);
    fflush(stdout);
}

static bool send_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t f[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(f, sizeof(f), type, S.seq++, payload, len);
    return n > 0 && ipc_send(S.fd, f, (size_t)n) == IPC_OK;
}

static void set_mode(uint64_t now, uint8_t mode, const char *by)
{
    char fields[160];
    snprintf(fields, sizeof(fields), "\"from\":\"%s\",\"to\":\"%s\",\"by\":\"%s\"",
             gama_obc_state_name(S.mode), gama_obc_state_name(mode), by);
    say(now, "mode", fields);
    S.mode = mode;
    S.mode_since = now;
    send_frame(GAMA_FRAME_IPC_MODE, &S.mode, GAMA_IPC_MODE_LEN);
}

/* Every event straight to its mode; see the header for what is left out. */
static uint8_t mode_for(uint8_t ev)
{
    switch (ev) {
    case GAMA_OBC_EV_TC_BASIC_INTER:  return GAMA_OBC_ST_BASIC_INTERMEDIATE;
    case GAMA_OBC_EV_TC_AOCS:         return GAMA_OBC_ST_ADVANCED_AOCS;
    case GAMA_OBC_EV_TC_MISSION_ADSB: return GAMA_OBC_ST_MISSION_ADSB;
    case GAMA_OBC_EV_TC_DOWNLINK:     return GAMA_OBC_ST_MISSION_DOWNLINK;
    case GAMA_OBC_EV_ADSB_TIMEOUT:    return GAMA_OBC_ST_MISSION_DOWNLINK;
    case GAMA_OBC_EV_TC_SURVIVAL:     return GAMA_OBC_ST_ENV_SURVIVAL;
    case GAMA_OBC_EV_TASK_DONE:       return GAMA_OBC_ST_PRE_TEST;
    default:                          return S.mode;
    }
}

static void send_telemetry(void)
{
    gama_ipc_telemetry_t t = {
        .battery_mv = GAMA_NO_DATA_U16, .current_ma = GAMA_NO_DATA_I16,
        .temp_bat_ccel = GAMA_NO_DATA_I16, .temp_ext_ccel = GAMA_NO_DATA_I16,
        .roll_cdeg = GAMA_NO_DATA_I16, .pitch_cdeg = GAMA_NO_DATA_I16,
        .yaw_cdeg = GAMA_NO_DATA_I16,
    };
    uint8_t p[GAMA_IPC_TELEMETRY_LEN];
    gama_ipc_telemetry_encode(p, sizeof(p), &t);
    send_frame(GAMA_FRAME_IPC_TELEMETRY, p, GAMA_IPC_TELEMETRY_LEN);
}

static void disconnect(uint64_t now, const char *why)
{
    char fields[96];
    snprintf(fields, sizeof(fields), "\"why\":\"%s\"", why);
    say(now, "ipc_down", fields);
    close(S.fd);
    S.fd = -1;
    S.retry_at = now + RETRY_MS;
}

static void try_connect(uint64_t now)
{
    S.fd = ipc_client_connect(S.ipc_path);
    if (S.fd < 0) {
        S.retry_at = now + RETRY_MS;
        return;
    }
    gama_ipc_hello_t h = { .role = GAMA_IPC_ROLE_OBC, .version = GAMA_IPC_VERSION };
    uint8_t p[GAMA_IPC_HELLO_LEN];
    gama_ipc_hello_encode(p, sizeof(p), &h);
    if (!send_frame(GAMA_FRAME_IPC_HELLO, p, GAMA_IPC_HELLO_LEN)) {
        disconnect(now, "hello");
        return;
    }
    say(now, "ipc_up", "");
    S.mode = GAMA_OBC_ST_UNKNOWN;
    set_mode(now, GAMA_OBC_ST_PRE_TEST, "connect");      /* PLANO D5 */
    send_telemetry();
    S.next_telemetry = now + TELEMETRY_MS;
}

static void read_ttcd(uint64_t now)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL + 1];
    size_t len;
    for (;;) {
        ipc_result_t r = ipc_recv(S.fd, buf, sizeof(buf), &len);
        if (r == IPC_EMPTY) {
            return;
        }
        if (r != IPC_OK && r != IPC_TRUNCATED) {
            disconnect(now, ipc_result_name(r));
            return;
        }
        gama_frame_t f;
        if (r != IPC_OK || gama_frame_decode(buf, len, &f) < 0) {
            say(now, "ipc_bad", "");
            continue;
        }
        if (f.type == GAMA_FRAME_IPC_TC_EVENT && f.len == GAMA_IPC_TC_EVENT_LEN) {
            char fields[48];
            snprintf(fields, sizeof(fields), "\"event_id\":%u", f.payload[0]);
            say(now, "tc_event", fields);
            uint8_t to = mode_for(f.payload[0]);
            if (to != S.mode) {
                set_mode(now, to, "event");
            }
        } else if (f.type == GAMA_FRAME_IPC_TIME_SET) {
            say(now, "time_set", "");
        } else {
            char fields[64];
            snprintf(fields, sizeof(fields), "\"type\":\"%s\"", gama_frame_type_name(f.type));
            say(now, "ipc_unexpected", fields);
        }
    }
}

int main(int argc, char **argv)
{
    S.fd = -1;
    S.adsb_timeout_ms = 600000u;
    S.mode = GAMA_OBC_ST_UNKNOWN;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ipc") == 0 && i + 1 < argc) {
            S.ipc_path = argv[++i];
        } else if (strcmp(argv[i], "--adsb-timeout") == 0 && i + 1 < argc) {
            S.adsb_timeout_ms = (uint64_t)strtoul(argv[++i], NULL, 10) * 1000u;
        } else {
            S.ipc_path = NULL;
            break;
        }
    }
    if (S.ipc_path == NULL) {
        fprintf(stderr, "usage: obc_standin --ipc PATH [--adsb-timeout SECONDS]\n");
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    uint64_t now = now_ms();
    say(now, "start", "");
    while (!stop_requested) {
        now = now_ms();
        if (S.fd < 0) {
            if (now >= S.retry_at) {
                try_connect(now);
            }
        } else {
            if (now >= S.next_telemetry) {
                send_telemetry();
                S.next_telemetry += TELEMETRY_MS;
                if (S.next_telemetry <= now) {
                    S.next_telemetry = now + TELEMETRY_MS;
                }
            }
            if (S.mode == GAMA_OBC_ST_MISSION_ADSB && S.adsb_timeout_ms != 0 &&
                now - S.mode_since >= S.adsb_timeout_ms) {
                set_mode(now, mode_for(GAMA_OBC_EV_ADSB_TIMEOUT), "adsb_timeout");
            }
        }
        /* The only wait: for ttcd, or for the next telemetry, at most 100 ms. */
        struct pollfd p = { .fd = S.fd, .events = POLLIN };
        int n = poll(&p, S.fd >= 0 ? 1u : 0u, 100);
        if (n > 0 && S.fd >= 0) {
            read_ttcd(now_ms());
        } else if (n < 0 && errno != EINTR) {
            break;
        }
    }
    say(now_ms(), "exit", "");
    if (S.fd >= 0) {
        close(S.fd);
    }
    return 0;
}
