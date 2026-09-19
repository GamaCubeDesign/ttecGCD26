/*
 * gama_gs_link.h — the ground station side of the link protocol (ADR-0007).
 *
 * The counterpart of the satellite's ttcd core. Runs on the ESP32 ground
 * station, in the bench tool on a second Raspberry Pi radio, and inside the
 * channel simulation that tests both sides together — the same code in all
 * three, for the same reason the codec is shared (ADR-0002).
 *
 * What it does:
 *   - queues telecommands and sends them one at a time, stop-and-wait:
 *     the next one waits for the acknowledgement of the current one;
 *   - retransmits with the same sequence number and payload when the ACK does
 *     not arrive, so the satellite can recognise the repeat; SET_TIME is the
 *     exception and is re-stamped on every attempt;
 *   - listens before talking, and prefers the reply gap the satellite leaves
 *     after each of its frames;
 *   - follows a commanded profile change, confirms it with a PING at the new
 *     profile, and reverts if the confirmation does not come back;
 *   - keeps the link alive with a PING when idle, falls back to SAFE when the
 *     satellite goes silent, and commands the previous profile again once it
 *     hears the satellite at SAFE;
 *   - counts downlink frames lost, by gaps in the satellite's sequence
 *     numbers (HLR-ADS-08).
 *
 * Pure state machine: no clock, no I/O, no allocation. Driven by events
 * stamped with a monotonic time in milliseconds.
 */

#ifndef GAMA_GS_LINK_H
#define GAMA_GS_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GS_TC_QUEUE    8u
#define GS_TC_MAX_ARGS 8u

/* Protocol timing constants, part of the HLR-COMM-01 bound (ADR-0007) and
 * reproduced by tools/analysis/lora_budget.py. */
#define GS_SETTLE_MS        50u    /* wait for the satellite to reconfigure   */
#define GS_BACKOFF_FIRST_MS 250u   /* first retransmission backoff, doubling  */
#define GS_BACKOFF_MAX_MS   2000u  /* ... up to this                          */

/* Results of gs_ops_t.radio_tx, the same convention as the satellite's. */
enum { GS_TX_STARTED = 0, GS_TX_BUSY = 1, GS_TX_ERROR = -1 };

typedef struct {
    uint8_t  initial_profile;
    uint32_t keepalive;        /* PING after this long without a telecommand */
    uint32_t contact_timeout;  /* satellite silence before falling to SAFE   */
    uint32_t rate_revert;      /* unconfirmed profile change reverts         */
    uint32_t give_up;          /* a telecommand is abandoned after this long */
} gs_params_t;

void gs_params_default(gs_params_t *p);

typedef enum {
    GS_EV_ACK = 1,     /* a telecommand was acknowledged                    */
    GS_EV_TC_FAILED,   /* a telecommand was abandoned after give_up         */
    GS_EV_FRAME,       /* a valid downlink frame arrived                    */
    GS_EV_LOSS,        /* the downlink sequence skipped `lost` frames       */
    GS_EV_RATE,        /* the ground changed profile                        */
    GS_EV_CONTACT      /* contact lost (`up` false) or regained (`up` true) */
} gs_event_kind_t;

typedef struct {
    gs_event_kind_t kind;
    uint32_t id;           /* ACK, TC_FAILED: submit id; 0 for internal TCs  */
    uint8_t  cmd;          /* ACK, TC_FAILED                                 */
    uint8_t  status;       /* ACK: gama_ack_status_t                         */
    uint8_t  attempts;     /* ACK, TC_FAILED                                 */
    uint32_t latency_ms;   /* ACK: from submission to the ACK arriving       */
    uint8_t  type;         /* FRAME                                          */
    uint16_t seq;          /* FRAME                                          */
    const uint8_t *payload;/* FRAME: valid only during the callback          */
    uint8_t  len;          /* FRAME                                          */
    int16_t  rssi;         /* FRAME                                          */
    int8_t   snr;          /* FRAME                                          */
    uint32_t lost;         /* LOSS                                           */
    uint8_t  from, to;     /* RATE                                           */
    const char *reason;    /* RATE                                           */
    bool     up;           /* CONTACT                                        */
} gs_event_t;

typedef struct {
    void *ctx;
    int  (*radio_tx)(void *ctx, const uint8_t *frame, size_t len);
    int  (*radio_set_profile)(void *ctx, uint8_t profile);
    void (*event)(void *ctx, uint64_t now_ms, const gs_event_t *ev);
    /* Wall-clock time for SET_TIME, read at each transmission attempt.
     * Optional: without it, SET_TIME is sent with the caller's arguments. */
    int  (*wall_time)(void *ctx, uint32_t *unix_s, uint32_t *nsec);
} gs_ops_t;

typedef struct {
    uint32_t id;
    uint8_t  payload[1u + GS_TC_MAX_ARGS];
    uint8_t  len;
    uint16_t seq;
    uint8_t  attempts;
    uint64_t submitted;
    uint64_t next_try;   /* earliest attempt outside a reply gap (backoff)   */
    uint64_t not_before; /* earliest attempt at all (settle after a switch)  */
    bool     gap_only;   /* transmit only in the gap after a satellite frame */
} gs_tc_t;

typedef struct {
    uint32_t dl_frames;     /* valid downlink frames                          */
    uint32_t dl_lost;       /* frames missing from the sequence               */
    uint32_t dl_bad;        /* frames that failed PHY CRC or decoding         */
    uint32_t dl_resyncs;    /* sequence jumps too large to be losses (reboot) */
    uint32_t tc_sent;       /* transmission attempts                          */
    uint32_t tc_acked;
    uint32_t tc_failed;
    uint32_t acks_unmatched;
    uint32_t lbt_defers;
    uint32_t tx_watchdogs;
} gs_counters_t;

typedef struct {
    gs_params_t   p;
    gs_ops_t      ops;
    gs_counters_t n;

    uint8_t  profile, prev_profile, desired_profile;
    bool     rate_pending;
    uint64_t rate_deadline;
    bool     recovering;          /* an automatic return from SAFE in flight */

    bool     tx_busy;
    uint64_t tx_started;
    uint64_t tx_deadline;         /* watchdog if the end is never reported   */
    bool     awaiting_ack;
    uint64_t ack_deadline;

    gs_tc_t  q[GS_TC_QUEUE];
    uint8_t  q_head, q_count;
    uint16_t next_seq;
    uint32_t next_id;

    uint64_t last_rx;             /* last valid satellite frame              */
    uint64_t last_tc_tx;          /* last telecommand put on the air         */
    bool     in_contact;
    bool     dl_seq_valid;
    uint16_t dl_last_seq;
} gs_link_t;

void gs_init(gs_link_t *g, const gs_params_t *p, const gs_ops_t *ops, uint64_t now);

/*
 * Queues a telecommand. Returns its id (> 0), or 0 if the queue is full or
 * the argument length is wrong for the command (checked against
 * gama_tc_arg_len, so a malformed command never reaches the air).
 */
uint32_t gs_submit(gs_link_t *g, uint64_t now, uint8_t cmd,
                   const uint8_t *args, uint8_t arg_len);

void gs_on_rx(gs_link_t *g, uint64_t now, const uint8_t *buf, size_t len,
              int16_t rssi, int8_t snr);
void gs_on_rx_error(gs_link_t *g, uint64_t now);
void gs_on_tx_done(gs_link_t *g, uint64_t now);
void gs_on_tick(gs_link_t *g, uint64_t now);
uint64_t gs_next_deadline(const gs_link_t *g);

/* How long to wait for an acknowledgement after a telecommand has left. */
uint32_t gs_ack_timeout_ms(uint8_t profile);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_GS_LINK_H */
