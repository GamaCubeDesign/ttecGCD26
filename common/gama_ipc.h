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

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to a payload below. A peer that
 * announces another version is refused at HELLO, not discovered later as
 * garbage. */
#define GAMA_IPC_VERSION 1u

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
 *  Platform quantities ttcd cannot measure itself (HLR-COMM-02). Sent by the
 *  OBC whenever it has fresh values; ttcd keeps the latest.
 *
 *  temp_bat_ccel is here although TM_HK does not carry it yet: HLR-EPS-04 is
 *  verified by telemetry analysis, and ttcd logs every IPC_TELEMETRY
 *  onboard, so battery temperature is recorded from the first integration
 *  even before the downlink record grows a field for it.
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
 *  IPC_MODE — OBC -> ttcd.  1 B
 *  The OBC's current gama_obc_state_t, sent on every transition and at
 *  connection. The OBC is the single source of truth for the mission mode;
 *  ttcd only reports it (ADR-0005).
 * ------------------------------------------------------------------ */
#define GAMA_IPC_MODE_LEN 1u

/* ------------------------------------------------------------------ *
 *  IPC_TRACKS — adsbd -> ttcd.  4 + 20 N B, N <= 12
 *
 *  u32 epoch_ms, then N track records exactly as they will go on the air
 *  (gama_tm.h). epoch_ms is adsbd's CLOCK_MONOTONIC in milliseconds, low 32
 *  bits, at the moment the snapshot was taken, and each record's age_ds is
 *  measured from its last update to that epoch.
 *
 *  ttcd rewrites each age to be measured to the start of the transmission
 *  instead (gama_track_age_add) and drops the epoch: both processes read the
 *  same monotonic clock, so the rewrite needs no clock synchronisation.
 *  Several frames with the same epoch form one snapshot.
 * ------------------------------------------------------------------ */
#define GAMA_IPC_TRACKS_HEADER_LEN 4u
#define GAMA_IPC_TRACKS_MAX        12u

/* ------------------------------------------------------------------ *
 *  IPC_STAT — adsbd -> ttcd.  12 B
 *  Payload counters for TM_STAT and TM_HK (HLR-ADS-08).
 * ------------------------------------------------------------------ */
#define GAMA_IPC_STAT_LEN 12u

typedef struct {
    uint32_t msgs_received;     /* SBS lines read from dump1090        */
    uint32_t msgs_decoded;      /* of those, applied to a track        */
    uint16_t aircraft_tracked;
    uint16_t dump1090_restarts;
} gama_ipc_stat_t;

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

/* IPC_TRACKS is variable length: header, then whole records. The decoder
 * reports how many records follow and rejects a payload that is not a whole
 * number of them. */
int gama_ipc_tracks_header_encode(uint8_t *out, size_t cap, uint32_t epoch_ms);
int gama_ipc_tracks_decode(const uint8_t *in, size_t len,
                           uint32_t *epoch_ms, size_t *n_records);

const char *gama_ipc_role_name(uint8_t role);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_IPC_H */
