/*
 * gama_tc.h — telecommand catalogue and the OBC mode vocabulary.
 *
 * This header is the authoritative list of what the ground can ask the
 * satellite to do (HLR-COMM-01). Every identifier here is pinned to an
 * explicit numeric value because these numbers travel on the wire and are
 * also implemented independently by the OBC team in a separate repository:
 * an implicitly-numbered enum would silently change meaning the day someone
 * inserts a value in the middle.
 *
 * Shared verbatim between the Raspberry Pi and the ESP32.
 * See docs/icd/ota-protocol-icd.md for the normative description.
 */

#ifndef GAMA_TC_H
#define GAMA_TC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Telecommand identifiers. Grouped by high nibble:
 *   0x0x  link and time      0x1x  mission mode
 *   0x2x  radio configuration 0x3x  telemetry streaming
 *   0x4x  on-demand reports  0x5x  bulk download
 *   0x7x  hazardous
 */
typedef enum {
    GAMA_TC_PING         = 0x01, /* no args; answered with TC_ACK          */
    GAMA_TC_SET_TIME     = 0x02, /* u32 unix seconds, u32 nanoseconds      */

    GAMA_TC_SET_MODE     = 0x10, /* u8 gama_obc_event_t                    */

    GAMA_TC_SET_RATE     = 0x20, /* u8 gama_rate_profile_t                 */
    GAMA_TC_SET_TX_POWER = 0x21, /* u8 dBm, 2..20                          */

    GAMA_TC_STREAM_START = 0x30, /* u16 period in deciseconds              */
    GAMA_TC_STREAM_STOP  = 0x31, /* no args                                */

    GAMA_TC_REQ_HK       = 0x40, /* no args                                */
    GAMA_TC_REQ_STAT     = 0x41, /* no args                                */
    GAMA_TC_REQ_ROSTER   = 0x42, /* no args                                */

    GAMA_TC_BULK_START   = 0x50, /* u32 byte offset, u16 max chunks        */
    GAMA_TC_BULK_ABORT   = 0x51, /* no args                                */

    GAMA_TC_SHUTDOWN     = 0x70  /* u16 magic, must be GAMA_TC_SHUTDOWN_MAGIC */
} gama_tc_id_t;

/* A shutdown ends the mission and cannot be undone from the ground, so it
 * carries a constant that a single corrupted byte is very unlikely to
 * produce. Defence in depth behind the frame CRC, not a substitute for it. */
#define GAMA_TC_SHUTDOWN_MAGIC 0xA5A5u

/*
 * Radio configuration profiles.
 *
 * Only these three combinations are reachable from the ground. Exposing SF,
 * bandwidth and coding rate as free parameters would let one malformed
 * telecommand put the two ends on settings that cannot hear each other, with
 * no way back. See docs/adr/0004-lora-phy-configuration.md.
 */
typedef enum {
    GAMA_RATE_SAFE    = 0, /* SF12 / BW125 / CR4:8 -- beacon and recovery  */
    GAMA_RATE_NOMINAL = 1, /* SF9  / BW125 / CR4:5 -- mission default      */
    GAMA_RATE_FAST    = 2  /* SF7  / BW125 / CR4:5 -- bulk download        */
} gama_rate_profile_t;

#define GAMA_RATE_COUNT 3

/*
 * OBC vocabulary, mirrored from the obc repository (src/fsm.h).
 *
 * These are wire values, not a copy of the OBC's enum: the OBC-side adapter
 * translates between them. Keeping them separate means either repository can
 * renumber internally without breaking the link, as long as the adapter is
 * updated. Documented in docs/icd/obc-ttec-icd.md.
 */
typedef enum {
    GAMA_OBC_EV_NONE            = 0,
    GAMA_OBC_EV_TC_BASIC_INTER  = 1,
    GAMA_OBC_EV_TC_AOCS         = 2,
    GAMA_OBC_EV_TC_MISSION_ADSB = 3,
    GAMA_OBC_EV_TC_DOWNLINK     = 4,
    GAMA_OBC_EV_TC_SURVIVAL     = 5,
    GAMA_OBC_EV_TASK_DONE       = 6,
    GAMA_OBC_EV_ADSB_TIMEOUT    = 7
} gama_obc_event_t;

typedef enum {
    GAMA_OBC_ST_PRE_TEST           = 0,
    GAMA_OBC_ST_BASIC_INTERMEDIATE = 1,
    GAMA_OBC_ST_ADVANCED_AOCS      = 2,
    GAMA_OBC_ST_MISSION_ADSB       = 3,
    GAMA_OBC_ST_MISSION_DOWNLINK   = 4,
    GAMA_OBC_ST_ENV_SURVIVAL       = 5,
    GAMA_OBC_ST_UNKNOWN            = 0xFF /* no IPC contact with the OBC yet */
} gama_obc_state_t;

/* ttcd link state, reported in housekeeping so the ground can tell a silent
 * satellite from one that is busy transmitting. */
typedef enum {
    GAMA_LINK_IDLE   = 0, /* receiving, waiting for a telecommand          */
    GAMA_LINK_TX     = 1, /* transmitting; half-duplex, receiver is off    */
    GAMA_LINK_STREAM = 2, /* periodic track snapshots during the mission   */
    GAMA_LINK_BULK   = 3, /* windowed download of the raw log              */
    GAMA_LINK_SAFE   = 4  /* dropped to SAFE rate after a contact timeout  */
} gama_link_state_t;

/* TC_ACK payload: u8 command id, u8 status, u16 echoed frame sequence. */
#define GAMA_TC_ACK_WIRE_LEN 4u

typedef enum {
    GAMA_ACK_OK             = 0,
    GAMA_ACK_UNKNOWN_CMD    = 1,
    GAMA_ACK_BAD_ARGS       = 2, /* wrong argument length or out of range  */
    GAMA_ACK_REJECTED_STATE = 3, /* valid command, not allowed right now   */
    GAMA_ACK_FAILED         = 4  /* accepted but the action did not succeed */
} gama_ack_status_t;

/*
 * Expected argument length for a telecommand, in bytes.
 *
 * Returns -1 for an unknown identifier. ttcd validates against this before
 * dispatching, so every handler can assume its arguments are present and
 * correctly sized.
 */
int gama_tc_arg_len(uint8_t cmd);

/* Names for logs and the ground CLI. Never NULL; unknown values render as
 * "UNKNOWN". */
const char *gama_tc_name(uint8_t cmd);
const char *gama_ack_status_name(uint8_t status);
const char *gama_rate_profile_name(uint8_t profile);
const char *gama_obc_state_name(uint8_t state);
const char *gama_link_state_name(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* GAMA_TC_H */
