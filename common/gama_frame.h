/*
 * gama_frame.h — GAMA framing layer, version 1.
 *
 * One frame carries one message. The same frame format is used on both
 * transports in the system:
 *
 *   - over the air, as the LoRa payload (Raspberry Pi <-> ESP32);
 *   - over AF_UNIX SOCK_SEQPACKET, as the IPC datagram (ttcd <-> adsbd,
 *     ttcd <-> obc).
 *
 * Using one codec for both means the serialisation path that carries
 * telecommands to the OBC is exercised by every ground-link test, and there
 * is only one place where a wire-format bug can live.
 *
 * Wire layout (all multi-byte fields little-endian):
 *
 *    0       1        2      4      5            5+len      7+len
 *    +-------+--------+------+------+------------+---------+
 *    | ver 1 | type 1 | seq 2| len 1| payload len| crc16 2 |
 *    +-------+--------+------+------+------------+---------+
 *
 *    ver     protocol version; a receiver rejects anything it does not know
 *    type    gama_frame_type_t
 *    seq     per-direction counter, wraps at 65535; used for loss accounting
 *            (HLR-ADS-08) and as the ARQ sequence during bulk transfer
 *    len     payload length in bytes, 0..248
 *    crc16   CRC-16/CCITT-FALSE over bytes [0 .. 5+len-1], i.e. header and
 *            payload but not the CRC field itself
 *
 * The 248-byte payload cap comes from the SX1278 FIFO: 255 bytes total minus
 * the 7 bytes of framing overhead.
 *
 * Shared verbatim between the Raspberry Pi and the ESP32.
 */

#ifndef GAMA_FRAME_H
#define GAMA_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAMA_FRAME_VERSION      1u
#define GAMA_FRAME_HEADER_LEN   5u
#define GAMA_FRAME_CRC_LEN      2u
#define GAMA_FRAME_OVERHEAD     (GAMA_FRAME_HEADER_LEN + GAMA_FRAME_CRC_LEN)
#define GAMA_FRAME_MAX_TOTAL    255u
#define GAMA_FRAME_MAX_PAYLOAD  (GAMA_FRAME_MAX_TOTAL - GAMA_FRAME_OVERHEAD)

/* Message types. Ranges are deliberate so a decoder can classify a frame by
 * its high nibble before it knows the specific type:
 *   0x0x  link control        0x1x  telemetry
 *   0x2x  bulk transfer       0x3x  internal IPC (never transmitted)
 *   0x7x  beacon / diagnostics
 */
typedef enum {
    /* --- link control, ground -> satellite unless noted --- */
    GAMA_FRAME_TC            = 0x01, /* telecommand; payload is gama_tc_* */
    GAMA_FRAME_TC_ACK        = 0x02, /* sat -> ground; acknowledges one TC   */

    /* --- telemetry, satellite -> ground --- */
    GAMA_FRAME_TM_HK         = 0x10, /* housekeeping: power, thermal, attitude */
    GAMA_FRAME_TM_TRACKS     = 0x11, /* ADS-B track snapshot                 */
    GAMA_FRAME_TM_ROSTER     = 0x12, /* ICAO -> callsign mapping             */
    GAMA_FRAME_TM_STAT       = 0x13, /* mission performance counters         */

    /* --- bulk transfer of the on-board raw log, post-mission --- */
    GAMA_FRAME_BULK_DATA     = 0x20, /* sat -> ground; one windowed chunk    */
    GAMA_FRAME_BULK_ACK      = 0x21, /* ground -> sat; receive-window bitmap */

    /* --- internal IPC only; these never reach the radio --- */
    GAMA_FRAME_IPC_TC_EVENT  = 0x30, /* ttcd -> obc; one obc fsm.h Event     */
    GAMA_FRAME_IPC_TIME_SET  = 0x31, /* ttcd -> obc/adsbd; wall-clock anchor */
    GAMA_FRAME_IPC_TELEMETRY = 0x32, /* obc -> ttcd; obclog.h Telemetry      */
    GAMA_FRAME_IPC_MODE      = 0x33, /* obc -> ttcd; current fsm.h State     */
    GAMA_FRAME_IPC_TRACKS    = 0x34, /* adsbd -> ttcd; track snapshot        */
    GAMA_FRAME_IPC_STAT      = 0x35, /* adsbd -> ttcd; payload counters      */
    GAMA_FRAME_IPC_HELLO     = 0x36, /* client -> ttcd; identifies the peer  */

    GAMA_FRAME_BEACON        = 0x7F  /* sat -> ground; minimal "I am alive"  */
} gama_frame_type_t;

/* Decode results. Negative values are errors so callers can test < 0. */
typedef enum {
    GAMA_FRAME_OK            =  0,
    GAMA_FRAME_ERR_SHORT     = -1, /* buffer smaller than the frame claims   */
    GAMA_FRAME_ERR_VERSION   = -2, /* unknown protocol version               */
    GAMA_FRAME_ERR_LEN       = -3, /* declared payload length out of range   */
    GAMA_FRAME_ERR_CRC       = -4, /* integrity check failed                 */
    GAMA_FRAME_ERR_ARG       = -5  /* NULL pointer or undersized output      */
} gama_frame_result_t;

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
    uint8_t  len;
    const uint8_t *payload; /* points into the caller's buffer; not copied */
} gama_frame_t;

/*
 * Serialises a frame into out.
 *
 * Returns the total number of bytes written, or a negative
 * gama_frame_result_t. Fails rather than truncating if out_cap is too small.
 */
int gama_frame_encode(uint8_t *out, size_t out_cap,
                      uint8_t type, uint16_t seq,
                      const uint8_t *payload, uint8_t payload_len);

/*
 * Parses one frame out of in.
 *
 * On success fills *frame and returns the total frame length in bytes, which
 * lets a caller reading a byte stream advance past it. frame->payload aliases
 * the input buffer, so it stays valid only as long as in does.
 *
 * Verifies version, declared length and CRC before reporting success. A frame
 * that fails any of those is reported, never silently dropped, so that the
 * caller can count it.
 */
int gama_frame_decode(const uint8_t *in, size_t in_len, gama_frame_t *frame);

/* Human-readable name for a type or a negative result code. Always returns a
 * valid string; unknown values render as "UNKNOWN". For logs and tests. */
const char *gama_frame_type_name(uint8_t type);
const char *gama_frame_result_name(int result);

/* True for types that are only ever valid on the IPC transport. ttcd uses
 * this to refuse to put an internal message on the air. */
static inline bool gama_frame_is_ipc_only(uint8_t type)
{
    return (type & 0xF0u) == 0x30u;
}

#ifdef __cplusplus
}
#endif

#endif /* GAMA_FRAME_H */
