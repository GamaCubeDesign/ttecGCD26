#include "config.h"
#include "gama_tc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ttcd_config_default(ttcd_config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->ipc_path, sizeof(c->ipc_path), "/run/gama/ttec.sock");
    snprintf(c->log_path, sizeof(c->log_path), "/var/lib/gama/ttcd.jsonl");
    c->log_max_bytes = 64u * 1024u * 1024u;
    snprintf(c->soc_temp_path, sizeof(c->soc_temp_path),
             "/sys/class/thermal/thermal_zone0/temp");
    snprintf(c->radio, sizeof(c->radio), "sx1278");
    snprintf(c->spi_dev, sizeof(c->spi_dev), "/dev/spidev0.0");
    c->spi_hz = 1000000u;
    snprintf(c->gpio_chip, sizeof(c->gpio_chip), "/dev/gpiochip0");
    c->reset_line = 20;            /* the previous mission's wiring */
    c->dio0_line = 21;
    snprintf(c->lbt, sizeof(c->lbt), "header");
    snprintf(c->udp_bind, sizeof(c->udp_bind), "127.0.0.1");
    c->udp_port = 47001;
    snprintf(c->udp_peer, sizeof(c->udp_peer), "127.0.0.1");
    c->udp_peer_port = 47002;
    ttcd_params_default(&c->p);
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

/* Unsigned integer in [lo, hi], decimal, nothing trailing. */
static int set_u(uint64_t *dst, uint64_t lo, uint64_t hi, const char *key,
                 const char *v, char *err, size_t n)
{
    if (*v == '\0' || *v == '-' || isspace((unsigned char)*v)) {
        return fail(err, n, "%s: not a number: %s", key, v);
    }
    char *end;
    errno = 0;
    unsigned long long x = strtoull(v, &end, 10);
    if (errno != 0 || *end != '\0') {
        return fail(err, n, "%s: not a number: %s", key, v);
    }
    if (x < lo || x > hi) {
        return fail(err, n, "%s: out of range: %s", key, v);
    }
    *dst = x;
    return 0;
}

#define U32(field, lo, hi)                                                   \
    do {                                                                     \
        uint64_t t_;                                                         \
        if (set_u(&t_, (lo), (hi), key, value, err, errlen) != 0) {          \
            return -1;                                                       \
        }                                                                    \
        c->field = (uint32_t)t_;                                             \
        return 0;                                                            \
    } while (0)

int ttcd_config_set(ttcd_config_t *c, const char *key, const char *value,
                    char *err, size_t errlen)
{
    const uint64_t DAY_MS = 86400000u;

    if (strcmp(key, "ipc_path") == 0)      { return set_str(c->ipc_path, sizeof(c->ipc_path), key, value, err, errlen); }
    if (strcmp(key, "log_path") == 0)      { return set_str(c->log_path, sizeof(c->log_path), key, value, err, errlen); }
    if (strcmp(key, "soc_temp_path") == 0) { return set_str(c->soc_temp_path, sizeof(c->soc_temp_path), key, value, err, errlen); }
    if (strcmp(key, "spi_dev") == 0)       { return set_str(c->spi_dev, sizeof(c->spi_dev), key, value, err, errlen); }
    if (strcmp(key, "gpio_chip") == 0)     { return set_str(c->gpio_chip, sizeof(c->gpio_chip), key, value, err, errlen); }
    if (strcmp(key, "udp_bind") == 0)      { return set_str(c->udp_bind, sizeof(c->udp_bind), key, value, err, errlen); }
    if (strcmp(key, "udp_peer") == 0)      { return set_str(c->udp_peer, sizeof(c->udp_peer), key, value, err, errlen); }

    if (strcmp(key, "radio") == 0) {
        if (strcmp(value, "sx1278") != 0 && strcmp(value, "udp") != 0) {
            return fail(err, errlen, "%s: expected sx1278 or udp, got %s", key, value);
        }
        return set_str(c->radio, sizeof(c->radio), key, value, err, errlen);
    }
    if (strcmp(key, "lbt") == 0) {
        if (strcmp(value, "header") != 0 && strcmp(value, "preamble") != 0) {
            return fail(err, errlen, "%s: expected header or preamble, got %s", key, value);
        }
        return set_str(c->lbt, sizeof(c->lbt), key, value, err, errlen);
    }
    if (strcmp(key, "profile") == 0) {
        for (uint8_t p = 0; p < GAMA_RATE_COUNT; p++) {
            const char *name = gama_rate_profile_name(p);
            char lower[16];
            size_t i = 0;
            for (; name[i] != '\0' && i + 1 < sizeof(lower); i++) {
                lower[i] = (char)tolower((unsigned char)name[i]);
            }
            lower[i] = '\0';
            if (strcmp(value, lower) == 0) {
                c->p.initial_profile = p;
                return 0;
            }
        }
        return fail(err, errlen, "%s: expected safe, nominal or fast, got %s", key, value);
    }
    if (strcmp(key, "log_max_bytes") == 0) {
        return set_u(&c->log_max_bytes, 4096u, UINT64_MAX, key, value, err, errlen);
    }
    if (strcmp(key, "tx_power_dbm") == 0) {
        uint64_t t;
        if (set_u(&t, 2, 20, key, value, err, errlen) != 0) {
            return -1;
        }
        c->p.tx_power_dbm = (int8_t)t;
        return 0;
    }
    if (strcmp(key, "udp_port") == 0 || strcmp(key, "udp_peer_port") == 0) {
        uint64_t t;
        if (set_u(&t, 1, 65535, key, value, err, errlen) != 0) {
            return -1;
        }
        if (key[4] == 'p' && key[5] == 'o') { c->udp_port = (uint16_t)t; }
        else                                { c->udp_peer_port = (uint16_t)t; }
        return 0;
    }
    if (strcmp(key, "spi_hz") == 0)                { U32(spi_hz, 100000u, 10000000u); }
    if (strcmp(key, "reset_line") == 0)            { U32(reset_line, 0, 511); }
    if (strcmp(key, "dio0_line") == 0)             { U32(dio0_line, 0, 511); }
    if (strcmp(key, "hk_period_ms") == 0)          { U32(p.hk_period, 1000, DAY_MS); }
    if (strcmp(key, "stat_period_ms") == 0)        { U32(p.stat_period, 1000, DAY_MS); }
    if (strcmp(key, "safe_beacon_period_ms") == 0) { U32(p.safe_beacon_period, 1000, DAY_MS); }
    if (strcmp(key, "safe_hk_period_ms") == 0)     { U32(p.safe_hk_period, 1000, DAY_MS); }
    if (strcmp(key, "contact_timeout_ms") == 0)    { U32(p.contact_timeout, 1000, DAY_MS); }
    if (strcmp(key, "rate_revert_ms") == 0)        { U32(p.rate_revert, 1000, DAY_MS); }
    if (strcmp(key, "reaction_margin_ms") == 0)    { U32(p.reaction_margin, 0, 1000); }
    if (strcmp(key, "dup_window_ms") == 0)         { U32(p.dup_window, 1000, DAY_MS); }

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

int ttcd_config_parse(ttcd_config_t *c, const char *text, char *err, size_t errlen)
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
        if (ttcd_config_set(c, trim(s), trim(eq + 1), msg, sizeof(msg)) != 0) {
            snprintf(err, errlen, "line %d: %s", line_no, msg);
            return -1;
        }
    }
    return 0;
}

int ttcd_config_load(ttcd_config_t *c, const char *path, char *err, size_t errlen)
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
    if (ttcd_config_parse(c, text, msg, sizeof(msg)) != 0) {
        snprintf(err, errlen, "%s: %s", path, msg);
        return -1;
    }
    return 0;
}

int ttcd_config_validate(const ttcd_config_t *c, char *err, size_t errlen)
{
    /* The satellite must give up on an unconfirmed rate change well before it
     * concludes the ground is gone, or a rate change could end in SAFE. */
    if (c->p.rate_revert >= c->p.contact_timeout) {
        snprintf(err, errlen, "rate_revert_ms must be shorter than contact_timeout_ms");
        return -1;
    }
    if (c->ipc_path[0] != '/') {
        snprintf(err, errlen, "ipc_path must be absolute");
        return -1;
    }
    return 0;
}
