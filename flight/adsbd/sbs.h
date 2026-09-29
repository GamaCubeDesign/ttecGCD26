/*
 * sbs.h — parser for the SBS-1 (BaseStation) lines that dump1090-fa writes on
 * TCP port 30003: one decoded Mode S / ADS-B message per line (HLR-ADS-02).
 *
 * Field layout, 0-based, as written by dump1090-fa (net_io.c):
 *
 *    0 "MSG"            6..9 message and log dates and times — ignored:
 *    1 type, 1..8            adsbd stamps the arrival itself (HLR-ADS-07)
 *    2 session id      10 callsign             16 vertical rate, ft/min
 *    3 aircraft id     11 altitude, ft         17 squawk
 *    4 hex ident       12 ground speed, kt     18 alert      -1 true,
 *    5 flight id       13 track, degrees       19 emergency   0 false,
 *                      14 latitude             20 SPI         empty unknown
 *                      15 longitude            21 on ground
 *
 * Fields 18 to 21 are BaseStation booleans: "-1" means TRUE. The prototype
 * (adsb_capture.c) read only "1" as true, so every aircraft on the ground
 * was logged with on_ground unknown, and the ground's estimator lost the
 * takeoff anchor it uses for the origin.
 *
 * A hex ident that starts with '~' is not an ICAO address (TIS-B, anonymous
 * or otherwise non-ICAO); such lines are recognised and skipped, not logged.
 *
 * Pure: no allocation, no I/O, no locale beyond the C default.
 */

#ifndef ADSBD_SBS_H
#define ADSBD_SBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which optional fields a message carried. */
#define SBS_HAVE_CALLSIGN (1u << 0)
#define SBS_HAVE_ALTITUDE (1u << 1)
#define SBS_HAVE_SPEED    (1u << 2)   /* ground speed                        */
#define SBS_HAVE_TRACK    (1u << 3)
#define SBS_HAVE_POSITION (1u << 4)   /* latitude and longitude together     */
#define SBS_HAVE_VRATE    (1u << 5)
#define SBS_HAVE_SQUAWK   (1u << 6)
#define SBS_HAVE_GROUND   (1u << 7)   /* the on-ground flag is known         */

typedef struct {
    uint8_t  type;               /* transmission type, 1..8                  */
    uint32_t icao;               /* 24-bit address                           */
    uint16_t have;               /* SBS_HAVE_*                               */
    char     callsign[9];        /* up to 8 characters, trailing spaces cut  */
    int32_t  altitude_ft;
    double   ground_speed_kt;
    double   track_deg;
    double   lat, lon;
    int32_t  vertical_rate_fpm;
    char     squawk[5];
    int8_t   on_ground;          /* 1 on the ground, 0 airborne, -1 unknown  */
} sbs_msg_t;

typedef enum {
    SBS_OK          =  0,
    SBS_IGNORED     =  1,  /* a well-formed line that is not a MSG           */
    SBS_NON_ICAO    =  2,  /* a MSG whose address is not ICAO ('~')          */
    SBS_ERR_FORMAT  = -1,  /* not an SBS line, or fewer fields than a MSG    */
    SBS_ERR_TYPE    = -2,  /* transmission type missing or outside 1..8      */
    SBS_ERR_ICAO    = -3,  /* hex ident is not six hexadecimal digits        */
    SBS_ERR_VALUE   = -4   /* a field present but unparseable or out of range */
} sbs_result_t;

/*
 * Parses one line, without its terminating newline (a trailing '\r' is
 * tolerated). The line need not be NUL-terminated. On SBS_OK fills *out;
 * on anything else *out is unspecified. Ranges match the ground's NDJSON
 * contract, so nothing adsbd logs can be rejected there for its value.
 */
sbs_result_t sbs_parse(const char *line, size_t len, sbs_msg_t *out);

const char *sbs_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif /* ADSBD_SBS_H */
