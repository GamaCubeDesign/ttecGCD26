/*
 * core.h — the obcd core: the mission modes of the CDR's Figure 44, the
 * burn-wire, the battery heaters and the OBC's side of the IPC contract,
 * with no system calls (ADR-0005, ADR-0016).
 *
 * Built like ttcd's and adsbd's cores. The shell (main.c) owns the clock,
 * the sockets, the GPIO lines and the sensor threads, and drives the core
 * with events stamped in CLOCK_MONOTONIC milliseconds; the tests drive it
 * with a virtual clock. The core acts only through obcd_ops_t, and never
 * waits for anything (AGENTS.md, hard rule 3): sensor readings arrive as
 * events, already taken by the shell's threads.
 *
 * The modes (CDR Figure 44; PLANO D1, D2):
 *
 *   boot -> PRE_TEST
 *   PRE_TEST --EV_TC_BASIC_INTER-->  BASIC_INTERMEDIATE --EV_TASK_DONE--> PRE_TEST
 *   PRE_TEST --EV_TC_AOCS------->    ADVANCED_AOCS      --EV_TASK_DONE--> PRE_TEST
 *   PRE_TEST --EV_TC_SURVIVAL--->    ENV_SURVIVAL       --EV_TASK_DONE--> PRE_TEST
 *   PRE_TEST --EV_TC_MISSION_ADSB--> MISSION_ADSB
 *   MISSION_ADSB --EV_ADSB_TIMEOUT (600 s) or EV_TC_DOWNLINK--> MISSION_DOWNLINK
 *   PRE_TEST --EV_TC_DOWNLINK--->    MISSION_DOWNLINK   --EV_TASK_DONE--> PRE_TEST
 *
 * Any other event in any mode is refused, counted and logged. Who posts
 * EV_TASK_DONE (D2): obcd itself when the burn-wire pulse ends; obcd when
 * the AOCS ESP32's lease of the I2C bus ends and its result is read, or at
 * once if no ESP32 takes the task; ttcd when the roster and final statistics
 * have left; the ground, by telecommand, to leave ENV_SURVIVAL.
 *
 * ADVANCED_AOCS (ADR-0017, docs/icd/obc-aocs-i2c.md): on entry the core asks
 * the shell's I2C thread to write a START; while the bus is lent the INA219
 * is not read and voltage and current go as "no data"; the lease's end
 * brings the result, whose attitude the telemetry then carries for a while.
 */

#ifndef OBCD_CORE_H
#define OBCD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gama_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every duration in milliseconds, every temperature in hundredths of a
 * degree. obcd_params_default() gives the PLANO's values (D3, D4, D6). */
typedef struct {
    uint32_t telemetry_period;   /* IPC_TELEMETRY to ttcd (D6): 1 s           */
    uint32_t adsb_timeout;       /* MISSION_ADSB ends by itself (OBC-6): 600 s */
    uint32_t burnwire_on;        /* the burn-wire pulse (D3): 5 s             */
    uint32_t aocs_timeout;       /* the most ADVANCED_AOCS may last: the
                                    ESP32's max_task plus the lease's margins */
    uint32_t aocs_hold;          /* the task's attitude stays in telemetry    */
    uint16_t aocs_azimuth_cdeg;  /* the azimuth the START asks for            */
    int16_t  heater_on_below;    /* thermostat (D4): on below 10.00 degC      */
    int16_t  heater_off_above;   /* ... off above 12.00 degC                  */
    uint32_t temp_stale;         /* no battery reading this long: sensor failed */
    uint32_t power_stale;        /* no INA219 reading this long: no data      */
    uint32_t report_period;      /* sensors and counters into the log         */
} obcd_params_t;

void obcd_params_default(obcd_params_t *p);

typedef struct {
    void *ctx;
    /* Sends one frame to ttcd without blocking. 0 if sent. */
    int  (*ipc_send)(void *ctx, const uint8_t *frame, size_t len);
    /* One event for the log: a name and a JSON object body. */
    void (*log)(void *ctx, uint64_t now_ms, const char *event, const char *fields);
    /* Drive the outputs. 0 on success; -1 if the line is not configured or
     * the write failed. Either may be NULL: the output does not exist. */
    int  (*burnwire_set)(void *ctx, bool on);
    int  (*heater_set)(void *ctx, bool on);
    /* Queue a START (an encoded COMMAND, common/gama_aocs.h) for the I2C
     * thread. 0 if queued; -1, or NULL, when there is no I2C bus: the task
     * then ends at once. The outcome comes back through
     * obcd_on_aocs_started() and obcd_on_aocs_result(). */
    int  (*aocs_start)(void *ctx, const uint8_t *frame, size_t len, uint8_t seq,
                       uint16_t max_task_s);
} obcd_ops_t;

/* What became of a START (the I2C thread's report). */
typedef enum {
    OBCD_AOCS_ABSENT = 0,      /* nobody answered: nothing lent           */
    OBCD_AOCS_REFUSED,         /* the ESP32 said no: nothing lent         */
    OBCD_AOCS_LENT             /* the bus is the ESP32's until lease_until */
} obcd_aocs_outcome_t;

typedef struct {
    uint32_t events;            /* mode events received or posted            */
    uint32_t refused;           /* of those, not valid in the mode they met  */
    uint32_t transitions;
    uint32_t ipc_rx_bad;        /* frames from ttcd that made no sense here  */
    uint32_t ipc_tx_drops;      /* frames ttcd's queue refused               */
    uint32_t power_failures;    /* INA219 readings that failed               */
    uint32_t temp_failures;     /* DS18B20 readings that failed              */
    uint32_t output_failures;   /* burn-wire or heater writes that failed    */
    uint32_t aocs_tasks;        /* STARTs the ESP32 took                     */
    uint32_t aocs_no_result;    /* leases whose result could not be read     */
} obcd_counters_t;

typedef struct {
    obcd_params_t   p;
    obcd_ops_t      ops;
    obcd_counters_t n;
    uint64_t        boot_ms;

    uint8_t  mode;               /* gama_obc_state_t                          */
    uint64_t mode_since;
    uint64_t adsb_deadline;      /* MISSION_ADSB: EV_ADSB_TIMEOUT at this time */
    uint64_t aocs_deadline;      /* ADVANCED_AOCS: EV_TASK_DONE at this time  */
    const char *aocs_end;        /* ... posted with this as its source        */
    bool     aocs_in_progress;   /* a START is out, its result not yet in     */
    bool     bus_lent;           /* the INA219 is not read: V and I no data   */
    uint64_t lease_until;
    uint8_t  aocs_seq;
    bool     have_attitude;      /* the last task's result                    */
    uint64_t attitude_at;
    int16_t  roll_cdeg, pitch_cdeg, yaw_cdeg;
    bool     burnwire_on;
    uint64_t burnwire_until;

    /* sensors: the latest reading and when it came */
    bool     have_power;
    uint64_t power_at;
    uint16_t battery_mv;
    int16_t  current_ma;
    bool     have_temp;
    uint64_t temp_at;
    int16_t  temp_bat_ccel;
    bool     temp_fault;         /* logged once per fault, cleared on recovery */

    bool     heater_on;

    bool     ipc_up;
    uint16_t ipc_seq;
    uint64_t next_telemetry, next_report;
    bool     time_synced;
} obcd_core_t;

void obcd_init(obcd_core_t *c, const obcd_params_t *p, const obcd_ops_t *ops,
               uint64_t now_ms);

/* The connection to ttcd came up (the shell has sent IPC_HELLO) or went
 * down. On the way up ttcd is told the mode and the telemetry at once. */
void obcd_on_ipc_link(obcd_core_t *c, uint64_t now_ms, bool up);
void obcd_on_ipc_frame(obcd_core_t *c, uint64_t now_ms, const uint8_t *buf, size_t len);

/* A mode event from anywhere but ttcd: the AOCS ESP32, a test. `by` names
 * the source for the log. */
void obcd_post_event(obcd_core_t *c, uint64_t now_ms, uint8_t event, const char *by);

/* Sensor readings, taken by the shell's threads. ok = false: the reading
 * failed; the value is then ignored. */
void obcd_on_power(obcd_core_t *c, uint64_t now_ms, bool ok, uint16_t battery_mv,
                   int16_t current_ma);
void obcd_on_battery_temp(obcd_core_t *c, uint64_t now_ms, bool ok, int16_t ccel);

/* The I2C thread's reports on the AOCS task: what became of the START, and,
 * at the lease's end, the STATUS read (valid = it decoded and answers our
 * START; status then holds GAMA_AOCS_STATUS_LEN bytes). */
void obcd_on_aocs_started(obcd_core_t *c, uint64_t now_ms, obcd_aocs_outcome_t outcome,
                          uint64_t lease_until);
void obcd_on_aocs_result(obcd_core_t *c, uint64_t now_ms, bool valid, const uint8_t *status);

/* The longest task the START may ask for, from aocs_timeout. */
uint16_t obcd_aocs_max_task_s(const obcd_params_t *p);

void obcd_on_tick(obcd_core_t *c, uint64_t now_ms);

/* The earliest instant obcd_on_tick() has something to do. */
uint64_t obcd_next_deadline(const obcd_core_t *c);

/* The mode an event leads to from `mode`, or GAMA_OBC_ST_UNKNOWN if the
 * event is not valid there. The whole of Figure 44, in one place. */
uint8_t obcd_transition(uint8_t mode, uint8_t event);

/* What the next IPC_TELEMETRY will carry: GAMA_NO_DATA_* for what was not
 * measured, or measured too long ago. */
void obcd_telemetry(const obcd_core_t *c, uint64_t now_ms, gama_ipc_telemetry_t *t);

#ifdef __cplusplus
}
#endif

#endif /* OBCD_CORE_H */
