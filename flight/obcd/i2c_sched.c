#include "i2c_sched.h"

#include <string.h>

void i2c_sched_init(i2c_sched_t *s, const i2c_sched_params_t *p, uint64_t now)
{
    memset(s, 0, sizeof(*s));
    s->p = *p;
    s->phase = I2C_PH_POLLING;
    s->next_power = now;
}

void i2c_sched_resume_lease(i2c_sched_t *s, uint64_t now, uint64_t lease_until)
{
    if (lease_until > now) {
        s->phase = I2C_PH_LENT;
        s->lease_until = lease_until;
        s->start_seq = 0;            /* whatever result comes is not ours */
    }
}

void i2c_sched_request_start(i2c_sched_t *s, const uint8_t *frame, uint8_t seq,
                             uint16_t max_task_s)
{
    memcpy(s->start, frame, GAMA_AOCS_COMMAND_LEN);
    s->start_seq = seq;
    s->max_task_s = max_task_s;
    s->start_pending = true;
}

static uint64_t lease_end(const i2c_sched_t *s)
{
    return s->t0 + GAMA_AOCS_T_TAKEOVER_MS + (uint64_t)s->max_task_s * 1000u +
           GAMA_AOCS_T_RESUME_MARGIN_MS;
}

/* The last instant a STATUS read may begin and still end before t0+100. */
static uint64_t last_start_read(const i2c_sched_t *s)
{
    return s->t0 + GAMA_AOCS_T_RELEASE_MS - I2C_SCHED_READ_MS;
}

static i2c_event_t lend(i2c_sched_t *s)
{
    s->phase = I2C_PH_LENT;
    s->lease_until = lease_end(s);
    return I2C_EV_AOCS_LENT;
}

i2c_event_t i2c_sched_next(i2c_sched_t *s, uint64_t now, i2c_op_t *op, uint64_t *wake)
{
    *op = I2C_OP_NONE;
    *wake = UINT64_MAX;
    switch (s->phase) {
    case I2C_PH_POLLING:
        if (s->start_pending) {
            *op = I2C_OP_AOCS_START;
            *wake = now;
            return I2C_EV_NONE;
        }
        if (now >= s->next_power) {
            *op = s->ina219_ready ? I2C_OP_INA219_READ : I2C_OP_INA219_INIT;
            *wake = now;
            return I2C_EV_NONE;
        }
        *wake = s->next_power;
        return I2C_EV_NONE;

    case I2C_PH_STARTING:
        if (s->tries >= I2C_SCHED_START_TRIES || s->next_try > last_start_read(s) ||
            now > last_start_read(s)) {
            /* No clear answer in time: the ESP32 may have accepted, so the
             * Pi assumes the worst and stays silent (ICD §4). */
            return lend(s);
        }
        if (now >= s->next_try) {
            *op = I2C_OP_AOCS_STATUS;
            *wake = now;
        } else {
            *wake = s->next_try;
        }
        return I2C_EV_NONE;

    case I2C_PH_LENT:
        if (now < s->lease_until) {
            *wake = s->lease_until;         /* not one transaction before */
            return I2C_EV_NONE;
        }
        s->phase = I2C_PH_RESUMING;
        s->tries = 0;
        s->next_try = now;
        *op = I2C_OP_AOCS_STATUS;
        *wake = now;
        return I2C_EV_NONE;

    case I2C_PH_RESUMING:
        if (now >= s->next_try) {
            *op = I2C_OP_AOCS_STATUS;
            *wake = now;
        } else {
            *wake = s->next_try;
        }
        return I2C_EV_NONE;
    }
    return I2C_EV_NONE;
}

/* A STATUS that answers our START, whatever it says. */
static bool ours(const i2c_sched_t *s, const uint8_t *bytes, gama_aocs_status_t *st)
{
    return bytes != NULL &&
           gama_aocs_status_decode(bytes, GAMA_AOCS_STATUS_LEN, st) == (int)GAMA_AOCS_STATUS_LEN &&
           st->seq == s->start_seq;
}

static void back_to_polling(i2c_sched_t *s, uint64_t now)
{
    s->phase = I2C_PH_POLLING;
    s->ina219_ready = false;            /* configure it again before reading */
    s->next_power = now;
}

i2c_event_t i2c_sched_done(i2c_sched_t *s, uint64_t now, i2c_op_t op, i2c_result_t r,
                           const uint8_t *status, bool *status_valid)
{
    gama_aocs_status_t st;
    *status_valid = false;
    switch (op) {
    case I2C_OP_INA219_INIT:
        s->ina219_ready = r == I2C_OK;
        if (r != I2C_OK) {
            s->next_power = now + s->p.power_period_ms;
        }
        return I2C_EV_NONE;

    case I2C_OP_INA219_READ:
        s->next_power = now + s->p.power_period_ms;
        if (r != I2C_OK) {
            s->ina219_ready = false;
        }
        return I2C_EV_NONE;

    case I2C_OP_AOCS_START:
        s->start_pending = false;
        if (r == I2C_NACK) {
            return I2C_EV_AOCS_ABSENT;   /* nobody took it: nothing lent */
        }
        /* Written, or failed in a way that may have reached the ESP32: from
         * here on the timeline runs, counted from the end of the write. */
        s->t0 = now;
        s->phase = I2C_PH_STARTING;
        s->tries = 0;
        s->next_try = s->t0 + GAMA_AOCS_T_READ_MIN_MS;
        return I2C_EV_NONE;

    case I2C_OP_AOCS_STATUS:
        if (s->phase == I2C_PH_STARTING) {
            s->tries++;
            s->next_try = now + I2C_SCHED_START_SPACING;
            if (r == I2C_OK && ours(s, status, &st)) {
                if (st.state == GAMA_AOCS_ST_ACCEPTED) {
                    return lend(s);
                }
                if (st.state == GAMA_AOCS_ST_REFUSED) {
                    back_to_polling(s, now);
                    return I2C_EV_AOCS_REFUSED;
                }
            }
            return I2C_EV_NONE;          /* i2c_sched_next() decides what follows */
        }
        if (s->phase == I2C_PH_RESUMING) {
            s->tries++;
            s->next_try = now + I2C_SCHED_RESUME_SPACING;
            if (r == I2C_OK && ours(s, status, &st)) {
                *status_valid = true;
                back_to_polling(s, now);
                return I2C_EV_AOCS_RESULT;
            }
            if (s->tries >= I2C_SCHED_RESUME_TRIES) {
                back_to_polling(s, now);
                return I2C_EV_AOCS_RESULT;   /* with no valid status */
            }
            return I2C_EV_NONE;
        }
        return I2C_EV_NONE;

    case I2C_OP_NONE:
    default:
        return I2C_EV_NONE;
    }
}
