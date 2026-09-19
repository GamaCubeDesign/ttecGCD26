/*
 * A radio simulated over UDP, for running ttcd and the ground tool on one
 * machine. Each datagram is [profile][frame]. The sender holds the frame for
 * its time on air before sending it, so the receiver sees it when the modem
 * would raise RxDone; a receiver on another profile, or transmitting at that
 * moment, drops it. There is no listen-before-talk: the channel's collision
 * behaviour is what tests/test_link_sim.c models; this backend is for
 * exercising the real processes, sockets and timers.
 */

#include "radio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

typedef struct {
    int      sock;
    int      timer;
    int      ep;                   /* the descriptor handed to the shell */
    struct sockaddr_in peer;
    uint8_t  profile;
    bool     tx_pending;
    uint8_t  tx[1 + GAMA_LORA_MAX_PAYLOAD];
    size_t   tx_len;
} udp_impl_t;

static int udp_set_profile(radio_t *r, uint8_t p)
{
    udp_impl_t *u = r->impl;
    if (u->tx_pending || gama_lora_profile(p) == NULL) {
        return -1;
    }
    u->profile = p;
    return 0;
}

static int udp_set_power(radio_t *r, int8_t dbm)
{
    (void)r;
    return (dbm >= 2 && dbm <= 20) ? 0 : -1;
}

static int udp_transmit(radio_t *r, const uint8_t *frame, size_t len)
{
    udp_impl_t *u = r->impl;
    if (u->tx_pending || len == 0 || len > GAMA_LORA_MAX_PAYLOAD) {
        return RADIO_TX_ERROR;
    }
    u->tx[0] = u->profile;
    memcpy(u->tx + 1, frame, len);
    u->tx_len = len + 1;
    u->tx_pending = true;

    uint32_t us = gama_lora_toa_us(gama_lora_profile(u->profile), (uint8_t)len);
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = us / 1000000u;
    its.it_value.tv_nsec = (long)(us % 1000000u) * 1000L;
    if (timerfd_settime(u->timer, 0, &its, NULL) != 0) {
        u->tx_pending = false;
        return RADIO_TX_ERROR;
    }
    return RADIO_TX_STARTED;
}

static int udp_next_event(radio_t *r, radio_event_t *ev)
{
    udp_impl_t *u = r->impl;

    uint64_t expirations;
    if (u->tx_pending && read(u->timer, &expirations, sizeof(expirations)) > 0) {
        /* The frame has been on the air for its full time: deliver it. */
        sendto(u->sock, u->tx, u->tx_len, 0,
               (const struct sockaddr *)&u->peer, sizeof(u->peer));
        u->tx_pending = false;
        ev->kind = RADIO_EV_TX_DONE;
        return 1;
    }

    for (;;) {
        uint8_t dgram[1 + GAMA_LORA_MAX_PAYLOAD + 1];
        ssize_t n = recv(u->sock, dgram, sizeof(dgram), MSG_DONTWAIT);
        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        }
        if (n < 2 || (size_t)n > 1 + GAMA_LORA_MAX_PAYLOAD) {
            continue;
        }
        if (u->tx_pending || dgram[0] != u->profile) {
            continue;              /* deaf while transmitting, or wrong profile */
        }
        ev->kind = RADIO_EV_RX;
        ev->len = (size_t)n - 1u;
        memcpy(ev->buf, dgram + 1, ev->len);
        ev->rssi = -50;
        ev->snr = 10;
        return 1;
    }
}

static void udp_close(radio_t *r)
{
    udp_impl_t *u = r->impl;
    close(u->ep);
    close(u->timer);
    close(u->sock);
    free(u);
    r->impl = NULL;
}

int radio_open_udp(radio_t *r, const char *bind_ip, uint16_t bind_port,
                   const char *peer_ip, uint16_t peer_port, uint8_t profile,
                   const char **why)
{
    udp_impl_t *u = calloc(1, sizeof(*u));
    if (u == NULL) {
        *why = "out of memory";
        return -1;
    }
    u->profile = profile;
    u->sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    u->timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    u->ep = epoll_create1(EPOLL_CLOEXEC);

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(bind_port);
    memset(&u->peer, 0, sizeof(u->peer));
    u->peer.sin_family = AF_INET;
    u->peer.sin_port = htons(peer_port);

    struct epoll_event e1 = { .events = EPOLLIN, .data.fd = u->sock };
    struct epoll_event e2 = { .events = EPOLLIN, .data.fd = u->timer };
    if (u->sock < 0 || u->timer < 0 || u->ep < 0 ||
        inet_pton(AF_INET, bind_ip, &me.sin_addr) != 1 ||
        inet_pton(AF_INET, peer_ip, &u->peer.sin_addr) != 1 ||
        bind(u->sock, (const struct sockaddr *)&me, sizeof(me)) != 0 ||
        epoll_ctl(u->ep, EPOLL_CTL_ADD, u->sock, &e1) != 0 ||
        epoll_ctl(u->ep, EPOLL_CTL_ADD, u->timer, &e2) != 0) {
        *why = "cannot set up the UDP radio (address, port in use?)";
        if (u->ep >= 0)    { close(u->ep); }
        if (u->timer >= 0) { close(u->timer); }
        if (u->sock >= 0)  { close(u->sock); }
        free(u);
        return -1;
    }

    memset(r, 0, sizeof(*r));
    r->impl = u;
    r->name = "udp";
    r->fd = u->ep;
    r->set_profile = udp_set_profile;
    r->set_power = udp_set_power;
    r->transmit = udp_transmit;
    r->next_event = udp_next_event;
    r->close = udp_close;
    return 0;
}
