/*
 * The ttcd core, one behaviour at a time, on a virtual clock.
 *
 * Every protocol property ADR-0007 claims for the satellite side is pinned
 * here: the acknowledgement discipline, the reply gap, listen-before-talk,
 * the rate change with its revert, the fall-back to SAFE, and the track age
 * rewrite that carries the HLR-ADS-08 latency. The two-sided behaviour —
 * satellite and ground together on a lossy half-duplex channel — is in
 * test_link_sim.c.
 */

#include "test_util.h"
#include "ttcd_harness.h"
#include "gama_ipc.h"
#include "gama_tm.h"

/* The reply gap, recomputed from its definition in ADR-0007 rather than read
 * from the core, so the test checks the rule and not just the code. */
static uint64_t core_gap_for_test(const ttcd_core_t *c)
{
    const gama_lora_phy_t *phy = gama_lora_profile(c->profile);
    uint32_t us = gama_lora_header_us(phy) + 2u * gama_lora_symbol_us(phy);
    return ((uint64_t)us + 999u) / 1000u + c->p.reaction_margin;
}

static ttcd_core_t C;
static ttcd_params_t P;

static void boot(void)
{
    ttcd_params_default(&P);
    h_reset(&C, &P);
}

/* IPC frame helpers ------------------------------------------------------- */

static void ipc_from(uint8_t role, uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[GAMA_FRAME_MAX_TOTAL];
    int n = gama_frame_encode(frame, sizeof(frame), type, 0, payload, len);
    ttcd_on_ipc_frame(&C, H.now, role, frame, (size_t)n);
}

/* Sends a snapshot of `n` aircraft to the core as adsbd would: split into
 * frames of twelve, all with the same epoch, each record `age_ds` old at the
 * epoch. ICAOs are 0x100000 + i so they can be recognised on the air. */
static void send_snapshot(uint32_t epoch, int n, uint16_t age_ds)
{
    uint8_t count = (uint8_t)((n + 11) / 12);
    if (count == 0) { count = 1; }
    for (uint8_t f = 0; f < count; f++) {
        uint8_t payload[GAMA_FRAME_MAX_PAYLOAD];
        gama_ipc_tracks_hdr_t h = { .epoch_ms = epoch, .index = f, .count = count };
        gama_ipc_tracks_header_encode(payload, sizeof(payload), &h);
        int in_frame = n - f * 12 > 12 ? 12 : n - f * 12;
        for (int r = 0; r < in_frame; r++) {
            gama_track_t t = { .icao = 0x100000u + (uint32_t)(f * 12 + r),
                               .latitude = -23.0, .longitude = -46.0,
                               .altitude_ft = 30000, .age_ds = age_ds,
                               .flags = GAMA_TRACK_F_POSITION };
            gama_track_encode(payload + GAMA_IPC_TRACKS_HEADER_LEN
                                  + (size_t)r * GAMA_TRACK_WIRE_LEN,
                              GAMA_TRACK_WIRE_LEN, &t);
        }
        ipc_from(GAMA_IPC_ROLE_ADSBD, GAMA_FRAME_IPC_TRACKS, payload,
                 (uint8_t)(GAMA_IPC_TRACKS_HEADER_LEN
                           + (unsigned)in_frame * GAMA_TRACK_WIRE_LEN));
    }
}

static void start_stream(uint16_t seq, uint16_t period_ds)
{
    uint8_t a[2];
    gama_put_u16(a, period_ds);
    h_send_tc(&C, seq, GAMA_TC_STREAM_START, a, 2);
}

int main(void)
{
    TEST_GROUP("core: boot configures the radio and announces itself");
    {
        boot();
        CHECK_EQ_INT(H.profile, GAMA_RATE_NOMINAL);
        CHECK_EQ_INT(H.power, 20);
        CHECK(step_to(&C, H.now + 1));
        CHECK_EQ_INT(H.n_tx, 1);
        CHECK_EQ_INT(tx_type(0), GAMA_FRAME_TM_HK);
        CHECK_EQ_INT(count_log("boot"), 1);
    }

    TEST_GROUP("core: housekeeping every 10 s, and nothing between");
    {
        boot();
        CHECK(step_to(&C, H.now + 35000));
        CHECK_EQ_INT(H.n_tx, 4);                /* t = 0, 10, 20, 30 s */
        for (int i = 0; i < H.n_tx; i++) {
            CHECK_EQ_INT(tx_type(i), GAMA_FRAME_TM_HK);
        }
        CHECK_EQ_INT(H.tx_at[1] - H.tx_at[0], 10000);
    }

    TEST_GROUP("core: HK reports unknown for data it never received");
    {
        boot();
        CHECK(step_to(&C, H.now + 1));
        gama_frame_t f; gama_hk_t hk;
        gama_frame_decode(H.tx[0], H.tx_len[0], &f);
        gama_hk_decode(f.payload, f.len, &hk);
        CHECK_EQ_INT(hk.battery_mv, 0);
        CHECK_EQ_INT(hk.current_ma, TTCD_HK_UNKNOWN);
        CHECK_EQ_INT(hk.roll_cdeg, TTCD_HK_UNKNOWN);
        CHECK_EQ_INT(hk.obc_mode, GAMA_OBC_ST_UNKNOWN);
        CHECK_EQ_INT(hk.link_state, GAMA_LINK_IDLE);
        CHECK_EQ_INT(hk.temp_soc_ccel, 4200);
        CHECK_EQ_INT(hk.flags, 0);
    }

    TEST_GROUP("core: HK carries what the OBC reported");
    {
        boot();
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_OBC, true);
        gama_ipc_telemetry_t t = { .battery_mv = 8100, .current_ma = -300,
                                   .temp_bat_ccel = 1200, .temp_ext_ccel = 2500,
                                   .roll_cdeg = 10, .pitch_cdeg = -20, .yaw_cdeg = 9000 };
        uint8_t p[GAMA_IPC_TELEMETRY_LEN];
        gama_ipc_telemetry_encode(p, sizeof(p), &t);
        ipc_from(GAMA_IPC_ROLE_OBC, GAMA_FRAME_IPC_TELEMETRY, p, sizeof(p));
        uint8_t mode = GAMA_OBC_ST_MISSION_ADSB;
        ipc_from(GAMA_IPC_ROLE_OBC, GAMA_FRAME_IPC_MODE, &mode, 1);
        /* The boot HK already left, before the mode arrived; ask for one that
         * reflects both reports. */
        int from_hk = H.n_tx;
        h_send_tc(&C, 1, GAMA_TC_REQ_HK, NULL, 0);
        CHECK(step_to(&C, H.now + 3000));
        int hk_at = find_tx(from_hk, GAMA_FRAME_TM_HK);
        CHECK(hk_at >= 0);
        gama_frame_t f; gama_hk_t hk;
        gama_frame_decode(H.tx[hk_at], H.tx_len[hk_at], &f);
        gama_hk_decode(f.payload, f.len, &hk);
        CHECK_EQ_INT(hk.battery_mv, 8100);
        CHECK_EQ_INT(hk.current_ma, -300);
        CHECK_EQ_INT(hk.yaw_cdeg, 9000);
        CHECK_EQ_INT(hk.obc_mode, GAMA_OBC_ST_MISSION_ADSB);
        CHECK(hk.flags & GAMA_HK_F_OBC_LINKED);
        /* Battery temperature is logged onboard even though HK lacks it. */
        CHECK_EQ_INT(count_log("obc_telemetry"), 1);

        /* When the OBC goes away, its values stop being reported as current. */
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_OBC, false);
        h_send_tc(&C, 2, GAMA_TC_REQ_HK, NULL, 0);
        CHECK(step_to(&C, H.now + 3000));
        int hk_i = find_tx(0, GAMA_FRAME_TM_HK);
        while (find_tx(hk_i + 1, GAMA_FRAME_TM_HK) >= 0) { hk_i = find_tx(hk_i + 1, GAMA_FRAME_TM_HK); }
        gama_frame_decode(H.tx[hk_i], H.tx_len[hk_i], &f);
        gama_hk_decode(f.payload, f.len, &hk);
        CHECK_EQ_INT(hk.battery_mv, 0);
        CHECK_EQ_INT(hk.obc_mode, GAMA_OBC_ST_UNKNOWN);
    }

    TEST_GROUP("core: a telecommand is acknowledged ahead of anything due");
    {
        boot();
        CHECK(step_to(&C, H.now + 5000));
        int before = H.n_tx;
        h_send_tc(&C, 77, GAMA_TC_PING, NULL, 0);
        CHECK_EQ_INT(H.n_tx, before + 1);      /* immediately, no gap wait */
        uint8_t cmd, st; uint16_t echo;
        ack_of(last_tx(), &cmd, &st, &echo);
        CHECK_EQ_INT(tx_type(last_tx()), GAMA_FRAME_TC_ACK);
        CHECK_EQ_INT(cmd, GAMA_TC_PING);
        CHECK_EQ_INT(st, GAMA_ACK_OK);
        CHECK_EQ_INT(echo, 77);
    }

    TEST_GROUP("core: every rejection path still gets an acknowledgement");
    {
        boot();
        CHECK(step_to(&C, H.now + 1000));
        uint8_t cmd, st; uint16_t echo;

        h_send_tc(&C, 1, 0x99, NULL, 0);                     /* unknown */
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(0, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_UNKNOWN_CMD);

        uint8_t junk = 1;
        int from = H.n_tx;
        h_send_tc(&C, 2, GAMA_TC_PING, &junk, 1);            /* PING takes none */
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_BAD_ARGS);

        uint8_t prof = 7;
        from = H.n_tx;
        h_send_tc(&C, 3, GAMA_TC_SET_RATE, &prof, 1);        /* no profile 7 */
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_BAD_ARGS);
        CHECK_EQ_INT(H.profile, GAMA_RATE_NOMINAL);

        uint8_t dbm = 25;
        from = H.n_tx;
        h_send_tc(&C, 4, GAMA_TC_SET_TX_POWER, &dbm, 1);
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_BAD_ARGS);
        CHECK_EQ_INT(H.power, 20);

        uint8_t magic[2] = { 0x12, 0x34 };
        from = H.n_tx;
        h_send_tc(&C, 5, GAMA_TC_SHUTDOWN, magic, 2);
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_BAD_ARGS);
        CHECK_EQ_INT(H.exit_code, 0);
    }

    TEST_GROUP("core: a frame with no command byte is counted, not answered");
    {
        boot();
        uint8_t frame[GAMA_FRAME_MAX_TOTAL];
        int n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TC, 9, NULL, 0);
        uint32_t bad = C.n.frames_rx_bad;
        ttcd_on_radio_rx(&C, H.now, frame, (size_t)n, -40, 9);
        CHECK_EQ_INT(C.n.frames_rx_bad, bad + 1);
        CHECK_EQ_INT(find_tx(0, GAMA_FRAME_TC_ACK), -1);
    }

    TEST_GROUP("core: corrupted and foreign frames are counted, never obeyed");
    {
        boot();
        uint8_t frame[GAMA_FRAME_MAX_TOTAL];
        uint8_t ping = GAMA_TC_PING;
        int n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TC, 1, &ping, 1);
        frame[5] ^= 0xFF;                               /* break the CRC */
        ttcd_on_radio_rx(&C, H.now, frame, (size_t)n, -40, 9);
        CHECK_EQ_INT(C.n.frames_rx_bad, 1);

        ttcd_on_radio_rx_error(&C, H.now);              /* PHY CRC */
        CHECK_EQ_INT(C.n.frames_rx_bad, 2);

        /* A valid TM_HK from someone else on the channel. */
        uint64_t contact = C.last_contact;
        n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_TM_HK, 1, NULL, 0);
        H.now += 5000;
        ttcd_on_radio_rx(&C, H.now, frame, (size_t)n, -40, 9);
        CHECK_EQ_INT(C.n.frames_foreign, 1);
        CHECK_EQ_INT(C.last_contact, contact);          /* not contact */
        CHECK_EQ_INT(find_tx(0, GAMA_FRAME_TC_ACK), -1);
    }

    TEST_GROUP("core: a retransmitted telecommand is acknowledged, not re-executed");
    {
        boot();
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_OBC, true);
        uint8_t ev = GAMA_OBC_EV_TC_MISSION_ADSB;
        h_send_tc(&C, 40, GAMA_TC_SET_MODE, &ev, 1);
        CHECK(step_to(&C, H.now + 2000));
        CHECK_EQ_INT(H.n_ipc, 1);                        /* one event to the OBC */

        h_send_tc(&C, 40, GAMA_TC_SET_MODE, &ev, 1);     /* the ACK was lost */
        CHECK(step_to(&C, H.now + 2000));
        CHECK_EQ_INT(H.n_ipc, 1);                        /* still one */
        CHECK_EQ_INT(C.n.tc_duplicates, 1);
        int acks = 0;
        for (int i = 0; i < H.n_tx; i++) { acks += tx_type(i) == GAMA_FRAME_TC_ACK; }
        CHECK_EQ_INT(acks, 2);                           /* but answered twice */

        h_send_tc(&C, 41, GAMA_TC_SET_MODE, &ev, 1);     /* a new command is new */
        CHECK(step_to(&C, H.now + 2000));
        CHECK_EQ_INT(H.n_ipc, 2);
    }

    TEST_GROUP("core: SET_MODE reaches the OBC as an IPC event, or fails visibly");
    {
        boot();
        uint8_t cmd, st; uint16_t echo;
        uint8_t ev = GAMA_OBC_EV_TC_MISSION_ADSB;
        h_send_tc(&C, 1, GAMA_TC_SET_MODE, &ev, 1);      /* OBC not connected */
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(0, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_FAILED);
        CHECK_EQ_INT(H.n_ipc, 0);

        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_OBC, true);
        int from = H.n_tx;
        h_send_tc(&C, 2, GAMA_TC_SET_MODE, &ev, 1);
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_OK);
        CHECK_EQ_INT(H.n_ipc, 1);
        CHECK_EQ_INT(H.ipc_role[0], GAMA_IPC_ROLE_OBC);
        gama_frame_t f;
        CHECK(gama_frame_decode(H.ipc[0], H.ipc_len[0], &f) > 0);
        CHECK_EQ_INT(f.type, GAMA_FRAME_IPC_TC_EVENT);
        CHECK_EQ_INT(f.payload[0], GAMA_OBC_EV_TC_MISSION_ADSB);

        uint8_t none = GAMA_OBC_EV_NONE;
        from = H.n_tx;
        h_send_tc(&C, 3, GAMA_TC_SET_MODE, &none, 1);
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_BAD_ARGS);

        H.ipc_fail = true;                               /* OBC queue full */
        from = H.n_tx;
        h_send_tc(&C, 4, GAMA_TC_SET_MODE, &ev, 1);
        CHECK(step_to(&C, H.now + 1000));
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_FAILED);
        CHECK_EQ_INT(C.n.ipc_tx_drops, 1);
    }

    TEST_GROUP("core: after its own frame the satellite leaves a reply gap");
    {
        boot();
        CHECK(step_to(&C, H.now + 1));                 /* HK goes out */
        uint64_t end = H.tx_done_at;
        CHECK(step_to(&C, end));                        /* ... and ends */
        /* Something is due right away, but must wait for the gap. */
        C.hk_requested = true;
        CHECK(step_to(&C, end + core_gap_for_test(&C) - 1));
        CHECK_EQ_INT(H.n_tx, 1);
        CHECK(step_to(&C, end + core_gap_for_test(&C)));
        CHECK_EQ_INT(H.n_tx, 2);
    }

    TEST_GROUP("core: a telecommand inside the gap is answered at once");
    {
        boot();
        CHECK(step_to(&C, H.now + 1));
        CHECK(step_to(&C, H.tx_done_at));              /* gap starts now */
        int before = H.n_tx;
        h_send_tc(&C, 5, GAMA_TC_PING, NULL, 0);
        CHECK_EQ_INT(H.n_tx, before + 1);
        CHECK_EQ_INT(tx_type(last_tx()), GAMA_FRAME_TC_ACK);
    }

    TEST_GROUP("core: listen before talk defers, then retries");
    {
        boot();
        H.busy_next = 2;
        CHECK(step_to(&C, H.now + 1000));
        CHECK_EQ_INT(C.n.lbt_defers, 2);
        CHECK_EQ_INT(H.n_tx, 1);                        /* HK went out at last */
        /* Each retry waited a header time (83 ms at NOMINAL). */
        CHECK(H.tx_at[0] >= 1000000u + 2u * 83u);
    }

    TEST_GROUP("core: a lost end-of-transmission does not silence the link");
    {
        boot();
        H.never_finish = true;
        CHECK(step_to(&C, H.now + 3000));
        CHECK_EQ_INT(C.n.tx_watchdogs, 1);
        CHECK(!C.tx_busy);
        CHECK(step_to(&C, H.now + 12000));
        CHECK(H.n_tx >= 2);                             /* HK resumed */
    }

    TEST_GROUP("core: a rate change is acknowledged at the old rate, then applied");
    {
        boot();
        CHECK(step_to(&C, H.now + 2000));
        uint8_t fast = GAMA_RATE_FAST;
        int from = H.n_tx;
        h_send_tc(&C, 10, GAMA_TC_SET_RATE, &fast, 1);
        int ack = find_tx(from, GAMA_FRAME_TC_ACK);
        CHECK(ack >= 0);
        CHECK_EQ_INT(H.tx_profile[ack], GAMA_RATE_NOMINAL);   /* old rate */
        CHECK_EQ_INT(H.profile, GAMA_RATE_NOMINAL);           /* not yet */
        CHECK(step_to(&C, H.tx_done_at));
        CHECK_EQ_INT(H.profile, GAMA_RATE_FAST);              /* now */
        CHECK(C.rate_pending);

        /* Heard at the new rate: confirmed, no revert later. */
        h_send_tc(&C, 11, GAMA_TC_PING, NULL, 0);
        CHECK(!C.rate_pending);
        CHECK(step_to(&C, H.now + 60000));
        CHECK_EQ_INT(H.profile, GAMA_RATE_FAST);
        CHECK_EQ_INT(C.n.rate_reverts, 0);
    }

    TEST_GROUP("core: an unconfirmed rate change reverts on its own");
    {
        boot();
        uint8_t fast = GAMA_RATE_FAST;
        h_send_tc(&C, 10, GAMA_TC_SET_RATE, &fast, 1);
        CHECK(step_to(&C, H.tx_done_at));
        uint64_t switched = H.now;
        CHECK_EQ_INT(H.profile, GAMA_RATE_FAST);
        CHECK(step_to(&C, switched + P.rate_revert - 1));
        CHECK_EQ_INT(H.profile, GAMA_RATE_FAST);
        CHECK(step_to(&C, switched + P.rate_revert + 2000));
        CHECK_EQ_INT(H.profile, GAMA_RATE_NOMINAL);
        CHECK_EQ_INT(C.n.rate_reverts, 1);
    }

    TEST_GROUP("core: after a revert, the ground's retry of SET_RATE is obeyed");
    {
        /* The case that breaks if SET_RATE is treated as a duplicate: the
         * satellite switched, the ground never heard the ACK, the satellite
         * reverted, and now the ground retransmits the same frame. */
        boot();
        uint8_t fast = GAMA_RATE_FAST;
        h_send_tc(&C, 20, GAMA_TC_SET_RATE, &fast, 1);
        CHECK(step_to(&C, H.tx_done_at));
        CHECK(step_to(&C, H.now + P.rate_revert + 2000));
        CHECK_EQ_INT(H.profile, GAMA_RATE_NOMINAL);      /* reverted */

        h_send_tc(&C, 20, GAMA_TC_SET_RATE, &fast, 1);   /* same seq, same bytes */
        CHECK(step_to(&C, H.tx_done_at));
        CHECK_EQ_INT(H.profile, GAMA_RATE_FAST);          /* switched again */
        CHECK_EQ_INT(C.n.tc_duplicates, 0);
    }

    TEST_GROUP("core: silence from the ground drops the link to SAFE");
    {
        boot();
        CHECK(step_to(&C, H.now + P.contact_timeout - 1000));
        CHECK_EQ_INT(H.profile, GAMA_RATE_NOMINAL);
        CHECK(step_to(&C, H.now + 5000));
        CHECK_EQ_INT(H.profile, GAMA_RATE_SAFE);
        CHECK_EQ_INT(C.n.contact_losses, 1);
        CHECK_EQ_INT(ttcd_link_mode(&C), GAMA_LINK_SAFE);

        /* In SAFE: a beacon every 30 s, housekeeping every 60 s, at SF12. */
        int from = H.n_tx;
        CHECK(step_to(&C, H.now + 125000));
        int beacons = 0, hks = 0;
        for (int i = from; i < H.n_tx; i++) {
            beacons += tx_type(i) == GAMA_FRAME_BEACON;
            hks += tx_type(i) == GAMA_FRAME_TM_HK;
            CHECK_EQ_INT(H.tx_profile[i], GAMA_RATE_SAFE);
        }
        CHECK(beacons >= 4 && beacons <= 5);
        CHECK(hks >= 2 && hks <= 3);
        /* Stays in SAFE: no second contact loss while already there. */
        CHECK_EQ_INT(C.n.contact_losses, 1);
    }

    TEST_GROUP("core: the ground brings the link back from SAFE");
    {
        boot();
        CHECK(step_to(&C, H.now + P.contact_timeout + 5000));
        CHECK_EQ_INT(H.profile, GAMA_RATE_SAFE);
        uint8_t nominal = GAMA_RATE_NOMINAL;
        h_send_tc(&C, 30, GAMA_TC_SET_RATE, &nominal, 1);
        CHECK(step_to(&C, H.tx_done_at));
        CHECK_EQ_INT(H.profile, GAMA_RATE_NOMINAL);
        h_send_tc(&C, 31, GAMA_TC_PING, NULL, 0);
        CHECK(!C.rate_pending);
        CHECK_EQ_INT(ttcd_link_mode(&C), GAMA_LINK_IDLE);
    }

    TEST_GROUP("core: tracks are sent only as complete snapshots");
    {
        boot();
        start_stream(1, 50);
        uint32_t epoch = (uint32_t)H.now;
        send_snapshot(epoch, 20, 3);
        CHECK(step_to(&C, H.now + 4000));
        int t1 = find_tx(0, GAMA_FRAME_TM_TRACKS);
        CHECK(t1 >= 0);
        int t2 = find_tx(t1 + 1, GAMA_FRAME_TM_TRACKS);
        CHECK(t2 >= 0);
        CHECK_EQ_INT(H.tx_len[t1], GAMA_FRAME_OVERHEAD + 12 * GAMA_TRACK_WIRE_LEN);
        CHECK_EQ_INT(H.tx_len[t2], GAMA_FRAME_OVERHEAD + 8 * GAMA_TRACK_WIRE_LEN);

        /* Half a snapshot (frame 0 of 2) never goes out. */
        boot();
        start_stream(1, 50);
        uint8_t payload[GAMA_FRAME_MAX_PAYLOAD];
        gama_ipc_tracks_hdr_t h = { .epoch_ms = (uint32_t)H.now, .index = 0, .count = 2 };
        gama_ipc_tracks_header_encode(payload, sizeof(payload), &h);
        memset(payload + 6, 0, GAMA_TRACK_WIRE_LEN);
        ipc_from(GAMA_IPC_ROLE_ADSBD, GAMA_FRAME_IPC_TRACKS, payload, 6 + GAMA_TRACK_WIRE_LEN);
        CHECK(step_to(&C, H.now + 12000));
        CHECK_EQ_INT(find_tx(0, GAMA_FRAME_TM_TRACKS), -1);
    }

    TEST_GROUP("core: the age on the air is measured to the start of transmission");
    {
        boot();
        start_stream(1, 50);
        CHECK(step_to(&C, H.now + 3000));
        uint32_t epoch = (uint32_t)H.now;
        send_snapshot(epoch, 5, 7);                     /* 0.7 s old at the epoch */
        CHECK(step_to(&C, H.now + 6000));
        int t = find_tx(0, GAMA_FRAME_TM_TRACKS);
        CHECK(t >= 0);
        gama_frame_t f; gama_track_t tr;
        gama_frame_decode(H.tx[t], H.tx_len[t], &f);
        gama_track_decode(f.payload, GAMA_TRACK_WIRE_LEN, &tr);
        uint32_t since_epoch_ds = (uint32_t)((H.tx_at[t] - epoch + 50u) / 100u);
        CHECK_EQ_INT(tr.age_ds, 7 + since_epoch_ds);
        CHECK_EQ_INT(tr.icao, 0x100000u);
    }

    TEST_GROUP("core: an old snapshot is not sent twice, nor a stale one at all");
    {
        boot();
        start_stream(1, 30);
        uint32_t e1 = (uint32_t)H.now;
        send_snapshot(e1, 3, 0);
        CHECK(step_to(&C, H.now + 10000));              /* three stream ticks */
        int n = 0;
        for (int i = 0; i < H.n_tx; i++) { n += tx_type(i) == GAMA_FRAME_TM_TRACKS; }
        CHECK_EQ_INT(n, 1);

        send_snapshot(e1 - 5000, 3, 0);                 /* older than held */
        CHECK(step_to(&C, H.now + 10000));
        n = 0;
        for (int i = 0; i < H.n_tx; i++) { n += tx_type(i) == GAMA_FRAME_TM_TRACKS; }
        CHECK_EQ_INT(n, 1);
    }

    TEST_GROUP("core: latency p95 counts each update once, at its first transmission");
    {
        boot();
        CHECK_EQ_INT(ttcd_latency_p95_ms(&C), 0xFFFF);
        start_stream(1, 50);
        /* Snapshot 1: every update 0.5 s old at the epoch. */
        uint32_t e1 = (uint32_t)H.now;
        send_snapshot(e1, 10, 5);
        CHECK(step_to(&C, H.now + 5000));
        uint16_t p1 = ttcd_latency_p95_ms(&C);
        CHECK(p1 >= 500 && p1 <= 1200);
        /* Snapshot 2 repeats the same updates, now 10 s old: they went out
         * already and must not inflate the latency. */
        uint32_t e2 = e1 + 5000;
        send_snapshot(e2, 10, 55);
        CHECK(step_to(&C, H.now + 5000));
        CHECK_EQ_INT(ttcd_latency_p95_ms(&C), p1);
    }

    TEST_GROUP("core: no tracks in SAFE, where one snapshot would take 23 s");
    {
        boot();
        start_stream(1, 50);
        CHECK(step_to(&C, H.now + P.contact_timeout + 5000));
        CHECK_EQ_INT(H.profile, GAMA_RATE_SAFE);
        int from = H.n_tx;
        send_snapshot((uint32_t)H.now, 20, 0);
        CHECK(step_to(&C, H.now + 60000));
        CHECK_EQ_INT(find_tx(from, GAMA_FRAME_TM_TRACKS), -1);
        CHECK(C.stream_on);                             /* resumes after SAFE */
    }

    TEST_GROUP("core: statistics every 30 s while streaming");
    {
        boot();
        start_stream(1, 50);
        CHECK(step_to(&C, H.now + 65000));
        int n = 0;
        for (int i = 0; i < H.n_tx; i++) { n += tx_type(i) == GAMA_FRAME_TM_STAT; }
        CHECK_EQ_INT(n, 2);
    }

    TEST_GROUP("core: SET_TIME anchors at the ground's transmission start");
    {
        boot();
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_OBC, true);
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_ADSBD, true);
        gama_ipc_time_t t = { .unix_s = 1789900000u, .nsec = 250000000u };
        uint8_t args[8];
        gama_ipc_time_encode(args, sizeof(args), &t);
        h_send_tc(&C, 50, GAMA_TC_SET_TIME, args, 8);
        /* 7 + 9 = 16-byte frame at NOMINAL: 165 ms on air. */
        CHECK_EQ_INT(C.time_anchor_mono, H.now - 165);
        CHECK(C.time_synced);
        CHECK_EQ_INT(H.n_ipc, 2);                        /* OBC and adsbd */
        gama_frame_t f; gama_ipc_time_t fwd;
        gama_frame_decode(H.ipc[0], H.ipc_len[0], &f);
        gama_ipc_time_decode(f.payload, f.len, &fwd);
        /* Forwarded as the time now: 165 ms after the ground's stamp. */
        CHECK_EQ_INT(fwd.unix_s, 1789900000u);
        CHECK_EQ_INT(fwd.nsec, 415000000u);

        t.nsec = 1000000000u;
        gama_ipc_time_encode(args, sizeof(args), &t);
        int from = H.n_tx;
        h_send_tc(&C, 51, GAMA_TC_SET_TIME, args, 8);
        CHECK(step_to(&C, H.now + 1000));
        uint8_t cmd, st; uint16_t echo;
        ack_of(find_tx(from, GAMA_FRAME_TC_ACK), &cmd, &st, &echo);
        CHECK_EQ_INT(st, GAMA_ACK_BAD_ARGS);
    }

    TEST_GROUP("core: a peer that connects after SET_TIME is told the time at once");
    {
        boot();
        gama_ipc_time_t t = { .unix_s = 1789900000u, .nsec = 250000000u };
        uint8_t args[8];
        gama_ipc_time_encode(args, sizeof(args), &t);
        h_send_tc(&C, 52, GAMA_TC_SET_TIME, args, 8);
        CHECK_EQ_INT(H.n_ipc, 0);                        /* nobody to tell yet */
        CHECK(step_to(&C, H.now + 5000));
        /* adsbd restarted, say: without this it would stamp its record with
         * the unsynchronised system clock until the next SET_TIME. */
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_ADSBD, true);
        CHECK_EQ_INT(H.n_ipc, 1);
        CHECK_EQ_INT(H.ipc_role[0], GAMA_IPC_ROLE_ADSBD);
        gama_frame_t f; gama_ipc_time_t now_t;
        gama_frame_decode(H.ipc[0], H.ipc_len[0], &f);
        CHECK_EQ_INT(f.type, GAMA_FRAME_IPC_TIME_SET);
        gama_ipc_time_decode(f.payload, f.len, &now_t);
        /* 165 ms on air, then 5 s: the time now, not the time of the anchor. */
        CHECK_EQ_INT(now_t.unix_s, 1789900005u);
        CHECK_EQ_INT(now_t.nsec, 415000000u);
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_OBC, true);
        CHECK_EQ_INT(H.n_ipc, 2);
        CHECK_EQ_INT(H.ipc_role[1], GAMA_IPC_ROLE_OBC);
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_TOOL, true);    /* tools are not */
        CHECK_EQ_INT(H.n_ipc, 2);
    }

    TEST_GROUP("core: before any SET_TIME a connecting peer is told nothing");
    {
        boot();
        ttcd_on_ipc_peer(&C, H.now, GAMA_IPC_ROLE_ADSBD, true);
        CHECK_EQ_INT(H.n_ipc, 0);
    }

    TEST_GROUP("core: SHUTDOWN exits only after its acknowledgement has left");
    {
        boot();
        uint8_t magic[2];
        gama_put_u16(magic, GAMA_TC_SHUTDOWN_MAGIC);
        h_send_tc(&C, 60, GAMA_TC_SHUTDOWN, magic, 2);
        CHECK_EQ_INT(H.exit_code, 0);                     /* ACK still on air */
        CHECK(step_to(&C, H.tx_done_at));
        CHECK_EQ_INT(H.exit_code, TTCD_EXIT_SHUTDOWN);
    }

    TEST_GROUP("core: unexpected IPC traffic is counted and ignored");
    {
        boot();
        uint8_t mode = GAMA_OBC_ST_MISSION_ADSB;
        ipc_from(GAMA_IPC_ROLE_ADSBD, GAMA_FRAME_IPC_MODE, &mode, 1);   /* wrong role */
        CHECK_EQ_INT(C.n.ipc_rx_bad, 1);
        CHECK_EQ_INT(C.obc_mode, GAMA_OBC_ST_UNKNOWN);
        uint8_t junk[3] = { 1, 2, 3 };
        ttcd_on_ipc_frame(&C, H.now, GAMA_IPC_ROLE_OBC, junk, 3);       /* not a frame */
        CHECK_EQ_INT(C.n.ipc_rx_bad, 2);
    }

    TEST_SUMMARY("test_ttcd_core");
}
