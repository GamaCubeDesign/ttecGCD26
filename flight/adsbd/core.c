/*
 * core.c — see core.h.
 *
 * The flow of one message: the shell hands over a line the moment it
 * arrives; the line is parsed (sbs.c), merged into the aircraft's track
 * (tracks.c) and recorded, stamped with its arrival (ndjson.c). Once a second
 * the table becomes a snapshot for ttcd, which sends the latest complete one
 * every stream period — 5 s — with each record's age extended to the start of
 * its transmission (ADR-0007). That age is the HLR-ADS-08 latency, record by
 * record.
 */

#include "core.h"
#include "gama_frame.h"
#include "ndjson.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NS_PER_MS 1000000u

void adsbd_params_default(adsbd_params_t *p)
{
    p->snapshot_period  = 1000;
    p->stat_period      = 1000;
    p->report_period    = 60000;
    p->track_expiry     = 60000;
    p->position_max_age = 10000;
    p->field_ttl        = 60000;
    p->snapshot_max     = 24;      /* two frames: the data budget's 20 aircraft */
    p->table_capacity   = ADSBD_TRACKS_MAX;
}

static void core_log(adsbd_core_t *c, uint64_t now_ns, const char *event,
                     const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void core_log(adsbd_core_t *c, uint64_t now_ns, const char *event,
                     const char *fmt, ...)
{
    if (c->ops.log == NULL) {
        return;
    }
    char fields[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fields, sizeof(fields), fmt, ap);
    va_end(ap);
    c->ops.log(c->ops.ctx, now_ns / NS_PER_MS, event, fields);
}

void adsbd_init(adsbd_core_t *c, const adsbd_params_t *p, const adsbd_ops_t *ops,
                uint64_t now, int64_t wall_offset_ns)
{
    memset(c, 0, sizeof(*c));
    c->p = *p;
    if (c->p.snapshot_max > ADSBD_SNAPSHOT_MAX) {
        c->p.snapshot_max = ADSBD_SNAPSHOT_MAX;
    }
    c->ops = *ops;
    c->wall_offset_ns = wall_offset_ns;
    tracks_init(&c->table, c->p.table_capacity);
    c->next_snapshot = now + (uint64_t)c->p.snapshot_period * NS_PER_MS;
    c->next_stat = now + (uint64_t)c->p.stat_period * NS_PER_MS;
    c->next_report = now + (uint64_t)c->p.report_period * NS_PER_MS;
}

uint64_t adsbd_wall_ns(const adsbd_core_t *c, uint64_t mono_ns)
{
    /* Both terms stay far from the limits of int64: a monotonic clock in ns
     * reaches 2^63 after 292 years of uptime. */
    int64_t wall = (int64_t)mono_ns + c->wall_offset_ns;
    return wall > 0 ? (uint64_t)wall : 0u;
}

void adsbd_on_line(adsbd_core_t *c, uint64_t now, const char *line, size_t len)
{
    if (len == 0 || (len == 1 && line[0] == '\r')) {
        return;
    }
    c->n.lines++;

    sbs_msg_t m;
    sbs_result_t r = sbs_parse(line, len, &m);
    if (r == SBS_IGNORED) {
        c->n.ignored++;
        return;
    }
    if (r == SBS_NON_ICAO) {
        c->n.non_icao++;
        return;
    }
    if (r != SBS_OK) {
        /* Counted, never used (AGENTS.md, hard rule 6). The first few are
         * named in the log so a systematic problem can be diagnosed. */
        c->n.rejected++;
        if (c->n.rejected <= ADSBD_REJECT_LOG_MAX) {
            core_log(c, now, "sbs_reject", "\"why\":\"%s\",\"len\":%zu",
                     sbs_result_name(r), len);
        }
        return;
    }

    if (!tracks_apply(&c->table, &m, now / NS_PER_MS)) {
        c->n.evictions++;
    }
    c->n.decoded++;

    /* HLR-ADS-07: every message recorded with its time of arrival. */
    char out[ADSBD_NDJSON_MAX];
    int k = adsbd_ndjson_format(out, sizeof(out), &m, adsbd_wall_ns(c, now));
    if (k > 0 && c->ops.record != NULL) {
        c->ops.record(c->ops.ctx, out, (size_t)k);
    }
}

void adsbd_on_overlong_line(adsbd_core_t *c, uint64_t now)
{
    c->n.lines++;
    c->n.rejected++;
    if (c->n.rejected <= ADSBD_REJECT_LOG_MAX) {
        core_log(c, now, "sbs_reject", "\"why\":\"overlong\"");
    }
}

void adsbd_on_dump1090_restart(adsbd_core_t *c, uint64_t now)
{
    (void)now;
    if (c->n.dump1090_restarts < UINT16_MAX) {
        c->n.dump1090_restarts++;
    }
}

static bool send_frame(adsbd_core_t *c, uint64_t now, uint8_t type,
                       const uint8_t *payload, uint8_t len)
{
    uint8_t frame[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(frame, sizeof(frame), type, c->ipc_seq, payload, len);
    if (n < 0) {
        return false;
    }
    c->ipc_seq++;
    if (c->ops.ipc_send(c->ops.ctx, frame, (size_t)n) != 0) {
        c->n.ipc_tx_drops++;
        core_log(c, now, "ipc_drop", "\"type\":\"%s\"", gama_frame_type_name(type));
        return false;
    }
    return true;
}

static void send_stat(adsbd_core_t *c, uint64_t now)
{
    gama_ipc_stat_t s = {
        .msgs_received     = c->n.lines,
        .msgs_decoded      = c->n.decoded,
        .aircraft_tracked  = (uint16_t)c->table.count,
        .dump1090_restarts = c->n.dump1090_restarts,
    };
    uint8_t payload[GAMA_IPC_STAT_LEN];
    gama_ipc_stat_encode(payload, sizeof(payload), &s);
    send_frame(c, now, GAMA_FRAME_IPC_STAT, payload, GAMA_IPC_STAT_LEN);
}

/* One snapshot: as many IPC_TRACKS frames as it takes, all with the same
 * epoch so ttcd knows when it holds every one (common/gama_ipc.h). An empty
 * table still sends one frame without records: the snapshot that says "no
 * aircraft now", so ttcd never keeps sending ones that have gone. */
static void send_snapshot(adsbd_core_t *c, uint64_t now)
{
    uint64_t now_ms = now / NS_PER_MS;
    adsbd_snapshot_rules_t rules = {
        .position_max_age = c->p.position_max_age,
        .field_ttl = c->p.field_ttl,
        .max_records = c->p.snapshot_max,
    };
    gama_track_t recs[ADSBD_SNAPSHOT_MAX];
    uint32_t left_out = 0;
    size_t n = tracks_snapshot(&c->table, now_ms, &rules, recs, ADSBD_SNAPSHOT_MAX, &left_out);
    c->n.left_out += left_out;

    size_t frames = n == 0 ? 1u : (n + GAMA_IPC_TRACKS_MAX - 1u) / GAMA_IPC_TRACKS_MAX;
    uint32_t epoch = (uint32_t)now_ms;       /* the low 32 bits, as ttcd expects */
    for (size_t f = 0; f < frames; f++) {
        uint8_t payload[GAMA_FRAME_MAX_PAYLOAD];
        gama_ipc_tracks_hdr_t h = { .epoch_ms = epoch, .index = (uint8_t)f,
                                    .count = (uint8_t)frames };
        size_t pos = (size_t)gama_ipc_tracks_header_encode(payload, sizeof(payload), &h);
        for (size_t r = f * GAMA_IPC_TRACKS_MAX; r < n && r < (f + 1u) * GAMA_IPC_TRACKS_MAX; r++) {
            pos += (size_t)gama_track_encode(payload + pos, sizeof(payload) - pos, &recs[r]);
        }
        send_frame(c, now, GAMA_FRAME_IPC_TRACKS, payload, (uint8_t)pos);
    }
    c->n.snapshots++;
}

void adsbd_on_ipc_link(adsbd_core_t *c, uint64_t now, bool up)
{
    c->ipc_up = up;
    if (up) {
        /* A (re)connected ttcd learns the counters and the aircraft at once. */
        c->next_stat = now;
        c->next_snapshot = now;
    }
}

void adsbd_on_ipc_frame(adsbd_core_t *c, uint64_t now, const uint8_t *buf, size_t len)
{
    gama_frame_t f;
    if (gama_frame_decode(buf, len, &f) < 0) {
        c->n.ipc_rx_bad++;
        core_log(c, now, "ipc_unexpected", "\"what\":\"undecodable\",\"len\":%zu", len);
        return;
    }
    if (f.type != GAMA_FRAME_IPC_TIME_SET) {
        c->n.ipc_rx_bad++;
        core_log(c, now, "ipc_unexpected", "\"type\":\"%s\"", gama_frame_type_name(f.type));
        return;
    }
    gama_ipc_time_t t;
    if (gama_ipc_time_decode(f.payload, f.len, &t) < 0 || t.nsec >= 1000000000u) {
        c->n.ipc_rx_bad++;
        core_log(c, now, "ipc_unexpected", "\"what\":\"bad time\"");
        return;
    }
    /* ADR-0009: the wall clock is now ttcd's, which is the ground's. ttcd
     * sent it as the time at sending; the socket hop is microseconds. The
     * step is logged, so a record can be re-timed afterwards if needed. */
    int64_t wall = (int64_t)t.unix_s * 1000000000 + (int64_t)t.nsec;
    int64_t offset = wall - (int64_t)now;
    int64_t step_ms = (offset - c->wall_offset_ns) / 1000000;
    c->wall_offset_ns = offset;
    c->time_synced = true;
    c->n.anchors++;
    core_log(c, now, "time_anchor",
             "\"unix_s\":%" PRIu32 ",\"nsec\":%" PRIu32 ",\"step_ms\":%" PRId64,
             t.unix_s, t.nsec, step_ms);
}

static uint64_t advance(uint64_t next, uint64_t now, uint32_t period_ms)
{
    uint64_t period = (uint64_t)period_ms * NS_PER_MS;
    next += period;
    return next <= now ? now + period : next;   /* fell behind: resync */
}

void adsbd_on_tick(adsbd_core_t *c, uint64_t now)
{
    if (now >= c->next_snapshot) {
        c->n.expired += tracks_expire(&c->table, now / NS_PER_MS, c->p.track_expiry);
        if (c->ipc_up) {
            send_snapshot(c, now);
        }
        c->next_snapshot = advance(c->next_snapshot, now, c->p.snapshot_period);
    }
    if (now >= c->next_stat) {
        if (c->ipc_up) {
            send_stat(c, now);
        }
        c->next_stat = advance(c->next_stat, now, c->p.stat_period);
    }
    if (now >= c->next_report) {
        const adsbd_counters_t *n = &c->n;
        core_log(c, now, "stats",
                 "\"lines\":%" PRIu32 ",\"decoded\":%" PRIu32 ",\"ignored\":%" PRIu32
                 ",\"non_icao\":%" PRIu32 ",\"rejected\":%" PRIu32 ",\"aircraft\":%" PRIu32
                 ",\"evictions\":%" PRIu32 ",\"expired\":%" PRIu32 ",\"snapshots\":%" PRIu32
                 ",\"left_out\":%" PRIu32 ",\"ipc_tx_drops\":%" PRIu32 ",\"ipc_rx_bad\":%" PRIu32
                 ",\"anchors\":%" PRIu32 ",\"dump1090_restarts\":%u,\"time_synced\":%s",
                 n->lines, n->decoded, n->ignored, n->non_icao, n->rejected, c->table.count,
                 n->evictions, n->expired, n->snapshots, n->left_out, n->ipc_tx_drops,
                 n->ipc_rx_bad, n->anchors, (unsigned)n->dump1090_restarts,
                 c->time_synced ? "true" : "false");
        c->next_report = advance(c->next_report, now, c->p.report_period);
    }
}

uint64_t adsbd_next_deadline(const adsbd_core_t *c)
{
    uint64_t d = c->next_snapshot;
    if (c->next_stat < d) {
        d = c->next_stat;
    }
    if (c->next_report < d) {
        d = c->next_report;
    }
    return d;
}
