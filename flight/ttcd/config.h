/*
 * config.h — ttcd configuration: a file of `key = value` lines, overridable
 * one key at a time from the command line (-o key=value).
 *
 * Nothing is compiled in that belongs to a deployment. The previous mission
 * built /home/pedro/adsb/... into its binary (adsb_capture.c:42-44); here
 * every path, device and period is a key, with a default that is sensible on
 * the flight image.
 *
 * An unknown key is an error, not a warning: a mistyped key would otherwise
 * leave the default in force, silently.
 */

#ifndef TTCD_CONFIG_H
#define TTCD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     ipc_path[108];        /* AF_UNIX sun_path limit               */
    char     log_path[256];        /* "-" for standard output              */
    uint64_t log_max_bytes;        /* stop logging, with a record, past it */
    char     soc_temp_path[128];   /* millidegrees C; empty to disable     */

    char     radio[16];            /* "sx1278" or "udp"                    */
    char     spi_dev[64];
    uint32_t spi_hz;
    char     gpio_chip[64];
    uint32_t reset_line;
    uint32_t dio0_line;
    char     lbt[16];              /* "header" or "preamble" (ADR-0007)    */

    char     udp_bind[40];
    uint16_t udp_port;
    char     udp_peer[40];
    uint16_t udp_peer_port;

    ttcd_params_t p;
} ttcd_config_t;

void ttcd_config_default(ttcd_config_t *c);

/* Sets one key. Returns 0, or -1 with a message in err. */
int ttcd_config_set(ttcd_config_t *c, const char *key, const char *value,
                    char *err, size_t errlen);

/* Parses `key = value` lines; '#' starts a comment. Stops at the first error,
 * reporting its line number. */
int ttcd_config_parse(ttcd_config_t *c, const char *text, char *err, size_t errlen);

/* Reads and parses a file. */
int ttcd_config_load(ttcd_config_t *c, const char *path, char *err, size_t errlen);

/* Cross-field checks that single keys cannot express. */
int ttcd_config_validate(const ttcd_config_t *c, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* TTCD_CONFIG_H */
