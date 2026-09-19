#include "gama_gs_link.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_lora.h"
#include "gama_tc.h"

#include <string.h>

/* GS_SETTLE_MS: the satellite needs a few SPI register writes to reconfigure
 * its modem after acknowledging a profile change; the ground waits that long
 * before transmitting at the new profile.
 *
 * GS_BACKOFF_*: 250 ms, doubling, capped at 2 s. The cap is short on purpose:
 * the usual reason an ACK stops coming is that the satellite switched profile
 * and is about to revert, and the ground should be quick to be heard when it
 * does. (Both in gama_gs_link.h.) */

void gs_params_default(gs_params_t *p)
{
    p->initial_profile = GAMA_RATE_NOMINAL;
    p->keepalive       = 30000;
    p->contact_timeout = 150000;   /* longer than the satellite's 120 s */
    p->rate_revert     = 30000;    /* longer than the satellite's 20 s  */
    p->give_up         = 60000;
}

static uint64_t ms_ceil(uint32_t us)
{
    return ((uint64_t)us + 999u) / 1000u;
}

uint32_t gs_ack_timeout_ms(uint8_t profile)
{
    const gama_lora_phy_t *phy = gama_lora_profile(profile);
    if (phy == NULL) {
        return 1000u;
    }
    /* The satellite answers as soon as the telecommand ends: the ACK's own
     * time on air, plus a header time and 100 ms of processing margin. */
    uint32_t us = gama_lora_toa_us(phy, (uint8_t)(GAMA_FRAME_OVERHEAD + GAMA_TC_ACK_WIRE_LEN))
                  + gama_lora_header_us(phy);
    return (uint32_t)ms_ceil(us) + 100u;
}

static void emit(gs_link_t *g, uint64_t now, const gs_event_t *ev)
{
    if (g->ops.event != NULL) {
        g->ops.event(g->ops.ctx, now, ev);
    }
}

static void set_profile(gs_link_t *g, uint64_t now, uint8_t profile, const char *reason)
{
    uint8_t from = g->profile;
    if (from == profile || g->ops.radio_set_profile(g->ops.ctx, profile) != 0) {
        return;
    }
    g->profile = profile;
    gs_event_t ev = { .kind = GS_EV_RATE, .from = from, .to = profile, .reason = reason };
    emit(g, now, &ev);
}

/* ---- telecommand queue (ring buffer, head is the one in flight) ---- */

static gs_tc_t *head(gs_link_t *g)
{
    return g->q_count > 0 ? &g->q[g->q_head] : NULL;
}

static void pop(gs_link_t *g)
{
    g->q_head = (uint8_t)((g->q_head + 1u) % GS_TC_QUEUE);
    g->q_count--;
}

/* Inserts at logical position `pos` (0 = head), shifting the rest back. */
static uint32_t insert(gs_link_t *g, uint64_t now, uint8_t pos, uint8_t cmd,
                       const uint8_t *args, uint8_t arg_len, bool internal)
{
    if (g->q_count == GS_TC_QUEUE) {
        return 0;
    }
    if (pos > g->q_count) {
        pos = g->q_count;
    }
    for (uint8_t i = g->q_count; i > pos; i--) {
        g->q[(g->q_head + i) % GS_TC_QUEUE] = g->q[(g->q_head + i - 1u) % GS_TC_QUEUE];
    }
    gs_tc_t *t = &g->q[(g->q_head + pos) % GS_TC_QUEUE];
    memset(t, 0, sizeof(*t));
    t->payload[0] = cmd;
    if (arg_len > 0) {
        memcpy(t->payload + 1, args, arg_len);
    }
    t->len = (uint8_t)(1u + arg_len);
    t->seq = g->next_seq++;
    t->submitted = now;
    t->next_try = now;
    t->not_before = now;
    t->id = internal ? 0u : g->next_id++;
    g->q_count++;
    return internal ? 1u : t->id;
}

/* Position right after whatever is in flight: an internal telecommand must
 * not displace the one awaiting its acknowledgement. */
static uint8_t next_free_slot(const gs_link_t *g)
{
    return (g->awaiting_ack || g->tx_busy) ? 1u : 0u;
}

static uint64_t lbt_backoff_ms(const gs_link_t *g)
{
    uint64_t t = ms_ceil(gama_lora_header_us(gama_lora_profile(g->profile)));
    return t < 10u ? 10u : t;
}

/*
 * Medium access (ADR-0007). A telecommand the operator is waiting for goes
 * out as soon as the modem is not receiving: listen-before-talk plus
 * retransmission covers the rare collision. A keepalive nobody is waiting for
 * goes only in the gap the satellite leaves right after each of its frames,
 * which is guaranteed silent — it is the most frequent telecommand, and it
 * should never cost a telemetry frame.
 */
static void try_transmit_at(gs_link_t *g, uint64_t now, bool in_gap)
{
    gs_tc_t *t = head(g);
    if (g->tx_busy || g->awaiting_ack || t == NULL || now < t->not_before) {
        return;
    }
    /* In the reply gap the channel is guaranteed free, so any backoff is moot:
     * go now. Waiting out a listen-before-talk backoff that expires a few tens
     * of milliseconds into the gap is exactly how a telecommand ends up
     * starting too late for the satellite to decode its header before the gap
     * closes — and then both frames are lost. */
    if (!in_gap && (t->gap_only || now < t->next_try)) {
        return;
    }
    /* SET_TIME is stamped at transmission, on every attempt. The new payload
     * also makes a retry distinct from the original, so the satellite applies
     * the fresh time instead of replaying an ACK for the stale one. */
    if (t->payload[0] == GAMA_TC_SET_TIME && g->ops.wall_time != NULL) {
        gama_ipc_time_t w;
        if (g->ops.wall_time(g->ops.ctx, &w.unix_s, &w.nsec) == 0) {
            gama_ipc_time_encode(t->payload + 1, GAMA_IPC_TIME_LEN, &w);
        }
    }
    uint8_t frame[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TC, t->seq, t->payload, t->len);
    if (n < 0) {
        return;
    }
    int r = g->ops.radio_tx(g->ops.ctx, frame, (size_t)n);
    if (r == GS_TX_BUSY) {
        g->n.lbt_defers++;
        t->next_try = now + lbt_backoff_ms(g);
        return;
    }
    if (r != GS_TX_STARTED) {
        t->next_try = now + 1000u;
        return;
    }
    uint32_t toa_us = gama_lora_toa_us(gama_lora_profile(g->profile), (uint8_t)n);
    g->tx_busy = true;
    g->tx_started = now;
    g->tx_deadline = now + ms_ceil(toa_us) * 3u / 2u + 500u;
    t->attempts++;
    g->n.tc_sent++;
    g->last_tc_tx = now;
}

static void try_transmit(gs_link_t *g, uint64_t now)
{
    try_transmit_at(g, now, false);
}

void gs_init(gs_link_t *g, const gs_params_t *p, const gs_ops_t *ops, uint64_t now)
{
    memset(g, 0, sizeof(*g));
    g->p = *p;
    g->ops = *ops;
    g->profile = g->prev_profile = g->desired_profile = p->initial_profile;
    g->ops.radio_set_profile(g->ops.ctx, g->profile);
    g->last_rx = now;
    g->last_tc_tx = now;
    g->next_seq = 1;
    g->next_id = 1;
}

uint32_t gs_submit(gs_link_t *g, uint64_t now, uint8_t cmd,
                   const uint8_t *args, uint8_t arg_len)
{
    int want = gama_tc_arg_len(cmd);
    if (want < 0 || (size_t)want != arg_len || arg_len > GS_TC_MAX_ARGS) {
        return 0;
    }
    /* A keepalive still waiting for its gap would hold the operator's command
     * behind it. Any telecommand keeps the satellite's contact timer alive,
     * so the keepalive is simply withdrawn. */
    gs_tc_t *h = head(g);
    if (h != NULL && h->gap_only && h->attempts == 0 && !g->tx_busy && !g->awaiting_ack) {
        pop(g);
    }
    uint32_t id = insert(g, now, g->q_count, cmd, args, arg_len, false);
    try_transmit(g, now);
    return id;
}

static void handle_ack(gs_link_t *g, uint64_t now, const uint8_t *p)
{
    gs_tc_t *t = head(g);
    uint8_t cmd = p[0], status = p[1];
    uint16_t echo = gama_get_u16(p + 2);
    if (t == NULL || echo != t->seq || cmd != t->payload[0]) {
        /* A late ACK for an earlier attempt, or for someone else. */
        g->n.acks_unmatched++;
        return;
    }
    g->awaiting_ack = false;
    g->n.tc_acked++;

    gs_event_t ev = {
        .kind = GS_EV_ACK, .id = t->id, .cmd = cmd, .status = status,
        .attempts = t->attempts, .latency_ms = (uint32_t)(now - t->submitted),
    };
    bool internal = t->id == 0;
    uint8_t target = t->payload[1];
    pop(g);
    emit(g, now, &ev);

    if (cmd == GAMA_TC_SET_RATE && status == GAMA_ACK_OK) {
        if (!internal) {
            g->desired_profile = target;
        }
        if (target != g->profile) {
            /* The satellite switches when this ACK has left. Follow it, then
             * confirm with a PING at the new profile once it has settled. */
            g->prev_profile = g->profile;
            set_profile(g, now, target, "commanded");
            g->rate_pending = true;
            g->rate_deadline = now + g->p.rate_revert;
            if (insert(g, now, 0, GAMA_TC_PING, NULL, 0, true) != 0) {
                g->q[g->q_head].next_try = now + GS_SETTLE_MS;
                g->q[g->q_head].not_before = now + GS_SETTLE_MS;
            }
        }
    }
}

void gs_on_rx(gs_link_t *g, uint64_t now, const uint8_t *buf, size_t len,
              int16_t rssi, int8_t snr)
{
    gama_frame_t f;
    if (gama_frame_decode(buf, len, &f) < 0) {
        g->n.dl_bad++;
        try_transmit_at(g, now, true);
        return;
    }
    if (gama_frame_is_ipc_only(f.type) || f.type == GAMA_FRAME_TC ||
        f.type == GAMA_FRAME_BULK_ACK) {
        return;                      /* not from a satellite */
    }

    g->last_rx = now;
    if (!g->in_contact) {
        g->in_contact = true;
        gs_event_t ev = { .kind = GS_EV_CONTACT, .up = true };
        emit(g, now, &ev);
    }

    if (g->dl_seq_valid) {
        uint16_t gap = (uint16_t)(f.seq - g->dl_last_seq - 1u);
        if (gap >= 1000u) {
            g->n.dl_resyncs++;       /* the satellite restarted its sequence */
        } else if (gap > 0) {
            g->n.dl_lost += gap;
            gs_event_t ev = { .kind = GS_EV_LOSS, .lost = gap };
            emit(g, now, &ev);
        }
    }
    g->dl_seq_valid = true;
    g->dl_last_seq = f.seq;
    g->n.dl_frames++;

    gs_event_t ev = {
        .kind = GS_EV_FRAME, .type = f.type, .seq = f.seq, .payload = f.payload,
        .len = f.len, .rssi = rssi, .snr = snr,
    };
    emit(g, now, &ev);

    if (f.type == GAMA_FRAME_TC_ACK && f.len == GAMA_TC_ACK_WIRE_LEN) {
        if (g->rate_pending) {
            /* An ACK heard at the new profile: both directions work. */
            g->rate_pending = false;
            g->recovering = false;
        }
        handle_ack(g, now, f.payload);
    }

    /* Heard the satellite at SAFE while wanting another profile: ask for it. */
    if (g->profile == GAMA_RATE_SAFE && g->desired_profile != GAMA_RATE_SAFE &&
        !g->recovering && !g->rate_pending) {
        uint8_t want = g->desired_profile;
        if (insert(g, now, next_free_slot(g), GAMA_TC_SET_RATE, &want, 1, true) != 0) {
            g->recovering = true;
        }
    }
    try_transmit_at(g, now, true);
}

void gs_on_rx_error(gs_link_t *g, uint64_t now)
{
    g->n.dl_bad++;
    try_transmit_at(g, now, true);   /* a frame ended, even if corrupted */
}

void gs_on_tx_done(gs_link_t *g, uint64_t now)
{
    if (!g->tx_busy) {
        return;
    }
    g->tx_busy = false;
    g->awaiting_ack = true;
    g->ack_deadline = now + gs_ack_timeout_ms(g->profile);
}

void gs_on_tick(gs_link_t *g, uint64_t now)
{
    if (g->tx_busy && now >= g->tx_deadline) {
        /* The end of the transmission was never reported. Carry on as if it
         * had ended: the ACK timeout then decides whether to retry. */
        g->n.tx_watchdogs++;
        gs_on_tx_done(g, now);
    }
    if (g->awaiting_ack && now >= g->ack_deadline) {
        g->awaiting_ack = false;
        gs_tc_t *t = head(g);
        if (t != NULL) {
            if (now - t->submitted >= g->p.give_up) {
                g->n.tc_failed++;
                gs_event_t ev = { .kind = GS_EV_TC_FAILED, .id = t->id,
                                  .cmd = t->payload[0], .attempts = t->attempts };
                if (t->payload[0] == GAMA_TC_SET_RATE && t->id == 0) {
                    g->recovering = false;
                }
                pop(g);
                emit(g, now, &ev);
            } else {
                uint32_t shift = t->attempts > 4u ? 4u : (uint32_t)t->attempts - 1u;
                uint64_t backoff = (uint64_t)GS_BACKOFF_FIRST_MS << shift;
                t->next_try = now + (backoff > GS_BACKOFF_MAX_MS ? GS_BACKOFF_MAX_MS : backoff);
            }
        }
    }

    if (g->rate_pending && now >= g->rate_deadline && !g->tx_busy) {
        g->rate_pending = false;
        bool was_recovery = g->recovering;
        g->recovering = false;
        set_profile(g, now, g->prev_profile, "revert");
        if (!was_recovery) {
            g->desired_profile = g->profile;   /* the change did not work */
        }
    }

    if (g->profile != GAMA_RATE_SAFE && now - g->last_rx >= g->p.contact_timeout &&
        !g->tx_busy) {
        g->in_contact = false;
        g->rate_pending = false;
        g->recovering = false;
        gs_event_t ev = { .kind = GS_EV_CONTACT, .up = false };
        emit(g, now, &ev);
        set_profile(g, now, GAMA_RATE_SAFE, "contact_lost");
    }

    if (g->q_count == 0 && !g->tx_busy && !g->awaiting_ack &&
        now - g->last_tc_tx >= g->p.keepalive) {
        if (insert(g, now, 0, GAMA_TC_PING, NULL, 0, true) != 0) {
            g->q[g->q_head].gap_only = true;
        }
    }

    try_transmit(g, now);
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

uint64_t gs_next_deadline(const gs_link_t *g)
{
    uint64_t d = UINT64_MAX;
    if (g->tx_busy) {
        d = g->tx_deadline;
    } else if (g->awaiting_ack) {
        d = g->ack_deadline;
    } else if (g->q_count > 0 && !g->q[g->q_head].gap_only) {
        const gs_tc_t *t = &g->q[g->q_head];
        d = t->next_try > t->not_before ? t->next_try : t->not_before;
    }
    if (g->rate_pending) {
        d = min_u64(d, g->rate_deadline);
    }
    if (g->profile != GAMA_RATE_SAFE) {
        d = min_u64(d, g->last_rx + g->p.contact_timeout);
    }
    if (g->q_count == 0) {
        d = min_u64(d, g->last_tc_tx + g->p.keepalive);
    }
    return d;
}
