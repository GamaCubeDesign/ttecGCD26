#include "sbs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SBS_FIELDS 22

typedef struct {
    const char *p;
    size_t      n;
} field_t;

/* Splits on commas, keeping empty fields. Fields past `max` are ignored. */
static size_t split(const char *line, size_t len, field_t *f, size_t max)
{
    size_t count = 0, start = 0;
    for (size_t i = 0; i <= len && count < max; i++) {
        if (i == len || line[i] == ',') {
            f[count].p = line + start;
            f[count].n = i - start;
            count++;
            start = i + 1;
        }
    }
    return count;
}

/* Field i, or an empty one when the line stopped short of it. */
static field_t at(const field_t *f, size_t n, size_t i)
{
    field_t empty = { "", 0 };
    return i < n ? f[i] : empty;
}

static bool is(field_t f, const char *s)
{
    size_t n = strlen(s);
    return f.n == n && memcmp(f.p, s, n) == 0;
}

/* Decimal numbers only: no exponent, no "inf", no "nan", no spaces. */
static bool plain_number(field_t f, bool allow_point)
{
    if (f.n == 0) {
        return false;
    }
    for (size_t i = 0; i < f.n; i++) {
        char c = f.p[i];
        bool sign = (c == '-' || c == '+') && i == 0;
        if (!((c >= '0' && c <= '9') || sign || (allow_point && c == '.'))) {
            return false;
        }
    }
    return true;
}

static bool to_long(field_t f, long lo, long hi, long *out)
{
    char buf[24];
    if (!plain_number(f, false) || f.n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, f.p, f.n);
    buf[f.n] = '\0';
    char *end;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static bool to_double(field_t f, double lo, double hi, double *out)
{
    char buf[32];
    if (!plain_number(f, true) || f.n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, f.p, f.n);
    buf[f.n] = '\0';
    char *end;
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0' || !(v >= lo && v <= hi)) {
        return false;
    }
    *out = v;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

/* The other BaseStation message types. dump1090-fa writes only MSG, but a
 * line of another type is well-formed SBS, not garbage. */
static bool other_type(field_t f)
{
    return is(f, "SEL") || is(f, "ID") || is(f, "AIR") || is(f, "STA") || is(f, "CLK");
}

sbs_result_t sbs_parse(const char *line, size_t len, sbs_msg_t *m)
{
    if (line == NULL || m == NULL) {
        return SBS_ERR_FORMAT;
    }
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    field_t f[SBS_FIELDS];
    size_t n = split(line, len, f, SBS_FIELDS);
    if (!is(f[0], "MSG")) {
        return other_type(f[0]) ? SBS_IGNORED : SBS_ERR_FORMAT;
    }
    if (n < 5) {
        return SBS_ERR_FORMAT;
    }

    memset(m, 0, sizeof(*m));
    m->on_ground = -1;

    long v;
    if (!to_long(f[1], 1, 8, &v)) {
        return SBS_ERR_TYPE;
    }
    m->type = (uint8_t)v;

    field_t hex = f[4];
    if (hex.n > 0 && hex.p[0] == '~') {
        return SBS_NON_ICAO;
    }
    if (hex.n != 6) {
        return SBS_ERR_ICAO;
    }
    for (size_t i = 0; i < 6; i++) {
        int d = hex_digit(hex.p[i]);
        if (d < 0) {
            return SBS_ERR_ICAO;
        }
        m->icao = (m->icao << 4) | (uint32_t)d;
    }

    /* Callsign: dump1090 pads it with spaces to eight characters. Only the
     * ICAO character set is accepted, which also keeps it safe to write
     * into a JSON string unescaped. */
    field_t cs = at(f, n, 10);
    while (cs.n > 0 && cs.p[cs.n - 1] == ' ') {
        cs.n--;
    }
    if (cs.n > 0) {
        if (cs.n > 8) {
            return SBS_ERR_VALUE;
        }
        for (size_t i = 0; i < cs.n; i++) {
            char c = cs.p[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')) {
                return SBS_ERR_VALUE;
            }
            m->callsign[i] = c;
        }
        m->callsign[cs.n] = '\0';
        m->have |= SBS_HAVE_CALLSIGN;
    }

    field_t alt = at(f, n, 11);
    if (alt.n > 0) {
        if (!to_long(alt, -3000, 200000, &v)) {
            return SBS_ERR_VALUE;
        }
        m->altitude_ft = (int32_t)v;
        m->have |= SBS_HAVE_ALTITUDE;
    }
    field_t gs = at(f, n, 12);
    if (gs.n > 0) {
        if (!to_double(gs, 0.0, 5000.0, &m->ground_speed_kt)) {
            return SBS_ERR_VALUE;
        }
        m->have |= SBS_HAVE_SPEED;
    }
    field_t trk = at(f, n, 13);
    if (trk.n > 0) {
        if (!to_double(trk, 0.0, 360.0, &m->track_deg)) {
            return SBS_ERR_VALUE;
        }
        m->have |= SBS_HAVE_TRACK;
    }
    field_t lat = at(f, n, 14), lon = at(f, n, 15);
    if (lat.n > 0 || lon.n > 0) {
        /* A position is a pair: half of one is a corrupt line. */
        if (!to_double(lat, -90.0, 90.0, &m->lat) ||
            !to_double(lon, -180.0, 180.0, &m->lon)) {
            return SBS_ERR_VALUE;
        }
        m->have |= SBS_HAVE_POSITION;
    }
    field_t vr = at(f, n, 16);
    if (vr.n > 0) {
        if (!to_long(vr, -100000, 100000, &v)) {
            return SBS_ERR_VALUE;
        }
        m->vertical_rate_fpm = (int32_t)v;
        m->have |= SBS_HAVE_VRATE;
    }
    field_t sq = at(f, n, 17);
    if (sq.n > 0) {
        if (sq.n != 4 || !plain_number(sq, false) || sq.p[0] == '-' || sq.p[0] == '+') {
            return SBS_ERR_VALUE;
        }
        memcpy(m->squawk, sq.p, 4);
        m->squawk[4] = '\0';
        m->have |= SBS_HAVE_SQUAWK;
    }
    field_t gnd = at(f, n, 21);
    if (gnd.n > 0) {
        if (is(gnd, "-1") || is(gnd, "1")) {
            m->on_ground = 1;
        } else if (is(gnd, "0")) {
            m->on_ground = 0;
        } else {
            return SBS_ERR_VALUE;
        }
        m->have |= SBS_HAVE_GROUND;
    }
    return SBS_OK;
}

const char *sbs_result_name(int result)
{
    switch (result) {
    case SBS_OK:         return "OK";
    case SBS_IGNORED:    return "IGNORED";
    case SBS_NON_ICAO:   return "NON_ICAO";
    case SBS_ERR_FORMAT: return "ERR_FORMAT";
    case SBS_ERR_TYPE:   return "ERR_TYPE";
    case SBS_ERR_ICAO:   return "ERR_ICAO";
    case SBS_ERR_VALUE:  return "ERR_VALUE";
    default:             return "UNKNOWN";
    }
}
