/*
 * ttcd_harness.h — a simulated shell for the ttcd core.
 *
 * Plays the part of main.c with a virtual clock: a fake radio that reports
 * the end of each transmission after the time on air the profile table
 * predicts, fake IPC peers that record what they receive, and a log that
 * records event names. Tests advance time with step_to(), which delivers
 * every tick and every end of transmission in order, exactly as the epoll
 * shell would.
 */

#ifndef TTCD_HARNESS_H
#define TTCD_HARNESS_H

#include "core.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_lora.h"
#include "gama_tc.h"

#include <string.h>

#define H_MAX_TX  512
#define H_MAX_IPC 64
#define H_MAX_LOG 2048

typedef struct {
    uint64_t now;

    /* radio */
    uint8_t  profile;
    int8_t   power;
    int      busy_next;             /* upcoming radio_tx calls that report BUSY */
    bool     never_finish;          /* swallow the end of the next transmission */
    bool     tx_pending;
    uint64_t tx_done_at;
    int      n_tx;
    uint8_t  tx[H_MAX_TX][GAMA_FRAME_MAX_TOTAL];
    size_t   tx_len[H_MAX_TX];
    uint8_t  tx_profile[H_MAX_TX];
    uint64_t tx_at[H_MAX_TX];

    /* ipc */
    bool     ipc_fail;
    int      n_ipc;
    uint8_t  ipc_role[H_MAX_IPC];
    uint8_t  ipc[H_MAX_IPC][GAMA_FRAME_MAX_TOTAL];
    size_t   ipc_len[H_MAX_IPC];

    /* log */
    int      n_log;
    char     log[H_MAX_LOG][40];

    int      exit_code;
    int16_t  soc_ccel;
} harness_t;

static harness_t H;

static int h_radio_tx(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (H.busy_next > 0) {
        H.busy_next--;
        return TTCD_TX_BUSY;
    }
    if (H.n_tx < H_MAX_TX) {
        memcpy(H.tx[H.n_tx], frame, len);
        H.tx_len[H.n_tx] = len;
        H.tx_profile[H.n_tx] = H.profile;
        H.tx_at[H.n_tx] = H.now;
    }
    H.n_tx++;
    if (H.never_finish) {
        H.never_finish = false;
        H.tx_pending = false;
        return TTCD_TX_STARTED;
    }
    H.tx_pending = true;
    uint32_t us = gama_lora_toa_us(gama_lora_profile(H.profile), (uint8_t)len);
    H.tx_done_at = H.now + (us + 999u) / 1000u;
    return TTCD_TX_STARTED;
}

static int h_set_profile(void *ctx, uint8_t p) { (void)ctx; H.profile = p; return 0; }
static int h_set_power(void *ctx, int8_t dbm)  { (void)ctx; H.power = dbm; return 0; }

static int h_ipc_send(void *ctx, uint8_t role, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (H.ipc_fail) {
        return -1;
    }
    if (H.n_ipc < H_MAX_IPC) {
        H.ipc_role[H.n_ipc] = role;
        memcpy(H.ipc[H.n_ipc], frame, len);
        H.ipc_len[H.n_ipc] = len;
    }
    H.n_ipc++;
    return 0;
}

static void h_log(void *ctx, uint64_t now, const char *event, const char *fields)
{
    (void)ctx; (void)now; (void)fields;
    if (H.n_log < H_MAX_LOG) {
        strncpy(H.log[H.n_log], event, sizeof(H.log[0]) - 1);
    }
    H.n_log++;
}

static int  h_soc(void *ctx, int16_t *c) { (void)ctx; *c = H.soc_ccel; return 0; }
static void h_exit(void *ctx, int code)  { (void)ctx; H.exit_code = code; }

static const ttcd_ops_t H_OPS = {
    .ctx = NULL, .radio_tx = h_radio_tx, .radio_set_profile = h_set_profile,
    .radio_set_power = h_set_power, .ipc_send = h_ipc_send, .log = h_log,
    .soc_temp = h_soc, .exit_request = h_exit,
};

static void h_reset(ttcd_core_t *c, const ttcd_params_t *p)
{
    memset(&H, 0, sizeof(H));
    H.now = 1000000;               /* anything but zero */
    H.soc_ccel = 4200;
    ttcd_init(c, p, &H_OPS, H.now);
}

/* Advances the virtual clock to `until`, delivering every end of
 * transmission and every core deadline on the way, in time order. Returns
 * false if the core stopped making progress (a scheduler bug). */
static bool step_to(ttcd_core_t *c, uint64_t until)
{
    for (int guard = 0; guard < 200000; guard++) {
        uint64_t done = H.tx_pending ? H.tx_done_at : UINT64_MAX;
        uint64_t dl = ttcd_next_deadline(c);
        uint64_t t = done < dl ? done : dl;
        if (t > until) {
            H.now = until;
            return true;
        }
        if (t > H.now) {
            H.now = t;
        }
        if (t == done) {
            H.tx_pending = false;
            ttcd_on_radio_tx_done(c, H.now);
        } else {
            uint64_t before = ttcd_next_deadline(c);
            int tx_before = H.n_tx;
            ttcd_on_tick(c, H.now);
            /* A tick that neither transmitted nor moved its own deadline
             * would spin here forever. */
            if (H.n_tx == tx_before && ttcd_next_deadline(c) == before &&
                !H.tx_pending && before <= H.now) {
                return false;
            }
        }
    }
    return false;
}

/* Delivers a telecommand as if the ground had just transmitted it. */
static void h_send_tc(ttcd_core_t *c, uint16_t seq, uint8_t cmd,
                      const uint8_t *args, uint8_t arg_len)
{
    uint8_t payload[16], frame[GAMA_FRAME_MAX_TOTAL];
    payload[0] = cmd;
    if (arg_len > 0) {
        memcpy(payload + 1, args, arg_len);
    }
    int n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TC, seq,
                              payload, (uint8_t)(1u + arg_len));
    ttcd_on_radio_rx(c, H.now, frame, (size_t)n, -40, 9);
}

static uint8_t tx_type(int i)  { return H.tx[i][1]; }
static int     last_tx(void)   { return H.n_tx - 1; }

/* Index of the first transmission at or after `from` with this type, or -1. */
static int find_tx(int from, uint8_t type)
{
    for (int i = from; i < H.n_tx && i < H_MAX_TX; i++) {
        if (tx_type(i) == type) {
            return i;
        }
    }
    return -1;
}

static void ack_of(int i, uint8_t *cmd, uint8_t *status, uint16_t *echo)
{
    gama_frame_t f;
    gama_frame_decode(H.tx[i], H.tx_len[i], &f);
    *cmd = f.payload[0];
    *status = f.payload[1];
    *echo = gama_get_u16(f.payload + 2);
}

static int count_log(const char *event)
{
    int n = 0;
    for (int i = 0; i < H.n_log && i < H_MAX_LOG; i++) {
        if (strcmp(H.log[i], event) == 0) {
            n++;
        }
    }
    return n;
}

#endif /* TTCD_HARNESS_H */
