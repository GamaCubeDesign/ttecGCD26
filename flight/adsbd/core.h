/*
 * core.h — the adsbd core: SBS lines in; the onboard record, track snapshots
 * and payload counters out.
 *
 * Built like ttcd's core (ADR-0005): no system calls. The shell (main.c)
 * owns dump1090, the sockets, the files and the clock, and drives the core
 * with events stamped in CLOCK_MONOTONIC nanoseconds; the tests drive it with
 * a virtual clock. The core acts only through adsbd_ops_t.
 *
 * Time (ADR-0009). Every instant here is monotonic. The wall clock appears
 * only as an offset added to the arrival time of each message, for the
 * onboard record: it starts as the system clock's, which on a Pi with no RTC
 * may be wrong, and is replaced whenever ttcd forwards the ground's SET_TIME.
 * Nothing here sets the system clock, and nothing here goes backwards when
 * someone else does.
 */

#ifndef ADSBD_CORE_H
#define ADSBD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gama_ipc.h"
#include "tracks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Most aircraft one snapshot can carry: 8 IPC frames of 12 records. */
#define ADSBD_SNAPSHOT_MAX (GAMA_IPC_TRACKS_MAX * GAMA_IPC_TRACKS_MAX_FRAMES)

/* Rejected lines logged one by one; the rest are only counted. */
#define ADSBD_REJECT_LOG_MAX 20u

/* Every duration in milliseconds. adsbd_params_default() gives the values
 * the link model and the ground's estimator were checked against. */
typedef struct {
    uint32_t snapshot_period;   /* IPC_TRACKS to ttcd; ADR-0007 assumes 1 s  */
    uint32_t stat_period;       /* IPC_STAT to ttcd                          */
    uint32_t report_period;     /* counters into the event log               */
    uint32_t track_expiry;      /* silence after which an aircraft is dropped */
    uint32_t position_max_age;  /* older positions are left out of a record  */
    uint32_t field_ttl;         /* older fields are left out of a record     */
    uint32_t snapshot_max;      /* aircraft per snapshot, <= ADSBD_SNAPSHOT_MAX */
    uint32_t table_capacity;    /* aircraft tracked at once, <= ADSBD_TRACKS_MAX */
} adsbd_params_t;

void adsbd_params_default(adsbd_params_t *p);

typedef struct {
    void *ctx;
    /* Sends one frame to ttcd without blocking. 0 if sent. */
    int  (*ipc_send)(void *ctx, const uint8_t *frame, size_t len);
    /* Appends one line, newline included, to the onboard record. */
    void (*record)(void *ctx, const char *line, size_t len);
    /* One event for the diagnostic log: a name and a JSON object body. */
    void (*log)(void *ctx, uint64_t now_ms, const char *event, const char *fields);
} adsbd_ops_t;

/* Counters: IPC_STAT, the event log, the tests. */
typedef struct {
    uint32_t lines;             /* non-empty SBS lines read from dump1090    */
    uint32_t decoded;           /* MSG lines applied to a track and recorded */
    uint32_t ignored;           /* well-formed lines that are not MSG        */
    uint32_t non_icao;          /* MSG lines with a non-ICAO address         */
    uint32_t rejected;          /* malformed lines: counted, never used      */
    uint32_t evictions;         /* aircraft dropped because the table was full */
    uint32_t expired;           /* aircraft dropped after track_expiry        */
    uint32_t snapshots;         /* snapshots sent to ttcd                    */
    uint32_t left_out;          /* aircraft beyond snapshot_max, summed      */
    uint32_t ipc_tx_drops;      /* frames ttcd's queue refused               */
    uint32_t ipc_rx_bad;        /* frames from ttcd that made no sense here  */
    uint32_t anchors;           /* wall-clock anchors applied                */
    uint16_t dump1090_restarts;
} adsbd_counters_t;

typedef struct {
    adsbd_params_t   p;
    adsbd_ops_t      ops;
    adsbd_counters_t n;
    adsbd_table_t    table;
    int64_t          wall_offset_ns;    /* wall clock = monotonic + this      */
    bool             time_synced;       /* the offset came from the ground    */
    bool             ipc_up;
    uint16_t         ipc_seq;
    uint64_t         next_snapshot, next_stat, next_report;
} adsbd_core_t;

/* wall_offset_ns: the system clock minus the monotonic one, at start. */
void adsbd_init(adsbd_core_t *c, const adsbd_params_t *p, const adsbd_ops_t *ops,
                uint64_t now_ns, int64_t wall_offset_ns);

/* One SBS line, without its newline, that arrived at now_ns. */
void adsbd_on_line(adsbd_core_t *c, uint64_t now_ns, const char *line, size_t len);

/* A line longer than the shell's buffer, discarded there: counted here. */
void adsbd_on_overlong_line(adsbd_core_t *c, uint64_t now_ns);

void adsbd_on_ipc_link(adsbd_core_t *c, uint64_t now_ns, bool up);
void adsbd_on_ipc_frame(adsbd_core_t *c, uint64_t now_ns, const uint8_t *buf, size_t len);

/* The shell started dump1090 again after it died. */
void adsbd_on_dump1090_restart(adsbd_core_t *c, uint64_t now_ns);

void adsbd_on_tick(adsbd_core_t *c, uint64_t now_ns);

/* The earliest instant adsbd_on_tick() has something to do. */
uint64_t adsbd_next_deadline(const adsbd_core_t *c);

/* The wall-clock time, in nanoseconds since the epoch, of a monotonic one. */
uint64_t adsbd_wall_ns(const adsbd_core_t *c, uint64_t mono_ns);

#ifdef __cplusplus
}
#endif

#endif /* ADSBD_CORE_H */
