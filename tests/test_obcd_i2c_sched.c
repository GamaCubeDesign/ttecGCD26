/*
 * obcd's I2C scheduler (flight/obcd/i2c_sched.h) on a virtual clock, against
 * a simulated AOCS ESP32 and INA219 on the shared bus (ADR-0017,
 * docs/icd/obc-aocs-i2c.md §4).
 *
 * The property that matters: once the ESP32 may own the bus, the Pi makes no
 * transaction at all until the lease ends — whatever happened on the bus
 * before. Every Pi transaction is recorded with the interval it occupies, and
 * each scenario checks none falls inside [t0 + 100 ms, lease end).
 */

#include "test_util.h"
#include "i2c_sched.h"
#include "gama_aocs.h"

#define MAX_TX 4096

typedef enum { ESP_ACCEPTS, ESP_REFUSES, ESP_ABSENT, ESP_GARBLED, ESP_STALE } esp_mode_t;

static struct {
    /* the simulated ESP32 */
    esp_mode_t mode;
    bool     resume_fails;      /* the final STATUS never reads cleanly      */
    uint64_t start_at;          /* end of the START write, as it saw it      */
    uint8_t  seq;
    bool     started;
    /* what the Pi did */
    uint64_t tx_begin[MAX_TX], tx_end[MAX_TX];
    i2c_op_t tx_op[MAX_TX];
    int      n_tx;
    int      ev[16];
    int      n_ev;
    bool     result_valid;
} B;

static i2c_sched_t S;

static uint32_t op_ms(i2c_op_t op)
{
    return op == I2C_OP_AOCS_STATUS ? 3u : op == I2C_OP_AOCS_START ? 2u : 1u;
}

/* What the bus answers. A STATUS from the simulated ESP32. */
static i2c_result_t perform(i2c_op_t op, uint64_t end, uint8_t *status)
{
    if (op == I2C_OP_INA219_INIT || op == I2C_OP_INA219_READ) {
        return I2C_OK;
    }
    if (B.mode == ESP_ABSENT) {
        return I2C_NACK;
    }
    if (op == I2C_OP_AOCS_START) {
        B.started = true;
        B.start_at = end;
        return I2C_OK;
    }
    /* STATUS: before the task ends the ESP32 answers its decision; after,
     * the result. */
    bool after_task = B.started && end >= B.start_at + GAMA_AOCS_T_TAKEOVER_MS + 120000u;
    gama_aocs_status_t st = { .seq = B.seq, .state = GAMA_AOCS_ST_ACCEPTED };
    if (B.mode == ESP_REFUSES) {
        st.state = GAMA_AOCS_ST_REFUSED;
        st.error = GAMA_AOCS_ERR_ARGS;
    }
    if (B.mode == ESP_STALE && !after_task) {
        st.seq = (uint8_t)(B.seq - 1u);        /* has not processed the START */
        st.state = GAMA_AOCS_ST_IDLE;
    }
    if (after_task) {
        st.state = GAMA_AOCS_ST_DONE;
        st.yaw_cdeg = 9012;
        st.task_s = 97;
    }
    gama_aocs_status_encode(status, GAMA_AOCS_STATUS_LEN, &st);
    if ((B.mode == ESP_GARBLED && !after_task) || (after_task && B.resume_fails)) {
        status[7] ^= 0x10;                      /* CRC fails */
    }
    return I2C_OK;
}

/* Runs the thread's loop, as obcd's I2C thread does, until `until`. */
static void run(uint64_t from, uint64_t until)
{
    uint64_t now = from;
    while (now < until) {
        i2c_op_t op;
        uint64_t wake;
        i2c_event_t ev = i2c_sched_next(&S, now, &op, &wake);
        if (ev != I2C_EV_NONE && B.n_ev < 16) { B.ev[B.n_ev++] = (int)ev; }
        if (op == I2C_OP_NONE) {
            now = wake > now ? wake : now + 1u;
            continue;
        }
        uint8_t status[GAMA_AOCS_STATUS_LEN] = { 0 };
        uint64_t end = now + op_ms(op);
        i2c_result_t r = perform(op, end, status);
        if (B.n_tx < MAX_TX) {
            B.tx_begin[B.n_tx] = now;
            B.tx_end[B.n_tx] = end;
            B.tx_op[B.n_tx] = op;
            B.n_tx++;
        }
        bool valid = false;
        ev = i2c_sched_done(&S, end, op, r, status, &valid);
        if (ev != I2C_EV_NONE && B.n_ev < 16) {
            B.ev[B.n_ev++] = (int)ev;
            if (ev == I2C_EV_AOCS_RESULT) { B.result_valid = valid; }
        }
        now = end;
    }
}

/* The Pi's transactions overlapping [lo, hi). */
static int on_bus_during(uint64_t lo, uint64_t hi)
{
    int n = 0;
    for (int i = 0; i < B.n_tx && i < MAX_TX; i++) {
        n += B.tx_begin[i] < hi && B.tx_end[i] > lo;
    }
    return n;
}

static int count_op(i2c_op_t op)
{
    int n = 0;
    for (int i = 0; i < B.n_tx && i < MAX_TX; i++) { n += B.tx_op[i] == op; }
    return n;
}

static void fresh(esp_mode_t mode)
{
    memset(&B, 0, sizeof(B));
    B.mode = mode;
    B.seq = 0x2C;
    i2c_sched_params_t p = { .power_period_ms = 1000 };
    i2c_sched_init(&S, &p, 0);
}

static void request_start(uint16_t max_task_s)
{
    gama_aocs_command_t c = { .seq = B.seq, .cmd = GAMA_AOCS_CMD_START,
                              .tasks = GAMA_AOCS_TASK_DETUMBLE, .max_task_s = max_task_s };
    uint8_t f[GAMA_AOCS_COMMAND_LEN];
    gama_aocs_command_encode(f, sizeof(f), &c);
    i2c_sched_request_start(&S, f, B.seq, max_task_s);
}

int main(void)
{
    TEST_GROUP("i2c: outside the AOCS the INA219 is configured once, then read every second");
    {
        fresh(ESP_ACCEPTS);
        run(0, 10500);
        CHECK_EQ_INT(count_op(I2C_OP_INA219_INIT), 1);
        /* The first reading right after the configuration, then one a second:
         * at 1 ms, 1001 ms, ... 10001 ms. */
        CHECK_EQ_INT(count_op(I2C_OP_INA219_READ), 11);
    }

    TEST_GROUP("i2c: an accepted START lends the bus, and the Pi is silent for the whole lease");
    {
        fresh(ESP_ACCEPTS);
        run(0, 3000);
        request_start(120);
        run(3000, 3000u + 125000u + 3000u);
        CHECK_EQ_INT(B.n_ev, 2);
        CHECK_EQ_INT(B.ev[0], I2C_EV_AOCS_LENT);
        CHECK_EQ_INT(B.ev[1], I2C_EV_AOCS_RESULT);
        CHECK(B.result_valid);
        uint64_t t0 = S.t0;
        CHECK(t0 > 3000u && t0 < 3010u);
        uint64_t lease_end = t0 + 250u + 120000u + 2000u;
        CHECK_EQ_INT(on_bus_during(t0 + GAMA_AOCS_T_RELEASE_MS, lease_end), 0);
        /* The result is read at the end of the lease, then the INA219 is
         * configured again before it is read. */
        int i = 0;
        while (i < B.n_tx && B.tx_begin[i] < lease_end) { i++; }
        CHECK(i < B.n_tx && B.tx_op[i] == I2C_OP_AOCS_STATUS && B.tx_begin[i] == lease_end);
        CHECK(i + 1 < B.n_tx && B.tx_op[i + 1] == I2C_OP_INA219_INIT);
    }

    TEST_GROUP("i2c: the lease's end is known from the START write, before any answer");
    {
        fresh(ESP_ACCEPTS);
        request_start(120);
        i2c_op_t op;
        uint64_t wake;
        bool valid;
        CHECK_EQ_INT(i2c_sched_next(&S, 10, &op, &wake), I2C_EV_NONE);
        CHECK_EQ_INT(op, I2C_OP_AOCS_START);
        CHECK_EQ_INT(i2c_sched_done(&S, 12, op, I2C_OK, NULL, &valid), I2C_EV_NONE);
        CHECK_EQ_INT(S.phase, I2C_PH_STARTING);
        CHECK(S.lease_until == 12u + 250u + 120000u + 2000u);
    }

    TEST_GROUP("i2c: a refused START lends nothing; the INA219 goes on");
    {
        fresh(ESP_REFUSES);
        request_start(120);
        run(0, 5000);
        CHECK_EQ_INT(B.n_ev, 1);
        CHECK_EQ_INT(B.ev[0], I2C_EV_AOCS_REFUSED);
        CHECK(count_op(I2C_OP_INA219_READ) >= 3);
    }

    TEST_GROUP("i2c: no ESP32 on the bus (NACK): nothing lent");
    {
        fresh(ESP_ABSENT);
        request_start(120);
        run(0, 3000);
        CHECK_EQ_INT(B.n_ev, 1);
        CHECK_EQ_INT(B.ev[0], I2C_EV_AOCS_ABSENT);
        CHECK_EQ_INT(count_op(I2C_OP_AOCS_STATUS), 0);
        CHECK(count_op(I2C_OP_INA219_READ) >= 2);
    }

    TEST_GROUP("i2c: no clear answer by t0+100 ms counts as lent — never two masters");
    {
        fresh(ESP_GARBLED);
        request_start(120);
        run(0, 130000);
        CHECK_EQ_INT(B.ev[0], I2C_EV_AOCS_LENT);
        uint64_t t0 = S.t0;
        CHECK_EQ_INT(count_op(I2C_OP_AOCS_STATUS) >= 3, 1);
        /* Every STATUS read of the handover ended before the release. */
        for (int i = 0; i < B.n_tx; i++) {
            if (B.tx_op[i] == I2C_OP_AOCS_STATUS && B.tx_begin[i] < t0 + 1000u) {
                CHECK(B.tx_end[i] <= t0 + GAMA_AOCS_T_RELEASE_MS);
            }
        }
        CHECK_EQ_INT(on_bus_during(t0 + GAMA_AOCS_T_RELEASE_MS, t0 + 250u + 120000u + 2000u), 0);
    }

    TEST_GROUP("i2c: a STATUS from before the START (old seq) is no answer either");
    {
        fresh(ESP_STALE);
        request_start(30);
        run(0, 40000);
        CHECK_EQ_INT(B.ev[0], I2C_EV_AOCS_LENT);
        CHECK_EQ_INT(on_bus_during(S.t0 + GAMA_AOCS_T_RELEASE_MS, S.t0 + 250u + 30000u + 2000u), 0);
    }

    TEST_GROUP("i2c: a result that never reads cleanly is reported invalid, after 5 tries");
    {
        fresh(ESP_ACCEPTS);
        B.resume_fails = true;
        request_start(120);
        run(0, 125000u);
        CHECK_EQ_INT(B.n_ev, 2);
        CHECK_EQ_INT(B.ev[1], I2C_EV_AOCS_RESULT);
        CHECK(!B.result_valid);
        CHECK_EQ_INT(count_op(I2C_OP_AOCS_STATUS), 1 + I2C_SCHED_RESUME_TRIES);
        CHECK(count_op(I2C_OP_INA219_READ) >= 1);           /* back to polling */
    }

    TEST_GROUP("i2c: a lease left by a previous obcd is kept to its end");
    {
        fresh(ESP_ACCEPTS);
        i2c_sched_resume_lease(&S, 0, 60000u);
        run(0, 65000u);
        CHECK_EQ_INT(on_bus_during(0, 60000u), 0);
        CHECK(B.n_tx > 0 && B.tx_op[0] == I2C_OP_AOCS_STATUS && B.tx_begin[0] == 60000u);
        CHECK(count_op(I2C_OP_INA219_READ) >= 3);
    }

    TEST_SUMMARY("test_obcd_i2c_sched");
}
