/*
 * ndjson.h — one line of the onboard ADS-B record: every message dump1090
 * decoded, stamped on arrival, one JSON object per line (HLR-SW-02,
 * HLR-ADS-07).
 *
 * The format is the prototype's (adsb_capture.c), byte for byte, for two
 * reasons: the ground's estimator (ground/host/ground-aeronaves) already
 * reads it, and tools/analysis/lora_budget.py sizes the whole data
 * budget from it — 160.4 B per line, 7.34 MiB per mission. tests/test_sbs.c
 * pins the three lines that budget is computed from.
 *
 * One deliberate difference: on_ground is 1 for an aircraft on the ground.
 * The prototype wrote -1 ("unknown") there, because it misread the
 * BaseStation boolean (see sbs.h).
 *
 * rx_epoch_ns is the wall-clock time of arrival, in nanoseconds: the
 * monotonic arrival time plus the anchor the ground supplied (ADR-0009).
 */

#ifndef ADSBD_NDJSON_H
#define ADSBD_NDJSON_H

#include <stddef.h>
#include <stdint.h>

#include "sbs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest line the format can produce, newline and NUL included, with room
 * to spare: every field present and at its widest is ~250 bytes. */
#define ADSBD_NDJSON_MAX 320u

/* Writes the line for m, '\n' included, NUL-terminated. Returns its length
 * without the NUL, or -1 if cap is too small. */
int adsbd_ndjson_format(char *buf, size_t cap, const sbs_msg_t *m, uint64_t rx_epoch_ns);

#ifdef __cplusplus
}
#endif

#endif /* ADSBD_NDJSON_H */
