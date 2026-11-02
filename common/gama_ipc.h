/*
 * gama_ipc.h — payloads of the internal messages between the flight processes.
 *
 * ttcd is the hub (ADR-0005): adsbd and the OBC connect to it over an AF_UNIX
 * SOCK_SEQPACKET socket (ADR-0006), and every datagram on that socket is a
 * complete GAMA frame (common/gama_frame.h) whose type is one of the
 * GAMA_FRAME_IPC_* values. This header defines what those frames carry.
 *
 * The OBC is maintained by another team in another repository. It receives
 * these files as part of the OBC<->TT&C reference implementation, so every
 * layout here is part of an interface contract (docs/icd/obc-ttec-icd.md):
 * fixed sizes, explicit little-endian fields, no struct casts.
 *
 * Shared verbatim with the OBC and the ESP32 build. No platform headers.
 */

#ifndef GAMA_IPC_H
#define GAMA_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "gama_tm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to a payload below. A peer that
 * announces another version is refused at HELLO, not discovered later as
 * garbage. */
#define GAMA_IPC_VERSION 2u   /* ADR-0013 */

/* Who is on the other end of a connection. A client's first frame must be
 * IPC_HELLO; frames arriving before it are rejected and counted. */
typedef enum {
    GAMA_IPC_ROLE_NONE  = 0,
    GAMA_IPC_ROLE_ADSBD = 1,  /* payload daemon                          */
    GAMA_IPC_ROLE_OBC   = 2,  /* on-board computer (other repository)    */
    GAMA_IPC_ROLE_TOOL  = 3   /* diagnostics and test clients            */
} gama_ipc_role_t;

/* ------------------------------------------------------------------ *
 *  IPC_HELLO — client -> ttcd, first frame on every connection.  2 B
 * ------------------------------------------------------------------ */
#define GAMA_IPC_HELLO_LEN 2u

typedef struct {
    uint8_t role;     /* gama_ipc_role_t */
    uint8_t version;  /* GAMA_IPC_VERSION */
} gama_ipc_hello_t;

/* ------------------------------------------------------------------ *
 *  IPC_TC_EVENT — ttcd -> OBC.  1 B
 *  One gama_obc_event_t (common/gama_tc.h). The OBC's adapter maps it to
 *  its own fsm.h Event and returns it from radio_poll_tc().
 * ------------------------------------------------------------------ */
#define GAMA_IPC_TC_EVENT_LEN 1u

/* ------------------------------------------------------------------ *
 *  IPC_TIME_SET — ttcd -> OBC and adsbd.  8 B
 *  Wall-clock time sent by the ground (GAMA_TC_SET_TIME carries the same
 *  8 bytes). The receiver pairs it with its own CLOCK_MONOTONIC reading on
 *  arrival to form the wall-clock anchor; nobody sets the system clock.
 * ------------------------------------------------------------------ */
#define GAMA_IPC_TIME_LEN 8u

typedef struct {
    uint32_t unix_s;  /* seconds since 1970-01-01T00:00:00Z */
    uint32_t nsec;    /* 0 .. 999 999 999                   */
} gama_ipc_time_t;

/* ------------------------------------------------------------------ *
 *  IPC_TELEMETRY — OBC -> ttcd.  14 B
 *  Platform quantities ttcd cannot measure itself (HLR-COMM-02, HLR-EPS-04).
 *  Sent by the OBC every 1 s (PLANO D6); ttcd keeps the latest.
 *
 *  A field the OBC could not read carries GAMA_NO_DATA_U16 (battery_mv) or
 *  GAMA_NO_DATA_I16 (the rest), so one failed sensor does not hide the
 *  others. The 14 bytes are, in the same order, bytes 0..13 of TM_HK
 *  (common/gama_tm.h): ttcd copies them through unchanged.
 * ------------------------------------------------------------------ */
#define GAMA_IPC_TELEMETRY_LEN 14u

typedef struct {
    uint16_t battery_mv;
    int16_t  current_ma;     /* negative: charging                   */
    int16_t  temp_bat_ccel;  /* battery, hundredths of a degree C    */
    int16_t  temp_ext_ccel;  /* external, hundredths of a degree C   */
    int16_t  roll_cdeg;      /* hundredths of a degree               */
    int16_t  pitch_cdeg;
    int16_t  yaw_cdeg;
} gama_ipc_telemetry_t;

/* ------------------------------------------------------------------ *
 *  IPC_MODE — OBC -> ttcd -> adsbd.  1 B
 *  The OBC's current gama_obc_state_t, sent on every transition and at
 *  connection. The OBC is the single source of truth for the mission mode;
 *  ttcd reports it and forwards it, unchanged, to adsbd, which adapts to it
 *  (CDR Table 55). ttcd also sends adsbd the current mode when adsbd
 *  connects, and GAMA_OBC_ST_UNKNOWN when the OBC disconnects.
 * ------------------------------------------------------------------ */
#define GAMA_IPC_MODE_LEN 1u

/* ------------------------------------------------------------------ *
 *  IPC_TRACKS — adsbd -> ttcd.  6 + 20 N B, N <= 12
 *
 *    u32 epoch_ms   adsbd's CLOCK_MONOTONIC, milliseconds, low 32 bits, at
 *                   the moment the snapshot was taken
 *    u8  index      this frame's position in the snapshot, from 0
 *    u8  count      frames in the snapshot, 1 .. GAMA_IPC_TRACKS_MAX_FRAMES
 *    N track records, exactly as they will go on the air (gama_tm.h), each
 *    age_ds measured from the aircraft's last update to epoch_ms
 *
 *  A snapshot of 20 aircraft does not fit one frame. index and count let
 *  ttcd know when it holds all of one, so a timer firing between two frames
 *  of the same snapshot cannot put half of it on the air.
 *
 *  ttcd rewrites each age to be measured to the start of the transmission
 *  instead (gama_track_age_add) and drops this header: both processes read
 *  the same monotonic clock, so the rewrite needs no synchronisation.
 * ------------------------------------------------------------------ */
#define GAMA_IPC_TRACKS_HEADER_LEN 6u
#define GAMA_IPC_TRACKS_MAX        12u  /* records per frame   */
#define GAMA_IPC_TRACKS_MAX_FRAMES  8u  /* frames per snapshot */

/* ------------------------------------------------------------------ *
 *  IPC_STAT — adsbd -> ttcd.  13 B
 *  Payload counters for TM_STAT and TM_HK (HLR-ADS-08).
 *
 *    u32 msgs_received  u32 msgs_decoded  u16 aircraft_tracked
 *    u16 dump1090_restarts  u8 flags (GAMA_IPC_STAT_F_*; others always 0)
 * ------------------------------------------------------------------ */
#define GAMA_IPC_STAT_LEN 13u

/* dump1090 is running and adsbd's SBS connection to it is open. ttcd
 * reports it as GAMA_HK_F_DUMP1090_UP. */
#define GAMA_IPC_STAT_F_DUMP1090_UP (1u << 0)

typedef struct {
    uint32_t msgs_received;     /* SBS lines read from dump1090        */
    uint32_t msgs_decoded;      /* of those, applied to a track        */
    uint16_t aircraft_tracked;
    uint16_t dump1090_restarts;
    uint8_t  flags;             /* GAMA_IPC_STAT_F_*                   */
} gama_ipc_stat_t;

/* ------------------------------------------------------------------ *
 *  IPC_ROSTER — adsbd -> ttcd.  11 N B, 1 <= N <= 22
 *
 *  N roster records (gama_tm.h), exactly as TM_ROSTER carries them. No
 *  header: each record stands alone, and receiving one twice is harmless.
 *  adsbd sends a record when it learns an aircraft's callsign or the
 *  callsign changes, and every record it holds when the connection to ttcd
 *  comes up, so a restarted ttcd recovers the whole mission's roster. ttcd
 *  keeps a table updated by ICAO address and answers REQ_ROSTER from it.
 * ------------------------------------------------------------------ */
#define GAMA_IPC_ROSTER_MAX GAMA_ROSTERS_PER_FRAME  /* records per frame */

/* Encoders return the number of bytes written; decoders the number of bytes
 * consumed. Both return a negative gama_frame_result_t on a NULL pointer or
 * a buffer of the wrong size. Decoders require the exact length: a payload
 * of any other size is a protocol error, not something to parse leniently. */
int gama_ipc_hello_encode(uint8_t *out, size_t cap, const gama_ipc_hello_t *h);
int gama_ipc_hello_decode(const uint8_t *in, size_t len, gama_ipc_hello_t *h);

int gama_ipc_time_encode(uint8_t *out, size_t cap, const gama_ipc_time_t *t);
int gama_ipc_time_decode(const uint8_t *in, size_t len, gama_ipc_time_t *t);

int gama_ipc_telemetry_encode(uint8_t *out, size_t cap, const gama_ipc_telemetry_t *t);
int gama_ipc_telemetry_decode(const uint8_t *in, size_t len, gama_ipc_telemetry_t *t);

int gama_ipc_stat_encode(uint8_t *out, size_t cap, const gama_ipc_stat_t *s);
int gama_ipc_stat_decode(const uint8_t *in, size_t len, gama_ipc_stat_t *s);

/* The number of roster records in an IPC_ROSTER payload, or
 * GAMA_FRAME_ERR_LEN if it is empty, too long or not whole records. */
int gama_ipc_roster_count(size_t len);

/* IPC_TRACKS is variable length: header, then whole records. The decoder
 * reports how many records follow, and rejects a payload that is not a whole
 * number of them or whose index/count are inconsistent. */
typedef struct {
    uint32_t epoch_ms;
    uint8_t  index;
    uint8_t  count;
} gama_ipc_tracks_hdr_t;

int gama_ipc_tracks_header_encode(uint8_t *out, size_t cap,
                                  const gama_ipc_tracks_hdr_t *h);
int gama_ipc_tracks_decode(const uint8_t *in, size_t len,
                           gama_ipc_tracks_hdr_t *h, size_t *n_records);

const char *gama_ipc_role_name(uint8_t role);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_IPC_H */
