#include "gama_frame.h"
#include "gama_bytes.h"
#include "crc16.h"

int gama_frame_encode(uint8_t *out, size_t out_cap,
                      uint8_t type, uint16_t seq,
                      const uint8_t *payload, uint8_t payload_len)
{
    if (out == NULL) {
        return GAMA_FRAME_ERR_ARG;
    }
    if (payload_len > GAMA_FRAME_MAX_PAYLOAD) {
        return GAMA_FRAME_ERR_LEN;
    }
    if (payload_len > 0 && payload == NULL) {
        return GAMA_FRAME_ERR_ARG;
    }

    size_t total = (size_t)GAMA_FRAME_OVERHEAD + payload_len;
    if (out_cap < total) {
        return GAMA_FRAME_ERR_ARG;
    }

    size_t pos = 0;
    pos += gama_put_u8(out + pos, (uint8_t)GAMA_FRAME_VERSION);
    pos += gama_put_u8(out + pos, type);
    pos += gama_put_u16(out + pos, seq);
    pos += gama_put_u8(out + pos, payload_len);

    for (uint8_t i = 0; i < payload_len; i++) {
        out[pos + i] = payload[i];
    }
    pos += payload_len;

    gama_put_u16(out + pos, gama_crc16(out, pos));
    pos += GAMA_FRAME_CRC_LEN;

    return (int)pos;
}

int gama_frame_decode(const uint8_t *in, size_t in_len, gama_frame_t *frame)
{
    if (in == NULL || frame == NULL) {
        return GAMA_FRAME_ERR_ARG;
    }
    /* Need the fixed header before the declared length can be trusted. */
    if (in_len < GAMA_FRAME_HEADER_LEN) {
        return GAMA_FRAME_ERR_SHORT;
    }

    uint8_t version = gama_get_u8(in + 0);
    if (version != GAMA_FRAME_VERSION) {
        return GAMA_FRAME_ERR_VERSION;
    }

    uint8_t len = gama_get_u8(in + 4);
    if (len > GAMA_FRAME_MAX_PAYLOAD) {
        return GAMA_FRAME_ERR_LEN;
    }

    size_t total = (size_t)GAMA_FRAME_OVERHEAD + len;
    if (in_len < total) {
        return GAMA_FRAME_ERR_SHORT;
    }

    size_t crc_off = (size_t)GAMA_FRAME_HEADER_LEN + len;
    uint16_t want = gama_get_u16(in + crc_off);
    uint16_t have = gama_crc16(in, crc_off);
    if (want != have) {
        return GAMA_FRAME_ERR_CRC;
    }

    frame->version = version;
    frame->type    = gama_get_u8(in + 1);
    frame->seq     = gama_get_u16(in + 2);
    frame->len     = len;
    frame->payload = (len > 0) ? (in + GAMA_FRAME_HEADER_LEN) : NULL;

    return (int)total;
}

const char *gama_frame_type_name(uint8_t type)
{
    switch (type) {
    case GAMA_FRAME_TC:            return "TC";
    case GAMA_FRAME_TC_ACK:        return "TC_ACK";
    case GAMA_FRAME_TM_HK:         return "TM_HK";
    case GAMA_FRAME_TM_TRACKS:     return "TM_TRACKS";
    case GAMA_FRAME_TM_ROSTER:     return "TM_ROSTER";
    case GAMA_FRAME_TM_STAT:       return "TM_STAT";
    case GAMA_FRAME_BULK_DATA:     return "BULK_DATA";
    case GAMA_FRAME_BULK_ACK:      return "BULK_ACK";
    case GAMA_FRAME_IPC_TC_EVENT:  return "IPC_TC_EVENT";
    case GAMA_FRAME_IPC_TIME_SET:  return "IPC_TIME_SET";
    case GAMA_FRAME_IPC_TELEMETRY: return "IPC_TELEMETRY";
    case GAMA_FRAME_IPC_MODE:      return "IPC_MODE";
    case GAMA_FRAME_IPC_TRACKS:    return "IPC_TRACKS";
    case GAMA_FRAME_IPC_STAT:      return "IPC_STAT";
    case GAMA_FRAME_IPC_HELLO:     return "IPC_HELLO";
    case GAMA_FRAME_BEACON:        return "BEACON";
    default:                       return "UNKNOWN";
    }
}

const char *gama_frame_result_name(int result)
{
    switch (result) {
    case GAMA_FRAME_OK:          return "OK";
    case GAMA_FRAME_ERR_SHORT:   return "SHORT";
    case GAMA_FRAME_ERR_VERSION: return "VERSION";
    case GAMA_FRAME_ERR_LEN:     return "LEN";
    case GAMA_FRAME_ERR_CRC:     return "CRC";
    case GAMA_FRAME_ERR_ARG:     return "ARG";
    default:                     return (result > 0) ? "OK" : "UNKNOWN";
    }
}
