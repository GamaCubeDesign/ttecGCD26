/*
 * tc_dispatch.c — telecommand validation, execution and acknowledgement
 * (HLR-COMM-01).
 *
 * Every telecommand gets exactly one TC_ACK with a status, on every path:
 * unknown command, wrong argument size, out-of-range argument, rejected in
 * the current state, or failed while executing. The ground never has to guess
 * from silence.
 *
 * At-most-once execution. The ground retransmits a telecommand whose ACK it
 * did not receive, with the same sequence number and payload. The satellite
 * remembers the last one it executed and answers a repeat by replaying the
 * ACK without executing again — otherwise a lost ACK would send a mode event
 * to the OBC twice, or shut down twice.
 *
 * The exception is SET_RATE, which is always executed. It is idempotent by
 * construction ("be at profile p"), and it must be: if the satellite switched
 * and then reverted because the ground never heard the ACK, the ground's
 * retransmission is the only thing that can bring the two ends back
 * together. Suppressing it as a duplicate would leave them on different
 * profiles for good (ADR-0007).
 *
 * No handler blocks. The previous mission slept for ten seconds inside its
 * handlers (ultima_missao/satellite/Module.cpp:139); here a handler returns a
 * status and, at most, a post-action that runs when the ACK has left.
 */

#include "core_internal.h"
#include "gama_bytes.h"
#include "gama_lora.h"
#include "gama_tc.h"

#include <inttypes.h>
#include <string.h>

bool ack_pending(const ttcd_core_t *c)
{
    return c->ack_count > 0;
}

const ttcd_ack_t *ack_peek(const ttcd_core_t *c)
{
    return &c->acks[c->ack_head];
}

void ack_pop(ttcd_core_t *c, ttcd_ack_t *out)
{
    *out = c->acks[c->ack_head];
    c->ack_head = (uint8_t)((c->ack_head + 1u) % TTCD_ACK_QUEUE);
    c->ack_count--;
}

static void queue_ack(ttcd_core_t *c, uint64_t now, uint8_t cmd, uint8_t status,
                      uint16_t echo_seq, ttcd_post_t post, int16_t post_arg)
{
    if (c->ack_count == TTCD_ACK_QUEUE) {
        /* Eight unacknowledged telecommands means the ground is not
         * listening; the newest wins, the oldest is logged and dropped. */
        ttcd_ack_t dropped;
        ack_pop(c, &dropped);
        core_log(c, now, "ack_overflow", "\"dropped_cmd\":\"%s\"", gama_tc_name(dropped.cmd));
    }
    uint8_t tail = (uint8_t)((c->ack_head + c->ack_count) % TTCD_ACK_QUEUE);
    c->acks[tail] = (ttcd_ack_t){
        .cmd = cmd, .status = status, .echo_seq = echo_seq,
        .post = post, .post_arg = post_arg,
    };
    c->ack_count++;
}

/* Adds whole milliseconds to a (seconds, nanoseconds) wall-clock time. */
static void time_add_ms(gama_ipc_time_t *t, uint64_t ms)
{
    uint64_t ns = (uint64_t)t->nsec + (ms % 1000u) * 1000000u;
    t->unix_s += (uint32_t)(ms / 1000u + ns / 1000000000u);
    t->nsec = (uint32_t)(ns % 1000000000u);
}

typedef struct {
    uint8_t     status;
    ttcd_post_t post;
    int16_t     post_arg;
    const char *detail;   /* why, for the log */
} tc_result_t;

static tc_result_t ok(void)                 { return (tc_result_t){ GAMA_ACK_OK, TTCD_POST_NONE, 0, "" }; }
static tc_result_t fail(uint8_t s, const char *why) { return (tc_result_t){ s, TTCD_POST_NONE, 0, why }; }

static tc_result_t do_set_time(ttcd_core_t *c, uint64_t now, const gama_frame_t *f,
                               const uint8_t *args)
{
    gama_ipc_time_t t;
    gama_ipc_time_decode(args, GAMA_IPC_TIME_LEN, &t);
    if (t.nsec >= 1000000000u) {
        return fail(GAMA_ACK_BAD_ARGS, "nsec out of range");
    }
    /* The ground stamped the time when it started transmitting. This frame
     * then spent its time on air reaching us, so that instant was earlier than
     * `now` by the frame's time on air. */
    uint32_t toa_us = gama_lora_toa_us(gama_lora_profile(c->profile),
                                       (uint8_t)(GAMA_FRAME_OVERHEAD + f->len));
    uint64_t toa_ms = ((uint64_t)toa_us + 500u) / 1000u;
    c->time_anchor_mono = now > toa_ms ? now - toa_ms : 0;
    c->time_anchor_unix_s = t.unix_s;
    c->time_anchor_nsec = t.nsec;
    c->time_synced = true;
    core_log(c, now, "time_anchor",
             "\"mono_ms\":%" PRIu64 ",\"unix_s\":%" PRIu32 ",\"nsec\":%" PRIu32,
             c->time_anchor_mono, t.unix_s, t.nsec);

    /* Forward the time as it is now, not as it was at the anchor: the
     * receivers pair it with their own clock on arrival. */
    gama_ipc_time_t current = t;
    time_add_ms(&current, now - c->time_anchor_mono);
    uint8_t payload[GAMA_IPC_TIME_LEN];
    gama_ipc_time_encode(payload, sizeof(payload), &current);
    if (c->obc_linked) {
        core_ipc_send(c, now, GAMA_IPC_ROLE_OBC, GAMA_FRAME_IPC_TIME_SET,
                      payload, GAMA_IPC_TIME_LEN);
    }
    if (c->adsbd_linked) {
        core_ipc_send(c, now, GAMA_IPC_ROLE_ADSBD, GAMA_FRAME_IPC_TIME_SET,
                      payload, GAMA_IPC_TIME_LEN);
    }
    return ok();
}

static tc_result_t do_set_mode(ttcd_core_t *c, uint64_t now, const uint8_t *args)
{
    uint8_t ev = args[0];
    if (ev == GAMA_OBC_EV_NONE || ev > GAMA_OBC_EV_ADSB_TIMEOUT) {
        return fail(GAMA_ACK_BAD_ARGS, "event out of range");
    }
    if (!c->obc_linked) {
        return fail(GAMA_ACK_FAILED, "obc not connected");
    }
    if (!core_ipc_send(c, now, GAMA_IPC_ROLE_OBC, GAMA_FRAME_IPC_TC_EVENT,
                       &ev, GAMA_IPC_TC_EVENT_LEN)) {
        return fail(GAMA_ACK_FAILED, "ipc send failed");
    }
    return ok();
}

static tc_result_t do_set_rate(ttcd_core_t *c, const uint8_t *args)
{
    uint8_t p = args[0];
    if (gama_lora_profile(p) == NULL) {
        return fail(GAMA_ACK_BAD_ARGS, "no such profile");
    }
    if (p == c->profile) {
        return ok();       /* already there; also the path of a replayed retry */
    }
    /* Acknowledge at the old profile; switch when the ACK has left. */
    return (tc_result_t){ GAMA_ACK_OK, TTCD_POST_SET_PROFILE, p, "" };
}

static tc_result_t do_set_tx_power(const uint8_t *args)
{
    int8_t dbm = (int8_t)args[0];
    if (dbm < 2 || dbm > 20) {
        return fail(GAMA_ACK_BAD_ARGS, "power out of range");
    }
    return (tc_result_t){ GAMA_ACK_OK, TTCD_POST_SET_POWER, dbm, "" };
}

static tc_result_t do_stream_start(ttcd_core_t *c, uint64_t now, const uint8_t *args)
{
    uint16_t period_ds = gama_get_u16(args);
    /* Below 3 s a 20-aircraft snapshot (2.1 s of air time at NOMINAL) would
     * leave no room for anything else; above a minute it stops being a
     * trajectory. */
    if (period_ds < 30u || period_ds > 600u) {
        return fail(GAMA_ACK_BAD_ARGS, "period out of range");
    }
    c->stream_on = true;
    c->stream_period = (uint32_t)period_ds * 100u;
    c->next_tracks = now;
    c->next_stat = now + c->p.stat_period;
    core_log(c, now, "stream", "\"on\":true,\"period_ms\":%" PRIu32, c->stream_period);
    return ok();
}

static tc_result_t dispatch(ttcd_core_t *c, uint64_t now, const gama_frame_t *f,
                            uint8_t cmd, const uint8_t *args)
{
    switch (cmd) {
    case GAMA_TC_PING:
        return ok();
    case GAMA_TC_SET_TIME:
        return do_set_time(c, now, f, args);
    case GAMA_TC_SET_MODE:
        return do_set_mode(c, now, args);
    case GAMA_TC_SET_RATE:
        return do_set_rate(c, args);
    case GAMA_TC_SET_TX_POWER:
        return do_set_tx_power(args);
    case GAMA_TC_STREAM_START:
        return do_stream_start(c, now, args);
    case GAMA_TC_STREAM_STOP:
        c->stream_on = false;
        c->sending.valid = false;
        core_log(c, now, "stream", "%s", "\"on\":false");
        return ok();
    case GAMA_TC_REQ_HK:
        c->hk_requested = true;
        return ok();
    case GAMA_TC_REQ_STAT:
        c->stat_requested = true;
        return ok();
    case GAMA_TC_REQ_ROSTER:
        /* Needs the callsign table from adsbd (PLANO phase 3). */
        return fail(GAMA_ACK_FAILED, "roster not implemented");
    case GAMA_TC_BULK_START:
    case GAMA_TC_BULK_ABORT:
        /* Deferred: the full raw log would take hours even at FAST, and it is
         * retrieved over USB after the mission instead (ADR-0003, ADR-0007). */
        return fail(GAMA_ACK_FAILED, "bulk transfer not implemented");
    case GAMA_TC_SHUTDOWN:
        if (gama_get_u16(args) != GAMA_TC_SHUTDOWN_MAGIC) {
            return fail(GAMA_ACK_BAD_ARGS, "bad magic");
        }
        return (tc_result_t){ GAMA_ACK_OK, TTCD_POST_EXIT, 0, "" };
    default:
        return fail(GAMA_ACK_UNKNOWN_CMD, "unknown command");
    }
}

void tc_handle(ttcd_core_t *c, uint64_t now, const gama_frame_t *f)
{
    if (f->len < 1) {
        /* Not even a command byte: nothing to acknowledge against. */
        c->n.frames_rx_bad++;
        core_log(c, now, "tc_empty", "\"seq\":%u", f->seq);
        return;
    }
    uint8_t cmd = f->payload[0];
    const uint8_t *args = f->payload + 1;
    size_t arg_len = (size_t)f->len - 1u;

    bool repeat = c->last_tc.valid && f->seq == c->last_tc.seq &&
                  f->len == c->last_tc.len &&
                  memcmp(f->payload, c->last_tc.payload, f->len) == 0 &&
                  now - c->last_tc.at <= c->p.dup_window;
    if (repeat && cmd != GAMA_TC_SET_RATE) {
        c->n.tc_duplicates++;
        core_log(c, now, "tc_duplicate", "\"cmd\":\"%s\",\"seq\":%u,\"status\":\"%s\"",
                 gama_tc_name(cmd), f->seq, gama_ack_status_name(c->last_tc.status));
        queue_ack(c, now, cmd, c->last_tc.status, f->seq, TTCD_POST_NONE, 0);
        return;
    }

    tc_result_t r;
    int want = gama_tc_arg_len(cmd);
    if (want < 0) {
        r = fail(GAMA_ACK_UNKNOWN_CMD, "unknown command");
    } else if ((size_t)want != arg_len) {
        r = fail(GAMA_ACK_BAD_ARGS, "wrong argument length");
    } else {
        r = dispatch(c, now, f, cmd, args);
    }

    c->n.tc_executed++;
    c->last_tc.valid = f->len <= sizeof(c->last_tc.payload);
    c->last_tc.seq = f->seq;
    c->last_tc.len = f->len;
    if (c->last_tc.valid) {
        memcpy(c->last_tc.payload, f->payload, f->len);
    }
    c->last_tc.status = r.status;
    c->last_tc.at = now;

    core_log(c, now, "tc", "\"cmd\":\"%s\",\"seq\":%u,\"status\":\"%s\",\"detail\":\"%s\"",
             gama_tc_name(cmd), f->seq, gama_ack_status_name(r.status), r.detail);
    queue_ack(c, now, cmd, r.status, f->seq, r.post, r.post_arg);
}
