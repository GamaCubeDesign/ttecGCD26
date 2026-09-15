#include "gama_tm.h"
#include "gama_bytes.h"
#include "gama_frame.h"

/*
 * Scales a physical value into a bounded integer.
 *
 * Out-of-range inputs are clamped rather than allowed to wrap, so a bad
 * sensor reading degrades into a saturated value instead of aliasing onto a
 * plausible-looking one -- a wrapped latitude would put an aircraft on the
 * other side of the planet with no indication anything went wrong.
 *
 * The in-range test is written as a positive assertion because every
 * comparison against NaN is false: NaN therefore falls through to the final
 * return and encodes as 0 rather than invoking undefined behaviour in the
 * cast. Callers should have cleared the corresponding validity flag anyway.
 */
static int32_t scale_clamp(double v, double scale, int32_t lo, int32_t hi)
{
    double s = v * scale;
    if (s >= (double)lo && s <= (double)hi) {
        return (int32_t)(s < 0.0 ? s - 0.5 : s + 0.5);
    }
    if (s > (double)hi) { return hi; }
    if (s < (double)lo) { return lo; }
    return 0;
}

/*
 * Clamps to a physical range before scaling.
 *
 * Saturating at the integer limit instead would overshoot the physical one:
 * i24 bottoms out at -8388608, which is -180.0045 degrees of longitude. A
 * consumer that trusts the documented range would see an impossible value.
 * NaN takes the final return, as in scale_clamp().
 */
static double clamp_d(double v, double lo, double hi)
{
    if (v >= lo && v <= hi) { return v; }
    if (v > hi) { return hi; }
    if (v < lo) { return lo; }
    return 0.0;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/* ------------------------------ track ------------------------------ */

int gama_track_encode(uint8_t *out, size_t out_cap, const gama_track_t *t)
{
    if (out == NULL || t == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (out_cap < GAMA_TRACK_WIRE_LEN) { return GAMA_FRAME_ERR_ARG; }

    /* Normalise heading into [0, 360). The range guard is written negated so
     * that NaN and absurd magnitudes take the reset branch instead of
     * spinning in a subtraction loop. */
    double trk = t->track_deg;
    if (!(trk >= -360.0 && trk <= 720.0)) {
        trk = 0.0;
    }
    /* Bounded by the guard above: at most two iterations each. */
    while (trk >= 360.0) { trk -= 360.0; }
    while (trk < 0.0)    { trk += 360.0; }

    size_t pos = 0;
    pos += gama_put_u24(out + pos, t->icao & 0x00FFFFFFu);
    pos += gama_put_i24(out + pos,
                        scale_clamp(clamp_d(t->latitude, -90.0, 90.0),
                                    GAMA_LAT_SCALE, -8388608, 8388607));
    pos += gama_put_i24(out + pos,
                        scale_clamp(clamp_d(t->longitude, -180.0, 180.0),
                                    GAMA_LON_SCALE, -8388608, 8388607));
    pos += gama_put_i16(out + pos,
                        (int16_t)clamp_i32(t->altitude_ft / 25, -32768, 32767));
    pos += gama_put_u16(out + pos,
                        (uint16_t)scale_clamp(t->ground_speed_kt, 10.0, 0, 65535));
    pos += gama_put_u16(out + pos,
                        (uint16_t)scale_clamp(trk, 100.0, 0, 65535));
    pos += gama_put_i16(out + pos,
                        (int16_t)clamp_i32(t->vertical_rate_fpm, -32768, 32767));
    pos += gama_put_u16(out + pos, t->age_ds);
    pos += gama_put_u8(out + pos, t->flags);

    return (int)pos;
}

int gama_track_decode(const uint8_t *in, size_t in_len, gama_track_t *t)
{
    if (in == NULL || t == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (in_len < GAMA_TRACK_WIRE_LEN) { return GAMA_FRAME_ERR_SHORT; }

    t->icao              = gama_get_u24(in + 0);
    t->latitude          = (double)gama_get_i24(in + 3) / GAMA_LAT_SCALE;
    t->longitude         = (double)gama_get_i24(in + 6) / GAMA_LON_SCALE;
    t->altitude_ft       = (int32_t)gama_get_i16(in + 9) * 25;
    t->ground_speed_kt   = (double)gama_get_u16(in + 11) / 10.0;
    t->track_deg         = (double)gama_get_u16(in + 13) / 100.0;
    t->vertical_rate_fpm = gama_get_i16(in + 15);
    t->age_ds            = gama_get_u16(in + 17);
    t->flags             = gama_get_u8(in + 19);

    return (int)GAMA_TRACK_WIRE_LEN;
}

/* ------------------------------ roster ----------------------------- */

int gama_roster_encode(uint8_t *out, size_t out_cap, const gama_roster_t *r)
{
    if (out == NULL || r == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (out_cap < GAMA_ROSTER_WIRE_LEN) { return GAMA_FRAME_ERR_ARG; }

    gama_put_u24(out, r->icao & 0x00FFFFFFu);

    /* Space-padded, not NUL-terminated: this is how ADS-B identification
     * messages carry it, and it saves a byte over a length prefix. */
    size_t n = 0;
    while (n < GAMA_CALLSIGN_LEN && r->callsign[n] != '\0') {
        out[3 + n] = (uint8_t)r->callsign[n];
        n++;
    }
    while (n < GAMA_CALLSIGN_LEN) {
        out[3 + n] = (uint8_t)' ';
        n++;
    }

    return (int)GAMA_ROSTER_WIRE_LEN;
}

int gama_roster_decode(const uint8_t *in, size_t in_len, gama_roster_t *r)
{
    if (in == NULL || r == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (in_len < GAMA_ROSTER_WIRE_LEN) { return GAMA_FRAME_ERR_SHORT; }

    r->icao = gama_get_u24(in + 0);

    for (size_t i = 0; i < GAMA_CALLSIGN_LEN; i++) {
        r->callsign[i] = (char)in[3 + i];
    }
    r->callsign[GAMA_CALLSIGN_LEN] = '\0';

    /* Strip the padding the encoder added, so callers get the identifier
     * rather than the fixed-width field. */
    for (size_t i = GAMA_CALLSIGN_LEN; i > 0 && r->callsign[i - 1] == ' '; i--) {
        r->callsign[i - 1] = '\0';
    }

    return (int)GAMA_ROSTER_WIRE_LEN;
}

/* --------------------------- housekeeping -------------------------- */

int gama_hk_encode(uint8_t *out, size_t out_cap, const gama_hk_t *hk)
{
    if (out == NULL || hk == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (out_cap < GAMA_HK_WIRE_LEN) { return GAMA_FRAME_ERR_ARG; }

    size_t pos = 0;
    pos += gama_put_u16(out + pos, hk->battery_mv);
    pos += gama_put_i16(out + pos, hk->current_ma);
    pos += gama_put_i16(out + pos, hk->temp_ext_ccel);
    pos += gama_put_i16(out + pos, hk->temp_soc_ccel);
    pos += gama_put_i16(out + pos, hk->roll_cdeg);
    pos += gama_put_i16(out + pos, hk->pitch_cdeg);
    pos += gama_put_i16(out + pos, hk->yaw_cdeg);
    pos += gama_put_u32(out + pos, hk->adsb_msgs);
    pos += gama_put_u8(out + pos, hk->obc_mode);
    pos += gama_put_u8(out + pos, hk->link_state);
    pos += gama_put_u32(out + pos, hk->uptime_s);
    pos += gama_put_u8(out + pos, hk->flags);

    return (int)pos;
}

int gama_hk_decode(const uint8_t *in, size_t in_len, gama_hk_t *hk)
{
    if (in == NULL || hk == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (in_len < GAMA_HK_WIRE_LEN) { return GAMA_FRAME_ERR_SHORT; }

    hk->battery_mv    = gama_get_u16(in + 0);
    hk->current_ma    = gama_get_i16(in + 2);
    hk->temp_ext_ccel = gama_get_i16(in + 4);
    hk->temp_soc_ccel = gama_get_i16(in + 6);
    hk->roll_cdeg     = gama_get_i16(in + 8);
    hk->pitch_cdeg    = gama_get_i16(in + 10);
    hk->yaw_cdeg      = gama_get_i16(in + 12);
    hk->adsb_msgs     = gama_get_u32(in + 14);
    hk->obc_mode      = gama_get_u8(in + 18);
    hk->link_state    = gama_get_u8(in + 19);
    hk->uptime_s      = gama_get_u32(in + 20);
    hk->flags         = gama_get_u8(in + 24);

    return (int)GAMA_HK_WIRE_LEN;
}

/* ---------------------------- statistics --------------------------- */

int gama_stat_encode(uint8_t *out, size_t out_cap, const gama_stat_t *s)
{
    if (out == NULL || s == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (out_cap < GAMA_STAT_WIRE_LEN) { return GAMA_FRAME_ERR_ARG; }

    size_t pos = 0;
    pos += gama_put_u32(out + pos, s->msgs_received);
    pos += gama_put_u32(out + pos, s->msgs_decoded);
    pos += gama_put_u16(out + pos, s->aircraft_tracked);
    pos += gama_put_u32(out + pos, s->tm_frames_sent);
    pos += gama_put_u32(out + pos, s->tc_frames_rx);
    pos += gama_put_u32(out + pos, s->tc_frames_bad);
    pos += gama_put_u16(out + pos, s->latency_p95_ms);
    pos += gama_put_u16(out + pos, s->dump1090_restarts);

    return (int)pos;
}

int gama_stat_decode(const uint8_t *in, size_t in_len, gama_stat_t *s)
{
    if (in == NULL || s == NULL) { return GAMA_FRAME_ERR_ARG; }
    if (in_len < GAMA_STAT_WIRE_LEN) { return GAMA_FRAME_ERR_SHORT; }

    s->msgs_received     = gama_get_u32(in + 0);
    s->msgs_decoded      = gama_get_u32(in + 4);
    s->aircraft_tracked  = gama_get_u16(in + 8);
    s->tm_frames_sent    = gama_get_u32(in + 10);
    s->tc_frames_rx      = gama_get_u32(in + 14);
    s->tc_frames_bad     = gama_get_u32(in + 18);
    s->latency_p95_ms    = gama_get_u16(in + 22);
    s->dump1090_restarts = gama_get_u16(in + 24);

    return (int)GAMA_STAT_WIRE_LEN;
}
