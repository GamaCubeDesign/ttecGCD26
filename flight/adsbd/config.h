/*
 * config.h — adsbd configuration: `key = value` lines, overridable one key at
 * a time from the command line (-o key=value), exactly like ttcd's.
 *
 * Nothing that belongs to a deployment is compiled in. The prototype had
 * /home/pedro/dump1090/dump1090 and /home/pedro/adsb/... in its binary
 * (adsb_capture.c:42-44); here the dump1090 command line, the ports and the
 * files are all keys. An unknown key is an error, not a warning.
 */

#ifndef ADSBD_CONFIG_H
#define ADSBD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     ipc_path[108];         /* ttcd's socket (AF_UNIX sun_path limit) */
    char     sbs_host[40];          /* dump1090's SBS output: IPv4 address   */
    uint16_t sbs_port;
    char     dump1090_cmd[400];     /* split on spaces, run without a shell;
                                       empty: start nothing, only connect    */
    char     ndjson_path[256];      /* the onboard record (HLR-SW-02)        */
    uint64_t ndjson_max_bytes;      /* stop recording, with an event, past it */
    char     log_path[256];         /* events; "-" for standard output       */
    uint64_t log_max_bytes;
    uint32_t retry_ms;              /* reconnection to dump1090 and to ttcd  */
    uint32_t restart_min_ms;        /* dump1090 restart backoff: first wait  */
    uint32_t restart_max_ms;        /* ... doubling up to this               */

    adsbd_params_t p;
} adsbd_config_t;

void adsbd_config_default(adsbd_config_t *c);

/* Sets one key. Returns 0, or -1 with a message in err. */
int adsbd_config_set(adsbd_config_t *c, const char *key, const char *value,
                     char *err, size_t errlen);

/* Parses `key = value` lines; '#' starts a comment. Stops at the first
 * error, reporting its line number. */
int adsbd_config_parse(adsbd_config_t *c, const char *text, char *err, size_t errlen);

/* Reads and parses a file. */
int adsbd_config_load(adsbd_config_t *c, const char *path, char *err, size_t errlen);

/* Cross-field checks that single keys cannot express. */
int adsbd_config_validate(const adsbd_config_t *c, char *err, size_t errlen);

/* Splits dump1090_cmd into argv (at most max-1 words, NULL-terminated) inside
 * buf, a copy the caller owns. Returns the word count, or -1 if too many. */
int adsbd_config_argv(const adsbd_config_t *c, char *buf, size_t buflen,
                      char **argv, int max);

#ifdef __cplusplus
}
#endif

#endif /* ADSBD_CONFIG_H */
