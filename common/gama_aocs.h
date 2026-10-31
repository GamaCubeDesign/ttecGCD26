/*
 * gama_aocs.h — the frames the OBC and the AOCS ESP32 exchange over I2C.
 *
 * DRAFT, protocol version 1: a proposal to the AOCS team, normative only once
 * they agree to it. The contract, its timing and the bus ownership are in
 * docs/icd/obc-aocs-i2c.md; ADR-0017 records why it looks like this.
 *
 * Two fixed-length frames and no register pointer. The ESP32 is an I2C slave
 * that cannot stretch the clock, so it cannot prepare data between a pointer
 * write and the read that follows; instead it keeps a complete STATUS frame
 * preloaded, and the Pi always reads exactly GAMA_AOCS_STATUS_LEN bytes.
 * Both frames fit the ESP32's 32-byte I2C FIFO, and both carry the same
 * CRC-16/CCITT-FALSE as the radio frames, so a bit flipped on the bus is
 * caught the same way.
 *
 * Shared verbatim by the Pi (flight/obcd) and, when it adopts it, the AOCS
 * ESP32 firmware: no platform headers, no allocation, like the rest of
 * common/. All multi-byte fields little-endian.
 */

#ifndef GAMA_AOCS_H
#define GAMA_AOCS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAMA_AOCS_MAGIC         0x47u   /* 'G' */
#define GAMA_AOCS_VERSION       1u
#define GAMA_AOCS_ADDR_DEFAULT  0x2Au   /* 7-bit; clear of the INA219's 0x40-0x4F,
                                           the BMI088's 0x18/0x19 and 0x68/0x69,
                                           and EEPROMs' 0x50-0x57 */

#define GAMA_AOCS_COMMAND_LEN   16u
#define GAMA_AOCS_STATUS_LEN    20u

/* Frame types, byte 2 of every frame. */
#define GAMA_AOCS_TYPE_COMMAND  0x01u
#define GAMA_AOCS_TYPE_STATUS   0x02u

/* Commands. PING asks for a fresh STATUS and nothing else: the ESP32 stays a
 * slave. START hands the bus over for one task (the timeline below). */
typedef enum {
    GAMA_AOCS_CMD_PING  = 0x01,
    GAMA_AOCS_CMD_START = 0x02
} gama_aocs_cmd_id_t;

/* Tasks of a START, as bits: the CDR's ADVANCED_AOCS is detumbling, then
 * pointing to an azimuth. */
#define GAMA_AOCS_TASK_DETUMBLE (1u << 0)
#define GAMA_AOCS_TASK_AZIMUTH  (1u << 1)

/* The ESP32's state, as its STATUS reports it. */
typedef enum {
    GAMA_AOCS_ST_IDLE     = 0,  /* no task since boot, or PING answered        */
    GAMA_AOCS_ST_ACCEPTED = 1,  /* START taken: the bus is the ESP32's soon    */
    GAMA_AOCS_ST_REFUSED  = 2,  /* START not taken; error says why; bus stays  */
    GAMA_AOCS_ST_DONE     = 3,  /* task ended; attitude is its result          */
    GAMA_AOCS_ST_FAILED   = 4   /* task ended badly; error says why            */
} gama_aocs_state_t;

typedef enum {
    GAMA_AOCS_ERR_NONE      = 0,
    GAMA_AOCS_ERR_FRAME     = 1,  /* the last COMMAND failed magic/version/CRC */
    GAMA_AOCS_ERR_ARGS      = 2,  /* a field out of range                      */
    GAMA_AOCS_ERR_SENSOR    = 3,  /* the BMI088 did not answer                 */
    GAMA_AOCS_ERR_ACTUATOR  = 4,
    GAMA_AOCS_ERR_TIMEOUT   = 5,  /* max_task_s ran out before the task ended  */
    GAMA_AOCS_ERR_OTHER     = 0xFF
} gama_aocs_error_t;

/*
 * Timeline of a START, in milliseconds from t0, the end of the Pi's COMMAND
 * write (docs/icd/obc-aocs-i2c.md §4). Every bound is counted from t0, never
 * from a later confirmation, so a confirmation lost on the bus cannot make
 * the two ends disagree about who owns it.
 */
#define GAMA_AOCS_T_PRELOAD_MS     5u    /* slave: STATUS ready after any STOP   */
#define GAMA_AOCS_T_READ_MIN_MS    20u   /* Pi: first STATUS read after START    */
#define GAMA_AOCS_T_RELEASE_MS     100u  /* Pi: no transaction from here on      */
#define GAMA_AOCS_T_TAKEOVER_MS    250u  /* ESP32: may become master from here   */
#define GAMA_AOCS_T_RESUME_MARGIN_MS 2000u /* Pi: after the ESP32's last moment  */

/* The most a task may last. One hour bounds a lease even if the field is
 * corrupted in a way the CRC misses. */
#define GAMA_AOCS_MAX_TASK_S    3600u

typedef struct {
    uint8_t  seq;            /* echoed by the STATUS that answers it            */
    uint8_t  cmd;            /* gama_aocs_cmd_id_t                              */
    uint8_t  tasks;          /* GAMA_AOCS_TASK_* (START only)                   */
    uint16_t max_task_s;     /* START only: the ESP32 is a slave again by then  */
    uint16_t azimuth_cdeg;   /* START with AZIMUTH: target, 0 .. 35999          */
} gama_aocs_command_t;

typedef struct {
    uint8_t  seq;            /* of the COMMAND this STATUS answers              */
    uint8_t  state;          /* gama_aocs_state_t                               */
    uint8_t  error;          /* gama_aocs_error_t                               */
    uint8_t  flags;          /* reserved, 0                                     */
    int16_t  roll_cdeg;      /* attitude at the end of the task; INT16_MIN      */
    int16_t  pitch_cdeg;     /*   (GAMA_NO_DATA_I16) when not measured          */
    int16_t  yaw_cdeg;
    uint16_t rate_cdps;      /* residual angular rate, 0.01 deg/s; 0xFFFF none  */
    uint16_t task_s;         /* how long the last task took                     */
} gama_aocs_status_t;

/* Encoders return the frame length; decoders the length consumed. Both
 * return a negative gama_frame_result_t: ARG for a NULL pointer or a short
 * output buffer; LEN for an input of any other length; VERSION for a wrong
 * magic, version or type; CRC for a checksum mismatch. The command encoder
 * also refuses (ARG) a START whose max_task_s is 0 or above
 * GAMA_AOCS_MAX_TASK_S, so the Pi can never ask for an unbounded lease. */
int gama_aocs_command_encode(uint8_t *out, size_t cap, const gama_aocs_command_t *c);
int gama_aocs_command_decode(const uint8_t *in, size_t len, gama_aocs_command_t *c);
int gama_aocs_status_encode(uint8_t *out, size_t cap, const gama_aocs_status_t *s);
int gama_aocs_status_decode(const uint8_t *in, size_t len, gama_aocs_status_t *s);

const char *gama_aocs_state_name(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_AOCS_H */
