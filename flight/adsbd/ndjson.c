#include "ndjson.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

static bool append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static bool append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    if (*pos >= cap) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos) {
        return false;
    }
    *pos += (size_t)n;
    return true;
}

int adsbd_ndjson_format(char *buf, size_t cap, const sbs_msg_t *m, uint64_t rx_epoch_ns)
{
    if (buf == NULL || m == NULL) {
        return -1;
    }
    size_t pos = 0;
    /* The callsign passed sbs_parse(): [A-Z0-9 ] only, nothing to escape. */
    bool ok = append(buf, cap, &pos,
                     "{\"icao\":\"%06" PRIX32 "\",\"rx_epoch_ns\":%" PRIu64 ","
                     "\"transmission_type\":%u,\"callsign\":\"%s\"",
                     m->icao, rx_epoch_ns, (unsigned)m->type,
                     (m->have & SBS_HAVE_CALLSIGN) ? m->callsign : "");
    if (ok && (m->have & SBS_HAVE_ALTITUDE)) {
        ok = append(buf, cap, &pos, ",\"altitude_ft\":%" PRId32, m->altitude_ft);
    }
    if (ok && (m->have & SBS_HAVE_SPEED)) {
        ok = append(buf, cap, &pos, ",\"ground_speed_kt\":%.1f", m->ground_speed_kt);
    }
    if (ok && (m->have & SBS_HAVE_TRACK)) {
        ok = append(buf, cap, &pos, ",\"track_deg\":%.1f", m->track_deg);
    }
    if (ok && (m->have & SBS_HAVE_POSITION)) {
        ok = append(buf, cap, &pos, ",\"lat\":%.6f,\"lon\":%.6f", m->lat, m->lon);
    }
    if (ok && (m->have & SBS_HAVE_VRATE)) {
        ok = append(buf, cap, &pos, ",\"vertical_rate_fpm\":%" PRId32, m->vertical_rate_fpm);
    }
    if (ok && (m->have & SBS_HAVE_SQUAWK)) {
        ok = append(buf, cap, &pos, ",\"squawk\":\"%s\"", m->squawk);
    }
    if (ok) {
        ok = append(buf, cap, &pos, ",\"on_ground\":%d}\n", (int)m->on_ground);
    }
    return ok ? (int)pos : -1;
}
