#include "tracks.h"

#include <string.h>

void tracks_init(adsbd_table_t *t, uint32_t capacity)
{
    memset(t, 0, sizeof(*t));
    t->capacity = capacity == 0 || capacity > ADSBD_TRACKS_MAX ? ADSBD_TRACKS_MAX : capacity;
}

/* A time in the past no older than max_age. Written so that a time in the
 * future (which a monotonic clock never produces) reads as stale rather than
 * wrapping into "fresh". */
static bool fresh(uint64_t at, uint64_t now, uint32_t max_age)
{
    return at <= now && now - at <= max_age;
}

static int find_slot(const adsbd_table_t *t, uint32_t icao)
{
    for (uint32_t i = 0; i < t->capacity; i++) {
        if (t->used[i] && t->slot[i].icao == icao) {
            return (int)i;
        }
    }
    return -1;
}

const adsbd_track_t *tracks_find(const adsbd_table_t *t, uint32_t icao)
{
    int i = find_slot(t, icao);
    return i < 0 ? NULL : &t->slot[i];
}

bool tracks_apply(adsbd_table_t *t, const sbs_msg_t *m, uint64_t now)
{
    bool kept_all = true;
    int s = find_slot(t, m->icao);
    if (s < 0) {
        int free_slot = -1, stalest = -1;
        for (uint32_t i = 0; i < t->capacity; i++) {
            if (!t->used[i]) {
                if (free_slot < 0) {
                    free_slot = (int)i;
                }
            } else if (stalest < 0 || t->slot[i].last_ms < t->slot[stalest].last_ms) {
                stalest = (int)i;
            }
        }
        if (free_slot >= 0) {
            s = free_slot;
            t->count++;
        } else {
            s = stalest;          /* full: the aircraft heard from least recently goes */
            kept_all = false;
        }
        memset(&t->slot[s], 0, sizeof(t->slot[s]));
        t->used[s] = true;
        t->slot[s].icao = m->icao;
        t->slot[s].first_ms = now;
    }

    adsbd_track_t *k = &t->slot[s];
    k->last_ms = now;
    k->msgs++;
    if (m->have & SBS_HAVE_POSITION) {
        k->lat = m->lat;
        k->lon = m->lon;
        k->pos_ms = now;
        k->have |= TRK_POS;
    }
    if (m->have & SBS_HAVE_ALTITUDE) {
        k->altitude_ft = m->altitude_ft;
        k->alt_ms = now;
        k->have |= TRK_ALT;
    }
    if (m->have & SBS_HAVE_SPEED) {
        k->ground_speed_kt = m->ground_speed_kt;
        k->spd_ms = now;
        k->have |= TRK_SPD;
    }
    if (m->have & SBS_HAVE_TRACK) {
        k->track_deg = m->track_deg;
        k->trk_ms = now;
        k->have |= TRK_TRK;
    }
    if (m->have & SBS_HAVE_VRATE) {
        k->vertical_rate_fpm = m->vertical_rate_fpm;
        k->vr_ms = now;
        k->have |= TRK_VR;
    }
    if (m->have & SBS_HAVE_GROUND) {
        k->on_ground = m->on_ground == 1;
        k->gnd_ms = now;
        k->have |= TRK_GND;
    }
    if (m->have & SBS_HAVE_CALLSIGN) {
        memcpy(k->callsign, m->callsign, sizeof(k->callsign));
        k->have |= TRK_CS;
    }
    return kept_all;
}

uint32_t tracks_expire(adsbd_table_t *t, uint64_t now, uint32_t expiry_ms)
{
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < t->capacity; i++) {
        if (t->used[i] && !fresh(t->slot[i].last_ms, now, expiry_ms)) {
            t->used[i] = false;
            t->count--;
            dropped++;
        }
    }
    return dropped;
}

typedef struct {
    uint16_t slot;
    bool     position;          /* a fresh position: listed first           */
    uint64_t at;                /* the record's time                        */
} candidate_t;

/* Fresh position first, then the newest record first. */
static bool before(const candidate_t *a, const candidate_t *b)
{
    if (a->position != b->position) {
        return a->position;
    }
    return a->at > b->at;
}

size_t tracks_snapshot(const adsbd_table_t *t, uint64_t now,
                       const adsbd_snapshot_rules_t *r,
                       gama_track_t *out, size_t cap, uint32_t *left_out)
{
    candidate_t c[ADSBD_TRACKS_MAX];
    size_t nc = 0;
    uint32_t ttl = r->field_ttl;

    for (uint32_t i = 0; i < t->capacity; i++) {
        if (!t->used[i]) {
            continue;
        }
        const adsbd_track_t *k = &t->slot[i];
        candidate_t x = { .slot = (uint16_t)i };
        x.position = (k->have & TRK_POS) && fresh(k->pos_ms, now, r->position_max_age);
        bool any = x.position;
        x.at = x.position ? k->pos_ms : 0;
        if (!x.position) {
            /* No usable position: time the record by its newest other field. */
            const uint64_t times[] = { k->alt_ms, k->spd_ms, k->trk_ms, k->vr_ms };
            const uint8_t bits[] = { TRK_ALT, TRK_SPD, TRK_TRK, TRK_VR };
            for (size_t f = 0; f < sizeof(bits); f++) {
                if ((k->have & bits[f]) && fresh(times[f], now, ttl)) {
                    any = true;
                    if (times[f] > x.at) {
                        x.at = times[f];
                    }
                }
            }
        }
        if (!any) {
            continue;           /* identification only, or everything stale */
        }
        /* Insertion into the sorted list: at most 128 entries. */
        size_t j = nc++;
        while (j > 0 && before(&x, &c[j - 1])) {
            c[j] = c[j - 1];
            j--;
        }
        c[j] = x;
    }

    size_t limit = cap < r->max_records ? cap : r->max_records;
    size_t n = nc < limit ? nc : limit;
    for (size_t j = 0; j < n; j++) {
        const adsbd_track_t *k = &t->slot[c[j].slot];
        gama_track_t *o = &out[j];
        memset(o, 0, sizeof(*o));
        o->icao = k->icao;
        uint8_t flags = 0;
        if (c[j].position) {
            o->latitude = k->lat;
            o->longitude = k->lon;
            flags |= GAMA_TRACK_F_POSITION;
        }
        if ((k->have & TRK_ALT) && fresh(k->alt_ms, now, ttl)) {
            o->altitude_ft = k->altitude_ft;
            flags |= GAMA_TRACK_F_ALTITUDE;
        }
        if ((k->have & TRK_SPD) && (k->have & TRK_TRK) &&
            fresh(k->spd_ms, now, ttl) && fresh(k->trk_ms, now, ttl)) {
            o->ground_speed_kt = k->ground_speed_kt;
            o->track_deg = k->track_deg;
            flags |= GAMA_TRACK_F_VELOCITY;
        }
        if ((k->have & TRK_VR) && fresh(k->vr_ms, now, ttl)) {
            o->vertical_rate_fpm = k->vertical_rate_fpm;
            flags |= GAMA_TRACK_F_VRATE;
        }
        if ((k->have & TRK_GND) && k->on_ground && fresh(k->gnd_ms, now, ttl)) {
            flags |= GAMA_TRACK_F_ON_GROUND;
        }
        uint64_t age_ds = (now - c[j].at + 50u) / 100u;
        o->age_ds = age_ds > 0xFFFFu ? 0xFFFFu : (uint16_t)age_ds;
        o->flags = flags;
    }
    if (left_out != NULL) {
        *left_out = (uint32_t)(nc - n);
    }
    return n;
}
