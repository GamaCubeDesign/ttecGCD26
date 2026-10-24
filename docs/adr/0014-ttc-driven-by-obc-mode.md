# ADR-0014: The OBC's mission mode drives ttcd's telemetry and adsbd's decoder

- **Status:** Proposed
- **Date:** 2026-10-24
- **Requirements:** HLR-GEN-03, HLR-ADS-05, HLR-ADS-08, HLR-SW-01, HLR-COMM-02
- **Deciders:** TT&C team

## Context

The mission modes belong to the OBC (CDR §ConOps, Figure 44); the TT&C
software must react to them (CDR Table 55, TTC-07, GEN-03.3):

| Mode | ttcd | adsbd |
|---|---|---|
| `PRE_TEST`, `BASIC_INTERMEDIATE`, `ADVANCED_AOCS`, `ENV_SURVIVAL` | housekeeping every 10 s; telecommands answered | decoder stopped, SDR powered and idle |
| `MISSION_ADSB` | snapshots every 5 s and statistics every 30 s, started by the mode, no ground command | decoder running |
| `MISSION_DOWNLINK` | the roster and the final statistics (260 B, 1.4 s on air), then the end of the task to the OBC | decoder stopped |

Until now the snapshots started only with a `STREAM_START` from the ground,
and dump1090 ran from boot whatever the mode. Protocol version 2 (ADR-0013)
gave both daemons the mode (`IPC_MODE`, OBC → ttcd → adsbd) and gave ttcd the
roster; this record decides what they do with them.

Two forces pull against each other. The mode must drive the payload, or the
10-minute test depends on an operator typing the right command at the right
second. But the OBC is a separate process (ADR-0005), and a crash of it must
not take the payload's 10 minutes down with it: systemd restarts it in about
a second, and it comes back in `PRE_TEST` (PLANO D5).

## Decision

**Only a known mode changes anything.** ttcd and adsbd each keep the last
mode the OBC reported. `GAMA_OBC_ST_UNKNOWN` — the OBC not connected, or not
yet — changes nothing: at boot that means everything off; mid-mission, it
means the decoder and the snapshots go on.

**ttcd** (`flight/ttcd/tm_sched.c`, `tm_on_mode`):
- entering `MISSION_ADSB` starts the snapshot stream at `stream_period_ms`
  (5000 by default) and the statistics 30 s later; leaving it stops them;
- entering `MISSION_DOWNLINK` queues the statistics, then the roster; when
  both have been transmitted, ttcd sends the OBC `IPC_TC_EVENT`
  `EV_TASK_DONE` (PLANO D2). Leaving the mode before that cancels the event;
- `STREAM_START` and `STREAM_STOP` remain as a manual override, valid until
  the next mode change.

**adsbd** (`flight/adsbd/core.c`, `adsbd_decoder_wanted`; the shell matches
it after every event):
- dump1090 runs, and its SBS output is read, only in `MISSION_ADSB`.
  Entering it starts dump1090; leaving it sends SIGTERM, SIGKILL after 3 s,
  and closes the SBS connection;
- a dump1090 that dies while wanted is started again after
  `restart_max_ms`, now 1 s by default (was 30 s): without its SDR it exits
  at once, and the plan asks for a retry every second until the SDR opens.
  Only those crashes are counted in `dump1090_restarts`; starts and stops by
  the mode are not;
- `decoder = always` restores the old behaviour, for the bench without an
  OBC. The default, and the flight value, is `mode`.

**Until `flight/obcd/` exists**, `tools/obc_standin` plays the OBC: it
follows every `SET_MODE` event at once, returns to `PRE_TEST` on
`EV_TASK_DONE`, ends `MISSION_ADSB` after 600 s, and sends `IPC_TELEMETRY`
every second with every field "no data". `make adsb-chain` and
`test_adsb_chain` run with it.

## Rationale

1. **UNKNOWN holds, because the alternatives lose a mission to a crash.** If
   UNKNOWN stopped the decoder, an OBC crash at minute 8 would stop
   dump1090; it would take seconds to open the SDR again even if the OBC came
   back in `MISSION_ADSB`, and by D5 it comes back in `PRE_TEST`, so the rest
   of the mission would be lost. Holding the last known mode costs nothing
   when the OBC is healthy and keeps the payload's fault isolation of
   ADR-0005 when it is not. The ground still sees the OBC gone: HK reports
   `obc_mode` UNKNOWN and clears `GAMA_HK_F_OBC_LINKED`.
2. **The end of the downlink is when the frames have left, not when they
   were queued.** `EV_TASK_DONE` returns the OBC to `PRE_TEST`; sent earlier,
   a ground watching for the mode change could see it before the roster.
   ttcd knows when a transmission ends (`tx_done`); the frames are not
   acknowledged by the ground, so "left" is the strongest statement ttcd can
   make. At 20 aircraft: 33 + 227 B, 1.4 s at NOMINAL; the core test checks
   the 260 B.
3. **Statistics first, then the roster.** That is the scheduler's existing
   priority (`tm_pick`), and the statistics are the smaller and more
   important of the two for HLR-ADS-08.
4. **The decoder follows the mode by closing its input too.** Stopping
   dump1090 alone would leave an externally started one (the replay, or a
   dump1090 run by hand) feeding the record outside the mission. Closing the
   SBS connection makes "outside `MISSION_ADSB`, nothing is decoded or
   recorded" true for both.
5. **A stand-in now, not obcd early.** The mode-driven behaviour has to be
   demonstrable this week (PLANO priority 1, "make adsb-chain only streams in
   MISSION_ADSB"); obcd is etapa 2. The stand-in is 250 lines with no
   decisions of its own: it encodes no transition rules, so nothing in it
   has to be kept in step with Figure 44.

## Alternatives considered

### UNKNOWN stops everything

The literal reading of "the decoder runs only in `MISSION_ADSB`". Lost for
the reason in Rationale 1: it turns one OBC crash into a lost mission.

### `EV_TASK_DONE` when the downlink frames are queued

Simpler, with no completion check. Lost because the OBC would leave the
mode before the frames that justify leaving it had been sent.

### The decoder always runs; only ttcd follows the mode

dump1090 uses 21.9% of one core (`docs/vv/bancada/2026-10-09/cpu.txt`), and the
record would keep growing outside the mission. The CDR promises the decoder
stopped outside `MISSION_ADSB` (Table 55), and `ENV_SURVIVAL` explicitly
stops non-essential processing.

### A mode-driven script on the ground instead (STREAM_START at the right time)

What the bench did until now. Lost because it makes the mission depend on
the ground link at the moment the mode changes: a telecommand lost at that
moment would cost snapshots, and TTC-07 asks for "started by the mode
without a ground command".

## Consequences

### Positive

- The 10-minute test runs from one `SET_MODE`: snapshots, statistics,
  roster and the return to `PRE_TEST` follow without an operator (TTC-07,
  GEN-03.3).
- An OBC crash mid-mission does not stop the payload.
- Outside the mission nothing is decoded or recorded; dump1090's CPU is
  free, and the SDR idle, in every other mode.

### Negative

- On the bench without an OBC, adsbd decodes nothing unless
  `decoder = always` is set or `tools/obc_standin` is running. An existing
  `/etc/gama/adsbd.conf` without the key gets `mode`.
- A dump1090 that keeps failing is restarted every second instead of
  backing off to 30 s: about one process start per second while the SDR is
  missing, only in `MISSION_ADSB`.
- If the OBC dies in `MISSION_ADSB` and never comes back, the decoder and
  the snapshots run until a `STREAM_STOP` or the end of the test. Accepted:
  that is the payload doing its job.

### Follow-up required

- `flight/obcd/` (PLANO etapa 2) replaces the stand-in, with the real
  transitions and its own 600 s timeout.
- On the bench, with the RA-02 pair and the SDR: the decoder starting and
  stopping with the mode, and the time from `SET_MODE MISSION_ADSB` to the
  first decoded message.

## Verification

| What | Where | Result |
|---|---|---|
| MISSION_ADSB starts snapshots (5 s) and statistics (30 s); PRE_TEST stops them; UNKNOWN changes nothing | `test_ttcd_core` | pass |
| STREAM_START remains an override until the next mode change | `test_ttcd_core` | pass |
| MISSION_DOWNLINK: statistics then roster, 260 B for 20 aircraft, then exactly one EV_TASK_DONE after the roster has left; none if the mode is left early | `test_ttcd_core` | pass |
| The real adsbd: no dump1090 and no SBS connection before MISSION_ADSB; started on it; kept through UNKNOWN; stopped, SBS closed and nothing counted on leaving it | `test_adsbd_integration` | pass |
| Real ttcd, adsbd and stand-in OBC on the UDP radio: PRE_TEST silent; one SET_MODE puts aircraft on the air; MISSION_DOWNLINK brings roster and statistics down and the OBC back to PRE_TEST | `test_adsb_chain` | pass |
| `make adsb-chain` on a 20 s synthetic capture | by hand, 2026-10-24 | PRE_TEST → MISSION_ADSB → MISSION_DOWNLINK → PRE_TEST; `downlink_done` with `task_done_sent` |
| On the Pi, with the SDR | — | to do |

## References

- CDR v4 (`docs/cdr/`), Table 55 and Figure 44; TTC-07; GEN-03.3.
- ADR-0005 (fault isolation), ADR-0013 (the IPC messages this uses).
- PLANO.md, etapa 1.2; decisions D1, D2, D5.
