/*
 * config.h — obcd configuration: `key = value` lines, overridable one key at
 * a time from the command line (-o key=value), exactly like ttcd's and
 * adsbd's. An unknown key is an error, not a warning.
 *
 * No pin and no bus is compiled in (AGENTS.md, hard rule 4, and the wiring
 * defects of the boards as fabricated): an output whose line is not set, or
 * a sensor whose device is not set, is simply absent, and the log says so.
 * The previous OBC compiled /dev/i2c-1, address 0x40 and its GPIO numbers
 * into the binary.
 */

#ifndef OBCD_CONFIG_H
#define OBCD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBCD_LINE_NONE (-1)

typedef struct {
    char     ipc_path[108];         /* ttcd's socket (AF_UNIX sun_path limit)  */
    char     log_path[256];         /* events; "-" for standard output         */
    uint64_t log_max_bytes;
    uint32_t retry_ms;              /* reconnection to ttcd                    */

    char     i2c_dev[64];           /* INA219's bus, e.g. /dev/i2c-1; "" none  */
    uint8_t  ina219_addr;
    uint32_t ina219_shunt_mohm;
    uint32_t power_period_ms;       /* INA219 reading period, in its thread    */

    char     w1_path[256];          /* DS18B20's w1_slave file; "" none        */
    uint32_t temp_period_ms;        /* DS18B20 reading period, in its thread   */

    char     gpio_chip[64];
    int32_t  burnwire_line;         /* OBCD_LINE_NONE: not connected           */
    int32_t  heater_line;

    obcd_params_t p;
} obcd_config_t;

void obcd_config_default(obcd_config_t *c);
int  obcd_config_set(obcd_config_t *c, const char *key, const char *value,
                     char *err, size_t errlen);
int  obcd_config_parse(obcd_config_t *c, const char *text, char *err, size_t errlen);
int  obcd_config_load(obcd_config_t *c, const char *path, char *err, size_t errlen);
int  obcd_config_validate(const obcd_config_t *c, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* OBCD_CONFIG_H */
