#include "radio.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    sx1278_linux_t hw;
    sx1278_t       dev;
} sx_impl_t;

static int sx_set_profile(radio_t *r, uint8_t p)
{
    return sx1278_set_profile(&((sx_impl_t *)r->impl)->dev, p);
}

static int sx_set_power(radio_t *r, int8_t dbm)
{
    return sx1278_set_power(&((sx_impl_t *)r->impl)->dev, dbm);
}

static int sx_transmit(radio_t *r, const uint8_t *frame, size_t len)
{
    sx_impl_t *s = r->impl;
    bool busy = false;
    if (sx1278_rx_busy(&s->dev, &busy) != 0) {
        return RADIO_TX_ERROR;
    }
    if (busy) {
        return RADIO_TX_BUSY;          /* listen before talk (ADR-0007) */
    }
    return sx1278_transmit(&s->dev, frame, (uint8_t)len) == 0 ? RADIO_TX_STARTED
                                                             : RADIO_TX_ERROR;
}

static int sx_next_event(radio_t *r, radio_event_t *ev)
{
    sx_impl_t *s = r->impl;
    sx1278_linux_irq_drain(&s->hw);
    sx1278_event_t e;
    uint8_t len = 0;
    if (sx1278_service(&s->dev, &e, ev->buf, &len, &ev->rssi, &ev->snr) != 0) {
        return -1;
    }
    switch (e) {
    case SX_EVENT_RX:           ev->kind = RADIO_EV_RX; ev->len = len; return 1;
    case SX_EVENT_RX_CRC_ERROR: ev->kind = RADIO_EV_RX_ERROR; return 1;
    case SX_EVENT_TX_DONE:      ev->kind = RADIO_EV_TX_DONE; return 1;
    case SX_EVENT_NONE:
    default:                    return 0;
    }
}

static void sx_close(radio_t *r)
{
    sx_impl_t *s = r->impl;
    sx1278_linux_close(&s->hw);
    free(s);
    r->impl = NULL;
}

int radio_open_sx1278(radio_t *r, const sx1278_linux_cfg_t *cfg, uint8_t profile,
                      int8_t power_dbm, sx1278_lbt_t lbt, const char **why)
{
    sx_impl_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        *why = "out of memory";
        return -1;
    }
    sx1278_bus_t bus;
    if (sx1278_linux_open(&s->hw, cfg, &bus, why) != 0) {
        free(s);
        return -1;
    }
    if (sx1278_init(&s->dev, &bus, profile, power_dbm, lbt, why) != 0) {
        sx1278_linux_close(&s->hw);
        free(s);
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->impl = s;
    r->name = "sx1278";
    r->fd = sx1278_linux_irq_fd(&s->hw);
    r->set_profile = sx_set_profile;
    r->set_power = sx_set_power;
    r->transmit = sx_transmit;
    r->next_event = sx_next_event;
    r->close = sx_close;
    return 0;
}
