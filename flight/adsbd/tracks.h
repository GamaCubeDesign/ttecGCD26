/*
 * tracks.h — the table of aircraft adsbd is tracking, and the snapshot it
 * sends to ttcd (HLR-ADS-03, HLR-ADS-05).
 *
 * ADS-B delivers an aircraft's state in pieces: position in one message,
 * velocity in another, identification in a third, each at its own rate. The
 * table merges them per ICAO address and remembers when each field last
 * arrived, so a snapshot can say how old its data is and leave out what is
 * stale.
 *
 * Fixed capacity, no allocation. The lookup is a linear scan: at 128 slots
 * and the ~80 messages per second of 20 aircraft that is ~10 000 comparisons
 * per second, which is not where the Pi's time goes (dump1090 is).
 *
 * What one record of a snapshot means (ADR-0007, ADR-0009):
 *
 *   - its time is the arrival of the aircraft's latest position, when that
 *     position is fresh (no older than position_max_age); otherwise the
 *     arrival of its newest other field. age_ds is measured from that time
 *     to the snapshot, and ttcd extends it to the start of transmission;
 *   - it carries each field whose last value is no older than field_ttl,
 *     flagged GAMA_TRACK_F_*. An aircraft whose positions stopped keeps
 *     being reported, without a position, until its other fields go stale;
 *   - aircraft with a fresh position come first, newest first, and at most
 *     max_records are sent. The rest are counted, not silently dropped.
 */

#ifndef ADSBD_TRACKS_H
#define ADSBD_TRACKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gama_tm.h"
#include "sbs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADSBD_TRACKS_MAX 128u

/* Which fields a track has ever received. */
#define TRK_POS (1u << 0)
#define TRK_ALT (1u << 1)
#define TRK_SPD (1u << 2)
#define TRK_TRK (1u << 3)
#define TRK_VR  (1u << 4)
#define TRK_GND (1u << 5)
#define TRK_CS  (1u << 6)

typedef struct {
    uint32_t icao;
    uint8_t  have;                        /* TRK_*                            */
    uint32_t msgs;
    uint64_t first_ms, last_ms;           /* first and latest message, any kind */
    uint64_t pos_ms, alt_ms, spd_ms, trk_ms, vr_ms, gnd_ms;
    double   lat, lon;
    int32_t  altitude_ft;
    double   ground_speed_kt;
    double   track_deg;
    int32_t  vertical_rate_fpm;
    bool     on_ground;
    char     callsign[9];
} adsbd_track_t;

typedef struct {
    adsbd_track_t slot[ADSBD_TRACKS_MAX];
    bool          used[ADSBD_TRACKS_MAX];
    uint32_t      capacity;               /* slots in use at most, <= MAX     */
    uint32_t      count;                  /* aircraft currently tracked       */
} adsbd_table_t;

typedef struct {
    uint32_t position_max_age;            /* ms                               */
    uint32_t field_ttl;                   /* ms                               */
    uint32_t max_records;                 /* per snapshot                     */
} adsbd_snapshot_rules_t;

void tracks_init(adsbd_table_t *t, uint32_t capacity);

/* Merges one message (sbs_parse() == SBS_OK) that arrived at now_ms. Returns
 * false if the table was full and the stalest aircraft was dropped to make
 * room for this one. */
bool tracks_apply(adsbd_table_t *t, const sbs_msg_t *m, uint64_t now_ms);

/* Drops aircraft that have sent nothing for longer than expiry_ms. Returns
 * how many were dropped. */
uint32_t tracks_expire(adsbd_table_t *t, uint64_t now_ms, uint32_t expiry_ms);

/* Writes the records of one snapshot taken at now_ms into out[0 .. cap-1],
 * ages measured to now_ms. Returns the number written; *left_out (may be
 * NULL) receives how many aircraft had data but did not fit. */
size_t tracks_snapshot(const adsbd_table_t *t, uint64_t now_ms,
                       const adsbd_snapshot_rules_t *r,
                       gama_track_t *out, size_t cap, uint32_t *left_out);

/* The track for an address, or NULL. */
const adsbd_track_t *tracks_find(const adsbd_table_t *t, uint32_t icao);

#ifdef __cplusplus
}
#endif

#endif /* ADSBD_TRACKS_H */
