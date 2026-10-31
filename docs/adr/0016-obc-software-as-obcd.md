# ADR-0016: The OBC software is obcd, a third process built like ttcd and adsbd

- **Status:** Proposed
- **Date:** 2026-10-24
- **Requirements:** HLR-SW-01, HLR-GEN-02, HLR-GEN-03, HLR-COMM-02, HLR-EPS-04, HLR-ADS-05
- **Deciders:** TT&C team (now also the OBC's)

## Context

Since 2026-10-20 the OBC software is written in this repository (PLANO,
etapa 2). The previous one (`../obc`, frozen) defined the mode and event
enums this repository already pins (`common/gama_tc.h`) and the transitions
of the CDR's Figure 44, but its structure is what the hard rules of
`AGENTS.md` forbid: a main loop that sleeps 1 s per cycle, wiringPi, paths
and I2C addresses compiled in, a radio driver of its own whose failure
stopped the OBC, and an INA219 reading that returned 0 V when it failed.

What the OBC must do (CDR Table 32, PLANO D1-D9): run the six modes with
`PRE_TEST` as the hub; fire the burn-wire once on entering
`BASIC_INTERMEDIATE`; end `MISSION_ADSB` after 600 s; drive the battery
heaters with a thermostat in every mode; report voltage, current and battery
temperature every second; boot into `PRE_TEST`; never wait in its main loop —
and the DS18B20 alone takes ~750 ms per reading.

## Decision

`flight/obcd/` is a third daemon, with the structure of `ttcd` and `adsbd`:

- **A pure core** (`core.c`): the transition table of Figure 44 in one
  function, `obcd_transition`; entry and exit actions (burn-wire, deadlines);
  the thermostat; the telemetry it sends `ttcd`. No system calls (ADR-0005):
  time and sensor readings arrive as events, outputs leave through an ops
  table. Tested on a virtual clock, every edge of the figure.
- **An epoll shell** (`main.c`): `timerfd` at the core's next deadline,
  `signalfd`, an IPC client of `ttcd` that reconnects, a JSON-lines log (D7),
  and systemd's notify and watchdog protocol (D8), without libsystemd.
- **Every sensor read in a thread of its own** — the DS18B20 (D9) and the
  INA219 too — delivering readings to the loop through a pipe. The main loop
  waits only in `epoll_wait`.
- **Hardware only through kernel interfaces** (ADR-0012): `i2c-dev` for the
  INA219, the GPIO character device for the burn-wire and the heaters, the
  kernel's `w1_therm` file for the DS18B20. No pin, bus or address is
  compiled in; a sensor or an output not configured is absent, and logged.
- **Refused events are refused.** An event not valid in the current mode
  changes nothing and is logged as `event_refused` (D1).
- **The AOCS gets a deadline now and its I2C contract later** (PLANO 2.4):
  `ADVANCED_AOCS` ends on an `EV_TASK_DONE` posted through
  `obcd_post_event`, or at `aocs_timeout_ms`, whichever comes first.

`tools/obc_standin`, which played the OBC for ADR-0014, is removed: the
tests and `make adsb-chain` run the real `obcd`.

## Rationale

1. **One architecture for three daemons.** The pure-core pattern is what
   lets `ttcd`'s link protocol be tested in simulation; here it gives the
   same for the mode machine, whose correctness is HLR-SW-01. Every edge of
   Figure 44 and every refusal is a unit test, not a bench session.
2. **The INA219 in a thread too.** Its transfers take about a millisecond,
   but on the boards as fabricated it shares the bus with the AOCS ESP32,
   which becomes the bus master during its task (PLANO 2.4). A transfer that
   meets a held bus waits for the i2c driver's timeout; in the main loop that
   would stall the thermostat and every mode transition. One thread per
   sensor also keeps a stuck DS18B20 from starving the INA219, and the other
   way round.
3. **Failed sensors say so.** A reading that fails is not a reading: the
   telemetry carries "no data" (ADR-0013) and the thermostat turns the
   heaters off instead of heating on a stale value. The previous OBC's 0 V
   on a failed read would have reached the ground as a dead battery.
4. **Current from the shunt voltage.** The current register depends on a
   calibration register the chip loses on a brown-out; the shunt voltage
   does not. The resistance is in the configuration, to be measured on the
   new board.
5. **Pins only in the configuration.** The boards as fabricated cannot run
   together (`AGENTS.md`), and the new OBC board's pins are not known.
   `obcd.conf.example` names the fabricated board's pins in comments and
   configures none.

## Alternatives considered

### Port `../obc`

Its state functions and drivers would have had to be rewritten anyway, one
by one, to remove the sleep, wiringPi, the radio and the fixed paths. What
it had of value — the enums, the transitions, the INA219 configuration
word — is kept.

### Mode logic inside ttcd

One process fewer. Lost on ADR-0005's criterion: a fault in the mode logic
or a stuck sensor would take the radio with it, and the radio is the only
way to recover the satellite.

### A blocking main loop with short timeouts

What the previous OBC did, at 1 s per cycle. Lost: rule 3, and the DS18B20
alone would take 75% of every cycle.

## Consequences

### Positive

- The mode machine, the thermostat and the burn-wire are tested without the
  board, and the whole chain — ground, `ttcd`, `adsbd`, `obcd` — runs on a PC.
- On the bench, the drivers run against loose sensors on a Pi by editing
  `obcd.conf`; the new board changes only that file.

### Negative

- Two threads, the first in this repository's flight code. They touch only
  their device and the write end of a pipe.
- A refused `SET_MODE` is still acknowledged OK by `ttcd`, which only
  delivered it; the ground learns of the refusal from the unchanged mode in
  housekeeping, and the reason only from `obcd`'s log.
- If `obcd` dies without passing through its exit path, what the kernel does
  with the released GPIO lines is not yet verified on the Pi. `obcd` turns
  both outputs off on SIGTERM and again on start; systemd restarts it after
  1 s.

### Follow-up required

- PLANO 2.4: the I2C contract with the AOCS ESP32, whose firmware is being
  written; its command on entering `ADVANCED_AOCS`, its result and its
  `EV_TASK_DONE`; roll, pitch and yaw in `IPC_TELEMETRY`.
- Bench: INA219 and DS18B20 breakouts and two LEDs on a Pi, against a
  multimeter and a thermometer (OBC-7); the GPIO lines after a SIGKILL.
- The new OBC board: its pins and shunt into `obcd.conf`.

## Verification

| What | Where | Result |
|---|---|---|
| Every edge of Figure 44 (11), and every other mode-event pair refused | `test_obcd_core` | pass |
| Burn-wire once for 5 s, then EV_TASK_DONE; off when the mode is left early; the mode ends even with no line | `test_obcd_core` | pass |
| MISSION_ADSB ends at 600 s; ADVANCED_AOCS at its deadline or on EV_TASK_DONE | `test_obcd_core` | pass |
| Thermostat 10 → 12 °C with hysteresis, in every mode; off on a failed or stale sensor, logged once | `test_obcd_core` | pass |
| IPC_TELEMETRY every second, "no data" for what was not measured | `test_obcd_core` | pass |
| INA219 against an emulated chip: scaling, sign, rounding, overflow, bus failure | `test_obcd_drivers` | pass |
| DS18B20: the kernel's text, CRC failure, the 85 °C power-on value, a missing file | `test_obcd_drivers` | pass |
| The real binary: HELLO, mode, telemetry from a w1_slave file, thermostat, refusals, burn-wire, reconnection, SIGTERM | `test_obcd_integration` | pass |
| The real obcd, ttcd and adsbd on the UDP radio: one SET_MODE walks PRE_TEST → MISSION_ADSB → MISSION_DOWNLINK → PRE_TEST | `test_adsb_chain`, `make adsb-chain` | pass |
| On a Pi with loose sensors | — | to do |

## References

- CDR v4 (`docs/cdr/`): Figure 44, Table 32 (OBC-1 to OBC-9), Table 55.
- PLANO.md, etapa 2 and decisions D1-D9.
- ADR-0005, ADR-0012, ADR-0013, ADR-0014.
- `docs/relatorios/2026-10-22-validacao-obc.html` — findings A5, A6, A7, A9.
