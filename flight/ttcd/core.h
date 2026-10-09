/*
 * core.h — the ttcd core: link protocol, telecommand dispatch and telemetry
 * scheduling, with no system calls (ADR-0005).
 *
 * The core is driven entirely by events stamped with a monotonic time in
 * milliseconds, and it acts on the world only through ttcd_ops_t. The shell
 * (main.c) owns the file descriptors, the epoll loop and the clock; the tests
 * own a simulated clock and a simulated radio. Nothing in the core may block,
 * sleep, read the clock or touch a file descriptor — that is what makes the
 * link protocol testable before the hardware (tests/test_link_sim.c).
 *
 * Protocol decisions implemented here are recorded in ADR-0007.
 */

#ifndef TTCD_CORE_H
#define TTCD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gama_frame.h"
#include "gama_ipc.h"
#include "gama_tm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exit status requested by GAMA_TC_SHUTDOWN. The systemd unit lists it in
 * RestartPreventExitStatus= so a commanded shutdown stays down. */
#define TTCD_EXIT_SHUTDOWN 64

/* Results of ttcd_ops_t.radio_tx. */
enum {
    TTCD_TX_STARTED = 0,
    TTCD_TX_BUSY    = 1,   /* a reception is in progress: listen before talk */
    TTCD_TX_ERROR   = -1
};

/* Extra silence after a commanded profile switch. Covers the ground's settle
 * time before its confirmation (50 ms, common/gama_gs_link.c) with margin. */
#define TTCD_SWITCH_GAP_EXTRA_MS 100u

/* Telemetry field value meaning "no data received yet" (HK has no per-field
 * validity bits). battery_mv uses 0 for the same purpose. */
#define TTCD_HK_UNKNOWN INT16_MIN

/* Every duration is in milliseconds. ttcd_params_default() gives the values
 * the data budget and ADR-0007 are built on; the shell overrides them from
 * the configuration file. */
typedef struct {
    uint8_t  initial_profile;       /* gama_rate_profile_t                    */
    int8_t   tx_power_dbm;          /* 2 .. 20                                */
    uint32_t hk_period;             /* housekeeping, outside SAFE             */
    uint32_t stat_period;           /* statistics, while streaming            */
    uint32_t safe_beacon_period;    /* beacon while in SAFE                   */
    uint32_t safe_hk_period;        /* housekeeping while in SAFE             */
    uint32_t contact_timeout;       /* silence from the ground before SAFE    */
    uint32_t rate_revert;           /* unconfirmed profile change reverts     */
    uint32_t reaction_margin;       /* ground turnaround, added to the gap    */
    uint32_t dup_window;            /* same seq + payload = retransmission    */
} ttcd_params_t;

void ttcd_params_default(ttcd_params_t *p);

typedef struct {
    void *ctx;
    /* Starts transmitting one frame and returns at once: TTCD_TX_STARTED,
     * TTCD_TX_BUSY if the modem is receiving, or TTCD_TX_ERROR. The shell
     * reports the end with ttcd_on_radio_tx_done(). */
    int  (*radio_tx)(void *ctx, const uint8_t *frame, size_t len);
    /* Reconfigures the modem. Called only while not transmitting. 0 on success. */
    int  (*radio_set_profile)(void *ctx, uint8_t profile);
    int  (*radio_set_power)(void *ctx, int8_t dbm);
    /* Sends a frame to a connected peer without blocking. 0 if sent. */
    int  (*ipc_send)(void *ctx, uint8_t role, const uint8_t *frame, size_t len);
    /* One structured log record: event name plus a JSON object body. */
    void (*log)(void *ctx, uint64_t now_ms, const char *event, const char *fields);
    /* SoC temperature in hundredths of a degree. Optional (may be NULL). */
    int  (*soc_temp)(void *ctx, int16_t *ccel);
    /* Asks the shell to exit with this status once the current event returns. */
    void (*exit_request)(void *ctx, int code);
} ttcd_ops_t;

/* Counters, exposed for the logs, TM_STAT and the tests. */
typedef struct {
    uint32_t frames_tx;        /* frames put on the air                        */
    uint32_t frames_rx_ok;     /* valid frames received                        */
    uint32_t frames_rx_bad;    /* rejected: PHY CRC, version, length, app CRC  */
    uint32_t frames_foreign;   /* valid frames of a type the ground never sends */
    uint32_t tc_executed;      /* telecommands executed (not duplicates)       */
    uint32_t tc_duplicates;    /* retransmissions answered by ACK replay       */
    uint32_t lbt_defers;       /* transmissions deferred by listen-before-talk */
    uint32_t tx_watchdogs;     /* transmissions whose end was never reported   */
    uint32_t rate_reverts;
    uint32_t contact_losses;
    uint32_t ipc_rx;
    uint32_t ipc_rx_bad;
    uint32_t ipc_tx_drops;     /* IPC sends that could not be delivered        */
} ttcd_counters_t;

enum { TTCD_ACK_QUEUE = 8, TTCD_SNAPSHOT_FRAMES = GAMA_IPC_TRACKS_MAX_FRAMES };

typedef enum {
    TTCD_POST_NONE = 0,
    TTCD_POST_SET_PROFILE,     /* switch after the ACK has left, at the old rate */
    TTCD_POST_SET_POWER,
    TTCD_POST_EXIT
} ttcd_post_t;

typedef struct {
    uint8_t     cmd;
    uint8_t     status;
    uint16_t    echo_seq;
    ttcd_post_t post;
    int16_t     post_arg;
} ttcd_ack_t;

typedef enum {
    TTCD_TXK_NONE = 0, TTCD_TXK_ACK, TTCD_TXK_HK, TTCD_TXK_STAT,
    TTCD_TXK_TRACKS, TTCD_TXK_BEACON
} ttcd_tx_kind_t;

/* One snapshot of the track table, as received from adsbd. */
typedef struct {
    bool     valid;
    uint32_t epoch_ms;
    uint8_t  count;                              /* frames in the snapshot   */
    uint8_t  have;                               /* bitmask of indices held  */
    uint8_t  nrec[TTCD_SNAPSHOT_FRAMES];
    uint8_t  rec[TTCD_SNAPSHOT_FRAMES][GAMA_IPC_TRACKS_MAX * GAMA_TRACK_WIRE_LEN];
} ttcd_snapshot_t;

/* The whole core state. Fixed size: the shell allocates one statically. */
typedef struct {
    ttcd_params_t    p;
    ttcd_ops_t       ops;
    ttcd_counters_t  n;
    uint64_t         boot_ms;

    /* radio */
    uint8_t  profile;
    uint8_t  prev_profile;
    bool     rate_pending;          /* switched, not yet heard at the new rate */
    uint64_t rate_deadline;
    int8_t   tx_power;
    bool     tx_busy;
    uint64_t tx_started;
    uint64_t tx_deadline;           /* watchdog if TxDone never arrives        */
    ttcd_tx_kind_t tx_kind;
    ttcd_ack_t tx_ack;              /* the ACK on the air, for its post-action */
    uint16_t tx_seq;
    uint16_t ipc_seq;
    uint64_t quiet_until;           /* no transmission before this instant     */
    uint64_t last_contact;          /* last valid frame from the ground        */

    /* telecommands */
    ttcd_ack_t acks[TTCD_ACK_QUEUE];
    uint8_t    ack_head, ack_count;
    struct {
        bool     valid;
        uint16_t seq;
        uint8_t  len;
        uint8_t  payload[16];
        uint8_t  status;
        uint64_t at;
    } last_tc;

    /* telemetry scheduling */
    bool     stream_on;
    uint32_t stream_period;
    uint64_t next_tracks, next_hk, next_stat, next_beacon;
    bool     hk_requested, stat_requested;
    ttcd_snapshot_t latest;         /* newest snapshot from adsbd              */
    ttcd_snapshot_t sending;        /* the one being transmitted               */
    uint8_t  sending_next;          /* next frame index of `sending`           */
    bool     has_started;           /* any snapshot has started transmitting   */
    uint32_t last_started_epoch;
    bool     has_boundary;
    uint32_t fresh_boundary;        /* epoch of the snapshot sent before this  */
    uint32_t latency_hist[256];     /* ages at transmission, 100 ms bins       */

    /* what the peers told us */
    bool     obc_linked, adsbd_linked;
    uint8_t  obc_mode;
    bool     have_telemetry;
    gama_ipc_telemetry_t telemetry;
    bool     have_stat;
    gama_ipc_stat_t      payload_stat;
    bool     time_synced;
    uint64_t time_anchor_mono;      /* monotonic ms at which ...               */
    uint32_t time_anchor_unix_s;    /* ... this wall-clock time held           */
    uint32_t time_anchor_nsec;
} ttcd_core_t;

void ttcd_init(ttcd_core_t *c, const ttcd_params_t *p, const ttcd_ops_t *ops,
               uint64_t now_ms);

void ttcd_on_radio_rx(ttcd_core_t *c, uint64_t now_ms,
                      const uint8_t *buf, size_t len, int16_t rssi_dbm, int8_t snr_db);
void ttcd_on_radio_rx_error(ttcd_core_t *c, uint64_t now_ms);
void ttcd_on_radio_tx_done(ttcd_core_t *c, uint64_t now_ms);

void ttcd_on_ipc_peer(ttcd_core_t *c, uint64_t now_ms, uint8_t role, bool connected);
void ttcd_on_ipc_frame(ttcd_core_t *c, uint64_t now_ms, uint8_t role,
                       const uint8_t *buf, size_t len);

void ttcd_on_tick(ttcd_core_t *c, uint64_t now_ms);

/* The earliest instant at which ttcd_on_tick() has something to do, or
 * UINT64_MAX. The shell arms one timer to it after every event. */
uint64_t ttcd_next_deadline(const ttcd_core_t *c);

/* The link mode reported in TM_HK: GAMA_LINK_IDLE, _STREAM or _SAFE. */
uint8_t ttcd_link_mode(const ttcd_core_t *c);

/* p95 of the latency from reception to transmission, in ms, over every
 * update transmitted for the first time; 0xFFFF before the first one. */
uint16_t ttcd_latency_p95_ms(const ttcd_core_t *c);

#ifdef __cplusplus
}
#endif

#endif /* TTCD_CORE_H */
