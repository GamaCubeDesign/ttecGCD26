#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void obcd_config_default(obcd_config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->ipc_path, sizeof(c->ipc_path), "/run/gama/ttec.sock");
    snprintf(c->log_path, sizeof(c->log_path), "/var/lib/gama/obcd.jsonl");
    c->log_max_bytes = 64u * 1024u * 1024u;
    c->retry_ms = 1000;
    c->ina219_addr = 0x40;               /* the chip's default; A0 = A1 = GND */
    c->ina219_shunt_mohm = 100;          /* the previous OBC's calibration     */
    c->power_period_ms = 1000;
    c->temp_period_ms = 2000;            /* a conversion takes ~750 ms         */
    snprintf(c->gpio_chip, sizeof(c->gpio_chip), "/dev/gpiochip0");
    c->burnwire_line = OBCD_LINE_NONE;
    c->heater_line = OBCD_LINE_NONE;
    obcd_params_default(&c->p);
}

static int fail(char *err, size_t n, const char *fmt, const char *a, const char *b)
{
    if (err != NULL && n > 0) {
        snprintf(err, n, fmt, a, b);
    }
    return -1;
}

static int set_str(char *dst, size_t cap, const char *key, const char *v,
                   char *err, size_t n)
{
    if (strlen(v) >= cap) {
        return fail(err, n, "%s: value too long: %s", key, v);
    }
    snprintf(dst, cap, "%s", v);
    return 0;
}

/* Integer in [lo, hi]: decimal, or hexadecimal with 0x; nothing trailing. */
static int set_i(int64_t *dst, int64_t lo, int64_t hi, const char *key,
                 const char *v, char *err, size_t n)
{
    if (*v == '\0' || isspace((unsigned char)*v)) {
        return fail(err, n, "%s: not a number: %s", key, v);
    }
    char *end;
    errno = 0;
    long long x = strtoll(v, &end, 0);
    if (errno != 0 || *end != '\0') {
        return fail(err, n, "%s: not a number: %s", key, v);
    }
    if (x < lo || x > hi) {
        return fail(err, n, "%s: out of range: %s", key, v);
    }
    *dst = x;
    return 0;
}

#define INT(field, type, lo, hi)                                             \
    do {                                                                     \
        int64_t t_;                                                          \
        if (set_i(&t_, (lo), (hi), key, value, err, errlen) != 0) {          \
            return -1;                                                       \
        }                                                                    \
        c->field = (type)t_;                                                 \
        return 0;                                                            \
    } while (0)

/* A GPIO line: its offset on the chip, or "none". */
static int set_line(int32_t *dst, const char *key, const char *v, char *err, size_t n)
{
    if (strcmp(v, "none") == 0) {
        *dst = OBCD_LINE_NONE;
        return 0;
    }
    int64_t t;
    if (set_i(&t, 0, 511, key, v, err, n) != 0) {
        return -1;
    }
    *dst = (int32_t)t;
    return 0;
}

int obcd_config_set(obcd_config_t *c, const char *key, const char *value,
                    char *err, size_t errlen)
{
    const int64_t DAY_MS = 86400000;

    if (strcmp(key, "ipc_path") == 0)  { return set_str(c->ipc_path, sizeof(c->ipc_path), key, value, err, errlen); }
    if (strcmp(key, "log_path") == 0)  { return set_str(c->log_path, sizeof(c->log_path), key, value, err, errlen); }
    if (strcmp(key, "i2c_dev") == 0)   { return set_str(c->i2c_dev, sizeof(c->i2c_dev), key, value, err, errlen); }
    if (strcmp(key, "w1_path") == 0)   { return set_str(c->w1_path, sizeof(c->w1_path), key, value, err, errlen); }
    if (strcmp(key, "gpio_chip") == 0) { return set_str(c->gpio_chip, sizeof(c->gpio_chip), key, value, err, errlen); }
    if (strcmp(key, "burnwire_line") == 0) { return set_line(&c->burnwire_line, key, value, err, errlen); }
    if (strcmp(key, "heater_line") == 0)   { return set_line(&c->heater_line, key, value, err, errlen); }

    if (strcmp(key, "log_max_bytes") == 0)         { INT(log_max_bytes, uint64_t, 4096, INT64_MAX); }
    if (strcmp(key, "retry_ms") == 0)              { INT(retry_ms, uint32_t, 100, 60000); }
    if (strcmp(key, "ina219_addr") == 0)           { INT(ina219_addr, uint8_t, 0x03, 0x77); }
    if (strcmp(key, "ina219_shunt_mohm") == 0)     { INT(ina219_shunt_mohm, uint32_t, 1, 100000); }
    if (strcmp(key, "power_period_ms") == 0)       { INT(power_period_ms, uint32_t, 100, 60000); }
    if (strcmp(key, "temp_period_ms") == 0)        { INT(temp_period_ms, uint32_t, 800, 60000); }
    if (strcmp(key, "telemetry_period_ms") == 0)   { INT(p.telemetry_period, uint32_t, 100, 60000); }
    if (strcmp(key, "adsb_timeout_ms") == 0)       { INT(p.adsb_timeout, uint32_t, 1000, DAY_MS); }
    if (strcmp(key, "burnwire_ms") == 0)           { INT(p.burnwire_on, uint32_t, 100, 60000); }
    if (strcmp(key, "aocs_timeout_ms") == 0)       { INT(p.aocs_timeout, uint32_t, 1000, DAY_MS); }
    if (strcmp(key, "heater_on_below_ccel") == 0)  { INT(p.heater_on_below, int16_t, -5500, 12500); }
    if (strcmp(key, "heater_off_above_ccel") == 0) { INT(p.heater_off_above, int16_t, -5500, 12500); }
    if (strcmp(key, "temp_stale_ms") == 0)         { INT(p.temp_stale, uint32_t, 1000, DAY_MS); }
    if (strcmp(key, "power_stale_ms") == 0)        { INT(p.power_stale, uint32_t, 1000, DAY_MS); }
    if (strcmp(key, "report_period_ms") == 0)      { INT(p.report_period, uint32_t, 1000, DAY_MS); }

    return fail(err, errlen, "unknown key: %s%s", key, "");
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = '\0';
    }
    return s;
}

int obcd_config_parse(obcd_config_t *c, const char *text, char *err, size_t errlen)
{
    int line_no = 0;
    const char *p = text;
    while (*p != '\0') {
        line_no++;
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        if (n >= sizeof(line)) {
            snprintf(err, errlen, "line %d: too long", line_no);
            return -1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p += n + (nl ? 1u : 0u);

        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        char *s = trim(line);
        if (*s == '\0') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL) {
            snprintf(err, errlen, "line %d: expected key = value", line_no);
            return -1;
        }
        *eq = '\0';
        char msg[200];
        if (obcd_config_set(c, trim(s), trim(eq + 1), msg, sizeof(msg)) != 0) {
            snprintf(err, errlen, "line %d: %s", line_no, msg);
            return -1;
        }
    }
    return 0;
}

int obcd_config_load(obcd_config_t *c, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    bool truncated = !feof(f);
    fclose(f);
    if (truncated) {
        snprintf(err, errlen, "%s: larger than %zu bytes", path, sizeof(text) - 1);
        return -1;
    }
    text[n] = '\0';
    char msg[240];
    if (obcd_config_parse(c, text, msg, sizeof(msg)) != 0) {
        snprintf(err, errlen, "%s: %s", path, msg);
        return -1;
    }
    return 0;
}

int obcd_config_validate(const obcd_config_t *c, char *err, size_t errlen)
{
    if (c->ipc_path[0] != '/') {
        snprintf(err, errlen, "ipc_path must be absolute");
        return -1;
    }
    if (c->p.heater_on_below >= c->p.heater_off_above) {
        snprintf(err, errlen, "need heater_on_below_ccel < heater_off_above_ccel (D4)");
        return -1;
    }
    /* A reading must be able to arrive before the last one goes stale. */
    if (c->p.temp_stale <= c->temp_period_ms) {
        snprintf(err, errlen, "temp_stale_ms must exceed temp_period_ms");
        return -1;
    }
    if (c->p.power_stale <= c->power_period_ms) {
        snprintf(err, errlen, "power_stale_ms must exceed power_period_ms");
        return -1;
    }
    if (c->burnwire_line != OBCD_LINE_NONE && c->burnwire_line == c->heater_line) {
        snprintf(err, errlen, "burnwire_line and heater_line must differ");
        return -1;
    }
    return 0;
}
