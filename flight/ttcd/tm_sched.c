/*
 * tm_sched.c — what the satellite transmits, and when.
 *
 * At each opportunity (not transmitting, the post-transmission gap elapsed)
 * the scheduler picks one frame, in this order:
 *
 *   1. acknowledgements            — the ground is waiting on them
 *   2. housekeeping, if requested or due
 *   3. statistics, if requested or due while streaming
 *   4. the next frame of the track snapshot being sent, or a new snapshot
 *      when the stream period has elapsed
 *   5. the beacon, in SAFE
 *
 * Periods come from docs/budgets/data-budget.md §5: snapshot every 5 s
 * (commanded), housekeeping every 10 s, statistics every 30 s. In SAFE the
 * link is at SF12, where one snapshot would take 23 s of air time, so tracks
 * are suppressed and only the beacon and a slower housekeeping go out.
 *
 * Track age (common/gama_tm.h, ADR-0007). Records arrive from adsbd with ages
 * measured to the snapshot's epoch. Just before a frame goes on the air, each
 * age is advanced by the time elapsed since that epoch, so it measures to the
 * start of this transmission. That rewritten age is, record by record, the
 * latency from reception to transmission that HLR-ADS-08 asks us to bound;
 * the p95 reported in TM_STAT is computed from it.
 */

#include "core_internal.h"
#include "gama_bytes.h"
#include "gama_tc.h"

#include <inttypes.h>
#include <string.h>

/* Signed difference of two wrapping 32-bit millisecond clocks. */
static bool epoch_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static bool snapshot_complete(const ttcd_snapshot_t *s)
{
    return s->valid && s->count > 0 &&
           s->have == (uint8_t)((1u << s->count) - 1u);
}

static bool sending_remaining(ttcd_core_t *c)
{
    if (!c->sending.valid) {
        return false;
    }
    /* Skip frames without records: nothing to put on the air. */
    while (c->sending_next < c->sending.count &&
           c->sending.nrec[c->sending_next] == 0) {
        c->sending_next++;
    }
    return c->sending_next < c->sending.count;
}

void tm_init(ttcd_core_t *c, uint64_t now)
{
    /* Housekeeping at once: it is how the ground learns we are alive. */
    c->next_hk = now;
    c->next_stat = now + c->p.stat_period;
    c->next_beacon = now;
    c->next_tracks = now;
}

void tm_on_profile_change(ttcd_core_t *c, uint64_t now, uint8_t from)
{
    if (c->profile == GAMA_RATE_SAFE) {
        c->sending.valid = false;          /* tracks do not fit SAFE */
        c->next_beacon = now;
        c->next_hk = now + c->p.safe_hk_period;
    } else if (from == GAMA_RATE_SAFE) {
        c->next_hk = now;
        c->next_tracks = now;
        c->next_stat = now + c->p.stat_period;
    }
}

void tm_on_tracks(ttcd_core_t *c, uint64_t now, const gama_frame_t *f)
{
    gama_ipc_tracks_hdr_t h;
    size_t n;
    if (gama_ipc_tracks_decode(f->payload, f->len, &h, &n) < 0) {
        c->n.ipc_rx_bad++;
        core_log(c, now, "ipc_bad", "%s", "\"what\":\"tracks\"");
        return;
    }
    ttcd_snapshot_t *s = &c->latest;
    if (s->valid && h.epoch_ms != s->epoch_ms && !epoch_after(h.epoch_ms, s->epoch_ms)) {
        return;                            /* older than what we hold */
    }
    if (!s->valid || h.epoch_ms != s->epoch_ms || h.count != s->count) {
        s->valid = true;
        s->epoch_ms = h.epoch_ms;
        s->count = h.count;
        s->have = 0;
    }
    memcpy(s->rec[h.index], f->payload + GAMA_IPC_TRACKS_HEADER_LEN,
           n * GAMA_TRACK_WIRE_LEN);
    s->nrec[h.index] = (uint8_t)n;
    s->have = (uint8_t)(s->have | (1u << h.index));
}

/* Starts sending the latest snapshot if it is complete and newer than the
 * last one sent. Advances the stream clock either way, keeping its phase. */
static void arm_snapshot(ttcd_core_t *c, uint64_t now)
{
    c->next_tracks += c->stream_period;
    if (c->next_tracks <= now) {
        c->next_tracks = now + c->stream_period;   /* fell behind: resync */
    }
    if (!snapshot_complete(&c->latest)) {
        return;
    }
    if (c->has_started && !epoch_after(c->latest.epoch_ms, c->last_started_epoch)) {
        return;                                    /* nothing new since */
    }
    c->sending = c->latest;
    c->sending_next = 0;
    c->has_boundary = c->has_started;
    c->fresh_boundary = c->last_started_epoch;
    c->last_started_epoch = c->latest.epoch_ms;
    c->has_started = true;
}

ttcd_tx_kind_t tm_pick(ttcd_core_t *c, uint64_t now)
{
    bool safe = c->profile == GAMA_RATE_SAFE;

    if (ack_pending(c)) {
        return TTCD_TXK_ACK;
    }
    if (c->hk_requested || now >= c->next_hk) {
        return TTCD_TXK_HK;
    }
    if (c->stat_requested || (!safe && c->stream_on && now >= c->next_stat)) {
        return TTCD_TXK_STAT;
    }
    if (!safe && c->stream_on) {
        if (sending_remaining(c)) {
            return TTCD_TXK_TRACKS;
        }
        if (now >= c->next_tracks) {
            arm_snapshot(c, now);
            if (sending_remaining(c)) {
                return TTCD_TXK_TRACKS;
            }
        }
    }
    if (safe && now >= c->next_beacon) {
        return TTCD_TXK_BEACON;
    }
    return TTCD_TXK_NONE;
}

/* Deciseconds elapsed since the snapshot epoch, rounded. */
static uint32_t since_epoch_ds(const ttcd_core_t *c, uint64_t now)
{
    uint32_t elapsed_ms = (uint32_t)now - c->sending.epoch_ms;
    return (elapsed_ms + 50u) / 100u;
}

static void fill_hk(const ttcd_core_t *c, uint64_t now, gama_hk_t *hk)
{
    memset(hk, 0, sizeof(*hk));
    if (c->have_telemetry) {
        hk->battery_mv    = c->telemetry.battery_mv;
        hk->current_ma    = c->telemetry.current_ma;
        hk->temp_ext_ccel = c->telemetry.temp_ext_ccel;
        hk->roll_cdeg     = c->telemetry.roll_cdeg;
        hk->pitch_cdeg    = c->telemetry.pitch_cdeg;
        hk->yaw_cdeg      = c->telemetry.yaw_cdeg;
    } else {
        hk->battery_mv    = 0;
        hk->current_ma    = TTCD_HK_UNKNOWN;
        hk->temp_ext_ccel = TTCD_HK_UNKNOWN;
        hk->roll_cdeg     = TTCD_HK_UNKNOWN;
        hk->pitch_cdeg    = TTCD_HK_UNKNOWN;
        hk->yaw_cdeg      = TTCD_HK_UNKNOWN;
    }
    int16_t soc = TTCD_HK_UNKNOWN;
    if (c->ops.soc_temp == NULL || c->ops.soc_temp(c->ops.ctx, &soc) != 0) {
        soc = TTCD_HK_UNKNOWN;
    }
    hk->temp_soc_ccel = soc;
    hk->adsb_msgs  = c->have_stat ? c->payload_stat.msgs_decoded : 0;
    hk->obc_mode   = c->obc_linked ? c->obc_mode : GAMA_OBC_ST_UNKNOWN;
    hk->link_state = ttcd_link_mode(c);
    hk->uptime_s   = (uint32_t)((now - c->boot_ms) / 1000u);
    hk->flags      = (uint8_t)((c->obc_linked   ? GAMA_HK_F_OBC_LINKED  : 0u) |
                               (c->adsbd_linked ? GAMA_HK_F_ADSB_LINKED : 0u) |
                               (c->time_synced  ? GAMA_HK_F_TIME_SYNCED : 0u));
    /* GAMA_HK_F_DUMP1090_UP needs adsbd to report it (PLANO phase 3). */
}

static void fill_stat(const ttcd_core_t *c, gama_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    if (c->have_stat) {
        st->msgs_received     = c->payload_stat.msgs_received;
        st->msgs_decoded      = c->payload_stat.msgs_decoded;
        st->aircraft_tracked  = c->payload_stat.aircraft_tracked;
        st->dump1090_restarts = c->payload_stat.dump1090_restarts;
    }
    st->tm_frames_sent = c->n.frames_tx;
    st->tc_frames_rx   = c->n.frames_rx_ok;
    st->tc_frames_bad  = c->n.frames_rx_bad;
    st->latency_p95_ms = ttcd_latency_p95_ms(c);
}

size_t tm_build(ttcd_core_t *c, uint64_t now, ttcd_tx_kind_t kind,
                uint8_t *frame, size_t cap)
{
    uint8_t payload[GAMA_FRAME_MAX_PAYLOAD];
    uint8_t type;
    int len;

    switch (kind) {
    case TTCD_TXK_ACK: {
        const ttcd_ack_t *a = ack_peek(c);
        payload[0] = a->cmd;
        payload[1] = a->status;
        gama_put_u16(payload + 2, a->echo_seq);
        type = GAMA_FRAME_TC_ACK;
        len = GAMA_TC_ACK_WIRE_LEN;
        break;
    }
    case TTCD_TXK_HK: {
        gama_hk_t hk;
        fill_hk(c, now, &hk);
        type = GAMA_FRAME_TM_HK;
        len = gama_hk_encode(payload, sizeof(payload), &hk);
        break;
    }
    case TTCD_TXK_STAT: {
        gama_stat_t st;
        fill_stat(c, &st);
        type = GAMA_FRAME_TM_STAT;
        len = gama_stat_encode(payload, sizeof(payload), &st);
        break;
    }
    case TTCD_TXK_TRACKS: {
        uint8_t i = c->sending_next;
        size_t bytes = (size_t)c->sending.nrec[i] * GAMA_TRACK_WIRE_LEN;
        memcpy(payload, c->sending.rec[i], bytes);
        uint32_t delta = since_epoch_ds(c, now);
        for (size_t r = 0; r < c->sending.nrec[i]; r++) {
            gama_track_age_add(payload + r * GAMA_TRACK_WIRE_LEN, delta);
        }
        type = GAMA_FRAME_TM_TRACKS;
        len = (int)bytes;
        break;
    }
    case TTCD_TXK_BEACON:
        type = GAMA_FRAME_BEACON;
        len = 0;
        break;
    case TTCD_TXK_NONE:
    default:
        return 0;
    }
    if (len < 0) {
        return 0;
    }
    int n = gama_frame_encode(frame, cap, type, c->tx_seq, payload, (uint8_t)len);
    return n < 0 ? 0 : (size_t)n;
}

/* Counts every update of this frame that is being transmitted for the first
 * time. An update older than the previous snapshot's epoch went out then;
 * counting it again would measure how long an aircraft went unheard, not how
 * long our pipeline took. */
static void record_latency(ttcd_core_t *c, uint64_t now)
{
    uint8_t i = c->sending_next;
    uint32_t delta = since_epoch_ds(c, now);
    for (size_t r = 0; r < c->sending.nrec[i]; r++) {
        const uint8_t *rec = c->sending.rec[i] + r * GAMA_TRACK_WIRE_LEN;
        uint32_t age_at_epoch = gama_get_u16(rec + 17);
        uint32_t updated_at = c->sending.epoch_ms - age_at_epoch * 100u;
        if (c->has_boundary && !epoch_after(updated_at, c->fresh_boundary)) {
            continue;
        }
        uint32_t latency_ds = age_at_epoch + delta;
        c->latency_hist[latency_ds > 255u ? 255u : latency_ds]++;
    }
}

void tm_commit(ttcd_core_t *c, uint64_t now, ttcd_tx_kind_t kind)
{
    bool safe = c->profile == GAMA_RATE_SAFE;
    switch (kind) {
    case TTCD_TXK_ACK:
        ack_pop(c, &c->tx_ack);
        break;
    case TTCD_TXK_HK:
        c->hk_requested = false;
        c->next_hk = now + (safe ? c->p.safe_hk_period : c->p.hk_period);
        break;
    case TTCD_TXK_STAT:
        c->stat_requested = false;
        c->next_stat = now + c->p.stat_period;
        break;
    case TTCD_TXK_TRACKS:
        record_latency(c, now);
        c->sending_next++;
        break;
    case TTCD_TXK_BEACON:
        c->next_beacon = now + c->p.safe_beacon_period;
        break;
    case TTCD_TXK_NONE:
    default:
        break;
    }
}

uint64_t tm_next_due(const ttcd_core_t *c)
{
    bool safe = c->profile == GAMA_RATE_SAFE;
    if (c->ack_count > 0 || c->hk_requested || c->stat_requested) {
        return 0;
    }
    if (!safe && c->stream_on && c->sending.valid && c->sending_next < c->sending.count) {
        return 0;
    }
    uint64_t d = c->next_hk;
    if (!safe && c->stream_on) {
        if (c->next_stat < d)   { d = c->next_stat; }
        if (c->next_tracks < d) { d = c->next_tracks; }
    }
    if (safe && c->next_beacon < d) {
        d = c->next_beacon;
    }
    return d;
}

uint16_t ttcd_latency_p95_ms(const ttcd_core_t *c)
{
    uint64_t total = 0;
    for (size_t i = 0; i < 256; i++) {
        total += c->latency_hist[i];
    }
    if (total == 0) {
        return 0xFFFFu;
    }
    uint64_t target = (95u * total + 99u) / 100u;   /* ceil(0.95 N) */
    uint64_t seen = 0;
    for (size_t i = 0; i < 256; i++) {
        seen += c->latency_hist[i];
        if (seen >= target) {
            /* Upper edge of the bin: never understate the latency. */
            return (uint16_t)((i + 1u) * 100u);
        }
    }
    return 0xFFFFu;
}
