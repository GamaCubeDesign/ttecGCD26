/*
 * gs_console.c — see gs_console.h.
 *
 * The output format is the one tools/gs_cli has written since the bench of
 * 2026-10-07: the bench scripts (tools/bench/run_step.sh) and the downlink
 * adapter (ground/host/tracks_to_ndjson.py) read it byte for byte, and
 * tests/test_gs_console.c pins it.
 */

#include "gs_console.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_lora.h"
#include "gama_tc.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- output ---- */

static bool wall_now(const gs_console_t *c, uint64_t *ms)
{
    if (c->ops.wall_ms != NULL && c->ops.wall_ms(c->ops.ctx, ms) == 0) {
        return true;
    }
    if (c->clock_set) {
        *ms = (uint64_t)((int64_t)c->now + c->clock_offset);
        return true;
    }
    return false;
}

static void out(gs_console_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void out(gs_console_t *c, const char *fmt, ...)
{
    uint64_t wall = 0;
    (void)wall_now(c, &wall);                  /* 0 while the clock is unknown */
    int n = snprintf(c->line, sizeof(c->line), "{\"mono_ms\":%" PRIu64 ",\"wall_ms\":%" PRIu64 ",",
                     c->now, wall);
    if (n > 0 && (size_t)n < sizeof(c->line)) {
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(c->line + n, sizeof(c->line) - (size_t)n, fmt, ap);
        va_end(ap);
        n = m < 0 ? -1 : n + m;
    }
    if (n < 0 || (size_t)n + 3u > sizeof(c->line)) {
        /* Never half a line: a truncated object is not JSON. Say so instead. */
        c->lines_too_long++;
        n = snprintf(c->line, sizeof(c->line),
                     "{\"mono_ms\":%" PRIu64 ",\"wall_ms\":%" PRIu64
                     ",\"ev\":\"error\",\"what\":\"line too long\"",
                     c->now, wall);
    }
    c->line[n++] = '}';
    c->line[n++] = '\n';
    c->line[n] = '\0';
    c->ops.write_line(c->ops.ctx, c->line, (size_t)n);
}

void gs_console_json_escape(char *dst, size_t cap, const char *src)
{
    static const char hexd[] = "0123456789abcdef";
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    for (; *src != '\0'; src++) {
        unsigned char ch = (unsigned char)*src;
        char tmp[7];
        size_t k = 0;
        if (ch == '"' || ch == '\\') {
            tmp[k++] = '\\';
            tmp[k++] = (char)ch;
        } else if (ch < 0x20u || ch >= 0x7Fu) {
            /* Control bytes, and anything not ASCII: a callsign arrives raw
             * from the air, an input line raw from a serial port. */
            tmp[k++] = '\\';
            tmp[k++] = 'u';
            tmp[k++] = '0';
            tmp[k++] = '0';
            tmp[k++] = hexd[ch >> 4];
            tmp[k++] = hexd[ch & 0x0Fu];
        } else {
            tmp[k++] = (char)ch;
        }
        if (o + k >= cap) {
            break;
        }
        memcpy(dst + o, tmp, k);
        o += k;
    }
    dst[o] = '\0';
}

void gs_console_event(gs_console_t *c, uint64_t now, const char *ev, const char *fields)
{
    c->now = now;
    out(c, "\"ev\":\"%s\"%s%s", ev, fields != NULL ? "," : "", fields != NULL ? fields : "");
}

/* ---- frames ---- */

static void print_frame(gs_console_t *c, const gs_event_t *e)
{
    for (uint8_t i = 0; i < e->len; i++) {
        snprintf(c->hex + 2 * i, 3, "%02x", e->payload[i]);
    }
    c->hex[2 * e->len] = '\0';

    if (e->type == GAMA_FRAME_TM_HK) {
        gama_hk_t h;
        if (gama_hk_decode(e->payload, e->len, &h) > 0) {
            out(c, "\"ev\":\"hk\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"battery_mv\":%u,"
                "\"current_ma\":%d,\"temp_bat_ccel\":%d,\"temp_ext_ccel\":%d,\"temp_soc_ccel\":%d,"
                "\"roll_cdeg\":%d,\"pitch_cdeg\":%d,\"yaw_cdeg\":%d,\"adsb_msgs\":%" PRIu32 ","
                "\"obc_mode\":\"%s\",\"link\":\"%s\",\"uptime_s\":%" PRIu32 ",\"flags\":%u,"
                "\"payload\":\"%s\"",
                e->seq, e->rssi, e->snr, h.battery_mv, h.current_ma, h.temp_bat_ccel,
                h.temp_ext_ccel, h.temp_soc_ccel, h.roll_cdeg, h.pitch_cdeg, h.yaw_cdeg, h.adsb_msgs,
                gama_obc_state_name(h.obc_mode), gama_link_state_name(h.link_state),
                h.uptime_s, h.flags, c->hex);
            return;
        }
    } else if (e->type == GAMA_FRAME_TM_STAT) {
        gama_stat_t s;
        if (gama_stat_decode(e->payload, e->len, &s) > 0) {
            c->per.stat = s;
            c->per.stat_seen = true;
            out(c, "\"ev\":\"stat\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"msgs_received\":%" PRIu32 ","
                "\"msgs_decoded\":%" PRIu32 ",\"aircraft\":%u,\"tm_frames_sent\":%" PRIu32 ","
                "\"tc_frames_rx\":%" PRIu32 ",\"tc_frames_bad\":%" PRIu32 ","
                "\"latency_p95_ms\":%u,\"dump1090_restarts\":%u,\"payload\":\"%s\"",
                e->seq, e->rssi, e->snr, s.msgs_received, s.msgs_decoded, s.aircraft_tracked,
                s.tm_frames_sent, s.tc_frames_rx, s.tc_frames_bad, s.latency_p95_ms,
                s.dump1090_restarts, c->hex);
            return;
        }
    } else if (e->type == GAMA_FRAME_TM_ROSTER && e->len % GAMA_ROSTER_WIRE_LEN == 0) {
        size_t pos = 0;
        c->recs[0] = '\0';
        for (size_t r = 0; r < e->len / GAMA_ROSTER_WIRE_LEN; r++) {
            gama_roster_t ro;
            char callsign[6u * GAMA_CALLSIGN_LEN + 1u];
            gama_roster_decode(e->payload + r * GAMA_ROSTER_WIRE_LEN, GAMA_ROSTER_WIRE_LEN, &ro);
            gs_console_json_escape(callsign, sizeof(callsign), ro.callsign);
            int n = snprintf(c->recs + pos, sizeof(c->recs) - pos,
                             "%s{\"icao\":\"%06" PRIX32 "\",\"callsign\":\"%s\"}",
                             r == 0 ? "" : ",", ro.icao, callsign);
            if (n < 0 || (size_t)n >= sizeof(c->recs) - pos) {
                break;              /* cannot happen at 22 records; out() would say */
            }
            pos += (size_t)n;
        }
        out(c, "\"ev\":\"roster\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"aircraft\":[%s],"
            "\"payload\":\"%s\"", e->seq, e->rssi, e->snr, c->recs, c->hex);
        return;
    } else if (e->type == GAMA_FRAME_TM_TRACKS && e->len % GAMA_TRACK_WIRE_LEN == 0) {
        /* The update time of each record in this station's clock: arrival,
         * minus the frame's time on air, minus the record's age (ADR-0007). */
        uint32_t toa_ms = (gama_lora_toa_us(gama_lora_profile(c->link.profile),
                                            (uint8_t)(GAMA_FRAME_OVERHEAD + e->len)) + 500u) / 1000u;
        size_t pos = 0;
        c->recs[0] = '\0';
        /* Every record, always: at its widest one is under 150 characters,
         * and twelve fit c->recs. */
        for (size_t r = 0; r < e->len / GAMA_TRACK_WIRE_LEN; r++) {
            gama_track_t t;
            gama_track_decode(e->payload + r * GAMA_TRACK_WIRE_LEN, GAMA_TRACK_WIRE_LEN, &t);
            int n = snprintf(c->recs + pos, sizeof(c->recs) - pos,
                             "%s{\"icao\":\"%06" PRIX32 "\",\"lat\":%.5f,\"lon\":%.5f,"
                             "\"alt_ft\":%" PRId32 ",\"gs_kt\":%.1f,\"trk\":%.2f,"
                             "\"vr_fpm\":%" PRId32 ",\"age_ds\":%u,\"flags\":%u}",
                             r ? "," : "", t.icao, t.latitude, t.longitude, t.altitude_ft,
                             t.ground_speed_kt, t.track_deg, t.vertical_rate_fpm,
                             t.age_ds, t.flags);
            if (n < 0 || (size_t)n >= sizeof(c->recs) - pos) {
                break;
            }
            pos += (size_t)n;
        }
        out(c, "\"ev\":\"tracks\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"toa_ms\":%" PRIu32 ",\"n\":%zu,"
            "\"records\":[%s],\"payload\":\"%s\"",
            e->seq, e->rssi, e->snr, toa_ms, (size_t)(e->len / GAMA_TRACK_WIRE_LEN), c->recs,
            c->hex);
        return;
    }
    out(c, "\"ev\":\"frame\",\"type\":\"%s\",\"seq\":%u,\"rssi\":%d,\"snr\":%d,\"payload\":\"%s\"",
        gama_frame_type_name(e->type), e->seq, e->rssi, e->snr, c->hex);
}

/* ---- the activity in progress ---- */

static bool submit(gs_console_t *c, uint8_t cmd, const uint8_t *args, uint8_t n)
{
    uint32_t id = gs_submit(&c->link, c->now, cmd, args, n);
    if (id == 0) {
        out(c, "\"ev\":\"error\",\"what\":\"could not queue %s\"", gama_tc_name(cmd));
        return false;
    }
    c->a.kind = GS_AWAIT_ACK;
    c->a.id = id;
    return true;
}

static void next_ping(gs_console_t *c)
{
    if (c->a.remaining == 0) {
        uint64_t mean = c->a.acked ? c->a.lat_sum / (uint64_t)c->a.acked : 0;
        out(c, "\"ev\":\"ping_summary\",\"sent\":%d,\"acked\":%d,\"failed\":%d,"
            "\"attempts\":%" PRIu32 ",\"latency_min_ms\":%" PRIu64 ",\"latency_mean_ms\":%" PRIu64
            ",\"latency_max_ms\":%" PRIu64,
            c->a.total, c->a.acked, c->a.failed, c->a.attempts,
            c->a.acked ? c->a.lat_min : 0, mean, c->a.lat_max);
        c->a.kind = GS_IDLE;
        if (c->a.per) {
            c->per.phase = 3;
            c->per.stat_seen = false;
            submit(c, GAMA_TC_REQ_STAT, NULL, 0);
            c->a.kind = GS_WAITING;               /* until the STAT arrives */
            c->a.until = c->now + 30000u;
        }
        return;
    }
    c->a.remaining--;
    c->a.id = gs_submit(&c->link, c->now, GAMA_TC_PING, NULL, 0);
}

static void start_pings(gs_console_t *c, int n, bool per)
{
    memset(&c->a, 0, sizeof(c->a));
    c->a.kind = GS_PINGING;
    c->a.remaining = n;
    c->a.total = n;
    c->a.lat_min = UINT64_MAX;
    c->a.per = per;
    next_ping(c);
}

static void per_start(gs_console_t *c, int n)
{
    memset(&c->per, 0, sizeof(c->per));
    c->per.phase = 1;
    c->per.gs_sent0 = c->link.n.tc_sent;
    c->per.dl_frames0 = c->link.n.dl_frames;
    c->per.dl_lost0 = c->link.n.dl_lost;
    submit(c, GAMA_TC_REQ_STAT, NULL, 0);
    c->a.kind = GS_WAITING;
    c->a.until = c->now + 30000u;
    c->a.total = n;                               /* remembered for phase 2 */
}

static void per_finish(gs_console_t *c)
{
    /* Uplink: telecommand frames the satellite received, over those we sent
     * (the final REQ_STAT is among the sent, and counted by the satellite
     * before it answers, so both sides include it). Downlink: frames missing
     * from the satellite's sequence, over all it sent us. */
    const gs_counters_t *g = &c->link.n;
    uint32_t sent = g->tc_sent - c->per.gs_sent0;
    uint32_t got  = c->per.stat.tc_frames_rx - c->per.sat_rx0;
    uint32_t dl_n = (g->dl_frames - c->per.dl_frames0) + (g->dl_lost - c->per.dl_lost0);
    uint32_t dl_l = g->dl_lost - c->per.dl_lost0;
    out(c, "\"ev\":\"per\",\"profile\":\"%s\",\"uplink_sent\":%" PRIu32 ",\"uplink_received\":%" PRIu32
        ",\"uplink_per\":%.4f,\"downlink_frames\":%" PRIu32 ",\"downlink_lost\":%" PRIu32
        ",\"downlink_per\":%.4f",
        gama_rate_profile_name(c->link.profile), sent, got,
        sent ? 1.0 - (double)got / sent : 0.0, dl_n, dl_l, dl_n ? (double)dl_l / dl_n : 0.0);
    c->per.phase = 0;
    c->a.kind = GS_IDLE;
}

/* Called while WAITING in a PER measurement. */
static void per_step(gs_console_t *c)
{
    if (c->per.phase == 1 && c->per.stat_seen) {
        c->per.sat_rx0 = c->per.stat.tc_frames_rx;
        c->per.gs_sent0 = c->link.n.tc_sent;
        c->per.dl_frames0 = c->link.n.dl_frames;
        c->per.dl_lost0 = c->link.n.dl_lost;
        c->per.phase = 2;
        start_pings(c, c->a.total, true);
    } else if (c->per.phase == 3 && c->per.stat_seen) {
        per_finish(c);
    }
}

/* ---- the link's callbacks ---- */

static int g_tx(void *ctx, const uint8_t *f, size_t n)
{
    gs_console_t *c = ctx;
    return c->ops.radio_tx(c->ops.ctx, f, n);
}

static int g_prof(void *ctx, uint8_t p)
{
    gs_console_t *c = ctx;
    return c->ops.radio_set_profile(c->ops.ctx, p);
}

/* SET_TIME is stamped from this at each attempt. Without a wall clock it
 * fails, and the console never submits SET_TIME in that state. */
static int g_wall(void *ctx, uint32_t *s, uint32_t *ns)
{
    gs_console_t *c = ctx;
    uint64_t ms;
    if (!wall_now(c, &ms)) {
        return -1;
    }
    *s = (uint32_t)(ms / 1000u);
    *ns = (uint32_t)(ms % 1000u) * 1000000u;
    return 0;
}

static void submit_time(gs_console_t *c, bool as_command)
{
    uint64_t ms;
    if (!wall_now(c, &ms)) {
        out(c, "\"ev\":\"error\",\"what\":\"no wall clock: SET_TIME not sent (set clock first)\"");
        return;
    }
    uint8_t a[GAMA_IPC_TIME_LEN] = { 0 };           /* stamped at transmission */
    if (as_command) {
        submit(c, GAMA_TC_SET_TIME, a, GAMA_IPC_TIME_LEN);
    } else {
        /* Outside the command in progress: its ACK is logged like any
         * other, and the operator's commands are not held up by it. */
        uint32_t id = gs_submit(&c->link, c->now, GAMA_TC_SET_TIME, a, GAMA_IPC_TIME_LEN);
        out(c, "\"ev\":\"auto_time\",\"id\":%" PRIu32, id);
    }
}

static void g_event(void *ctx, uint64_t now, const gs_event_t *e)
{
    gs_console_t *c = ctx;
    c->now = now;
    switch (e->kind) {
    case GS_EV_ACK:
        out(c, "\"ev\":\"ack\",\"id\":%" PRIu32 ",\"cmd\":\"%s\",\"status\":\"%s\","
            "\"latency_ms\":%" PRIu32 ",\"attempts\":%u",
            e->id, gama_tc_name(e->cmd), gama_ack_status_name(e->status),
            e->latency_ms, e->attempts);
        if (e->id != 0 && c->a.kind == GS_AWAIT_ACK && e->id == c->a.id) {
            c->a.kind = GS_IDLE;
        } else if (e->id != 0 && c->a.kind == GS_PINGING && e->id == c->a.id) {
            c->a.acked++;
            c->a.attempts += e->attempts;
            c->a.lat_sum += e->latency_ms;
            if (e->latency_ms > c->a.lat_max) { c->a.lat_max = e->latency_ms; }
            if (e->latency_ms < c->a.lat_min) { c->a.lat_min = e->latency_ms; }
            next_ping(c);
        }
        break;
    case GS_EV_TC_FAILED:
        out(c, "\"ev\":\"tc_failed\",\"id\":%" PRIu32 ",\"cmd\":\"%s\",\"attempts\":%u",
            e->id, gama_tc_name(e->cmd), e->attempts);
        if (c->a.kind == GS_AWAIT_ACK && e->id == c->a.id) {
            c->a.kind = GS_IDLE;
        } else if (c->a.kind == GS_PINGING && e->id == c->a.id) {
            c->a.failed++;
            next_ping(c);
        }
        break;
    case GS_EV_FRAME:
        print_frame(c, e);
        break;
    case GS_EV_LOSS:
        out(c, "\"ev\":\"loss\",\"lost\":%" PRIu32, e->lost);
        break;
    case GS_EV_RATE:
        out(c, "\"ev\":\"rate\",\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\"",
            gama_rate_profile_name(e->from), gama_rate_profile_name(e->to), e->reason);
        break;
    case GS_EV_CONTACT:
        out(c, "\"ev\":\"contact\",\"up\":%s", e->up ? "true" : "false");
        if (e->up && c->auto_time) {
            submit_time(c, false);
        }
        break;
    default:
        break;
    }
}

/* ---- setup ---- */

void gs_console_init(gs_console_t *c, const gs_console_ops_t *ops, const char *radio)
{
    memset(c, 0, sizeof(*c));
    c->ops = *ops;
    c->radio = radio;
    gs_params_default(&c->params);
}

static int parse_profile(const char *v)
{
    return strcmp(v, "safe") == 0 ? GAMA_RATE_SAFE
         : strcmp(v, "nominal") == 0 ? GAMA_RATE_NOMINAL
         : strcmp(v, "fast") == 0 ? GAMA_RATE_FAST : -1;
}

/* A decimal number in [lo, hi], nothing trailing. */
static bool parse_real(const char *v, double lo, double hi, double *out_v)
{
    if (v == NULL) {
        return false;
    }
    char *end;
    double d = strtod(v, &end);
    if (end == v || *end != '\0' || !(d >= lo && d <= hi)) {
        return false;
    }
    *out_v = d;
    return true;
}

static bool parse_dbm(const char *v, int8_t *dbm)
{
    double d;
    if (!parse_real(v, 2.0, 20.0, &d) || d != (double)(int)d) {
        return false;
    }
    *dbm = (int8_t)d;
    return true;
}

int gs_console_set(gs_console_t *c, uint64_t now, const char *key, const char *value,
                   const char **why)
{
    c->now = now;
    if (key == NULL || value == NULL) {
        *why = "set KEY VALUE";
        return -1;
    }
    if (strcmp(key, "clock") == 0) {
        /* ms since 1970. Anything before 2026 is a mistake, not a clock. */
        char *end;
        unsigned long long ms = strtoull(value, &end, 10);
        if (end == value || *end != '\0' || ms < 1767225600000ull) {
            *why = "clock: milliseconds since 1970, after 2026-01-01";
            return -1;
        }
        c->clock_offset = (int64_t)ms - (int64_t)now;
        c->clock_set = true;
        return 0;
    }
    if (c->begun) {
        *why = "only clock can be set after begin";
        return -1;
    }
    if (strcmp(key, "profile") == 0) {
        int p = parse_profile(value);
        if (p < 0) {
            *why = "profile: safe|nominal|fast";
            return -1;
        }
        c->params.initial_profile = (uint8_t)p;
        return 0;
    }
    if (strcmp(key, "power") == 0) {
        int8_t dbm;
        if (!parse_dbm(value, &dbm)) {
            *why = "power: 2..20 dBm";
            return -1;
        }
        if (c->ops.radio_set_power == NULL || c->ops.radio_set_power(c->ops.ctx, dbm) != 0) {
            *why = "power: the radio refused";
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "lbt") == 0) {
        bool pre = strcmp(value, "preamble") == 0;
        if (!pre && strcmp(value, "header") != 0) {
            *why = "lbt: header|preamble";
            return -1;
        }
        if (c->ops.radio_set_lbt == NULL || c->ops.radio_set_lbt(c->ops.ctx, pre) != 0) {
            *why = "lbt: the radio refused";
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "auto_time") == 0) {
        if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) {
            *why = "auto_time: on|off";
            return -1;
        }
        c->auto_time = strcmp(value, "on") == 0;
        return 0;
    }
    *why = "unknown setting";
    return -1;
}

void gs_console_begin(gs_console_t *c, uint64_t now)
{
    c->now = now;
    gs_ops_t go = { .ctx = c, .radio_tx = g_tx, .radio_set_profile = g_prof,
                    .event = g_event, .wall_time = g_wall };
    gs_init(&c->link, &c->params, &go, now);
    c->begun = true;
    out(c, "\"ev\":\"start\",\"radio\":\"%s\",\"profile\":\"%s\"", c->radio,
        gama_rate_profile_name(c->params.initial_profile));
}

/* ---- commands ---- */

static int parse_event(const char *s)
{
    static const struct { const char *name; int ev; } names[] = {
        { "basic_inter", GAMA_OBC_EV_TC_BASIC_INTER }, { "aocs", GAMA_OBC_EV_TC_AOCS },
        { "mission_adsb", GAMA_OBC_EV_TC_MISSION_ADSB }, { "downlink", GAMA_OBC_EV_TC_DOWNLINK },
        { "survival", GAMA_OBC_EV_TC_SURVIVAL }, { "task_done", GAMA_OBC_EV_TASK_DONE },
        { "adsb_timeout", GAMA_OBC_EV_ADSB_TIMEOUT },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(s, names[i].name) == 0) { return names[i].ev; }
    }
    char *end;
    long v = strtol(s, &end, 10);
    return (*s != '\0' && *end == '\0' && v >= 1 && v <= 7) ? (int)v : -1;
}

static void setup_line(gs_console_t *c, const char *cmd, char *rest)
{
    if (strcmp(cmd, "begin") == 0) {
        gs_console_begin(c, c->now);
        return;
    }
    if (strcmp(cmd, "set") == 0) {
        char *key = strtok(rest, " \t\r\n");
        char *value = strtok(NULL, " \t\r\n");
        const char *why = "";
        char k[64], v[64];
        gs_console_json_escape(k, sizeof(k), key != NULL ? key : "");
        gs_console_json_escape(v, sizeof(v), value != NULL ? value : "");
        if (gs_console_set(c, c->now, key, value, &why) == 0) {
            out(c, "\"ev\":\"set\",\"key\":\"%s\",\"value\":\"%s\"", k, v);
        } else {
            out(c, "\"ev\":\"error\",\"what\":\"%s\"", why);
        }
        return;
    }
    out(c, "\"ev\":\"error\",\"what\":\"begin first: only set and begin before it\"");
}

void gs_console_line(gs_console_t *c, uint64_t now, char *line)
{
    c->now = now;
    char *cmd = strtok(line, " \t\r\n");
    if (cmd == NULL || cmd[0] == '#') {
        return;
    }
    if (!c->begun || strcmp(cmd, "set") == 0) {
        setup_line(c, cmd, strtok(NULL, ""));
        return;
    }
    char *arg = strtok(NULL, " \t\r\n");
    char text[2u * GS_CONSOLE_INPUT_MAX];
    char raw[GS_CONSOLE_INPUT_MAX + 2u];
    snprintf(raw, sizeof(raw), "%s%s%s", cmd, arg ? " " : "", arg ? arg : "");
    gs_console_json_escape(text, sizeof(text), raw);
    out(c, "\"ev\":\"command\",\"text\":\"%s\"", text);

    double d;
    if (strcmp(cmd, "ping") == 0) {
        int n = arg ? atoi(arg) : 1;
        start_pings(c, n > 0 ? n : 1, false);
    } else if (strcmp(cmd, "per") == 0 && arg != NULL && atoi(arg) > 0) {
        per_start(c, atoi(arg));
    } else if (strcmp(cmd, "hk") == 0) {
        submit(c, GAMA_TC_REQ_HK, NULL, 0);
    } else if (strcmp(cmd, "roster") == 0) {
        submit(c, GAMA_TC_REQ_ROSTER, NULL, 0);
    } else if (strcmp(cmd, "stat") == 0) {
        submit(c, GAMA_TC_REQ_STAT, NULL, 0);
    } else if (strcmp(cmd, "rate") == 0 && arg != NULL) {
        int p = parse_profile(arg);
        if (p < 0) { out(c, "\"ev\":\"error\",\"what\":\"rate safe|nominal|fast\""); return; }
        uint8_t pb = (uint8_t)p;
        submit(c, GAMA_TC_SET_RATE, &pb, 1);
    } else if (strcmp(cmd, "power") == 0 && arg != NULL) {
        int8_t dbm = (int8_t)atoi(arg);          /* the satellite checks the range */
        submit(c, GAMA_TC_SET_TX_POWER, (const uint8_t *)&dbm, 1);
    } else if (strcmp(cmd, "ground_power") == 0 && arg != NULL) {
        int8_t dbm;
        if (!parse_dbm(arg, &dbm)) {
            out(c, "\"ev\":\"error\",\"what\":\"ground_power: 2..20 dBm\"");
        } else if (c->link.tx_busy) {
            out(c, "\"ev\":\"error\",\"what\":\"ground_power: transmitting, try again\"");
        } else if (c->ops.radio_set_power == NULL ||
                   c->ops.radio_set_power(c->ops.ctx, dbm) != 0) {
            out(c, "\"ev\":\"error\",\"what\":\"ground_power: the radio refused\"");
        }
    } else if (strcmp(cmd, "mode") == 0 && arg != NULL) {
        int ev = parse_event(arg);
        if (ev < 0) { out(c, "\"ev\":\"error\",\"what\":\"unknown OBC event\""); return; }
        uint8_t e = (uint8_t)ev;
        submit(c, GAMA_TC_SET_MODE, &e, 1);
    } else if (strcmp(cmd, "stream") == 0 && arg != NULL) {
        if (!parse_real(arg, 0.0, 6553.5, &d)) {
            out(c, "\"ev\":\"error\",\"what\":\"stream SECONDS\"");
            return;
        }
        uint8_t a[2];
        gama_put_u16(a, (uint16_t)(d * 10.0));
        submit(c, GAMA_TC_STREAM_START, a, 2);
    } else if (strcmp(cmd, "stop") == 0) {
        submit(c, GAMA_TC_STREAM_STOP, NULL, 0);
    } else if (strcmp(cmd, "time") == 0) {
        submit_time(c, true);
    } else if (strcmp(cmd, "shutdown") == 0) {
        if (arg == NULL || strcmp(arg, "confirm") != 0) {
            out(c, "\"ev\":\"error\",\"what\":\"type: shutdown confirm\"");
            return;
        }
        uint8_t a[2];
        gama_put_u16(a, GAMA_TC_SHUTDOWN_MAGIC);
        submit(c, GAMA_TC_SHUTDOWN, a, 2);
    } else if (strcmp(cmd, "deafen") == 0 && arg != NULL) {
        if (!parse_real(arg, 0.0, 86400.0, &d)) {
            out(c, "\"ev\":\"error\",\"what\":\"deafen SECONDS\"");
            return;
        }
        c->deaf_until = now + (uint64_t)(d * 1000.0);  /* returns at once */
    } else if (strcmp(cmd, "wait") == 0 && arg != NULL) {
        if (!parse_real(arg, 0.0, 86400.0, &d)) {
            out(c, "\"ev\":\"error\",\"what\":\"wait SECONDS\"");
            return;
        }
        c->a.kind = GS_WAITING;
        c->a.until = now + (uint64_t)(d * 1000.0);
    } else if (strcmp(cmd, "quit") == 0) {
        c->quit = true;
    } else {
        out(c, "\"ev\":\"error\",\"what\":\"unknown command\"");
    }
}

void gs_console_eof(gs_console_t *c)
{
    c->quit = true;
}

bool gs_console_wants_line(const gs_console_t *c)
{
    return !c->quit && c->a.kind == GS_IDLE;
}

bool gs_console_finished(const gs_console_t *c)
{
    return c->quit && c->a.kind == GS_IDLE;
}

/* ---- the radio's events ---- */

void gs_console_on_rx(gs_console_t *c, uint64_t now, const uint8_t *buf, size_t len,
                      int16_t rssi, int8_t snr)
{
    c->now = now;
    if (!c->begun) {
        return;
    }
    if (now < c->deaf_until) {
        c->deaf_dropped++;
        out(c, "\"ev\":\"deaf_drop\",\"type\":\"%s\"",
            len >= 2u ? gama_frame_type_name(buf[1]) : "SHORT");
        return;
    }
    gs_on_rx(&c->link, now, buf, len, rssi, snr);
}

void gs_console_on_rx_error(gs_console_t *c, uint64_t now)
{
    c->now = now;
    if (!c->begun) {
        return;
    }
    gs_on_rx_error(&c->link, now);
    out(c, "\"ev\":\"rx_crc_error\"");
}

void gs_console_on_tx_done(gs_console_t *c, uint64_t now)
{
    c->now = now;
    if (c->begun) {
        gs_on_tx_done(&c->link, now);
    }
}

void gs_console_on_tick(gs_console_t *c, uint64_t now)
{
    c->now = now;
    if (!c->begun) {
        return;
    }
    gs_on_tick(&c->link, now);
    if (c->a.kind == GS_WAITING) {
        if (c->per.phase != 0) {
            per_step(c);
        }
        if (c->a.kind == GS_WAITING && now >= c->a.until) {
            if (c->per.phase != 0) {
                out(c, "\"ev\":\"error\",\"what\":\"no TM_STAT answer; PER incomplete\"");
                c->per.phase = 0;
            }
            c->a.kind = GS_IDLE;
        }
    }
}

uint64_t gs_console_next_deadline(const gs_console_t *c)
{
    if (!c->begun) {
        return UINT64_MAX;
    }
    uint64_t d = gs_next_deadline(&c->link);
    if (c->a.kind == GS_WAITING && c->a.until < d) {
        d = c->a.until;
    }
    return d;
}

void gs_console_summary(gs_console_t *c, uint64_t now)
{
    c->now = now;
    const gs_counters_t *g = &c->link.n;
    out(c, "\"ev\":\"summary\",\"dl_frames\":%" PRIu32 ",\"dl_lost\":%" PRIu32 ",\"dl_bad\":%" PRIu32
        ",\"tc_sent\":%" PRIu32 ",\"tc_acked\":%" PRIu32 ",\"tc_failed\":%" PRIu32
        ",\"lbt_defers\":%" PRIu32,
        g->dl_frames, g->dl_lost, g->dl_bad, g->tc_sent, g->tc_acked, g->tc_failed,
        g->lbt_defers);
}
