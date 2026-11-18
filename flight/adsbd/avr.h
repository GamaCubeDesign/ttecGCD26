/*
 * avr.h — parser for the raw messages dump1090-fa writes on its raw output
 * port (30002 when enabled, --net-ro-port): one Mode S message per line, in
 * AVR format, "*8D4840D6202CC371C32CE0576098;" (ADR-0023).
 *
 * adsbd reads this port for one thing the SBS output does not carry: the
 * ADS-B emitter category, sent in the identification message (DF17 or DF18,
 * type codes 1 to 4) about every 5 s. Everything else on the port is the
 * same traffic the SBS output already decoded, and is only counted.
 *
 * The 112-bit identification message, bit 1 first:
 *
 *    1-5   DF       17 (ADS-B) or 18 (non-transponder ADS-B / TIS-B)
 *    6-8   CA / CF  for DF18, CF 0 means an ICAO address; other CFs do not
 *    9-32  AA       the 24-bit ICAO address
 *   33-37  TC       type code: 4 set A, 3 set B, 2 set C, 1 set D
 *   38-40  CA       the category within the set (A3: TC 4, CA 3)
 *   41-88           the callsign (the SBS output already gives it)
 *   89-112 PI       CRC-24 over bits 1-88, generator 0xFFF409
 *
 * A line may also start with '@' and a 12-hex-digit timestamp, as
 * dump1090 writes when asked for MLAT timestamps; the timestamp is skipped
 * (adsbd stamps the arrival itself, HLR-ADS-07).
 *
 * Pure: no allocation, no I/O. ADR-0005.
 */

#ifndef ADSBD_AVR_H
#define ADSBD_AVR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t icao;               /* 24-bit address                           */
    char     set;                /* 'A'..'D', from the type code              */
    uint8_t  category;           /* 0..7 within the set                      */
    uint8_t  cls;                /* GAMA_TRACK_CLASS_*                       */
} avr_msg_t;

typedef enum {
    AVR_OK          =  0,  /* an identification message with an ICAO address */
    AVR_NOT_IDENT   =  1,  /* a well-formed message of any other kind        */
    AVR_NON_ICAO    =  2,  /* DF18 identification without an ICAO address    */
    AVR_ERR_FORMAT  = -1,  /* not '*' or '@', no ';', odd or bad hex, wrong length */
    AVR_ERR_CRC     = -2   /* an identification message whose CRC-24 fails  */
} avr_result_t;

/* Parses one line, without its newline (a trailing '\r' is tolerated).
 * On AVR_OK fills *out; on anything else *out is unspecified. */
avr_result_t avr_parse(const char *line, size_t len, avr_msg_t *out);

/* The class (GAMA_TRACK_CLASS_*) of a set and category, ADR-0023:
 * A1 A2 B1 light; A3-A6 large; A7 rotorcraft; anything else unknown. */
uint8_t avr_class(char set, uint8_t category);

/* The CRC-24 of Mode S over n bytes: 0 for an intact 112-bit DF17/18
 * message taken whole. Exposed for the tests and for sbs_replay's check. */
uint32_t avr_crc24(const uint8_t *msg, size_t n);

const char *avr_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif /* ADSBD_AVR_H */
