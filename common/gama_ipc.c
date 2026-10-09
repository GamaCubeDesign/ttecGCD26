#include "gama_ipc.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_tm.h"

int gama_ipc_hello_encode(uint8_t *out, size_t cap, const gama_ipc_hello_t *h)
{
    if (out == NULL || h == NULL || cap < GAMA_IPC_HELLO_LEN) { return GAMA_FRAME_ERR_ARG; }
    out[0] = h->role;
    out[1] = h->version;
    return (int)GAMA_IPC_HELLO_LEN;
}

int gama_ipc_hello_decode(const uint8_t *in, size_t len, gama_ipc_hello_t *h)
{
    if (in == NULL || h == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (len != GAMA_IPC_HELLO_LEN) { return GAMA_FRAME_ERR_LEN; }
    h->role = in[0];
    h->version = in[1];
    return (int)GAMA_IPC_HELLO_LEN;
}

int gama_ipc_time_encode(uint8_t *out, size_t cap, const gama_ipc_time_t *t)
{
    if (out == NULL || t == NULL || cap < GAMA_IPC_TIME_LEN) { return GAMA_FRAME_ERR_ARG; }
    gama_put_u32(out, t->unix_s);
    gama_put_u32(out + 4, t->nsec);
    return (int)GAMA_IPC_TIME_LEN;
}

int gama_ipc_time_decode(const uint8_t *in, size_t len, gama_ipc_time_t *t)
{
    if (in == NULL || t == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (len != GAMA_IPC_TIME_LEN) { return GAMA_FRAME_ERR_LEN; }
    t->unix_s = gama_get_u32(in);
    t->nsec = gama_get_u32(in + 4);
    return (int)GAMA_IPC_TIME_LEN;
}

int gama_ipc_telemetry_encode(uint8_t *out, size_t cap, const gama_ipc_telemetry_t *t)
{
    if (out == NULL || t == NULL || cap < GAMA_IPC_TELEMETRY_LEN) { return GAMA_FRAME_ERR_ARG; }
    size_t pos = 0;
    pos += gama_put_u16(out + pos, t->battery_mv);
    pos += gama_put_i16(out + pos, t->current_ma);
    pos += gama_put_i16(out + pos, t->temp_bat_ccel);
    pos += gama_put_i16(out + pos, t->temp_ext_ccel);
    pos += gama_put_i16(out + pos, t->roll_cdeg);
    pos += gama_put_i16(out + pos, t->pitch_cdeg);
    pos += gama_put_i16(out + pos, t->yaw_cdeg);
    return (int)pos;
}

int gama_ipc_telemetry_decode(const uint8_t *in, size_t len, gama_ipc_telemetry_t *t)
{
    if (in == NULL || t == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (len != GAMA_IPC_TELEMETRY_LEN) { return GAMA_FRAME_ERR_LEN; }
    t->battery_mv    = gama_get_u16(in + 0);
    t->current_ma    = gama_get_i16(in + 2);
    t->temp_bat_ccel = gama_get_i16(in + 4);
    t->temp_ext_ccel = gama_get_i16(in + 6);
    t->roll_cdeg     = gama_get_i16(in + 8);
    t->pitch_cdeg    = gama_get_i16(in + 10);
    t->yaw_cdeg      = gama_get_i16(in + 12);
    return (int)GAMA_IPC_TELEMETRY_LEN;
}

int gama_ipc_stat_encode(uint8_t *out, size_t cap, const gama_ipc_stat_t *s)
{
    if (out == NULL || s == NULL || cap < GAMA_IPC_STAT_LEN) { return GAMA_FRAME_ERR_ARG; }
    size_t pos = 0;
    pos += gama_put_u32(out + pos, s->msgs_received);
    pos += gama_put_u32(out + pos, s->msgs_decoded);
    pos += gama_put_u16(out + pos, s->aircraft_tracked);
    pos += gama_put_u16(out + pos, s->dump1090_restarts);
    return (int)pos;
}

int gama_ipc_stat_decode(const uint8_t *in, size_t len, gama_ipc_stat_t *s)
{
    if (in == NULL || s == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (len != GAMA_IPC_STAT_LEN) { return GAMA_FRAME_ERR_LEN; }
    s->msgs_received     = gama_get_u32(in + 0);
    s->msgs_decoded      = gama_get_u32(in + 4);
    s->aircraft_tracked  = gama_get_u16(in + 8);
    s->dump1090_restarts = gama_get_u16(in + 10);
    return (int)GAMA_IPC_STAT_LEN;
}

int gama_ipc_tracks_header_encode(uint8_t *out, size_t cap,
                                  const gama_ipc_tracks_hdr_t *h)
{
    if (out == NULL || h == NULL || cap < GAMA_IPC_TRACKS_HEADER_LEN) {
        return GAMA_FRAME_ERR_ARG;
    }
    if (h->count == 0 || h->count > GAMA_IPC_TRACKS_MAX_FRAMES || h->index >= h->count) {
        return GAMA_FRAME_ERR_ARG;
    }
    gama_put_u32(out, h->epoch_ms);
    out[4] = h->index;
    out[5] = h->count;
    return (int)GAMA_IPC_TRACKS_HEADER_LEN;
}

int gama_ipc_tracks_decode(const uint8_t *in, size_t len,
                           gama_ipc_tracks_hdr_t *h, size_t *n_records)
{
    if (in == NULL || h == NULL || n_records == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (len < GAMA_IPC_TRACKS_HEADER_LEN) { return GAMA_FRAME_ERR_SHORT; }

    size_t body = len - GAMA_IPC_TRACKS_HEADER_LEN;
    if (body % GAMA_TRACK_WIRE_LEN != 0) { return GAMA_FRAME_ERR_LEN; }
    if (body / GAMA_TRACK_WIRE_LEN > GAMA_IPC_TRACKS_MAX) { return GAMA_FRAME_ERR_LEN; }

    uint8_t index = in[4], count = in[5];
    if (count == 0 || count > GAMA_IPC_TRACKS_MAX_FRAMES || index >= count) {
        return GAMA_FRAME_ERR_LEN;
    }
    h->epoch_ms = gama_get_u32(in);
    h->index = index;
    h->count = count;
    *n_records = body / GAMA_TRACK_WIRE_LEN;
    return (int)GAMA_IPC_TRACKS_HEADER_LEN;
}

const char *gama_ipc_role_name(uint8_t role)
{
    switch (role) {
    case GAMA_IPC_ROLE_ADSBD: return "adsbd";
    case GAMA_IPC_ROLE_OBC:   return "obc";
    case GAMA_IPC_ROLE_TOOL:  return "tool";
    default:                  return "none";
    }
}
