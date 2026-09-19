/* Shared between the three translation units of the ttcd core. Not part of
 * the core's API: the shell and the tests include core.h only. */

#ifndef TTCD_CORE_INTERNAL_H
#define TTCD_CORE_INTERNAL_H

#include "core.h"

/* ---- link.c ---- */

void core_log(ttcd_core_t *c, uint64_t now, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Silence the satellite keeps after each of its own transmissions, so that
 * the ground's reply is detectable before the next one starts (ADR-0007). */
uint64_t core_gap_ms(const ttcd_core_t *c);

void core_try_transmit(ttcd_core_t *c, uint64_t now);

/* Sends one IPC frame to a peer role; counts and logs a failed delivery. */
bool core_ipc_send(ttcd_core_t *c, uint64_t now, uint8_t role, uint8_t type,
                   const uint8_t *payload, uint8_t len);

/* Starts a commanded profile change: switch now, revert unless confirmed. */
void core_begin_rate_change(ttcd_core_t *c, uint64_t now, uint8_t profile);

/* ---- tc_dispatch.c ---- */

void tc_handle(ttcd_core_t *c, uint64_t now, const gama_frame_t *f);
bool ack_pending(const ttcd_core_t *c);
void ack_pop(ttcd_core_t *c, ttcd_ack_t *out);
const ttcd_ack_t *ack_peek(const ttcd_core_t *c);

/* ---- tm_sched.c ---- */

void tm_init(ttcd_core_t *c, uint64_t now);

/* What to transmit next, if anything is due. May arm the next snapshot. */
ttcd_tx_kind_t tm_pick(ttcd_core_t *c, uint64_t now);

/* Encodes the frame for `kind` with sequence c->tx_seq. No side effects:
 * the transmission may still be deferred by listen-before-talk. */
size_t tm_build(ttcd_core_t *c, uint64_t now, ttcd_tx_kind_t kind,
                uint8_t *frame, size_t cap);

/* Side effects of a transmission that actually started. */
void tm_commit(ttcd_core_t *c, uint64_t now, ttcd_tx_kind_t kind);

/* Earliest instant anything becomes due; 0 if something is due now. */
uint64_t tm_next_due(const ttcd_core_t *c);

void tm_on_profile_change(ttcd_core_t *c, uint64_t now, uint8_t from);
void tm_on_tracks(ttcd_core_t *c, uint64_t now, const gama_frame_t *f);

#endif /* TTCD_CORE_INTERNAL_H */
