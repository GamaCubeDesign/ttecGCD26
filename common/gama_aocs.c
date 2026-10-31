#include "gama_aocs.h"
#include "crc16.h"
#include "gama_bytes.h"
#include "gama_frame.h"

/*
 * Layout (docs/icd/obc-aocs-i2c.md §3):
 *
 *   COMMAND, 16 B             STATUS, 20 B
 *   0  magic 'G'              0  magic 'G'
 *   1  version                1  version
 *   2  type 0x01              2  type 0x02
 *   3  seq                    3  seq (echo)
 *   4  cmd                    4  state
 *   5  tasks                  5  error
 *   6  max_task_s  u16        6  roll_cdeg   i16
 *   8  azimuth_cdeg u16       8  pitch_cdeg  i16
 *   10 reserved, 4 B of 0     10 yaw_cdeg    i16
 *   14 crc16 over 0..13       12 rate_cdps   u16
 *                             14 task_s      u16
 *                             16 flags, reserved
 *                             18 crc16 over 0..17
 */

static int check_header(const uint8_t *in, size_t len, size_t want, uint8_t type)
{
    if (len != want) {
        return GAMA_FRAME_ERR_LEN;
    }
    if (in[0] != GAMA_AOCS_MAGIC || in[1] != GAMA_AOCS_VERSION || in[2] != type) {
        return GAMA_FRAME_ERR_VERSION;
    }
    if (gama_get_u16(in + want - 2u) != gama_crc16(in, want - 2u)) {
        return GAMA_FRAME_ERR_CRC;
    }
    return GAMA_FRAME_OK;
}

int gama_aocs_command_encode(uint8_t *out, size_t cap, const gama_aocs_command_t *c)
{
    if (out == NULL || c == NULL || cap < GAMA_AOCS_COMMAND_LEN) {
        return GAMA_FRAME_ERR_ARG;
    }
    if (c->cmd == GAMA_AOCS_CMD_START &&
        (c->max_task_s == 0u || c->max_task_s > GAMA_AOCS_MAX_TASK_S)) {
        return GAMA_FRAME_ERR_ARG;
    }
    size_t pos = 0;
    pos += gama_put_u8(out + pos, GAMA_AOCS_MAGIC);
    pos += gama_put_u8(out + pos, GAMA_AOCS_VERSION);
    pos += gama_put_u8(out + pos, GAMA_AOCS_TYPE_COMMAND);
    pos += gama_put_u8(out + pos, c->seq);
    pos += gama_put_u8(out + pos, c->cmd);
    pos += gama_put_u8(out + pos, c->tasks);
    pos += gama_put_u16(out + pos, c->max_task_s);
    pos += gama_put_u16(out + pos, c->azimuth_cdeg);
    while (pos < GAMA_AOCS_COMMAND_LEN - 2u) {
        pos += gama_put_u8(out + pos, 0);
    }
    pos += gama_put_u16(out + pos, gama_crc16(out, GAMA_AOCS_COMMAND_LEN - 2u));
    return (int)pos;
}

int gama_aocs_command_decode(const uint8_t *in, size_t len, gama_aocs_command_t *c)
{
    if (in == NULL || c == NULL) {
        return GAMA_FRAME_ERR_ARG;
    }
    int r = check_header(in, len, GAMA_AOCS_COMMAND_LEN, GAMA_AOCS_TYPE_COMMAND);
    if (r != GAMA_FRAME_OK) {
        return r;
    }
    c->seq          = gama_get_u8(in + 3);
    c->cmd          = gama_get_u8(in + 4);
    c->tasks        = gama_get_u8(in + 5);
    c->max_task_s   = gama_get_u16(in + 6);
    c->azimuth_cdeg = gama_get_u16(in + 8);
    return (int)GAMA_AOCS_COMMAND_LEN;
}

int gama_aocs_status_encode(uint8_t *out, size_t cap, const gama_aocs_status_t *s)
{
    if (out == NULL || s == NULL || cap < GAMA_AOCS_STATUS_LEN) {
        return GAMA_FRAME_ERR_ARG;
    }
    size_t pos = 0;
    pos += gama_put_u8(out + pos, GAMA_AOCS_MAGIC);
    pos += gama_put_u8(out + pos, GAMA_AOCS_VERSION);
    pos += gama_put_u8(out + pos, GAMA_AOCS_TYPE_STATUS);
    pos += gama_put_u8(out + pos, s->seq);
    pos += gama_put_u8(out + pos, s->state);
    pos += gama_put_u8(out + pos, s->error);
    pos += gama_put_i16(out + pos, s->roll_cdeg);
    pos += gama_put_i16(out + pos, s->pitch_cdeg);
    pos += gama_put_i16(out + pos, s->yaw_cdeg);
    pos += gama_put_u16(out + pos, s->rate_cdps);
    pos += gama_put_u16(out + pos, s->task_s);
    pos += gama_put_u8(out + pos, s->flags);
    pos += gama_put_u8(out + pos, 0);
    pos += gama_put_u16(out + pos, gama_crc16(out, GAMA_AOCS_STATUS_LEN - 2u));
    return (int)pos;
}

int gama_aocs_status_decode(const uint8_t *in, size_t len, gama_aocs_status_t *s)
{
    if (in == NULL || s == NULL) {
        return GAMA_FRAME_ERR_ARG;
    }
    int r = check_header(in, len, GAMA_AOCS_STATUS_LEN, GAMA_AOCS_TYPE_STATUS);
    if (r != GAMA_FRAME_OK) {
        return r;
    }
    s->seq        = gama_get_u8(in + 3);
    s->state      = gama_get_u8(in + 4);
    s->error      = gama_get_u8(in + 5);
    s->roll_cdeg  = gama_get_i16(in + 6);
    s->pitch_cdeg = gama_get_i16(in + 8);
    s->yaw_cdeg   = gama_get_i16(in + 10);
    s->rate_cdps  = gama_get_u16(in + 12);
    s->task_s     = gama_get_u16(in + 14);
    s->flags      = gama_get_u8(in + 16);
    return (int)GAMA_AOCS_STATUS_LEN;
}

const char *gama_aocs_state_name(uint8_t state)
{
    switch (state) {
    case GAMA_AOCS_ST_IDLE:     return "IDLE";
    case GAMA_AOCS_ST_ACCEPTED: return "ACCEPTED";
    case GAMA_AOCS_ST_REFUSED:  return "REFUSED";
    case GAMA_AOCS_ST_DONE:     return "DONE";
    case GAMA_AOCS_ST_FAILED:   return "FAILED";
    default:                    return "UNKNOWN";
    }
}
