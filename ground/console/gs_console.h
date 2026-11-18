/*
 * gs_console.h — the ground station's operator console, shared by the bench
 * tool (tools/gs_cli, Linux) and the ESP32 ground station (ground/esp32).
 *
 * It owns the ground side of the link (common/gama_gs_link.c) and adds what
 * an operator, a bench script or the host bridge needs: one command per text
 * line, and one JSON object per event out. Both ends of a bench step read the
 * same lines whichever ground produced them (ADR-0018):
 *
 *   {"mono_ms":<ground monotonic ms>,"wall_ms":<ground wall clock ms>,"ev":...}
 *
 * Portable C11: no clock, no I/O, no allocation. Time comes in with every
 * call; lines leave through ops.write_line. The platform supplies the radio.
 *
 * Two phases. Before begin, only `set KEY VALUE` lines are taken (clock,
 * profile, power, lbt, auto_time): the bench tool maps its options onto them,
 * the ESP32's host bridge sends them over the serial port. `begin` starts the
 * link and prints the `start` line. After it, the commands:
 *
 *   ping [N]              N sequential PINGs (default 1), latency of each
 *   per N                 N PINGs, then uplink and downlink loss from TM_STAT
 *   hk | stat | roster    request housekeeping / statistics / the roster
 *   rate safe|nominal|fast
 *   power DBM             the satellite's transmit power, 2..20
 *   ground_power DBM      this ground station's transmit power, 2..20
 *   mode EVENT            OBC event: basic_inter aocs mission_adsb downlink
 *                         survival task_done adsb_timeout, or its number
 *                         mission_adsb first waits for a successful SET_TIME
 *   stream SECONDS        start streaming track snapshots at this period
 *   stop                  stop streaming
 *   time                  send the ground's wall clock (SET_TIME)
 *   shutdown confirm      stop ttcd (it will not restart by itself)
 *   wait SECONDS          just listen
 *   deafen SECONDS        ignore everything received for this long, then
 *                         carry on — provokes a lost ACK on purpose
 *   quit
 *
 * A command is read only when the previous one has finished (its ACK, its
 * PINGs, its wait): a file of commands is a repeatable procedure. Every line
 * that is not blank or a '#' comment is echoed as a `command` event first.
 *
 * The ground's wall clock stamps SET_TIME (ADR-0009) and every line. The
 * ESP32 has none until the host sends `set clock`; until then `time` and
 * --auto-time are refused, because a SET_TIME without a clock would anchor
 * the satellite in 1970.
 */

#ifndef GS_CONSOLE_H
#define GS_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gama_gs_link.h"
#include "gama_tm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The longest line the console prints: a TM_TRACKS frame of 12 records with
 * every field at its widest, its payload in hex, and the envelope. */
#define GS_CONSOLE_LINE_MAX 3072u
/* The longest input line taken; longer ones are cut. */
#define GS_CONSOLE_INPUT_MAX 512u

typedef struct {
    void *ctx;
    /* The radio, as gs_link wants it (common/gama_gs_link.h). */
    int  (*radio_tx)(void *ctx, const uint8_t *frame, size_t len);
    int  (*radio_set_profile)(void *ctx, uint8_t profile);
    /* This station's transmit power and listen-before-talk mode. Optional:
     * NULL, or non-zero, means the radio refused. */
    int  (*radio_set_power)(void *ctx, int8_t dbm);
    int  (*radio_set_lbt)(void *ctx, bool preamble);
    /* The wall clock, in ms since 1970. Optional: when NULL or non-zero, the
     * console's own, from `set clock`, is used. */
    int  (*wall_ms)(void *ctx, uint64_t *ms);
    /* One complete line, '\n' included. */
    void (*write_line)(void *ctx, const char *line, size_t len);
} gs_console_ops_t;

typedef struct {
    gs_console_ops_t ops;
    gs_link_t        link;
    gs_params_t      params;
    const char      *radio;
    uint64_t         now;          /* the time of the call in progress        */
    bool             begun, quit;
    bool             auto_time;
    bool             adsb_after_time; /* ordered SET_TIME -> SET_MODE startup */
    bool             clock_set;
    int64_t          clock_offset; /* wall = mono + this, from `set clock`    */
    uint64_t         deaf_until;
    uint32_t         deaf_dropped;
    uint32_t         lines_too_long;

    /* The activity the current command is waiting on. */
    struct {
        enum { GS_IDLE, GS_AWAIT_ACK, GS_PINGING, GS_WAITING } kind;
        uint32_t id;
        int      remaining, total, acked, failed;
        uint64_t lat_sum, lat_max, lat_min;
        uint32_t attempts;
        bool     per;
        uint64_t until;
    } a;

    /* PER bookkeeping: the satellite's view before and after. */
    struct {
        int         phase;         /* 0 none, 1 first STAT, 2 pinging, 3 last STAT */
        uint32_t    sat_rx0, gs_sent0, dl_frames0, dl_lost0;
        bool        stat_seen;
        gama_stat_t stat;
    } per;

    /* Output buffers, here rather than on the stack: the ESP32's task stack
     * is small. */
    char line[GS_CONSOLE_LINE_MAX];
    char recs[2048];
    char hex[2u * 248u + 1u];
} gs_console_t;

/* Ready for `set` lines; the link has not started. `radio` names the radio
 * in the `start` line (the bench's "udp" or "sx1278"; the ESP32's "esp32"). */
void gs_console_init(gs_console_t *c, const gs_console_ops_t *ops, const char *radio);

/* One setting, before begin (`clock` also after it). Keys: clock (ms since
 * 1970), profile (safe|nominal|fast), power (dBm), lbt (header|preamble),
 * auto_time (on|off). Returns 0, or -1 with *why set. Prints nothing. */
int gs_console_set(gs_console_t *c, uint64_t now, const char *key, const char *value,
                   const char **why);

/* Starts the link and prints the `start` line. The `begin` input line does
 * the same. */
void gs_console_begin(gs_console_t *c, uint64_t now);

/* One input line, without its newline (it is modified in place). Call it
 * only while gs_console_wants_line(). */
void gs_console_line(gs_console_t *c, uint64_t now, char *line);

/* No more input will come: finish the command in progress, then stop. */
void gs_console_eof(gs_console_t *c);

/* True when the console can take the next input line. */
bool gs_console_wants_line(const gs_console_t *c);

/* True once `quit` (or the end of input) has been reached and nothing is
 * left in progress: print the summary and stop. */
bool gs_console_finished(const gs_console_t *c);

/* The radio's events. A frame received while deafened is dropped here. */
void gs_console_on_rx(gs_console_t *c, uint64_t now, const uint8_t *buf, size_t len,
                      int16_t rssi, int8_t snr);
void gs_console_on_rx_error(gs_console_t *c, uint64_t now);
void gs_console_on_tx_done(gs_console_t *c, uint64_t now);
void gs_console_on_tick(gs_console_t *c, uint64_t now);

/* The earliest instant gs_console_on_tick() has something to do. */
uint64_t gs_console_next_deadline(const gs_console_t *c);

/* The `summary` line: what the link counted since begin. */
void gs_console_summary(gs_console_t *c, uint64_t now);

/* One more event line in the console's format, for what the platform knows
 * and the console does not (the ESP32's boot, for instance). `fields` are
 * the JSON members after "ev", already formatted, or NULL. */
void gs_console_event(gs_console_t *c, uint64_t now, const char *ev, const char *fields);

/* Copies src into dst as the inside of a JSON string: quote, backslash,
 * control and non-ASCII bytes escaped. Always terminates dst. */
void gs_console_json_escape(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* GS_CONSOLE_H */
