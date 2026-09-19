/* The ground link on its own: API contracts the channel simulation does not
 * isolate. The two-sided behaviour is in test_link_sim.c. */

#include "test_util.h"
#include "gama_bytes.h"
#include "gama_frame.h"
#include "gama_gs_link.h"
#include "gama_tc.h"

static gs_link_t G;
static uint64_t T = 1000000;
static int n_tx, n_failed, n_loss_events;
static uint32_t last_lost;
static uint8_t last_frame[GAMA_FRAME_MAX_TOTAL];

static int f_tx(void *c, const uint8_t *f, size_t n) { (void)c; memcpy(last_frame, f, n); n_tx++; return GS_TX_STARTED; }
static int f_prof(void *c, uint8_t p) { (void)c; (void)p; return 0; }
static void f_ev(void *c, uint64_t now, const gs_event_t *e)
{
    (void)c; (void)now;
    if (e->kind == GS_EV_TC_FAILED) { n_failed++; }
    if (e->kind == GS_EV_LOSS) { n_loss_events++; last_lost = e->lost; }
}

static void reset(void)
{
    gs_params_t p;
    gs_params_default(&p);
    gs_ops_t o = { .radio_tx = f_tx, .radio_set_profile = f_prof, .event = f_ev };
    n_tx = n_failed = n_loss_events = 0;
    last_lost = 0;
    gs_init(&G, &p, &o, T);
}

/* A satellite frame of `type` with sequence `seq`, delivered to the ground. */
static void sat_frame(uint8_t type, uint16_t seq, const uint8_t *payload, uint8_t len)
{
    uint8_t f[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(f, sizeof(f), type, seq, payload, len);
    gs_on_rx(&G, T, f, (size_t)n, -50, 7);
}

int main(void)
{
    TEST_GROUP("gs: malformed commands never reach the queue");
    {
        reset();
        CHECK_EQ_INT(gs_submit(&G, T, 0x99, NULL, 0), 0);              /* unknown */
        uint8_t one = 1;
        CHECK_EQ_INT(gs_submit(&G, T, GAMA_TC_PING, &one, 1), 0);      /* PING takes none */
        CHECK_EQ_INT(gs_submit(&G, T, GAMA_TC_SET_RATE, NULL, 0), 0);  /* SET_RATE takes one */
        CHECK_EQ_INT(G.q_count, 0);
        CHECK_EQ_INT(n_tx, 0);
    }

    TEST_GROUP("gs: stop-and-wait, and a full queue is refused");
    {
        reset();
        CHECK(gs_submit(&G, T, GAMA_TC_PING, NULL, 0) > 0);
        CHECK_EQ_INT(n_tx, 1);                     /* first goes at once */
        for (int i = 1; i < (int)GS_TC_QUEUE; i++) {
            CHECK(gs_submit(&G, T, GAMA_TC_REQ_HK, NULL, 0) > 0);
        }
        CHECK_EQ_INT(n_tx, 1);                     /* the rest wait their turn */
        CHECK_EQ_INT(gs_submit(&G, T, GAMA_TC_PING, NULL, 0), 0);
    }

    TEST_GROUP("gs: an ACK for something else is counted and ignored");
    {
        reset();
        gs_submit(&G, T, GAMA_TC_PING, NULL, 0);
        gs_on_tx_done(&G, T);
        uint8_t ack[4] = { GAMA_TC_PING, GAMA_ACK_OK, 0, 0 };
        gama_put_u16(ack + 2, 999);                /* not our sequence */
        sat_frame(GAMA_FRAME_TC_ACK, 1, ack, 4);
        CHECK_EQ_INT(G.n.acks_unmatched, 1);
        CHECK(G.awaiting_ack);                     /* still waiting for ours */
        CHECK_EQ_INT(G.q_count, 1);
    }

    TEST_GROUP("gs: downlink gaps are losses; a restart is not");
    {
        reset();
        sat_frame(GAMA_FRAME_TM_HK, 10, NULL, 0);
        sat_frame(GAMA_FRAME_TM_HK, 11, NULL, 0);
        sat_frame(GAMA_FRAME_TM_HK, 15, NULL, 0);  /* 12, 13, 14 missing */
        CHECK_EQ_INT(G.n.dl_lost, 3);
        CHECK_EQ_INT(n_loss_events, 1);
        CHECK_EQ_INT(last_lost, 3);
        sat_frame(GAMA_FRAME_TM_HK, 0, NULL, 0);   /* satellite rebooted */
        CHECK_EQ_INT(G.n.dl_lost, 3);
        CHECK_EQ_INT(G.n.dl_resyncs, 1);
        sat_frame(GAMA_FRAME_TM_HK, 65535, NULL, 0);
        sat_frame(GAMA_FRAME_TM_HK, 0, NULL, 0);   /* wrap is continuous */
        CHECK_EQ_INT(G.n.dl_lost, 3);
    }

    TEST_GROUP("gs: frames a satellite never sends are ignored");
    {
        reset();
        uint8_t ping = GAMA_TC_PING;
        sat_frame(GAMA_FRAME_TC, 1, &ping, 1);     /* another ground station */
        CHECK_EQ_INT(G.n.dl_frames, 0);
        CHECK(!G.in_contact);
    }

    TEST_GROUP("gs: an idle link gets a keepalive, sent only in a reply gap");
    {
        reset();
        T += 31000;
        gs_on_tick(&G, T);
        CHECK_EQ_INT(G.q_count, 1);                /* queued ... */
        CHECK_EQ_INT(n_tx, 0);                     /* ... but not sent at random */
        sat_frame(GAMA_FRAME_TM_HK, 1, NULL, 0);   /* a frame ends: gap open */
        CHECK_EQ_INT(n_tx, 1);
        CHECK_EQ_INT(last_frame[5], GAMA_TC_PING);
    }

    TEST_GROUP("gs: the operator's command displaces a waiting keepalive");
    {
        reset();
        T += 31000;
        gs_on_tick(&G, T);
        CHECK_EQ_INT(G.q_count, 1);
        CHECK(gs_submit(&G, T, GAMA_TC_REQ_HK, NULL, 0) > 0);
        CHECK_EQ_INT(G.q_count, 1);                /* keepalive withdrawn */
        CHECK_EQ_INT(n_tx, 1);                     /* operator's goes now */
        CHECK_EQ_INT(last_frame[5], GAMA_TC_REQ_HK);
    }

    TEST_GROUP("gs: a telecommand is abandoned after give-up, with an event");
    {
        reset();
        gs_submit(&G, T, GAMA_TC_PING, NULL, 0);
        for (int i = 0; i < 400 && n_failed == 0; i++) {
            if (G.tx_busy) {
                gs_on_tx_done(&G, T);
            }
            T = gs_next_deadline(&G);
            gs_on_tick(&G, T);
        }
        CHECK_EQ_INT(n_failed, 1);
        CHECK_EQ_INT(G.n.tc_failed, 1);
        CHECK(n_tx >= 10);                         /* it really tried */
    }

    TEST_SUMMARY("test_gs_link");
}
