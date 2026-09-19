/*
 * radio.h — the radio as the shells see it: one pollable descriptor, a way to
 * transmit, and events to collect when the descriptor is readable.
 *
 * Two backends:
 *   sx1278  the RA-02 on the Pi's SPI bus (flight and bench)
 *   udp     a simulated radio over UDP, for running the real binaries on a
 *           development machine: a frame reaches the peer only after its time
 *           on air, only if the peer is on the same profile, and never while
 *           the receiver is itself transmitting
 */

#ifndef TTEC_RADIO_H
#define TTEC_RADIO_H

#include <stddef.h>
#include <stdint.h>

#include "gama_lora.h"
#include "sx1278_linux.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RADIO_TX_STARTED = 0, RADIO_TX_BUSY = 1, RADIO_TX_ERROR = -1 };

typedef enum {
    RADIO_EV_NONE = 0,
    RADIO_EV_RX,           /* a packet that passed the PHY CRC */
    RADIO_EV_RX_ERROR,     /* a packet that failed it          */
    RADIO_EV_TX_DONE
} radio_ev_t;

typedef struct {
    radio_ev_t kind;
    uint8_t    buf[GAMA_LORA_MAX_PAYLOAD];
    size_t     len;
    int16_t    rssi;
    int8_t     snr;
} radio_event_t;

typedef struct radio radio_t;
struct radio {
    void       *impl;
    const char *name;
    int         fd;      /* readable when next_event() may have something */
    int  (*set_profile)(radio_t *r, uint8_t profile);
    int  (*set_power)(radio_t *r, int8_t dbm);
    int  (*transmit)(radio_t *r, const uint8_t *frame, size_t len);
    /* 1 with *ev filled, 0 when nothing is pending, -1 on error. Call until
     * it returns 0 each time the descriptor becomes readable, and also
     * periodically: an edge-triggered interrupt can be missed. */
    int  (*next_event)(radio_t *r, radio_event_t *ev);
    void (*close)(radio_t *r);
};

int radio_open_sx1278(radio_t *r, const sx1278_linux_cfg_t *cfg, uint8_t profile,
                      int8_t power_dbm, sx1278_lbt_t lbt, const char **why);

int radio_open_udp(radio_t *r, const char *bind_ip, uint16_t bind_port,
                   const char *peer_ip, uint16_t peer_port, uint8_t profile,
                   const char **why);

#ifdef __cplusplus
}
#endif

#endif /* TTEC_RADIO_H */
