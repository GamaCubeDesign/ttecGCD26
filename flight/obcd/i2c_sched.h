/*
 * i2c_sched.h — what obcd's I2C thread does on the shared bus, and when.
 *
 * Pure decisions, no I/O (ADR-0005): the thread asks what to do next,
 * performs it on /dev/i2c-1 and reports how it went. The bus is shared with
 * the AOCS ESP32 and lent to it during ADVANCED_AOCS (ADR-0017,
 * docs/icd/obc-aocs-i2c.md §4); this is where the lease is kept, so the
 * timeline lives in one testable place:
 *
 *   POLLING   the INA219, every power_period (re-configured first after a
 *             lease, or after a failure)
 *   STARTING  START written at t0; STATUS read from t0+20 ms, never at or
 *             after t0+100 ms: ACCEPTED or no clear answer -> LENT;
 *             REFUSED -> POLLING; a START that did not go through and no
 *             STATUS read that did either -> nobody there, POLLING
 *   LENT      no transaction at all until lease_until
 *             (t0 + 250 ms + max_task + 2 s)
 *   RESUMING  STATUS read (the task's result), up to 5 tries, then POLLING
 *
 * A START whose write was NACKed (no ESP32) lends nothing. Neither does one
 * whose write failed otherwise if no STATUS read goes through after it: the
 * Pi's driver reports an address nobody answers as EIO, not only as a NACK
 * (bench of 2026-11-05), and an ESP32 that took the START answers STATUS
 * within 5 ms (ICD §3), long before it may take the bus at t0+250 ms.
 */

#ifndef OBCD_I2C_SCHED_H
#define OBCD_I2C_SCHED_H

/* The longest a STATUS read may take: 21 bytes at 100 kHz is about 2 ms. A
 * read is started only if it ends before the Pi's release, t0+100 ms. */
#define I2C_SCHED_READ_MS        5u
#define I2C_SCHED_START_TRIES    3
#define I2C_SCHED_START_SPACING  20u
#define I2C_SCHED_RESUME_TRIES   5
#define I2C_SCHED_RESUME_SPACING 50u

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gama_aocs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    I2C_OP_NONE = 0,
    I2C_OP_INA219_INIT,      /* write the INA219's configuration           */
    I2C_OP_INA219_READ,      /* one voltage and current reading            */
    I2C_OP_AOCS_START,       /* write the START frame                      */
    I2C_OP_AOCS_STATUS       /* read one STATUS frame                      */
} i2c_op_t;

/* How a transaction went. NACK: the address did not answer — nobody there. */
typedef enum { I2C_OK = 0, I2C_NACK = 1, I2C_ERROR = 2 } i2c_result_t;

typedef enum {
    I2C_PH_POLLING = 0, I2C_PH_STARTING, I2C_PH_LENT, I2C_PH_RESUMING
} i2c_phase_t;

/* What the thread reports to the main loop. */
typedef enum {
    I2C_EV_NONE = 0,
    I2C_EV_AOCS_ABSENT,      /* nobody took the START: no lease            */
    I2C_EV_AOCS_REFUSED,     /* the ESP32 said no: no lease                */
    I2C_EV_AOCS_LENT,        /* ACCEPTED, or no clear answer: bus lent     */
    I2C_EV_AOCS_RESULT,      /* the lease ended; status valid or not       */
} i2c_event_t;

typedef struct {
    uint32_t power_period_ms;   /* INA219 polls while the bus is ours       */
} i2c_sched_params_t;

typedef struct {
    i2c_sched_params_t p;
    i2c_phase_t phase;
    uint64_t next_power;        /* next INA219 reading                      */
    bool     ina219_ready;      /* configured since the last lease/failure  */

    bool     start_pending;     /* the main loop asked for a START          */
    uint8_t  start[GAMA_AOCS_COMMAND_LEN];
    uint8_t  start_seq;
    uint16_t max_task_s;

    uint64_t t0;                /* end of the START write                   */
    uint64_t lease_until;       /* from the START write: the first instant
                                   the Pi may talk, if the ESP32 takes the
                                   bus — known before its answer, so it can
                                   be kept on disk at once                  */
    uint64_t next_try;
    int      tries;
    bool     start_written;     /* the START write went through             */
    bool     answered;          /* a STATUS read went through since          */
} i2c_sched_t;

void i2c_sched_init(i2c_sched_t *s, const i2c_sched_params_t *p, uint64_t now_ms);

/* A lease that a previous obcd left behind (kept on disk): the bus stays
 * silent until it ends. */
void i2c_sched_resume_lease(i2c_sched_t *s, uint64_t now_ms, uint64_t lease_until);

/* The main loop wants the ESP32 to start a task: frame is an encoded
 * COMMAND (common/gama_aocs.h) with cmd START. */
void i2c_sched_request_start(i2c_sched_t *s, const uint8_t *frame, uint8_t seq,
                             uint16_t max_task_s);

/* What to do now (*op, or I2C_OP_NONE) and when to ask again (*wake). Some
 * steps depend only on time — no clear answer by t0+100 ms means the bus is
 * lent — so this may also return an event for the main loop; then *op is
 * I2C_OP_NONE and *wake is now: ask again at once. */
i2c_event_t i2c_sched_next(i2c_sched_t *s, uint64_t now_ms, i2c_op_t *op, uint64_t *wake);

/* The outcome of the operation i2c_sched_next() gave, done at now_ms (its
 * end). For I2C_OP_AOCS_STATUS, status holds the bytes read. Returns what to
 * tell the main loop; *status_valid says whether those bytes were a valid
 * STATUS answering our START (for I2C_EV_AOCS_RESULT). */
i2c_event_t i2c_sched_done(i2c_sched_t *s, uint64_t now_ms, i2c_op_t op, i2c_result_t r,
                           const uint8_t *status, bool *status_valid);

#ifdef __cplusplus
}
#endif

#endif /* OBCD_I2C_SCHED_H */
