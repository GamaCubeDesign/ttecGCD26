/*
 * link_sim.h — both ends of the link on a simulated half-duplex LoRa channel.
 *
 * The satellite is the real ttcd core; the ground is the real gs_link. They
 * talk through a model of two SX1278 radios:
 *
 *   - a frame occupies the air for exactly the time on air of its profile;
 *   - a radio cannot hear while it transmits, so if both transmit at once
 *     neither frame is received (a collision);
 *   - a receiver on a different profile, or one that retunes mid-frame,
 *     hears nothing;
 *   - listen-before-talk: a radio sees the other's transmission only once the
 *     header has been decoded (gama_lora_header_us), and only if it was
 *     listening when that transmission began — the pessimistic assumption;
 *   - frames can be lost at random, in blackouts, or by type on demand.
 *
 * Time is virtual. An hour of link runs in milliseconds, and a run is
 * reproducible from its seed.
 */

#ifndef LINK_SIM_H
#define LINK_SIM_H

#include "core.h"
#include "gama_bytes.h"
#include "gama_gs_link.h"
#include "gama_ipc.h"
#include "gama_lora.h"
#include "gama_tc.h"
#include "gama_tm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SAT = 0, GND = 1 };
#define SIM_MAX_TC 4096
#define SIM_WALL_BASE 1789900000u   /* the ground's wall clock at t = 0 */

typedef struct {
    bool     active;
    uint64_t start, end;
    uint8_t  profile;
    uint8_t  frame[GAMA_FRAME_MAX_TOTAL];
    size_t   len;
    bool     peer_listening;  /* the other radio was receiving when this began */
    bool     collided;        /* both on the air at some point                 */
    bool     retuned;         /* the receiver changed profile mid-frame        */
    bool     dropped;         /* lost by the channel model                     */
} air_t;

typedef struct {
    uint32_t id;
    uint16_t seq;
    uint8_t  cmd;
    uint64_t submitted, executed, acked;
    uint8_t  status, attempts;
    bool     done, failed;
} sim_tc_t;

typedef struct {
    uint64_t now;
    uint64_t rng;

    uint8_t  profile[2];
    air_t    air[2];

    uint32_t detect_symbols;      /* 0: header time (pessimistic default)       */
    uint32_t loss_ppm[2];         /* by transmitter: [SAT] downlink, [GND] uplink */
    uint64_t blackout_until[2];
    uint8_t  drop_type[2];
    int      drop_count[2];

    uint32_t sent[2], delivered[2], lost_channel[2], lost_collision[2], lost_profile[2];
    uint32_t collisions;
    uint8_t  sat_seq_delivered[65536];  /* 1 delivered, 2 sent but lost */
    int      first_sat_seq_delivered;
    int      last_sat_seq_delivered;

    ttcd_core_t sat;
    gs_link_t   gs;

    bool     adsbd_on;
    uint64_t next_adsbd;
    int      aircraft;

    sim_tc_t tc[SIM_MAX_TC];
    int      n_tc;

    uint32_t rx_tracks, rx_hk, rx_stat, rx_beacon, rx_ack;
    int      obc_events;
    uint64_t obc_event_at[1024];
    int      sat_exit;
} sim_t;

static sim_t S;

static uint64_t rnd(void)
{
    S.rng ^= S.rng >> 12;
    S.rng ^= S.rng << 25;
    S.rng ^= S.rng >> 27;
    return S.rng * 2685821657736338717ULL;
}

static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
    return lo + (uint32_t)(rnd() % (uint64_t)(hi - lo + 1u));
}

static bool chance_ppm(uint32_t ppm)
{
    return (uint32_t)(rnd() % 1000000u) < ppm;
}

static bool channel_drops(int x, uint8_t type)
{
    if (S.now < S.blackout_until[x]) {
        return true;
    }
    if (S.drop_count[x] > 0 && type == S.drop_type[x]) {
        S.drop_count[x]--;
        return true;
    }
    return chance_ppm(S.loss_ppm[x]);
}

static int radio_tx(int x, const uint8_t *frame, size_t len)
{
    int y = 1 - x;
    air_t *other = &S.air[y];

    /* Listen before talk: x's modem reports a reception in progress only if
     * it was listening when y began and has decoded y's header since. */
    if (other->active && other->profile == S.profile[x] && other->peer_listening) {
        const gama_lora_phy_t *phy = gama_lora_profile(other->profile);
        uint32_t detect_us = S.detect_symbols
                                 ? S.detect_symbols * gama_lora_symbol_us(phy)
                                 : gama_lora_header_us(phy);
        if ((S.now - other->start) * 1000u >= detect_us) {
            return 1;                                   /* busy */
        }
    }

    air_t *a = &S.air[x];
    uint32_t us = gama_lora_toa_us(gama_lora_profile(S.profile[x]), (uint8_t)len);
    a->active = true;
    a->start = S.now;
    a->end = S.now + (us + 999u) / 1000u;
    a->profile = S.profile[x];
    memcpy(a->frame, frame, len);
    a->len = len;
    a->peer_listening = !other->active;
    a->collided = false;
    a->retuned = false;
    a->dropped = channel_drops(x, frame[1]);
    if (other->active) {
        a->collided = true;
        other->collided = true;
        S.collisions++;
    }
    S.sent[x]++;
    if (x == SAT) {
        S.sat_seq_delivered[gama_get_u16(frame + 2)] = 2;
    }
    return 0;
}

static void set_profile(int x, uint8_t p)
{
    if (S.air[1 - x].active && S.profile[x] != p) {
        S.air[1 - x].retuned = true;        /* x retuned while receiving */
    }
    S.profile[x] = p;
}

/* ---- satellite ops ---- */

static int sat_tx(void *c, const uint8_t *f, size_t n)
{
    (void)c;
    return radio_tx(SAT, f, n) == 0 ? TTCD_TX_STARTED : TTCD_TX_BUSY;
}
static int sat_prof(void *c, uint8_t p) { (void)c; set_profile(SAT, p); return 0; }
static int sat_pow(void *c, int8_t d)   { (void)c; (void)d; return 0; }

static int sat_ipc(void *c, uint8_t role, const uint8_t *f, size_t n)
{
    (void)c;
    gama_frame_t fr;
    if (role == GAMA_IPC_ROLE_OBC && gama_frame_decode(f, n, &fr) > 0 &&
        fr.type == GAMA_FRAME_IPC_TC_EVENT && S.obc_events < 1024) {
        S.obc_event_at[S.obc_events++] = S.now;
    }
    return 0;
}

static void sat_log(void *c, uint64_t now, const char *event, const char *fields)
{
    (void)c;
    if (strcmp(event, "tc") != 0) {
        return;
    }
    const char *p = strstr(fields, "\"seq\":");
    if (p == NULL) {
        return;
    }
    uint16_t seq = (uint16_t)strtoul(p + 6, NULL, 10);
    for (int i = S.n_tc - 1; i >= 0; i--) {
        if (S.tc[i].seq == seq && S.tc[i].executed == 0) {
            S.tc[i].executed = now;
            break;
        }
    }
}

static void sat_exit(void *c, int code) { (void)c; S.sat_exit = code; }

static int sat_soc(void *c, int16_t *v) { (void)c; *v = 4500; return 0; }

/* ---- ground ops ---- */

static int gs_tx(void *c, const uint8_t *f, size_t n)
{
    (void)c;
    return radio_tx(GND, f, n) == 0 ? GS_TX_STARTED : GS_TX_BUSY;
}
static int gs_prof(void *c, uint8_t p) { (void)c; set_profile(GND, p); return 0; }

static int gs_wall(void *c, uint32_t *s, uint32_t *ns)
{
    (void)c;
    *s = SIM_WALL_BASE + (uint32_t)(S.now / 1000u);
    *ns = (uint32_t)(S.now % 1000u) * 1000000u;
    return 0;
}

static void gs_event(void *c, uint64_t now, const gs_event_t *ev)
{
    (void)c;
    switch (ev->kind) {
    case GS_EV_ACK:
        S.rx_ack++;
        for (int i = 0; i < S.n_tc; i++) {
            if (ev->id != 0 && S.tc[i].id == ev->id) {
                S.tc[i].acked = now;
                S.tc[i].status = ev->status;
                S.tc[i].attempts = ev->attempts;
                S.tc[i].done = true;
            }
        }
        break;
    case GS_EV_TC_FAILED:
        for (int i = 0; i < S.n_tc; i++) {
            if (ev->id != 0 && S.tc[i].id == ev->id) {
                S.tc[i].failed = true;
                S.tc[i].attempts = ev->attempts;
            }
        }
        break;
    case GS_EV_FRAME:
        S.rx_tracks += ev->type == GAMA_FRAME_TM_TRACKS;
        S.rx_hk     += ev->type == GAMA_FRAME_TM_HK;
        S.rx_stat   += ev->type == GAMA_FRAME_TM_STAT;
        S.rx_beacon += ev->type == GAMA_FRAME_BEACON;
        break;
    default:
        break;
    }
}

/* ---- fake adsbd: a fresh 20-aircraft snapshot every second ---- */

static void push_snapshot(void)
{
    int n = S.aircraft;
    uint8_t count = (uint8_t)((n + 11) / 12);
    if (count == 0) { count = 1; }
    for (uint8_t f = 0; f < count; f++) {
        uint8_t payload[GAMA_FRAME_MAX_PAYLOAD], frame[GAMA_FRAME_MAX_TOTAL];
        gama_ipc_tracks_hdr_t h = { .epoch_ms = (uint32_t)S.now, .index = f, .count = count };
        gama_ipc_tracks_header_encode(payload, sizeof(payload), &h);
        int in_frame = n - f * 12 > 12 ? 12 : n - f * 12;
        for (int r = 0; r < in_frame; r++) {
            gama_track_t t = { .icao = 0xE40000u + (uint32_t)(f * 12 + r),
                               .latitude = -23.5, .longitude = -46.6,
                               .altitude_ft = 35000, .ground_speed_kt = 450.0,
                               .age_ds = (uint16_t)rnd_range(0, 9),
                               .flags = GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE };
            gama_track_encode(payload + GAMA_IPC_TRACKS_HEADER_LEN
                                  + (size_t)r * GAMA_TRACK_WIRE_LEN,
                              GAMA_TRACK_WIRE_LEN, &t);
        }
        int len = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_IPC_TRACKS, 0, payload,
                                    (uint8_t)(GAMA_IPC_TRACKS_HEADER_LEN
                                              + (unsigned)in_frame * GAMA_TRACK_WIRE_LEN));
        ttcd_on_ipc_frame(&S.sat, S.now, GAMA_IPC_ROLE_ADSBD, frame, (size_t)len);
    }
}

/* ---- the event loop ---- */

static void end_tx(int x)
{
    air_t a = S.air[x];
    S.air[x].active = false;
    int y = 1 - x;

    if (x == SAT) {
        ttcd_on_radio_tx_done(&S.sat, S.now);
    } else {
        gs_on_tx_done(&S.gs, S.now);
    }

    if (a.dropped)                      { S.lost_channel[x]++;   return; }
    if (a.collided)                     { S.lost_collision[x]++; return; }
    if (a.retuned || S.profile[y] != a.profile) { S.lost_profile[x]++; return; }

    S.delivered[x]++;
    if (x == SAT) {
        uint16_t seq = gama_get_u16(a.frame + 2);
        S.sat_seq_delivered[seq] = 1;
        if (S.first_sat_seq_delivered < 0) { S.first_sat_seq_delivered = seq; }
        S.last_sat_seq_delivered = seq;
        gs_on_rx(&S.gs, S.now, a.frame, a.len, -60, 8);
    } else {
        ttcd_on_radio_rx(&S.sat, S.now, a.frame, a.len, -60, 8);
    }
}

static bool run_until(uint64_t until)
{
    uint64_t same_t = 0; int spins = 0;
    for (;;) {
        uint64_t t_sat_end = S.air[SAT].active ? S.air[SAT].end : UINT64_MAX;
        uint64_t t_gnd_end = S.air[GND].active ? S.air[GND].end : UINT64_MAX;
        uint64_t t_adsbd   = S.adsbd_on ? S.next_adsbd : UINT64_MAX;
        uint64_t t_sat     = ttcd_next_deadline(&S.sat);
        uint64_t t_gnd     = gs_next_deadline(&S.gs);

        uint64_t t = t_sat_end;
        if (t_gnd_end < t) { t = t_gnd_end; }
        if (t_adsbd < t)   { t = t_adsbd; }
        if (t_sat < t)     { t = t_sat; }
        if (t_gnd < t)     { t = t_gnd; }

        if (t > until) {
            S.now = until;
            return true;
        }
        if (t > S.now) {
            S.now = t;
        }
        if (S.now == same_t) {
            if (++spins > 10000) {
                printf("  sim stuck at t=%llu\n", (unsigned long long)S.now);
                return false;
            }
        } else {
            same_t = S.now;
            spins = 0;
        }

        /* Ends of transmission first, so a transmitter is back in reception
         * before the receiver reacts to what it heard. */
        if (t == t_sat_end)      { end_tx(SAT); }
        else if (t == t_gnd_end) { end_tx(GND); }
        else if (t == t_adsbd)   { push_snapshot(); S.next_adsbd = S.now + 1000u; }
        else if (t == t_sat)     { ttcd_on_tick(&S.sat, S.now); }
        else                     { gs_on_tick(&S.gs, S.now); }
    }
}

static void sim_reset(uint64_t seed)
{
    memset(&S, 0, sizeof(S));
    S.rng = seed * 0x9E3779B97F4A7C15ULL + 1u;
    S.now = 1000000;
    S.aircraft = 20;
    S.first_sat_seq_delivered = -1;
    S.last_sat_seq_delivered = -1;

    ttcd_params_t sp;
    ttcd_params_default(&sp);
    ttcd_ops_t so = { .radio_tx = sat_tx, .radio_set_profile = sat_prof,
                      .radio_set_power = sat_pow, .ipc_send = sat_ipc, .log = sat_log,
                      .soc_temp = sat_soc, .exit_request = sat_exit };
    ttcd_init(&S.sat, &sp, &so, S.now);

    gs_params_t gp;
    gs_params_default(&gp);
    gs_ops_t go = { .radio_tx = gs_tx, .radio_set_profile = gs_prof,
                    .event = gs_event, .wall_time = gs_wall };
    gs_init(&S.gs, &gp, &go, S.now);
}

static int submit(uint8_t cmd, const uint8_t *args, uint8_t n)
{
    if (S.n_tc >= SIM_MAX_TC) {
        return -1;
    }
    uint32_t id = gs_submit(&S.gs, S.now, cmd, args, n);
    if (id == 0) {
        return -1;
    }
    sim_tc_t *t = &S.tc[S.n_tc];
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->cmd = cmd;
    t->submitted = S.now;
    for (uint8_t i = 0; i < S.gs.q_count; i++) {
        const gs_tc_t *q = &S.gs.q[(S.gs.q_head + i) % GS_TC_QUEUE];
        if (q->id == id) {
            t->seq = q->seq;
        }
    }
    return S.n_tc++;
}

/* Advances until telecommand i is acknowledged or `limit_ms` passes. */
static bool run_until_done(int i, uint64_t limit_ms)
{
    uint64_t end = S.now + limit_ms;
    while (!S.tc[i].done && !S.tc[i].failed && S.now < end) {
        run_until(S.now + 50);
    }
    return S.tc[i].done;
}

static void start_streaming(void)
{
    uint8_t a[2];
    gama_put_u16(a, 50);
    submit(GAMA_TC_STREAM_START, a, 2);
    S.adsbd_on = true;
    S.next_adsbd = S.now;
    ttcd_on_ipc_peer(&S.sat, S.now, GAMA_IPC_ROLE_ADSBD, true);
}

/* Satellite frames lost between the first and the last one the ground
 * received: exactly what the ground can detect from sequence gaps. */
static uint32_t detectable_downlink_losses(void)
{
    uint32_t n = 0;
    for (int s = S.first_sat_seq_delivered + 1; s < S.last_sat_seq_delivered; s++) {
        n += S.sat_seq_delivered[s] == 2;
    }
    return n;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct { uint64_t p50, p95, max; int n; } stats_t;

/* Percentiles of submit -> execution over TCs matching `attempts` (0 = any). */
static stats_t exec_stats(uint8_t attempts)
{
    static uint64_t v[SIM_MAX_TC];
    int n = 0;
    for (int i = 0; i < S.n_tc; i++) {
        if (S.tc[i].executed && (attempts == 0 || S.tc[i].attempts == attempts)) {
            v[n++] = S.tc[i].executed - S.tc[i].submitted;
        }
    }
    stats_t st = { 0, 0, 0, n };
    if (n == 0) {
        return st;
    }
    qsort(v, (size_t)n, sizeof(v[0]), cmp_u64);
    st.p50 = v[n / 2];
    st.p95 = v[(n * 95 + 99) / 100 - 1];
    st.max = v[n - 1];
    return st;
}

static uint32_t toa_ms(uint8_t profile, uint8_t len)
{
    return (gama_lora_toa_us(gama_lora_profile(profile), len) + 999u) / 1000u;
}

#endif /* LINK_SIM_H */
