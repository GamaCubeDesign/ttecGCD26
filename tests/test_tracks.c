/*
 * The track table and the snapshot rules (HLR-ADS-03, HLR-ADS-05,
 * HLR-ADS-07), on a virtual clock in milliseconds.
 */

#include "test_util.h"
#include "tracks.h"

#include <stdio.h>
#include <string.h>

static adsbd_table_t T;
static const adsbd_snapshot_rules_t RULES = {
    .position_max_age = 10000, .field_ttl = 60000, .max_records = 24,
};

/* Applies one SBS line at time `at`; returns tracks_apply()'s result. */
static bool feed(const char *line, uint64_t at)
{
    sbs_msg_t m;
    if (sbs_parse(line, strlen(line), &m) != SBS_OK) {
        printf("  bad test line: %s\n", line);
        return false;
    }
    return tracks_apply(&T, &m, at);
}

static void position(uint32_t icao, double lat, double lon, int alt, uint64_t at)
{
    char l[160];
    snprintf(l, sizeof(l), "MSG,3,1,1,%06X,1,,,,,,%d,,,%.6f,%.6f,,,0,,0,0",
             (unsigned)icao, alt, lat, lon);
    feed(l, at);
}

static void velocity(uint32_t icao, double gs, double trk, int vr, uint64_t at)
{
    char l[160];
    snprintf(l, sizeof(l), "MSG,4,1,1,%06X,1,,,,,,,%.1f,%.1f,,,%d,,,,,", (unsigned)icao, gs, trk, vr);
    feed(l, at);
}

static const gama_track_t *find(const gama_track_t *r, size_t n, uint32_t icao)
{
    for (size_t i = 0; i < n; i++) {
        if (r[i].icao == icao) {
            return &r[i];
        }
    }
    return NULL;
}

int main(void)
{
    gama_track_t out[ADSBD_TRACKS_MAX];
    uint32_t left;

    TEST_GROUP("tracks: position, velocity and identity merge into one aircraft");
    {
        tracks_init(&T, 128);
        position(0xE48DF5, -23.559616, -46.658908, 37000, 100000);
        velocity(0xE48DF5, 451.7, 128.4, -1216, 100400);
        feed("MSG,1,1,1,E48DF5,1,,,,,TAM3054,,,,,,,,,,,", 100900);
        CHECK_EQ_INT(T.count, 1);
        const adsbd_track_t *k = tracks_find(&T, 0xE48DF5);
        CHECK(k != NULL);
        CHECK_EQ_INT(k->msgs, 3);
        CHECK_STR_EQ(k->callsign, "TAM3054");

        size_t n = tracks_snapshot(&T, 101000, &RULES, out, 128, &left);
        CHECK_EQ_INT(n, 1);
        CHECK_EQ_INT(left, 0);
        CHECK_EQ_INT(out[0].icao, 0xE48DF5);
        CHECK_EQ_INT(out[0].flags, GAMA_TRACK_F_POSITION | GAMA_TRACK_F_ALTITUDE |
                                   GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE);
        CHECK_NEAR(out[0].latitude, -23.559616, 1e-9);
        CHECK_EQ_INT(out[0].altitude_ft, 37000);
        CHECK_NEAR(out[0].ground_speed_kt, 451.7, 1e-9);
        CHECK_EQ_INT(out[0].vertical_rate_fpm, -1216);
        /* Timed by its position, 1.0 s before the snapshot. */
        CHECK_EQ_INT(out[0].age_ds, 10);
    }

    TEST_GROUP("tracks: a stale position is left out, and the record timed by the rest");
    {
        tracks_init(&T, 128);
        position(0xE48DF5, -23.5, -46.6, 37000, 100000);
        feed("MSG,5,1,1,E48DF5,1,,,,,,36975,,,,,,,0,,0,0", 112000);      /* altitude only */
        size_t n = tracks_snapshot(&T, 112500, &RULES, out, 128, NULL);
        CHECK_EQ_INT(n, 1);
        CHECK(!(out[0].flags & GAMA_TRACK_F_POSITION));   /* 12.5 s > 10 s */
        CHECK(out[0].flags & GAMA_TRACK_F_ALTITUDE);
        CHECK_EQ_INT(out[0].altitude_ft, 36975);
        CHECK_EQ_INT(out[0].age_ds, 5);                   /* by the altitude */
    }

    TEST_GROUP("tracks: fields older than the TTL are dropped, then the aircraft");
    {
        tracks_init(&T, 128);
        position(0xE48DF5, -23.5, -46.6, 37000, 100000);
        velocity(0xE48DF5, 450.0, 128.0, 0, 130000);
        size_t n = tracks_snapshot(&T, 165000, &RULES, out, 128, NULL);
        CHECK_EQ_INT(n, 1);                               /* velocity 35 s old */
        CHECK_EQ_INT(out[0].flags, GAMA_TRACK_F_VELOCITY | GAMA_TRACK_F_VRATE);
        n = tracks_snapshot(&T, 190001, &RULES, out, 128, NULL);
        CHECK_EQ_INT(n, 0);                               /* everything > 60 s */
        CHECK_EQ_INT(tracks_expire(&T, 190000, 60000), 0); /* silent exactly 60 s */
        CHECK_EQ_INT(tracks_expire(&T, 190001, 60000), 1);
        CHECK_EQ_INT(T.count, 0);
        CHECK(tracks_find(&T, 0xE48DF5) == NULL);
    }

    TEST_GROUP("tracks: identification alone is not a record");
    {
        tracks_init(&T, 128);
        feed("MSG,1,1,1,E48DF5,1,,,,,TAM3054,,,,,,,,,,,", 100000);
        CHECK_EQ_INT(tracks_snapshot(&T, 100500, &RULES, out, 128, NULL), 0);
        CHECK_EQ_INT(T.count, 1);
    }

    TEST_GROUP("tracks: an aircraft on the ground is flagged so (BaseStation -1)");
    {
        tracks_init(&T, 128);
        feed("MSG,2,1,1,E49608,1,,,,,,,12.0,275.3,-15.870000,-47.920000,,,,,,-1", 100000);
        size_t n = tracks_snapshot(&T, 100000, &RULES, out, 128, NULL);
        CHECK_EQ_INT(n, 1);
        CHECK(out[0].flags & GAMA_TRACK_F_ON_GROUND);
        CHECK(out[0].flags & GAMA_TRACK_F_POSITION);
        feed("MSG,3,1,1,E49608,1,,,,,,1500,,,-15.86,-47.93,,,0,,0,0", 101000);  /* took off */
        n = tracks_snapshot(&T, 101000, &RULES, out, 128, NULL);
        CHECK(!(out[0].flags & GAMA_TRACK_F_ON_GROUND));
    }

    TEST_GROUP("tracks: a full table drops the aircraft heard from least recently");
    {
        tracks_init(&T, 3);
        position(0xA00001, 1, 1, 1000, 100000);
        position(0xA00002, 2, 2, 1000, 100100);
        position(0xA00003, 3, 3, 1000, 100200);
        position(0xA00001, 1, 1, 1000, 100300);             /* heard again */
        CHECK(!feed("MSG,3,1,1,A00004,1,,,,,,1000,,,4.0,4.0,,,0,,0,0", 100400));
        CHECK_EQ_INT(T.count, 3);
        CHECK(tracks_find(&T, 0xA00002) == NULL);            /* the stalest */
        CHECK(tracks_find(&T, 0xA00001) != NULL);
        CHECK(tracks_find(&T, 0xA00004) != NULL);
    }

    TEST_GROUP("tracks: 30 aircraft, 24 per snapshot — positions first, newest first");
    {
        tracks_init(&T, 128);
        for (uint32_t i = 0; i < 28; i++) {
            position(0xB00000 + i, -15.0 - 0.01 * i, -47.0, 30000, 100000 + 100u * i);
        }
        /* Two with no position, but fresh altitude: listed after all positions. */
        feed("MSG,5,1,1,C00001,1,,,,,,12000,,,,,,,0,,0,0", 104000);
        feed("MSG,5,1,1,C00002,1,,,,,,13000,,,,,,,0,,0,0", 104100);
        size_t n = tracks_snapshot(&T, 105000, &RULES, out, 128, &left);
        CHECK_EQ_INT(n, 24);
        CHECK_EQ_INT(left, 6);
        CHECK_EQ_INT(out[0].icao, 0xB00000 + 27);            /* newest position */
        CHECK_EQ_INT(out[23].icao, 0xB00000 + 4);
        CHECK(find(out, n, 0xC00001) == NULL);               /* left out */
        bool all_positioned = true;
        for (size_t i = 0; i < n; i++) {
            all_positioned = all_positioned && (out[i].flags & GAMA_TRACK_F_POSITION);
        }
        CHECK(all_positioned);

        adsbd_snapshot_rules_t wide = RULES;
        wide.max_records = 96;
        n = tracks_snapshot(&T, 105000, &wide, out, 128, &left);
        CHECK_EQ_INT(n, 30);
        CHECK_EQ_INT(left, 0);
        CHECK_EQ_INT(out[28].icao, 0xC00002);               /* then by time */
        CHECK_EQ_INT(out[29].icao, 0xC00001);
        CHECK_EQ_INT(tracks_snapshot(&T, 105000, &wide, out, 10, &left), 10);  /* cap */
        CHECK_EQ_INT(left, 20);
    }

    TEST_GROUP("tracks: the age saturates instead of wrapping");
    {
        tracks_init(&T, 128);
        position(0xE48DF5, -23.5, -46.6, 37000, 1000);
        adsbd_snapshot_rules_t forever = { 0xFFFFFFFFu, 0xFFFFFFFFu, 24 };
        size_t n = tracks_snapshot(&T, 1000 + 7000000u, &forever, out, 128, NULL);
        CHECK_EQ_INT(n, 1);
        CHECK_EQ_INT(out[0].age_ds, 65535);
    }

    TEST_GROUP("tracks: 20 aircraft for 10 minutes at 4 messages/s each");
    {
        /* HLR-ADS-05 at the nominal rate: the table holds all 20 throughout,
         * and every snapshot carries all 20 with a fresh position. */
        tracks_init(&T, 128);
        bool ok = true;
        for (uint64_t t = 0; t < 600000; t += 250) {
            for (uint32_t a = 0; a < 20; a++) {
                uint64_t at = 1000000 + t + a;
                if ((t / 250) % 2 == 0) {
                    position(0xD00000 + a, -15.0 + 1e-5 * (double)t, -47.0 + 0.01 * a, 20000, at);
                } else {
                    velocity(0xD00000 + a, 300.0, 90.0, 0, at);
                }
            }
            if (t % 1000 == 0) {
                size_t n = tracks_snapshot(&T, 1000000 + t + 999, &RULES, out, 128, &left);
                ok = ok && n == 20 && left == 0 && T.count == 20;
            }
        }
        CHECK(ok);
    }

    TEST_SUMMARY("test_tracks");
}
