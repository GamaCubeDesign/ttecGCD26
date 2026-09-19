/*
 * link.c — event entry points, medium access and profile management.
 *
 * Medium access (ADR-0007). The SX1278 is half-duplex: while transmitting it
 * cannot hear a telecommand. Three rules keep the two ends from talking over
 * each other:
 *
 *  1. After each of its own transmissions the satellite stays silent for a
 *     gap long enough for the ground's reply to be detected: the ground's
 *     turnaround plus the preamble and header of a packet (core_gap_ms).
 *  2. Before transmitting, the satellite asks the modem whether a reception
 *     is in progress, and defers if so (listen before talk; radio_tx returns
 *     TTCD_TX_BUSY).
 *  3. A valid frame from the ground ends the gap at once: the reply window
 *     has served its purpose, and the acknowledgement should follow at once.
 *
 * Profile changes (ADR-0004, ADR-0007). A commanded change is acknowledged at
 * the old profile, applied when that acknowledgement has left, and reverted
 * unless a frame is heard at the new profile within rate_revert. Silence from
 * the ground for contact_timeout drops the link to SAFE, where the ground
 * station looks for it after its own timeout.
 */

#include "core_internal.h"
#include "gama_bytes.h"
#include "gama_lora.h"
#include "gama_tc.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ttcd_params_default(ttcd_params_t *p)
{
    p->initial_profile    = GAMA_RATE_NOMINAL;
    p->tx_power_dbm       = 20;
    p->hk_period          = 10000;
    p->stat_period        = 30000;
    p->safe_beacon_period = 30000;
    p->safe_hk_period     = 60000;
    p->contact_timeout    = 120000;
    p->rate_revert        = 20000;
    p->reaction_margin    = 20;
    p->dup_window         = 60000;
}

void core_log(ttcd_core_t *c, uint64_t now, const char *event, const char *fmt, ...)
{
    if (c->ops.log == NULL) {
        return;
    }
    char fields[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fields, sizeof(fields), fmt, ap);
    va_end(ap);
    c->ops.log(c->ops.ctx, now, event, fields);
}

static uint64_t ms_ceil(uint32_t us)
{
    return ((uint64_t)us + 999u) / 1000u;
}

uint64_t core_gap_ms(const ttcd_core_t *c)
{
    const gama_lora_phy_t *phy = gama_lora_profile(c->profile);
    return ms_ceil(gama_lora_header_us(phy) + 2u * gama_lora_symbol_us(phy))
           + c->p.reaction_margin;
}

/* How long to wait before asking the modem again after a deferral. */
static uint64_t lbt_backoff_ms(const ttcd_core_t *c)
{
    uint64_t t = ms_ceil(gama_lora_header_us(gama_lora_profile(c->profile)));
    return t < 10u ? 10u : t;
}

static void apply_profile(ttcd_core_t *c, uint64_t now, uint8_t profile,
                          const char *reason)
{
    uint8_t from = c->profile;
    if (c->ops.radio_set_profile(c->ops.ctx, profile) != 0) {
        core_log(c, now, "rate_error", "\"to\":\"%s\",\"reason\":\"%s\"",
                 gama_rate_profile_name(profile), reason);
        return;
    }
    c->profile = profile;
    core_log(c, now, "rate", "\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\"",
             gama_rate_profile_name(from), gama_rate_profile_name(profile), reason);
    tm_on_profile_change(c, now, from);
}

void core_begin_rate_change(ttcd_core_t *c, uint64_t now, uint8_t profile)
{
    uint8_t old = c->profile;
    apply_profile(c, now, profile, "commanded");
    if (c->profile != profile) {
        return;
    }
    c->prev_profile = old;
    c->rate_pending = true;
    c->rate_deadline = now + c->p.rate_revert;
}

bool core_ipc_send(ttcd_core_t *c, uint64_t now, uint8_t role, uint8_t type,
                   const uint8_t *payload, uint8_t len)
{
    uint8_t frame[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(frame, sizeof(frame), type, c->ipc_seq, payload, len);
    if (n < 0) {
        return false;
    }
    c->ipc_seq++;
    if (c->ops.ipc_send(c->ops.ctx, role, frame, (size_t)n) != 0) {
        c->n.ipc_tx_drops++;
        core_log(c, now, "ipc_drop", "\"to\":\"%s\",\"type\":\"%s\"",
                 gama_ipc_role_name(role), gama_frame_type_name(type));
        return false;
    }
    return true;
}

void ttcd_init(ttcd_core_t *c, const ttcd_params_t *p, const ttcd_ops_t *ops,
               uint64_t now)
{
    memset(c, 0, sizeof(*c));
    c->p = *p;
    c->ops = *ops;
    c->boot_ms = now;
    c->profile = p->initial_profile;
    c->prev_profile = p->initial_profile;
    c->tx_power = p->tx_power_dbm;
    c->last_contact = now;       /* the contact timeout counts from boot */
    c->quiet_until = now;
    c->obc_mode = GAMA_OBC_ST_UNKNOWN;

    c->ops.radio_set_profile(c->ops.ctx, c->profile);
    c->ops.radio_set_power(c->ops.ctx, c->tx_power);
    tm_init(c, now);

    core_log(c, now, "boot",
             "\"profile\":\"%s\",\"tx_power_dbm\":%d,\"hk_period_ms\":%" PRIu32
             ",\"contact_timeout_ms\":%" PRIu32 ",\"rate_revert_ms\":%" PRIu32,
             gama_rate_profile_name(c->profile), c->tx_power, p->hk_period,
             p->contact_timeout, p->rate_revert);
}

void core_try_transmit(ttcd_core_t *c, uint64_t now)
{
    if (c->tx_busy || now < c->quiet_until) {
        return;
    }
    ttcd_tx_kind_t kind = tm_pick(c, now);
    if (kind == TTCD_TXK_NONE) {
        return;
    }
    uint8_t frame[GAMA_FRAME_MAX_TOTAL];
    size_t len = tm_build(c, now, kind, frame, sizeof(frame));
    if (len == 0) {
        return;
    }

    int r = c->ops.radio_tx(c->ops.ctx, frame, len);
    if (r == TTCD_TX_BUSY) {
        c->n.lbt_defers++;
        c->quiet_until = now + lbt_backoff_ms(c);
        core_log(c, now, "lbt_defer", "\"type\":\"%s\"",
                 gama_frame_type_name(frame[1]));
        return;
    }
    if (r != TTCD_TX_STARTED) {
        c->quiet_until = now + 1000u;
        core_log(c, now, "tx_error", "\"type\":\"%s\"", gama_frame_type_name(frame[1]));
        return;
    }

    uint32_t toa_us = gama_lora_toa_us(gama_lora_profile(c->profile), (uint8_t)len);
    c->tx_busy = true;
    c->tx_started = now;
    c->tx_kind = kind;
    /* If the shell never reports the end of this transmission, give up on it
     * well after it must have finished, rather than stay mute forever. */
    c->tx_deadline = now + ms_ceil(toa_us) * 3u / 2u + 500u;
    c->n.frames_tx++;
    core_log(c, now, "tx",
             "\"type\":\"%s\",\"seq\":%u,\"len\":%zu,\"profile\":\"%s\",\"toa_us\":%" PRIu32,
             gama_frame_type_name(frame[1]), c->tx_seq, len,
             gama_rate_profile_name(c->profile), toa_us);
    tm_commit(c, now, kind);
    c->tx_seq++;
}

static void run_post_action(ttcd_core_t *c, uint64_t now, const ttcd_ack_t *a)
{
    switch (a->post) {
    case TTCD_POST_SET_PROFILE:
        core_begin_rate_change(c, now, (uint8_t)a->post_arg);
        break;
    case TTCD_POST_SET_POWER:
        if (c->ops.radio_set_power(c->ops.ctx, (int8_t)a->post_arg) == 0) {
            c->tx_power = (int8_t)a->post_arg;
            core_log(c, now, "tx_power", "\"dbm\":%d", c->tx_power);
        }
        break;
    case TTCD_POST_EXIT:
        core_log(c, now, "shutdown", "\"code\":%d", TTCD_EXIT_SHUTDOWN);
        if (c->ops.exit_request != NULL) {
            c->ops.exit_request(c->ops.ctx, TTCD_EXIT_SHUTDOWN);
        }
        break;
    case TTCD_POST_NONE:
    default:
        break;
    }
}

static void finish_tx(ttcd_core_t *c, uint64_t now, bool watchdog)
{
    c->tx_busy = false;
    if (watchdog) {
        c->n.tx_watchdogs++;
        core_log(c, now, "tx_watchdog", "\"elapsed_ms\":%" PRIu64, now - c->tx_started);
    } else {
        core_log(c, now, "tx_done", "\"elapsed_ms\":%" PRIu64, now - c->tx_started);
    }
    bool switched = false;
    if (c->tx_kind == TTCD_TXK_ACK) {
        uint8_t before = c->profile;
        run_post_action(c, now, &c->tx_ack);
        switched = c->profile != before;
    }
    c->tx_kind = TTCD_TXK_NONE;
    /* After the post-action, so a new profile gets its own gap. After a
     * switch, longer still: the ground waits for us to reconfigure before it
     * sends its confirmation (GS_SETTLE_MS, 50 ms), and that confirmation must
     * land inside our silence. */
    c->quiet_until = now + core_gap_ms(c) + (switched ? TTCD_SWITCH_GAP_EXTRA_MS : 0u);
}

/* Revert and contact timeouts. Never while transmitting: the modem cannot be
 * reconfigured mid-packet, so they wait for the end of the transmission. */
static void check_timers(ttcd_core_t *c, uint64_t now)
{
    if (c->tx_busy) {
        return;
    }
    if (c->rate_pending && now >= c->rate_deadline) {
        c->rate_pending = false;
        c->n.rate_reverts++;
        apply_profile(c, now, c->prev_profile, "revert");
    }
    if (c->profile != GAMA_RATE_SAFE &&
        now - c->last_contact >= c->p.contact_timeout) {
        c->n.contact_losses++;
        c->rate_pending = false;
        core_log(c, now, "contact_lost", "\"silent_ms\":%" PRIu64, now - c->last_contact);
        apply_profile(c, now, GAMA_RATE_SAFE, "contact_lost");
    }
}

void ttcd_on_radio_rx(ttcd_core_t *c, uint64_t now,
                      const uint8_t *buf, size_t len, int16_t rssi_dbm, int8_t snr_db)
{
    gama_frame_t f;
    int r = gama_frame_decode(buf, len, &f);
    if (r < 0) {
        c->n.frames_rx_bad++;
        core_log(c, now, "rx_bad", "\"reason\":\"%s\",\"len\":%zu,\"rssi\":%d,\"snr\":%d",
                 gama_frame_result_name(r), len, rssi_dbm, snr_db);
        core_try_transmit(c, now);
        return;
    }
    if (f.type != GAMA_FRAME_TC && f.type != GAMA_FRAME_BULK_ACK) {
        /* Valid framing, but not something the ground sends: another node on
         * the channel, or a misconfigured tool. Counted, never obeyed. */
        c->n.frames_foreign++;
        core_log(c, now, "rx_foreign", "\"type\":\"%s\",\"seq\":%u",
                 gama_frame_type_name(f.type), f.seq);
        return;
    }

    c->n.frames_rx_ok++;
    c->last_contact = now;
    c->quiet_until = now;
    core_log(c, now, "rx", "\"type\":\"%s\",\"seq\":%u,\"len\":%u,\"rssi\":%d,\"snr\":%d",
             gama_frame_type_name(f.type), f.seq, f.len, rssi_dbm, snr_db);

    if (c->rate_pending) {
        /* Heard the ground at the new profile: the change is confirmed. */
        c->rate_pending = false;
        core_log(c, now, "rate_commit", "\"profile\":\"%s\"",
                 gama_rate_profile_name(c->profile));
    }

    if (f.type == GAMA_FRAME_TC) {
        tc_handle(c, now, &f);
    } else {
        core_log(c, now, "bulk_ack_ignored", "\"seq\":%u", f.seq);
    }
    core_try_transmit(c, now);
}

void ttcd_on_radio_rx_error(ttcd_core_t *c, uint64_t now)
{
    c->n.frames_rx_bad++;
    core_log(c, now, "rx_bad", "\"reason\":\"PHY_CRC\"");
    core_try_transmit(c, now);
}

void ttcd_on_radio_tx_done(ttcd_core_t *c, uint64_t now)
{
    if (!c->tx_busy) {
        core_log(c, now, "tx_done_spurious", "%s", "\"ignored\":true");
        return;
    }
    finish_tx(c, now, false);
    check_timers(c, now);
    core_try_transmit(c, now);
}

void ttcd_on_ipc_peer(ttcd_core_t *c, uint64_t now, uint8_t role, bool connected)
{
    if (role == GAMA_IPC_ROLE_OBC) {
        c->obc_linked = connected;
        if (!connected) {
            /* Stale values would be reported as current; report unknown. */
            c->obc_mode = GAMA_OBC_ST_UNKNOWN;
            c->have_telemetry = false;
        }
    } else if (role == GAMA_IPC_ROLE_ADSBD) {
        c->adsbd_linked = connected;
    }
    core_log(c, now, "ipc_peer", "\"role\":\"%s\",\"connected\":%s",
             gama_ipc_role_name(role), connected ? "true" : "false");
}

void ttcd_on_ipc_frame(ttcd_core_t *c, uint64_t now, uint8_t role,
                       const uint8_t *buf, size_t len)
{
    gama_frame_t f;
    if (gama_frame_decode(buf, len, &f) < 0 || !gama_frame_is_ipc_only(f.type)) {
        c->n.ipc_rx_bad++;
        core_log(c, now, "ipc_bad", "\"from\":\"%s\",\"len\":%zu",
                 gama_ipc_role_name(role), len);
        return;
    }
    c->n.ipc_rx++;

    if (role == GAMA_IPC_ROLE_OBC && f.type == GAMA_FRAME_IPC_TELEMETRY) {
        gama_ipc_telemetry_t t;
        if (gama_ipc_telemetry_decode(f.payload, f.len, &t) < 0) {
            c->n.ipc_rx_bad++;
            return;
        }
        c->telemetry = t;
        c->have_telemetry = true;
        /* Logged in full: the onboard log is where battery temperature is
         * kept until TM_HK carries it (HLR-EPS-04). */
        core_log(c, now, "obc_telemetry",
                 "\"battery_mv\":%u,\"current_ma\":%d,\"temp_bat_ccel\":%d,"
                 "\"temp_ext_ccel\":%d,\"roll_cdeg\":%d,\"pitch_cdeg\":%d,\"yaw_cdeg\":%d",
                 t.battery_mv, t.current_ma, t.temp_bat_ccel, t.temp_ext_ccel,
                 t.roll_cdeg, t.pitch_cdeg, t.yaw_cdeg);
    } else if (role == GAMA_IPC_ROLE_OBC && f.type == GAMA_FRAME_IPC_MODE &&
               f.len == GAMA_IPC_MODE_LEN) {
        if (f.payload[0] != c->obc_mode) {
            core_log(c, now, "obc_mode", "\"from\":\"%s\",\"to\":\"%s\"",
                     gama_obc_state_name(c->obc_mode), gama_obc_state_name(f.payload[0]));
        }
        c->obc_mode = f.payload[0];
    } else if (role == GAMA_IPC_ROLE_ADSBD && f.type == GAMA_FRAME_IPC_TRACKS) {
        tm_on_tracks(c, now, &f);
    } else if (role == GAMA_IPC_ROLE_ADSBD && f.type == GAMA_FRAME_IPC_STAT) {
        gama_ipc_stat_t s;
        if (gama_ipc_stat_decode(f.payload, f.len, &s) < 0) {
            c->n.ipc_rx_bad++;
            return;
        }
        c->payload_stat = s;
        c->have_stat = true;
    } else {
        c->n.ipc_rx_bad++;
        core_log(c, now, "ipc_unexpected", "\"from\":\"%s\",\"type\":\"%s\"",
                 gama_ipc_role_name(role), gama_frame_type_name(f.type));
        return;
    }
    core_try_transmit(c, now);
}

void ttcd_on_tick(ttcd_core_t *c, uint64_t now)
{
    if (c->tx_busy && now >= c->tx_deadline) {
        finish_tx(c, now, true);
    }
    check_timers(c, now);
    core_try_transmit(c, now);
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

uint64_t ttcd_next_deadline(const ttcd_core_t *c)
{
    uint64_t d = UINT64_MAX;
    if (c->tx_busy) {
        d = c->tx_deadline;
    } else {
        uint64_t due = tm_next_due(c);
        if (due != UINT64_MAX) {
            d = due > c->quiet_until ? due : c->quiet_until;
        }
    }
    if (c->rate_pending) {
        d = min_u64(d, c->rate_deadline);
    }
    if (c->profile != GAMA_RATE_SAFE) {
        d = min_u64(d, c->last_contact + c->p.contact_timeout);
    }
    return d;
}

uint8_t ttcd_link_mode(const ttcd_core_t *c)
{
    if (c->profile == GAMA_RATE_SAFE) {
        return GAMA_LINK_SAFE;
    }
    return c->stream_on ? GAMA_LINK_STREAM : GAMA_LINK_IDLE;
}
