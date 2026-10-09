/*
 * The link protocol end to end: the real satellite core and the real ground
 * link on a simulated half-duplex channel (tests/link_sim.h).
 *
 * Each scenario checks one claim of ADR-0007 and prints what it measured;
 * those printed figures are the ones the ADR quotes.
 */

#include "test_util.h"
#include "link_sim.h"

/* The longest frame the satellite sends while streaming: a full TM_TRACKS
 * frame, 7 + 12 x 20 = 247 bytes. */
#define TRACKS_FRAME 247u
/* The longest telecommand frame: 7 + 1 + 8 (SET_TIME) = 16 bytes. */
#define TC_FRAME 16u

static void print_stats(const char *label)
{
    stats_t all = exec_stats(0), first = exec_stats(1);
    int failed = 0, retried = 0;
    for (int i = 0; i < S.n_tc; i++) {
        failed += S.tc[i].failed;
        retried += S.tc[i].done && S.tc[i].attempts > 1;
    }
    printf("  %-34s TCs %4d | exec ms p50 %5llu p95 %5llu max %5llu"
           " | 1st-try max %5llu | retried %3d failed %d | collisions %u\n",
           label, S.n_tc,
           (unsigned long long)all.p50, (unsigned long long)all.p95,
           (unsigned long long)all.max, (unsigned long long)first.max,
           retried, failed, S.collisions);
}

/* An operator sending telecommands one at a time: wait a random interval,
 * send, wait for the answer (or for give-up), repeat. Latency is measured
 * from submission, so it includes waiting for the channel but not queueing
 * behind the operator's own previous command. */
static void operator_pings(int n, uint32_t gap_lo_ms, uint32_t gap_hi_ms)
{
    for (int i = 0; i < n; i++) {
        run_until(S.now + rnd_range(gap_lo_ms, gap_hi_ms));
        int k = submit(GAMA_TC_PING, NULL, 0);
        if (k >= 0) {
            run_until_done(k, 70000);
        }
    }
    run_until(S.now + 5000);
}

static int count_failed(void)
{
    int n = 0;
    for (int i = 0; i < S.n_tc; i++) { n += S.tc[i].failed; }
    return n;
}

static int count_unacked(void)
{
    int n = 0;
    for (int i = 0; i < S.n_tc; i++) { n += !S.tc[i].done; }
    return n;
}

int main(void)
{
    printf("scenario results (virtual time; NOMINAL unless stated)\n");

    TEST_GROUP("sim: idle link, clean channel");
    {
        sim_reset(1);
        operator_pings(400, 500, 12000);
        print_stats("idle, clean");
        CHECK_EQ_INT(count_failed(), 0);
        CHECK_EQ_INT(count_unacked(), 0);
        /* First-try bound: wait out the longest frame sent while idle
         * (TM_STAT, 33 B), then the telecommand's own time on air. */
        uint64_t bound = toa_ms(GAMA_RATE_NOMINAL, 33) + toa_ms(GAMA_RATE_NOMINAL, TC_FRAME);
        CHECK(exec_stats(1).max <= bound);
        CHECK_EQ_INT(S.gs.n.dl_lost, detectable_downlink_losses());
    }

    TEST_GROUP("sim: streaming 20 aircraft, clean channel");
    {
        sim_reset(2);
        start_streaming();
        uint64_t t0 = S.now;
        operator_pings(400, 500, 12000);
        print_stats("streaming, clean");
        CHECK_EQ_INT(count_failed(), 0);
        CHECK_EQ_INT(count_unacked(), 0);
        /* HLR-COMM-01 bound (ADR-0007). B0: wait out the longest frame the
         * satellite sends, then the telecommand's own time on air. B1: one
         * frame lost, so add one retry cycle — ACK timeout, first backoff, and
         * again a longest frame and a telecommand. */
        uint64_t b0 = toa_ms(GAMA_RATE_NOMINAL, TRACKS_FRAME)
                      + toa_ms(GAMA_RATE_NOMINAL, TC_FRAME);
        uint64_t b1 = b0 + gs_ack_timeout_ms(GAMA_RATE_NOMINAL) + GS_BACKOFF_FIRST_MS + b0;
        printf("    HLR-COMM-01 bound: %llu ms without loss, %llu ms with one loss\n",
               (unsigned long long)b0, (unsigned long long)b1);
        CHECK(exec_stats(1).max <= b0);
        uint64_t worst_two = 0;
        for (int k = 0; k < S.n_tc; k++) {
            if (S.tc[k].executed && S.tc[k].attempts <= 2) {
                uint64_t d = S.tc[k].executed - S.tc[k].submitted;
                if (d > worst_two) { worst_two = d; }
            }
        }
        CHECK(worst_two <= b1);
        /* The ground's loss count from sequence gaps is exact. */
        CHECK_EQ_INT(S.gs.n.dl_lost, detectable_downlink_losses());
        /* Snapshots kept their 5 s cadence: two frames each. */
        uint64_t periods = (S.now - t0) / 5000u;
        printf("    snapshots: %u tracks frames for %llu periods; downlink lost %u of %u\n",
               S.rx_tracks, (unsigned long long)periods, S.gs.n.dl_lost, S.sent[SAT]);
        CHECK(S.rx_tracks >= (uint32_t)(periods * 2u * 95u / 100u));
        uint16_t p95 = ttcd_latency_p95_ms(&S.sat);
        printf("    onboard latency p95, reception to transmission: %u ms\n", p95);
        CHECK(p95 <= 5000);                           /* HLR-ADS-08 target */
        /* Collisions only come from operator telecommands racing a frame. */
        CHECK(S.collisions <= (uint32_t)S.n_tc / 5u);
    }

    TEST_GROUP("sim: streaming, with preamble-level detection (sensitivity)");
    {
        /* If the modem reports a reception after ~5 preamble symbols instead
         * of after the whole header, the collision window shrinks by 4x. The
         * bench measures which one the SX1278 actually does (PLANO 2.7). */
        sim_reset(2);
        S.detect_symbols = 5;
        start_streaming();
        operator_pings(400, 500, 12000);
        print_stats("streaming, 5-symbol detection");
        CHECK_EQ_INT(count_failed(), 0);
        CHECK_EQ_INT(S.gs.n.dl_lost, detectable_downlink_losses());
    }

    TEST_GROUP("sim: streaming, 10 % loss in each direction");
    {
        sim_reset(3);
        S.loss_ppm[SAT] = 100000;
        S.loss_ppm[GND] = 100000;
        start_streaming();
        operator_pings(300, 1000, 12000);
        print_stats("streaming, 10% loss");
        CHECK_EQ_INT(count_failed(), 0);
        CHECK_EQ_INT(count_unacked(), 0);
        CHECK_EQ_INT(S.gs.n.dl_lost, detectable_downlink_losses());
        printf("    downlink: %u lost of %u sent (%.1f%%), all counted by the ground\n",
               S.gs.n.dl_lost, S.sent[SAT], 100.0 * S.gs.n.dl_lost / S.sent[SAT]);
    }

    TEST_GROUP("sim: keepalives alone never collide");
    {
        sim_reset(4);
        start_streaming();
        run_until(S.now + 10000);                     /* setup, then count */
        uint32_t base = S.collisions, acked = S.gs.n.tc_acked;
        run_until(S.now + 30u * 60u * 1000u);         /* 30 minutes, no operator */
        printf("  %-34s keepalives acked %u | collisions %u | contact losses %u\n",
               "streaming, keepalives only", S.gs.n.tc_acked - acked,
               S.collisions - base, S.sat.n.contact_losses);
        CHECK_EQ_INT(S.collisions - base, 0);
        CHECK_EQ_INT(S.sat.n.contact_losses, 0);
        CHECK(S.gs.n.tc_acked >= 55);                 /* one every 30 s */
    }

    TEST_GROUP("sim: rate changes, clean channel");
    {
        sim_reset(5);
        uint8_t p = GAMA_RATE_FAST;
        int i = submit(GAMA_TC_SET_RATE, &p, 1);
        run_until(S.now + 3000);
        CHECK(S.tc[i].done);
        CHECK_EQ_INT(S.profile[SAT], GAMA_RATE_FAST);
        CHECK_EQ_INT(S.profile[GND], GAMA_RATE_FAST);
        CHECK(!S.sat.rate_pending);
        CHECK(!S.gs.rate_pending);

        p = GAMA_RATE_SAFE;
        i = submit(GAMA_TC_SET_RATE, &p, 1);
        run_until(S.now + 15000);
        CHECK(S.tc[i].done);
        CHECK_EQ_INT(S.profile[SAT], GAMA_RATE_SAFE);
        CHECK_EQ_INT(S.profile[GND], GAMA_RATE_SAFE);
        CHECK(!S.sat.rate_pending);
        CHECK(!S.gs.rate_pending);

        p = GAMA_RATE_NOMINAL;
        i = submit(GAMA_TC_SET_RATE, &p, 1);
        run_until(S.now + 15000);
        CHECK(S.tc[i].done);
        CHECK_EQ_INT(S.profile[SAT], GAMA_RATE_NOMINAL);
        CHECK_EQ_INT(S.profile[GND], GAMA_RATE_NOMINAL);
        run_until(S.now + 60000);
        CHECK_EQ_INT(S.sat.n.rate_reverts, 0);
    }

    TEST_GROUP("sim: rate change whose acknowledgement is lost");
    {
        /* The satellite switches; the ground never hears it did. */
        sim_reset(6);
        S.drop_type[SAT] = GAMA_FRAME_TC_ACK;
        S.drop_count[SAT] = 1;
        uint8_t p = GAMA_RATE_FAST;
        uint64_t t0 = S.now;
        int i = submit(GAMA_TC_SET_RATE, &p, 1);
        uint64_t desync = 0, synced_at = 0;
        while (S.now < t0 + 60000) {
            uint64_t before = S.now;
            run_until(S.now + 100);
            if (S.profile[SAT] != S.profile[GND]) { desync += S.now - before; }
            else if (synced_at == 0 && S.tc[i].done && !S.sat.rate_pending &&
                     !S.gs.rate_pending && S.profile[SAT] == GAMA_RATE_FAST) {
                synced_at = S.now;
            }
        }
        printf("  %-34s both on FAST after %llu ms | desynchronised %llu ms"
               " | satellite reverts %u\n", "rate change, ACK lost",
               (unsigned long long)(synced_at - t0), (unsigned long long)desync,
               S.sat.n.rate_reverts);
        CHECK(S.tc[i].done && !S.tc[i].failed);
        CHECK_EQ_INT(S.profile[SAT], GAMA_RATE_FAST);
        CHECK_EQ_INT(S.profile[GND], GAMA_RATE_FAST);
        CHECK_EQ_INT(S.sat.n.rate_reverts, 1);
        /* Bounded by the satellite's revert timer plus one retry cycle. */
        CHECK(desync <= S.sat.p.rate_revert + 3000u);
    }

    TEST_GROUP("sim: rate change whose confirmation never gets through");
    {
        /* The ACK arrives and both switch, but nothing from the ground is
         * heard at the new profile: both must fall back, on their own. */
        sim_reset(7);
        uint8_t p = GAMA_RATE_FAST;
        int i = submit(GAMA_TC_SET_RATE, &p, 1);
        CHECK(run_until_done(i, 10000));             /* ACK delivered */
        S.blackout_until[GND] = S.now + 35000;       /* uplink dead for 35 s */
        uint64_t t0 = S.now, desync = 0;
        while (S.now < t0 + 45000) {
            uint64_t before = S.now;
            run_until(S.now + 100);
            if (S.profile[SAT] != S.profile[GND]) { desync += S.now - before; }
        }
        printf("  %-34s both back on NOMINAL | desynchronised %llu ms\n",
               "rate change, confirmation lost", (unsigned long long)desync);
        CHECK_EQ_INT(S.profile[SAT], GAMA_RATE_NOMINAL);
        CHECK_EQ_INT(S.profile[GND], GAMA_RATE_NOMINAL);
        /* Between the satellite's revert (20 s) and the ground's (30 s). */
        CHECK(desync <= S.gs.p.rate_revert - S.sat.p.rate_revert + 1000u);
        int j = submit(GAMA_TC_PING, NULL, 0);        /* and the link works */
        run_until(S.now + 3000);
        CHECK(S.tc[j].done);
    }

    TEST_GROUP("sim: 200 s uplink blackout while streaming");
    {
        sim_reset(8);
        start_streaming();
        run_until(S.now + 20000);
        uint64_t t0 = S.now;
        S.blackout_until[GND] = t0 + 200000;
        uint32_t tracks_before = S.rx_tracks;
        run_until(t0 + 200000);
        CHECK_EQ_INT(S.sat.n.contact_losses, 1);      /* the satellite fell to SAFE */
        uint64_t recovered = 0;
        while (S.now < t0 + 400000) {
            run_until(S.now + 500);
            if (!recovered && S.profile[SAT] == GAMA_RATE_NOMINAL &&
                S.profile[GND] == GAMA_RATE_NOMINAL && !S.sat.rate_pending &&
                !S.gs.rate_pending) {
                recovered = S.now;
            }
        }
        uint32_t tracks_after = S.rx_tracks;
        run_until(S.now + 30000);
        printf("  %-34s back on NOMINAL %llu s after the uplink returned;"
               " tracks resumed: %s\n", "200 s uplink blackout",
               (unsigned long long)((recovered - (t0 + 200000)) / 1000u),
               S.rx_tracks > tracks_after ? "yes" : "no");
        CHECK(recovered > 0);
        CHECK(S.rx_tracks > tracks_after);
        CHECK(tracks_after >= tracks_before);
        CHECK(S.sat.stream_on);
    }

    TEST_GROUP("sim: 300 s blackout in both directions");
    {
        sim_reset(9);
        run_until(S.now + 5000);
        uint64_t t0 = S.now;
        S.blackout_until[SAT] = t0 + 300000;
        S.blackout_until[GND] = t0 + 300000;
        run_until(t0 + 300000);
        CHECK_EQ_INT(S.profile[SAT], GAMA_RATE_SAFE);
        CHECK_EQ_INT(S.profile[GND], GAMA_RATE_SAFE);
        uint64_t recovered = 0;
        while (S.now < t0 + 500000 && !recovered) {
            run_until(S.now + 500);
            if (S.profile[SAT] == GAMA_RATE_NOMINAL && S.profile[GND] == GAMA_RATE_NOMINAL &&
                !S.sat.rate_pending && !S.gs.rate_pending) {
                recovered = S.now;
            }
        }
        printf("  %-34s both fell to SAFE; back on NOMINAL %llu s after the blackout\n",
               "300 s total blackout",
               (unsigned long long)(recovered ? (recovered - (t0 + 300000)) / 1000u : 0));
        CHECK(recovered > 0);
        /* No faster than the SAFE beacon period, no slower than two of them. */
        CHECK(recovered - (t0 + 300000) <= 2u * S.sat.p.safe_beacon_period + 20000u);
    }

    TEST_GROUP("sim: SET_MODE reaches the OBC, and only once per command");
    {
        sim_reset(10);
        ttcd_on_ipc_peer(&S.sat, S.now, GAMA_IPC_ROLE_OBC, true);
        start_streaming();
        uint8_t ev = GAMA_OBC_EV_TC_MISSION_ADSB;
        int first = S.n_tc;
        for (int k = 0; k < 100; k++) {
            run_until(S.now + rnd_range(1000, 10000));
            submit(GAMA_TC_SET_MODE, &ev, 1);
        }
        run_until(S.now + 70000);
        int modes = S.n_tc - first;
        CHECK_EQ_INT(S.obc_events, modes);             /* exactly one each */
        uint64_t worst = 0;
        for (int k = first, e = 0; k < S.n_tc && e < S.obc_events; k++, e++) {
            uint64_t d = S.obc_event_at[e] - S.tc[k].submitted;
            if (d > worst) { worst = d; }
        }
        printf("  %-34s %d commands, %d OBC events, worst submit->OBC %llu ms\n",
               "SET_MODE to the OBC", modes, S.obc_events, (unsigned long long)worst);

        /* The ACK of one command is lost: the ground retries, the satellite
         * replays the ACK, and the OBC still sees a single event. */
        int before = S.obc_events;
        S.drop_type[SAT] = GAMA_FRAME_TC_ACK;
        S.drop_count[SAT] = 1;
        int k = submit(GAMA_TC_SET_MODE, &ev, 1);
        run_until(S.now + 10000);
        CHECK(S.tc[k].done);
        CHECK(S.tc[k].attempts >= 2);
        CHECK_EQ_INT(S.obc_events, before + 1);
        CHECK(S.sat.n.tc_duplicates >= 1);
    }

    TEST_GROUP("sim: SET_TIME transfers the ground's clock, even after a retry");
    {
        sim_reset(11);
        S.drop_type[SAT] = GAMA_FRAME_TC_ACK;   /* force a retry of SET_TIME */
        S.drop_count[SAT] = 1;
        uint8_t args[8] = { 0 };                /* re-stamped at each attempt */
        int k = submit(GAMA_TC_SET_TIME, args, 8);
        run_until(S.now + 10000);
        CHECK(S.tc[k].done);
        CHECK(S.tc[k].attempts >= 2);
        CHECK(S.sat.time_synced);
        /* The anchor pairs the satellite's clock with the ground's wall time
         * at the moment the (retried) frame started: both clocks agree. */
        uint64_t anchor_wall_ms = (uint64_t)(S.sat.time_anchor_unix_s - SIM_WALL_BASE) * 1000u
                                  + S.sat.time_anchor_nsec / 1000000u;
        int64_t err = (int64_t)anchor_wall_ms - (int64_t)S.sat.time_anchor_mono;
        printf("  %-34s anchor error %lld ms after %u attempts\n",
               "SET_TIME with a lost ACK", (long long)err, S.tc[k].attempts);
        CHECK(err >= -2 && err <= 2);
        CHECK_EQ_INT(S.sat.n.tc_duplicates, 0);         /* retry was re-stamped */
    }

    TEST_SUMMARY("test_link_sim");
}
