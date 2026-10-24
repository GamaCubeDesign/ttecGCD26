/*
 * ds18b20.h — the battery temperature, through the kernel's 1-Wire driver
 * (w1_therm), as the text of its w1_slave file (OBC-7, HLR-EPS-04, D9).
 *
 * The kernel does the 1-Wire protocol and the ~750 ms conversion; reading
 * the file blocks for that long, which is why obcd reads it in a thread of
 * its own and never in the main loop (AGENTS.md, hard rule 3). The file is
 * two lines, the second ending in the temperature in thousandths of a
 * degree:
 *
 *   72 01 4b 46 7f ff 0e 10 57 : crc=57 YES
 *   72 01 4b 46 7f ff 0e 10 57 t=23125
 *
 * A "NO" on the first line is a CRC failure on the wire; the value is then
 * garbage and is not reported. 85000 is the chip's power-on value, read
 * when a conversion did not happen (typically a missing pull-up, hardware
 * finding A7); it is refused too.
 */

#ifndef OBCD_DS18B20_H
#define OBCD_DS18B20_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS18B20_OK = 0,
    DS18B20_ERR_FORMAT = -1,   /* not the two lines above                   */
    DS18B20_ERR_CRC = -2,      /* the kernel says the CRC failed            */
    DS18B20_ERR_POWER_ON = -3, /* 85.000 degC: no conversion took place     */
    DS18B20_ERR_RANGE = -4,    /* outside the chip's -55 .. +125 degC       */
    DS18B20_ERR_IO = -5        /* the file could not be read                */
} ds18b20_result_t;

/* Parses the text of w1_slave into hundredths of a degree, rounded. */
ds18b20_result_t ds18b20_parse(const char *text, size_t len, int16_t *ccel);

/* Reads and parses the file at path. Blocks for the conversion. */
ds18b20_result_t ds18b20_read_file(const char *path, int16_t *ccel);

const char *ds18b20_result_name(ds18b20_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* OBCD_DS18B20_H */
