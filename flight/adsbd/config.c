#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The prototype's command line (adsb_capture.c), minus what adsbd does not
 * use: no JSON snapshots written to the SD card every second, no network
 * port other than SBS, and that one on loopback only. The gain is the
 * prototype's until the sweep of PLANO 3.4 measures a better one; how
 * dump1090-fa reads "-10" depends on its version, which is one more reason
 * the whole line is a configuration key. */
#define DEFAULT_DUMP1090 \
    "/usr/bin/dump1090-fa --device-index 0 --gain -10 --net " \
    "--net-bind-address 127.0.0.1 --net-sbs-port 30003 " \
    "--net-ro-port 0 --net-bo-port 0 --net-bi-port 0 --net-ri-port 0 --quiet"

void adsbd_config_default(adsbd_config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->ipc_path, sizeof(c->ipc_path), "/run/gama/ttec.sock");
    snprintf(c->sbs_host, sizeof(c->sbs_host), "127.0.0.1");
    c->sbs_port = 30003;
    snprintf(c->dump1090_cmd, sizeof(c->dump1090_cmd), "%s", DEFAULT_DUMP1090);
    snprintf(c->ndjson_path, sizeof(c->ndjson_path), "/var/lib/gama/adsb.ndjson");
    c->ndjson_max_bytes = 1024u * 1024u * 1024u;   /* data-budget.md §7 */
    snprintf(c->log_path, sizeof(c->log_path), "/var/lib/gama/adsbd.jsonl");
    c->log_max_bytes = 64u * 1024u * 1024u;
    c->retry_ms = 1000;
    c->restart_min_ms = 1000;
    c->restart_max_ms = 30000;
    adsbd_params_default(&c->p);
}

static int fail(char *err, size_t n, const char *fmt, const char *a, const char *b)
{
    if (err != NULL && n > 0) {
        snprintf(err, n, fmt, a, b);
    }
    return -1;
}

/* Values end up inside JSON strings in the event log, unescaped: quotes,
 * backslashes and control characters are refused here instead. */
static int set_str(char *dst, size_t cap, const char *key, const char *v,
                   char *err, size_t n)
{
    if (strlen(v) >= cap) {
        return fail(err, n, "%s: value too long: %s", key, v);
    }
    for (const char *p = v; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20u) {
            return fail(err, n, "%s: quotes, backslashes and control characters "
                                "are not allowed: %s", key, v);
        }
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

int adsbd_config_set(adsbd_config_t *c, const char *key, const char *value,
                     char *err, size_t errlen)
{
    const uint64_t DAY_MS = 86400000u;

    if (strcmp(key, "ipc_path") == 0)     { return set_str(c->ipc_path, sizeof(c->ipc_path), key, value, err, errlen); }
    if (strcmp(key, "sbs_host") == 0)     { return set_str(c->sbs_host, sizeof(c->sbs_host), key, value, err, errlen); }
    if (strcmp(key, "dump1090_cmd") == 0) { return set_str(c->dump1090_cmd, sizeof(c->dump1090_cmd), key, value, err, errlen); }
    if (strcmp(key, "ndjson_path") == 0)  { return set_str(c->ndjson_path, sizeof(c->ndjson_path), key, value, err, errlen); }
    if (strcmp(key, "log_path") == 0)     { return set_str(c->log_path, sizeof(c->log_path), key, value, err, errlen); }

    if (strcmp(key, "sbs_port") == 0) {
        uint64_t t;
        if (set_u(&t, 1, 65535, key, value, err, errlen) != 0) {
            return -1;
        }
        c->sbs_port = (uint16_t)t;
        return 0;
    }
    if (strcmp(key, "ndjson_max_bytes") == 0) {
        return set_u(&c->ndjson_max_bytes, 4096u, UINT64_MAX, key, value, err, errlen);
    }
    if (strcmp(key, "log_max_bytes") == 0) {
        return set_u(&c->log_max_bytes, 4096u, UINT64_MAX, key, value, err, errlen);
    }
    if (strcmp(key, "retry_ms") == 0)              { U32(retry_ms, 100, 60000); }
    if (strcmp(key, "restart_min_ms") == 0)        { U32(restart_min_ms, 100, 600000); }
    if (strcmp(key, "restart_max_ms") == 0)        { U32(restart_max_ms, 100, 600000); }
    if (strcmp(key, "snapshot_period_ms") == 0)    { U32(p.snapshot_period, 100, 60000); }
    if (strcmp(key, "stat_period_ms") == 0)        { U32(p.stat_period, 100, DAY_MS); }
    if (strcmp(key, "report_period_ms") == 0)      { U32(p.report_period, 1000, DAY_MS); }
    if (strcmp(key, "track_expiry_ms") == 0)       { U32(p.track_expiry, 1000, DAY_MS); }
    if (strcmp(key, "position_max_age_ms") == 0)   { U32(p.position_max_age, 100, DAY_MS); }
    if (strcmp(key, "field_ttl_ms") == 0)          { U32(p.field_ttl, 100, DAY_MS); }
    if (strcmp(key, "snapshot_max_aircraft") == 0) { U32(p.snapshot_max, 1, ADSBD_SNAPSHOT_MAX); }
    if (strcmp(key, "table_capacity") == 0)        { U32(p.table_capacity, 1, ADSBD_TRACKS_MAX); }

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

int adsbd_config_parse(adsbd_config_t *c, const char *text, char *err, size_t errlen)
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
        if (adsbd_config_set(c, trim(s), trim(eq + 1), msg, sizeof(msg)) != 0) {
            snprintf(err, errlen, "line %d: %s", line_no, msg);
            return -1;
        }
    }
    return 0;
}

int adsbd_config_load(adsbd_config_t *c, const char *path, char *err, size_t errlen)
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
    if (adsbd_config_parse(c, text, msg, sizeof(msg)) != 0) {
        snprintf(err, errlen, "%s: %s", path, msg);
        return -1;
    }
    return 0;
}

int adsbd_config_validate(const adsbd_config_t *c, char *err, size_t errlen)
{
    if (c->ipc_path[0] != '/') {
        snprintf(err, errlen, "ipc_path must be absolute");
        return -1;
    }
    if (c->ndjson_path[0] == '\0') {
        snprintf(err, errlen, "ndjson_path must name a file: the record is the mission's data");
        return -1;
    }
    if (c->restart_min_ms > c->restart_max_ms) {
        snprintf(err, errlen, "restart_min_ms must not exceed restart_max_ms");
        return -1;
    }
    /* A record may carry a position only while its other fields are still
     * reported, and a field only while its aircraft is still tracked. */
    if (c->p.position_max_age > c->p.field_ttl || c->p.field_ttl > c->p.track_expiry) {
        snprintf(err, errlen,
                 "need position_max_age_ms <= field_ttl_ms <= track_expiry_ms");
        return -1;
    }
    if (c->p.snapshot_max > c->p.table_capacity) {
        snprintf(err, errlen, "snapshot_max_aircraft must not exceed table_capacity");
        return -1;
    }
    char buf[sizeof(c->dump1090_cmd)];
    char *argv[40];
    if (adsbd_config_argv(c, buf, sizeof(buf), argv, 40) < 0) {
        snprintf(err, errlen, "dump1090_cmd: more than 39 words");
        return -1;
    }
    if (argv[0] != NULL && argv[0][0] != '/') {
        snprintf(err, errlen, "dump1090_cmd must start with an absolute path (no shell, no PATH)");
        return -1;
    }
    return 0;
}

int adsbd_config_argv(const adsbd_config_t *c, char *buf, size_t buflen,
                      char **argv, int max)
{
    snprintf(buf, buflen, "%s", c->dump1090_cmd);
    int n = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, " \t", &save); w != NULL; w = strtok_r(NULL, " \t", &save)) {
        if (n >= max - 1) {
            return -1;
        }
        argv[n++] = w;
    }
    argv[n] = NULL;
    return n;
}
