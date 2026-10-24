/*
 * core.c — see core.h.
 *
 * What runs in each mode (CDR Table 55 and Figure 44). The payload follows
 * the mode on its own, through ttcd (ADR-0014); here are only the OBC's
 * actions:
 *
 *   BASIC_INTERMEDIATE  the burn-wire, once per entry, for burnwire_on ms;
 *                       then EV_TASK_DONE (D3). Never at boot.
 *   ADVANCED_AOCS       the ESP32's task (PLANO 2.4, not yet here); at
 *                       aocs_timeout, EV_TASK_DONE even without its answer.
 *   MISSION_ADSB        EV_ADSB_TIMEOUT adsb_timeout after entry (OBC-6).
 *   every mode          the heaters' thermostat (D4) and IPC_TELEMETRY
 *                       every second (D6).
 *
 * Leaving BASIC_INTERMEDIATE by any edge turns the burn-wire off: a ground
 * telecommand that ends the mode early must not leave it burning.
 */

#include "core.h"
#include "gama_frame.h"
#include "gama_tc.h"
#include "gama_tm.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void obcd_params_default(obcd_params_t *p)
{
    p->telemetry_period = 1000;
    p->adsb_timeout     = 600000;
    p->burnwire_on      = 5000;
    p->aocs_timeout     = 300000;   /* placeholder until the AOCS contract (2.4) */
    p->heater_on_below  = 1000;
    p->heater_off_above = 1200;
    p->temp_stale       = 10000;
    p->power_stale      = 5000;
    p->report_period    = 60000;
}

static void core_log(obcd_core_t *c, uint64_t now, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void core_log(obcd_core_t *c, uint64_t now, const char *event, const char *fmt, ...)
{
    if (c->ops.log == NULL) {
        return;
    }
    char fields[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fields, sizeof(fields), fmt, ap);
    va_end(ap);
    c->ops.log(c->ops.ctx, now, event, fields);
}

static void send_frame(obcd_core_t *c, uint64_t now, uint8_t type,
                       const uint8_t *payload, uint8_t len)
{
    if (!c->ipc_up) {
        return;
    }
    uint8_t frame[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(frame, sizeof(frame), type, c->ipc_seq, payload, len);
    if (n < 0) {
        return;
    }
    c->ipc_seq++;
    if (c->ops.ipc_send(c->ops.ctx, frame, (size_t)n) != 0) {
        c->n.ipc_tx_drops++;
        core_log(c, now, "ipc_drop", "\"type\":\"%s\"", gama_frame_type_name(type));
    }
}

static void send_mode(obcd_core_t *c, uint64_t now)
{
    send_frame(c, now, GAMA_FRAME_IPC_MODE, &c->mode, GAMA_IPC_MODE_LEN);
}

static void send_telemetry(obcd_core_t *c, uint64_t now)
{
    gama_ipc_telemetry_t t;
    obcd_telemetry(c, now, &t);
    uint8_t p[GAMA_IPC_TELEMETRY_LEN];
    gama_ipc_telemetry_encode(p, sizeof(p), &t);
    send_frame(c, now, GAMA_FRAME_IPC_TELEMETRY, p, GAMA_IPC_TELEMETRY_LEN);
}

/* -------------------------------- outputs ------------------------------- */

static void set_burnwire(obcd_core_t *c, uint64_t now, bool on, const char *why)
{
    int r = c->ops.burnwire_set != NULL ? c->ops.burnwire_set(c->ops.ctx, on) : -1;
    if (r != 0) {
        c->n.output_failures++;
    }
    c->burnwire_on = on;
    core_log(c, now, "burnwire", "\"on\":%s,\"why\":\"%s\",\"driven\":%s",
             on ? "true" : "false", why, r == 0 ? "true" : "false");
}

static void set_heater(obcd_core_t *c, uint64_t now, bool on, const char *why)
{
    int r = c->ops.heater_set != NULL ? c->ops.heater_set(c->ops.ctx, on) : -1;
    if (r != 0) {
        c->n.output_failures++;
    }
    bool changed = on != c->heater_on;
    c->heater_on = on;
    if (changed || r != 0) {
        core_log(c, now, "heater", "\"on\":%s,\"why\":\"%s\",\"temp_ccel\":%d,\"driven\":%s",
                 on ? "true" : "false", why,
                 c->have_temp ? c->temp_bat_ccel : GAMA_NO_DATA_I16,
                 r == 0 ? "true" : "false");
    }
}

/* ------------------------------ the modes ------------------------------- */

uint8_t obcd_transition(uint8_t mode, uint8_t event)
{
    switch (mode) {
    case GAMA_OBC_ST_PRE_TEST:
        switch (event) {
        case GAMA_OBC_EV_TC_BASIC_INTER:  return GAMA_OBC_ST_BASIC_INTERMEDIATE;
        case GAMA_OBC_EV_TC_AOCS:         return GAMA_OBC_ST_ADVANCED_AOCS;
        case GAMA_OBC_EV_TC_SURVIVAL:     return GAMA_OBC_ST_ENV_SURVIVAL;
        case GAMA_OBC_EV_TC_MISSION_ADSB: return GAMA_OBC_ST_MISSION_ADSB;
        case GAMA_OBC_EV_TC_DOWNLINK:     return GAMA_OBC_ST_MISSION_DOWNLINK;
        default:                          return GAMA_OBC_ST_UNKNOWN;
        }
    case GAMA_OBC_ST_MISSION_ADSB:
        return (event == GAMA_OBC_EV_ADSB_TIMEOUT || event == GAMA_OBC_EV_TC_DOWNLINK)
                   ? GAMA_OBC_ST_MISSION_DOWNLINK : GAMA_OBC_ST_UNKNOWN;
    case GAMA_OBC_ST_BASIC_INTERMEDIATE:
    case GAMA_OBC_ST_ADVANCED_AOCS:
    case GAMA_OBC_ST_ENV_SURVIVAL:
    case GAMA_OBC_ST_MISSION_DOWNLINK:
        return event == GAMA_OBC_EV_TASK_DONE ? GAMA_OBC_ST_PRE_TEST : GAMA_OBC_ST_UNKNOWN;
    default:
        return GAMA_OBC_ST_UNKNOWN;
    }
}

static void leave(obcd_core_t *c, uint64_t now)
{
    if (c->mode == GAMA_OBC_ST_BASIC_INTERMEDIATE && c->burnwire_on) {
        set_burnwire(c, now, false, "mode left");
    }
}

static void enter(obcd_core_t *c, uint64_t now)
{
    switch (c->mode) {
    case GAMA_OBC_ST_BASIC_INTERMEDIATE:
        /* D3: one attempt per entry; the pulse's end is the task's end. */
        c->burnwire_until = now + c->p.burnwire_on;
        set_burnwire(c, now, true, "deploy");
        break;
    case GAMA_OBC_ST_ADVANCED_AOCS:
        /* PLANO 2.4: the command to the ESP32 goes here. Until then the
         * deadline alone ends the mode, as it will when the ESP32 is silent. */
        c->aocs_deadline = now + c->p.aocs_timeout;
        break;
    case GAMA_OBC_ST_MISSION_ADSB:
        c->adsb_deadline = now + c->p.adsb_timeout;      /* OBC-6 */
        break;
    default:
        break;
    }
}

void obcd_post_event(obcd_core_t *c, uint64_t now, uint8_t event, const char *by)
{
    c->n.events++;
    uint8_t to = obcd_transition(c->mode, event);
    if (to == GAMA_OBC_ST_UNKNOWN) {
        /* D1: not valid in this mode. Refused, and said so: the ground sees
         * the mode unchanged in housekeeping, the reason is here. */
        c->n.refused++;
        core_log(c, now, "event_refused", "\"event\":\"%s\",\"mode\":\"%s\",\"by\":\"%s\"",
                 gama_obc_event_name(event), gama_obc_state_name(c->mode), by);
        return;
    }
    uint8_t from = c->mode;
    uint64_t lasted = now - c->mode_since;
    leave(c, now);
    c->mode = to;
    c->mode_since = now;
    c->n.transitions++;
    /* OBC-5: every transition of Figure 44, in the log. */
    core_log(c, now, "mode",
             "\"from\":\"%s\",\"to\":\"%s\",\"event\":\"%s\",\"by\":\"%s\",\"after_ms\":%" PRIu64,
             gama_obc_state_name(from), gama_obc_state_name(to),
             gama_obc_event_name(event), by, lasted);
    send_mode(c, now);
    enter(c, now);
}

/* ------------------------------ thermostat ------------------------------ */

/* D4: hysteresis between heater_on_below and heater_off_above, in every
 * mode. Without a valid reading the heaters are off: a failed sensor must
 * not leave them heating a battery nobody is measuring. */
static void thermostat(obcd_core_t *c, uint64_t now)
{
    bool valid = c->have_temp && now - c->temp_at <= c->p.temp_stale;
    if (!valid) {
        if (!c->temp_fault) {
            c->temp_fault = true;
            core_log(c, now, "temp_fault", "\"have\":%s,\"age_ms\":%" PRIu64,
                     c->have_temp ? "true" : "false", c->have_temp ? now - c->temp_at : 0u);
        }
        if (c->heater_on) {
            set_heater(c, now, false, "sensor fault");
        }
        return;
    }
    if (c->temp_fault) {
        c->temp_fault = false;
        core_log(c, now, "temp_ok", "\"temp_ccel\":%d", c->temp_bat_ccel);
    }
    if (!c->heater_on && c->temp_bat_ccel < c->p.heater_on_below) {
        set_heater(c, now, true, "cold");
    } else if (c->heater_on && c->temp_bat_ccel > c->p.heater_off_above) {
        set_heater(c, now, false, "warm");
    }
}

/* ------------------------------- the API -------------------------------- */

void obcd_init(obcd_core_t *c, const obcd_params_t *p, const obcd_ops_t *ops, uint64_t now)
{
    memset(c, 0, sizeof(*c));
    c->p = *p;
    c->ops = *ops;
    c->boot_ms = now;
    c->mode = GAMA_OBC_ST_PRE_TEST;      /* D5: boot and restart in PRE_TEST */
    c->mode_since = now;
    c->next_telemetry = now;
    c->next_report = now + p->report_period;
    /* Both outputs off at start, whatever a previous run left them at:
     * the burn-wire never fires at boot (D3). */
    set_burnwire(c, now, false, "boot");
    c->heater_on = true;                 /* so that the next call is logged */
    set_heater(c, now, false, "boot");
    core_log(c, now, "boot",
             "\"mode\":\"PRE_TEST\",\"adsb_timeout_ms\":%" PRIu32 ",\"burnwire_ms\":%" PRIu32
             ",\"aocs_timeout_ms\":%" PRIu32 ",\"heater_on_below_ccel\":%d"
             ",\"heater_off_above_ccel\":%d",
             p->adsb_timeout, p->burnwire_on, p->aocs_timeout,
             p->heater_on_below, p->heater_off_above);
}

void obcd_on_ipc_link(obcd_core_t *c, uint64_t now, bool up)
{
    c->ipc_up = up;
    if (up) {
        send_mode(c, now);
        send_telemetry(c, now);
        c->next_telemetry = now + c->p.telemetry_period;
    }
}

void obcd_on_ipc_frame(obcd_core_t *c, uint64_t now, const uint8_t *buf, size_t len)
{
    gama_frame_t f;
    if (gama_frame_decode(buf, len, &f) < 0) {
        c->n.ipc_rx_bad++;
        core_log(c, now, "ipc_unexpected", "\"what\":\"undecodable\",\"len\":%zu", len);
        return;
    }
    if (f.type == GAMA_FRAME_IPC_TC_EVENT && f.len == GAMA_IPC_TC_EVENT_LEN) {
        obcd_post_event(c, now, f.payload[0], "ttcd");
        return;
    }
    if (f.type == GAMA_FRAME_IPC_TIME_SET) {
        gama_ipc_time_t t;
        if (gama_ipc_time_decode(f.payload, f.len, &t) < 0 || t.nsec >= 1000000000u) {
            c->n.ipc_rx_bad++;
            core_log(c, now, "ipc_unexpected", "\"what\":\"bad time\"");
            return;
        }
        /* ADR-0009: the log's monotonic stamps become wall-clock time with
         * this pair; nothing sets the system clock. */
        c->time_synced = true;
        core_log(c, now, "time_anchor", "\"unix_s\":%" PRIu32 ",\"nsec\":%" PRIu32,
                 t.unix_s, t.nsec);
        return;
    }
    c->n.ipc_rx_bad++;
    core_log(c, now, "ipc_unexpected", "\"type\":\"%s\"", gama_frame_type_name(f.type));
}

void obcd_on_power(obcd_core_t *c, uint64_t now, bool ok, uint16_t battery_mv,
                   int16_t current_ma)
{
    if (!ok) {
        if (c->n.power_failures++ == 0) {
            core_log(c, now, "power_fault", "%s", "\"first\":true");
        }
        return;
    }
    c->have_power = true;
    c->power_at = now;
    c->battery_mv = battery_mv;
    c->current_ma = current_ma;
}

void obcd_on_battery_temp(obcd_core_t *c, uint64_t now, bool ok, int16_t ccel)
{
    if (ok) {
        c->have_temp = true;
        c->temp_at = now;
        c->temp_bat_ccel = ccel;
    } else {
        c->n.temp_failures++;
    }
    thermostat(c, now);
}

void obcd_telemetry(const obcd_core_t *c, uint64_t now, gama_ipc_telemetry_t *t)
{
    bool power = c->have_power && now - c->power_at <= c->p.power_stale;
    bool temp = c->have_temp && now - c->temp_at <= c->p.temp_stale;
    t->battery_mv    = power ? c->battery_mv : GAMA_NO_DATA_U16;
    t->current_ma    = power ? c->current_ma : GAMA_NO_DATA_I16;
    t->temp_bat_ccel = temp ? c->temp_bat_ccel : GAMA_NO_DATA_I16;   /* HLR-EPS-04 */
    /* No external temperature sensor on the boards as fabricated (A7), and
     * the attitude comes from the AOCS (PLANO 2.4). */
    t->temp_ext_ccel = GAMA_NO_DATA_I16;
    t->roll_cdeg     = GAMA_NO_DATA_I16;
    t->pitch_cdeg    = GAMA_NO_DATA_I16;
    t->yaw_cdeg      = GAMA_NO_DATA_I16;
}

void obcd_on_tick(obcd_core_t *c, uint64_t now)
{
    if (c->mode == GAMA_OBC_ST_BASIC_INTERMEDIATE && c->burnwire_on && now >= c->burnwire_until) {
        set_burnwire(c, now, false, "pulse done");
        obcd_post_event(c, now, GAMA_OBC_EV_TASK_DONE, "burnwire");
    }
    if (c->mode == GAMA_OBC_ST_MISSION_ADSB && now >= c->adsb_deadline) {
        obcd_post_event(c, now, GAMA_OBC_EV_ADSB_TIMEOUT, "timer");
    }
    if (c->mode == GAMA_OBC_ST_ADVANCED_AOCS && now >= c->aocs_deadline) {
        obcd_post_event(c, now, GAMA_OBC_EV_TASK_DONE, "aocs_deadline");
    }
    if (now >= c->next_telemetry) {
        send_telemetry(c, now);
        c->next_telemetry += c->p.telemetry_period;
        if (c->next_telemetry <= now) {
            c->next_telemetry = now + c->p.telemetry_period;
        }
    }
    /* A sensor that stops reporting is noticed here, not only on its next
     * (failed) reading: the thread reading it may itself be stuck. */
    thermostat(c, now);
    if (now >= c->next_report) {
        gama_ipc_telemetry_t t;
        obcd_telemetry(c, now, &t);
        core_log(c, now, "status",
                 "\"mode\":\"%s\",\"battery_mv\":%u,\"current_ma\":%d,\"temp_bat_ccel\":%d"
                 ",\"heater\":%s,\"events\":%" PRIu32 ",\"refused\":%" PRIu32
                 ",\"power_failures\":%" PRIu32 ",\"temp_failures\":%" PRIu32
                 ",\"output_failures\":%" PRIu32 ",\"ipc_tx_drops\":%" PRIu32,
                 gama_obc_state_name(c->mode), t.battery_mv, t.current_ma, t.temp_bat_ccel,
                 c->heater_on ? "true" : "false", c->n.events, c->n.refused,
                 c->n.power_failures, c->n.temp_failures, c->n.output_failures,
                 c->n.ipc_tx_drops);
        c->next_report = now + c->p.report_period;
    }
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

uint64_t obcd_next_deadline(const obcd_core_t *c)
{
    uint64_t d = min_u64(c->next_telemetry, c->next_report);
    if (c->mode == GAMA_OBC_ST_BASIC_INTERMEDIATE && c->burnwire_on) {
        d = min_u64(d, c->burnwire_until);
    }
    if (c->mode == GAMA_OBC_ST_MISSION_ADSB) {
        d = min_u64(d, c->adsb_deadline);
    }
    if (c->mode == GAMA_OBC_ST_ADVANCED_AOCS) {
        d = min_u64(d, c->aocs_deadline);
    }
    if (c->have_temp && !c->temp_fault) {
        d = min_u64(d, c->temp_at + c->p.temp_stale + 1u);
    }
    return d;
}
