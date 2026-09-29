# ADR-0009: Monotonic clocks everywhere, one wall-clock anchor from the ground

- **Status:** Proposed — implemented and tested without hardware; awaiting team review
- **Date:** 2026-09-28
- **Requirements:** HLR-ADS-07, HLR-ADS-08, HLR-SW-02
- **Deciders:** TT&C team (LoRa and ADS-B sub-teams)

## Context

The Raspberry Pi Zero 2 W has no real-time clock. At boot its wall clock is
whatever was saved at the last shutdown, or 1970; at the venue there is no
network for NTP; and if NTP ever does reach it, the clock jumps. Time is
still what trajectory reconstruction is made of (HLR-ADS-07), and it has
three different consumers:

| Consumer | Needs | Already solved? |
|---|---|---|
| The live link | the time of each track update, at the ground | Yes: each record carries its age; the ground rebuilds the time in its own clock, with no synchronisation (ADR-0007 §6) |
| The onboard record (`adsbd`, retrieved by USB) | a wall-clock time per message: ordered, free of jumps, on the ground's timeline | No: the prototype stamped `CLOCK_REALTIME` and required the clock "set before `main()`" (`adsb_capture.c`) |
| The logs of `ttcd` and the OBC | ordering, and a date | Yes: monotonic milliseconds plus one wall-clock note (AGENTS.md rule 8, `obc/docs/log_schema.md`) |

Two facts sharpen the second row. The ground's estimator
(`ground/host/ground-aeronaves`) orders by `rx_epoch_ns`, works on windows
of 10 to 120 s and starts a new tracking episode after a 15-minute gap: a
jump in the record's clock in the middle of a mission splits or scrambles
trajectories. And the team agreed on 2026-09-28 that the ground gives the
Pi its time when the link comes up.

## Decision

1. **Every process stamps with `CLOCK_MONOTONIC`.** Nothing sets the system
   clock.
2. **Wall-clock time is the monotonic time plus an offset**, and the offset
   comes from the ground. The ground station sends `GAMA_TC_SET_TIME` by
   itself every time it gains or regains contact with the satellite
   (`gs_cli --auto-time on`; the ESP32 will do the same). `ttcd` anchors it
   at the ground's transmission start, subtracting the frame's time on air,
   and forwards `IPC_TIME_SET` to the OBC and to `adsbd` — and to either of
   them that connects later, so a restarted process is re-anchored at once.
3. **`adsbd` writes `rx_epoch_ns` = arrival on the SBS socket, monotonic, plus
   the offset.** Until the first anchor the offset is the system clock's at
   start, and the event log says so; every new anchor is logged with the
   step it made. The record's line format does not change.
4. **The live link keeps ADR-0007's rule:** the age in each record, no
   synchronisation.

## Rationale

**A monotonic clock never jumps**, so the record's timestamps change pace
only at an anchor, and every anchor is logged with its size: a record can be
re-timed afterwards if a step ever lands where it hurts.

**The anchor comes from the clock that matters.** The trajectories the jury
sees are built at the ground, in the ground's time; the organisers' scenario
runs on theirs. Anchoring the Pi to the ground puts the onboard record on the
same timeline as everything downlinked.

**Sending the time on contact** places the first — possibly large — step
before the mission, when there is nothing to split. After that, each anchor
only corrects the drift of the Pi's crystal: at 50 ppm, 30 ms over a
10-minute mission.

**The line format stays byte for byte the prototype's.** The data budget is
sized from it (160.4 B per line, 7.34 MiB per mission, pinned by
`tests/test_sbs.c`), and the ground's estimator already reads it.

**Resending the time on (re)connection closes a gap found while
implementing this:** a restarted `adsbd` used to stamp with the unsynchronised
clock until the next `SET_TIME`. `tests/test_adsb_chain.c` kills and restarts
`adsbd` and checks that its record is back on the ground's clock without a
new telecommand.

**Precision.** The anchor's error is the ground clock's error, plus the
error of the time-on-air model (verified to the microsecond against the
calculation; to be measured on the bench), plus the IPC hop, microseconds.

## Alternatives considered

### `CLOCK_REALTIME`, as the prototype

Correct only if something set the clock before the mission, and without a
network nothing does. A correction during the mission becomes a jump in the
record, which the estimator reads as a gap or a reversal.

### Setting the system clock from `SET_TIME`

Needs `CAP_SYS_TIME` in a process that runs without privileges (ADR-0012), and
moves every process's wall clock at once, including logs being written.

### Both clocks on every line

Makes any re-timing trivial, at ~25 bytes more per line: 7.34 becomes ~8.5 MiB
per mission, and ADR-0003, ADR-0004 and the data budget need errata. The
logged anchor steps give the same ability at no cost per line.

### NTP or chrony with the ground as server

Needs an IP link; LoRa is not one. chrony also slews over minutes, longer
than the time between contact and mission.

### A GPS receiver as time source

Not in the hardware list: another device, antenna and power budget.

## Consequences

### Positive

- The record's timestamps have no jumps, and are on the ground's timeline
  from the first contact.
- A process that restarts is re-anchored as soon as it reconnects.
- No privileges, no change to the record's format, nothing to change in the
  ground's estimator.

### Negative

- **Before the first contact the record's absolute times are the Pi's
  guess**, possibly days off. They stay consistent among themselves, and the
  log names them unsynchronised; they can be re-timed from the first anchor
  event, which logs its step.
- **Each anchor steps the record's clock.** After the first, by milliseconds;
  the first can be large, which is why the ground sends it on contact.
- **The timestamp is the arrival on the SBS socket, not the RF reception**,
  as ADR-0008 already records. dump1090's Beast output carries a 12 MHz
  counter if a reviewer ever requires the receiver's own time.

### Follow-up required

- ESP32 ground station: send `SET_TIME` on contact, as `gs_cli --auto-time on`.
- OBC: handle `IPC_TIME_SET` (OBC↔TT&C ICD).
- Bench: measure the anchor's error on the Pi, ground time against the
  anchored time of a known event.

## References

- ADR-0007 §6 — the age in each record
- ADR-0008 — the timestamp is the arrival on the SBS socket
- `flight/adsbd/core.c` — `adsbd_on_ipc_frame()`, `adsbd_wall_ns()`
- `flight/ttcd/tc_dispatch.c` — `core_send_time()`; `flight/ttcd/link.c` — on (re)connection
- `tools/gs_cli/gs_cli.c` — `--auto-time`
- `tests/test_adsbd_core.c`, `tests/test_adsbd_integration.c`, `tests/test_adsb_chain.c`
- `obc/docs/log_schema.md` — the convention this extends
