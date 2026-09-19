# ADR-0005: Separate processes for telecom and payload, with ttcd as the hub

- **Status:** Accepted
- **Date:** 2026-09-19
- **Requirements:** HLR-GEN-03, HLR-COMM-01, HLR-ADS-05, HLR-SW-01
- **Deciders:** TT&C team (LoRa and ADS-B sub-teams)

## Context

The Raspberry Pi Zero 2 W runs everything onboard: the OBC, the ADS-B
decoder and the radio link. Its resources are modest — four Cortex-A53 cores
at 1 GHz and 512 MB of RAM — and `dump1090-fa` alone will take a significant
share of one core (ADR-0008).

The software is written by three groups: the OBC team, in its own repository;
the ADS-B sub-team; and the LoRa sub-team. The last two share this
repository.

What must keep working when something else fails is not symmetric. **The
radio is the only way to diagnose and recover the satellite.** A crash in the
ADS-B parser is a degraded mission; a crash that takes the radio with it is a
lost one.

## Decision

Four processes, each supervised by systemd:

| Process | Owns | Written by |
|---|---|---|
| `ttcd` | the LoRa radio, the ground protocol, telecommand dispatch, telemetry scheduling | LoRa sub-team |
| `adsbd` | the ADS-B chain: starts and supervises `dump1090-fa`, keeps the track table, writes the NDJSON log | ADS-B sub-team |
| `dump1090-fa` | the SDR; demodulation and 1090ES decoding | upstream, child of `adsbd` |
| `obc` | mission modes and platform sensors | OBC team, other repository |

**`ttcd` is the hub.** It listens on one IPC socket (ADR-0006); `adsbd` and
the OBC connect to it. They never talk to each other directly.

**The OBC is the single source of truth for the mission mode.** `ttcd` does
not keep a copy of the mode state machine. It translates ground telecommands
into OBC events, and it reports the mode the OBC announces.

**`ttcd` is built as a pure core inside a thin shell.** The core — link state
machine, telecommand dispatch, telemetry scheduler — makes no system calls. It
receives events stamped with a time (a frame arrived, a transmission ended, a
timer expired) and acts through a small table of operations (transmit this
frame, change profile, send this IPC frame). The shell owns the file
descriptors, the `epoll` loop and the clock, and translates between the
operating system and the core.

## Rationale

**Fault isolation where it matters.** A segmentation fault in the SBS parser
kills `adsbd`; systemd restarts it, and `ttcd` keeps transmitting housekeeping
and answering telecommands the whole time. In a single process, the same
fault would silence the radio. Separate processes are the cheapest isolation
Linux offers.

**The boundary matches the team split.** The two sub-teams work in parallel
against one contract — the `IPC_TRACKS` and `IPC_STAT` payloads — and each can
test its side without the other: `adsbd` against a recorded SBS stream,
`ttcd` against a fake `adsbd`.

**One hub, one socket, one protocol.** A star with `ttcd` at the centre needs
one listening socket and one client implementation per peer. A mesh would
need the OBC to talk to `adsbd` as well, doubling what the other team must
integrate.

**No second copy of the mode machine.** Two state machines describing the same
modes drift apart; the day they disagree, the satellite reports one mode and
behaves as another. The OBC owns modes; `ttcd` owns the link.

**The pure core makes the protocol testable before the hardware.** The link
protocol's properties — telecommand response time, recovery from a lost
acknowledgement, fallback to the safe profile — depend on timing. With the
clock injected, the core can be driven through hours of simulated link time in
milliseconds, against a simulated ground station on a simulated half-duplex
channel, deterministically. On real hardware those scenarios take hours and
cannot be made to repeat.

## Alternatives considered

### One multi-threaded process

Simpler to deploy, no IPC. Rejected: any memory error in any thread kills the
radio with it, and the two sub-teams would share one binary and one release
cycle. The IPC it saves is a solved problem (ADR-0006).

### `adsbd` inside the OBC

Rejected in ADR-0008: the OBC's single-threaded 1 Hz loop cannot host a
decoder, and the payload is not the OBC team's deliverable.

### A mesh: every process talks to every other

Rejected. More sockets, more client code in the OBC, and no message that needs
it: everything the OBC needs from the payload is a quantity `ttcd` already
aggregates for telemetry.

### A conventional event loop with the logic inside the handlers

The usual way to write a daemon. Rejected for `ttcd` because its logic would
then only be testable in real time, with real sockets and timers. The shell
still exists, but it is kept small enough to verify by reading, and it is
exercised end to end by an integration test.

## Consequences

### Positive

- The radio survives payload crashes.
- Each sub-team can test alone, and the link protocol can be tested without
  hardware.
- The OBC integrates one client, for one peer.

### Negative

- **Four processes to configure, start and supervise.** Each needs a systemd
  unit, and their start order matters: `ttcd` must be listening before its
  clients connect, and clients must retry.
- **IPC adds a hop** between reception of an ADS-B message and its
  transmission. It is microseconds against a 5 s snapshot period, but it is
  real.
- **The core/shell split is a discipline**, not something the compiler
  enforces. A system call added to the core breaks testability silently; code
  review must catch it.

### Follow-up required

- systemd units for `ttcd` and `adsbd` with `Restart=`, and clients that retry
  their connection.
- An integration test that runs the real `ttcd` binary, not just its core.

## References

- ADR-0006 — the IPC transport
- ADR-0008 — ownership of the ADS-B chain
- `obc/src/fsm.h` — the OBC's mode state machine
