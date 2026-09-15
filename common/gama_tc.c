#include "gama_tc.h"

#include <stddef.h>

int gama_tc_arg_len(uint8_t cmd)
{
    switch (cmd) {
    case GAMA_TC_PING:         return 0;
    case GAMA_TC_SET_TIME:     return 8;  /* u32 seconds + u32 nanoseconds */
    case GAMA_TC_SET_MODE:     return 1;
    case GAMA_TC_SET_RATE:     return 1;
    case GAMA_TC_SET_TX_POWER: return 1;
    case GAMA_TC_STREAM_START: return 2;
    case GAMA_TC_STREAM_STOP:  return 0;
    case GAMA_TC_REQ_HK:       return 0;
    case GAMA_TC_REQ_STAT:     return 0;
    case GAMA_TC_REQ_ROSTER:   return 0;
    case GAMA_TC_BULK_START:   return 6;  /* u32 offset + u16 max chunks   */
    case GAMA_TC_BULK_ABORT:   return 0;
    case GAMA_TC_SHUTDOWN:     return 2;  /* u16 magic                     */
    default:                   return -1;
    }
}

const char *gama_tc_name(uint8_t cmd)
{
    switch (cmd) {
    case GAMA_TC_PING:         return "PING";
    case GAMA_TC_SET_TIME:     return "SET_TIME";
    case GAMA_TC_SET_MODE:     return "SET_MODE";
    case GAMA_TC_SET_RATE:     return "SET_RATE";
    case GAMA_TC_SET_TX_POWER: return "SET_TX_POWER";
    case GAMA_TC_STREAM_START: return "STREAM_START";
    case GAMA_TC_STREAM_STOP:  return "STREAM_STOP";
    case GAMA_TC_REQ_HK:       return "REQ_HK";
    case GAMA_TC_REQ_STAT:     return "REQ_STAT";
    case GAMA_TC_REQ_ROSTER:   return "REQ_ROSTER";
    case GAMA_TC_BULK_START:   return "BULK_START";
    case GAMA_TC_BULK_ABORT:   return "BULK_ABORT";
    case GAMA_TC_SHUTDOWN:     return "SHUTDOWN";
    default:                   return "UNKNOWN";
    }
}

const char *gama_ack_status_name(uint8_t status)
{
    switch (status) {
    case GAMA_ACK_OK:             return "OK";
    case GAMA_ACK_UNKNOWN_CMD:    return "UNKNOWN_CMD";
    case GAMA_ACK_BAD_ARGS:       return "BAD_ARGS";
    case GAMA_ACK_REJECTED_STATE: return "REJECTED_STATE";
    case GAMA_ACK_FAILED:         return "FAILED";
    default:                      return "UNKNOWN";
    }
}

const char *gama_rate_profile_name(uint8_t profile)
{
    switch (profile) {
    case GAMA_RATE_SAFE:    return "SAFE";
    case GAMA_RATE_NOMINAL: return "NOMINAL";
    case GAMA_RATE_FAST:    return "FAST";
    default:                return "UNKNOWN";
    }
}

const char *gama_obc_state_name(uint8_t state)
{
    switch (state) {
    case GAMA_OBC_ST_PRE_TEST:           return "PRE_TEST";
    case GAMA_OBC_ST_BASIC_INTERMEDIATE: return "BASIC_INTERMEDIATE";
    case GAMA_OBC_ST_ADVANCED_AOCS:      return "ADVANCED_AOCS";
    case GAMA_OBC_ST_MISSION_ADSB:       return "MISSION_ADSB";
    case GAMA_OBC_ST_MISSION_DOWNLINK:   return "MISSION_DOWNLINK";
    case GAMA_OBC_ST_ENV_SURVIVAL:       return "ENV_SURVIVAL";
    case GAMA_OBC_ST_UNKNOWN:            return "UNKNOWN";
    default:                             return "UNKNOWN";
    }
}

const char *gama_link_state_name(uint8_t state)
{
    switch (state) {
    case GAMA_LINK_IDLE:   return "IDLE";
    case GAMA_LINK_TX:     return "TX";
    case GAMA_LINK_STREAM: return "STREAM";
    case GAMA_LINK_BULK:   return "BULK";
    case GAMA_LINK_SAFE:   return "SAFE";
    default:               return "UNKNOWN";
    }
}
