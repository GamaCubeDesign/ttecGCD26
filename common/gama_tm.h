/*
 * gama_tm.h — telemetry record encoding.
 *
 * These are the payloads carried inside GAMA frames. Every record has a fixed
 * wire size so a receiver can split a payload into records by division, with
 * no per-record length prefix. That matters: at SF9 a byte of overhead costs
 * real time-on-air, and the whole downlink budget for the mission is 58 KB
 * (see docs/budgets/data-budget.md).
 *
 * Physical quantities are transmitted as scaled integers, never as floats.
 * Float layout is not guaranteed across compilers, the values have known
 * bounded ranges, and the quantisation step is chosen to sit at or below the
 * resolution the source data actually has.
 *
 * Shared verbatim between the Raspberry Pi and the ESP32.
 */

#ifndef GAMA_TM_H
#define GAMA_TM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  Track record: one aircraft, one point in time.  20 bytes.
 * ------------------------------------------------------------------ */

#define GAMA_TRACK_WIRE_LEN 20u

/*
 * Position is sent as an absolute 24-bit value, not as a delta from the
 * previously transmitted position. A delta encoding would save ~4 bytes per
 * record, but the link has no guaranteed delivery: one lost frame would
 * corrupt every position that follows it until the next keyframe. Loss
 * tolerance is worth more here than the compression, because the downlink
 * budget is already sufficient at 20 bytes per record.
 *
 * Resolution: latitude 1/93206 deg ~ 1.2 m, longitude 1/46603 deg ~ 2.4 m at
 * the equator. Both are far below ADS-B's own position accuracy.
 */
#define GAMA_LAT_SCALE 93206.0  /* degrees -> i24, full scale +/- 90 deg  */
#define GAMA_LON_SCALE 46603.0  /* degrees -> i24, full scale +/- 180 deg */

/* Validity bits. A field whose bit is clear was never received for this
 * aircraft and its value in the struct is unspecified. ADS-B delivers
 * position, velocity and identification in separate message types, so a
 * track is routinely partial -- especially in its first seconds. */
#define GAMA_TRACK_F_POSITION  (1u << 0)
#define GAMA_TRACK_F_ALTITUDE  (1u << 1)
#define GAMA_TRACK_F_VELOCITY  (1u << 2) /* covers ground speed and track  */
#define GAMA_TRACK_F_VRATE     (1u << 3)
#define GAMA_TRACK_F_ON_GROUND (1u << 4)
#define GAMA_TRACK_F_CALLSIGN  (1u << 5) /* callsign already sent in ROSTER */

typedef struct {
    uint32_t icao;            /* 24-bit ICAO address                       */
    double   latitude;        /* degrees, -90 .. +90                       */
    double   longitude;       /* degrees, -180 .. +180                     */
    int32_t  altitude_ft;     /* barometric feet, quantised to 25 ft       */
    double   ground_speed_kt; /* knots, quantised to 0.1 kt                */
    double   track_deg;       /* degrees true, 0 .. 360, step 0.01         */
    int32_t  vertical_rate_fpm; /* feet per minute, step 1                 */
    uint16_t age_ds;          /* see "Track age" below                     */
    uint8_t  flags;           /* GAMA_TRACK_F_*                            */
} gama_track_t;

/*
 * Track age (ADR-0007).
 *
 * On the air, age_ds is the time from this aircraft's last update to the
 * start of the transmission of the frame that carries the record, in
 * deciseconds, saturating at 65535. ttcd writes it immediately before
 * transmitting.
 *
 * This makes each record self-contained in time. The ground station timestamps
 * the frame's arrival with its own clock and subtracts the time on air (known
 * from the profile, common/gama_lora.h) and the age: the result is the time of
 * the update in the ground's clock domain, with no satellite clock and no
 * synchronisation involved. And the age itself is the HLR-ADS-08 latency from
 * reception to transmission for that update, delivered in every record.
 *
 * Inside the satellite, on IPC_TRACKS (common/gama_ipc.h), the same field is
 * measured to the snapshot epoch instead; ttcd converts it with
 * gama_track_age_add().
 */

/* Adds delta_ds to the age of an encoded record, in place, saturating at
 * 65535 instead of wrapping. Returns the new age. The record's frame CRC must
 * be computed after this call, not before. */
uint16_t gama_track_age_add(uint8_t *record, uint32_t delta_ds);

/* Encode/decode exactly GAMA_TRACK_WIRE_LEN bytes. Return the byte count, or
 * a negative gama_frame_result_t on a NULL pointer / undersized buffer.
 * Encoding clamps out-of-range inputs to the representable range rather than
 * wrapping, so a corrupt sensor value degrades instead of aliasing onto a
 * plausible-looking position. */
int gama_track_encode(uint8_t *out, size_t out_cap, const gama_track_t *t);
int gama_track_decode(const uint8_t *in, size_t in_len, gama_track_t *t);

/* How many whole track records fit in one frame payload: 248 / 20 = 12. */
#define GAMA_TRACKS_PER_FRAME (248u / GAMA_TRACK_WIRE_LEN)

/* ------------------------------------------------------------------ *
 *  Roster record: ICAO -> callsign.  11 bytes.
 * ------------------------------------------------------------------ */

#define GAMA_ROSTER_WIRE_LEN  11u
#define GAMA_CALLSIGN_LEN      8u  /* ADS-B identification is 8 characters */

typedef struct {
    uint32_t icao;
    char     callsign[GAMA_CALLSIGN_LEN + 1]; /* NUL-terminated in memory  */
} gama_roster_t;

/* On the wire the callsign is 8 bytes, space-padded and not terminated. */
int gama_roster_encode(uint8_t *out, size_t out_cap, const gama_roster_t *r);
int gama_roster_decode(const uint8_t *in, size_t in_len, gama_roster_t *r);

/* ------------------------------------------------------------------ *
 *  Housekeeping: the HLR-COMM-02 payload.  25 bytes.
 * ------------------------------------------------------------------ */

#define GAMA_HK_WIRE_LEN 25u

typedef struct {
    uint16_t battery_mv;      /* millivolts                                */
    int16_t  current_ma;      /* milliamps, signed: negative is charging   */
    int16_t  temp_ext_ccel;   /* external temperature, hundredths of degC  */
    int16_t  temp_soc_ccel;   /* SoC temperature, hundredths of degC       */
    int16_t  roll_cdeg;       /* hundredths of a degree                    */
    int16_t  pitch_cdeg;
    int16_t  yaw_cdeg;
    uint32_t adsb_msgs;       /* cumulative decoded ADS-B messages         */
    uint8_t  obc_mode;        /* obc fsm.h State, as reported over IPC     */
    uint8_t  link_state;      /* ttcd link state machine                   */
    uint32_t uptime_s;
    uint8_t  flags;           /* bit 0: OBC IPC connected                  *
                               * bit 1: adsbd IPC connected                *
                               * bit 2: wall clock synchronised            *
                               * bit 3: dump1090 running                   */
} gama_hk_t;

#define GAMA_HK_F_OBC_LINKED   (1u << 0)
#define GAMA_HK_F_ADSB_LINKED  (1u << 1)
#define GAMA_HK_F_TIME_SYNCED  (1u << 2)
#define GAMA_HK_F_DUMP1090_UP  (1u << 3)

int gama_hk_encode(uint8_t *out, size_t out_cap, const gama_hk_t *hk);
int gama_hk_decode(const uint8_t *in, size_t in_len, gama_hk_t *hk);

/* ------------------------------------------------------------------ *
 *  Mission statistics: the HLR-ADS-08 evidence.  26 bytes.
 * ------------------------------------------------------------------ */

#define GAMA_STAT_WIRE_LEN 26u

typedef struct {
    uint32_t msgs_received;     /* SBS lines read from dump1090            */
    uint32_t msgs_decoded;      /* of those, parsed into a track update    */
    uint16_t aircraft_tracked;  /* distinct ICAOs currently in the table   */
    uint32_t tm_frames_sent;
    uint32_t tc_frames_rx;      /* telecommand frames that passed CRC      */
    uint32_t tc_frames_bad;     /* frames that failed version/len/CRC      */
    uint16_t latency_p95_ms;    /* ADS-B reception -> TM transmission      */
    uint16_t dump1090_restarts;
} gama_stat_t;

int gama_stat_encode(uint8_t *out, size_t out_cap, const gama_stat_t *s);
int gama_stat_decode(const uint8_t *in, size_t in_len, gama_stat_t *s);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_TM_H */
