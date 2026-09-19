# ADR-0008: The payload daemon owns the SDR, through dump1090-fa

- **Status:** Accepted
- **Date:** 2026-09-19
- **Requirements:** HLR-ADS-01, HLR-ADS-02, HLR-ADS-03, HLR-ADS-04, HLR-ADS-07
- **Deciders:** TT&C team (ADS-B and LoRa sub-teams), recorded at the OBC team's request

## Context

Two incompatible designs for the ADS-B receive chain existed in the two
repositories:

- **OBC, branch `feat/modulo-aocs`** (`src/drivers/sdr.c`, `sdr.h`): the OBC
  process opens `/dev/ttyACM0` at 115200 baud and counts lines starting with
  `MSG,`. Its header comment states the assumption explicitly: the receiver
  is "o modulo USB proprio da equipe", "nao e um dongle SDR generico", and
  "o modulo ja entrega os frames 1090ES decodificados por uma porta serial
  sobre USB" — a receiver that decodes in its own firmware.
- **ttec, `adsb_capture.c`**: a separate process forks `dump1090-fa`, which
  demodulates and decodes 1090ES in software from an RTL-SDR, and reads the
  decoded messages from its SBS-1 port on loopback.

Only one process can own the physical receiver, and the two designs disagree
about which process that is and about whether decoding happens in hardware or
in software.

The hardware settles the second question. The team's receiver is a **NooElec
NESDR Nano 3**: an RTL2832U with an R820T2 tuner. It is a general-purpose SDR
that delivers raw IQ samples over USB. It does not decode anything, and it
does not enumerate as `/dev/ttyACM0`. The module `sdr.c` was written for is not
part of the system.

The team notes (`docs/anotacoes_cubedesign2026.pdf`) assign HLR-ADS-01 to 03
and HLR-ADS-05 to the ADS-B sub-team of TT&C, and ADR-0003 places decoding and
aggregation onboard.

## Decision

The ADS-B receive chain belongs to **`adsbd`**, a TT&C process separate from
both the OBC and `ttcd` (ADR-0005):

```
antenna (1090 MHz) → NESDR Nano 3 → dump1090-fa → SBS-1, 127.0.0.1:30003 → adsbd
```

- `adsbd` starts `dump1090-fa` as a child process, supervises it, and
  restarts it if it dies, counting every restart (`gama_stat_t.dump1090_restarts`).
- `adsbd` keeps the per-aircraft track table and writes every decoded message
  to the onboard NDJSON log (HLR-SW-02, ADR-0003).
- `adsbd` sends track snapshots and payload counters to `ttcd` over IPC
  (`GAMA_FRAME_IPC_TRACKS`, `GAMA_FRAME_IPC_STAT`, ADR-0006).
- **The OBC does not open the SDR.** It receives the ADS-B message count, like
  every other payload quantity, through `ttcd`. The `sdr.c` driver in
  `feat/modulo-aocs` is superseded; this record and the OBC↔TT&C ICD are how
  the OBC team is told.

## Rationale

1. **The hardware that `sdr.c` assumes does not exist in this system.** This
   alone rules the OBC-side design out, independently of any preference.
2. **Software decoding is the only option with the procured receiver**, and
   `dump1090-fa` is the reference open-source 1090ES decoder: CRC checking,
   single-bit error correction, CPR position decoding and a documented SBS-1
   output, none of which we have the schedule to write or validate ourselves.
3. **Ownership follows the team split.** The payload is the ADS-B sub-team's
   deliverable; placing it in their process lets them develop and test it
   against a recorded SBS stream without the OBC or the radio.
4. **The OBC's cooperative loop cannot host it.** The OBC runs one thread at
   1 Hz (`obc/src/main.c`). A decoder consuming 2.4 MS/s cannot live inside
   that loop, and a child process supervised from it would couple the OBC's
   health to the SDR's.

## Alternatives considered

### The OBC owns the SDR and runs dump1090 itself

Rejected for reasons 3 and 4 above: it moves the payload into another team's
process and couples the mission-mode state machine to a USB device that can
disconnect.

### A dedicated hardware ADS-B decoder module

The design `sdr.c` assumed. It would remove the largest unmeasured risk in the
architecture — `dump1090` CPU load on a Pi Zero 2 W — because decoding would
happen off the Pi. Rejected because it is not procured and the schedule does
not allow procurement and integration before the Design Package. **It is the
fallback** if the CPU measurement in PLANO phase 3.4 shows `dump1090` cannot
run at an acceptable sample rate; `adsbd` already consumes SBS-1, so the
change would be confined to where the lines come from.

### A different software decoder

`readsb`, a maintained fork of `dump1090`, is the obvious candidate if
`dump1090-fa` proves too heavy. Not chosen now because nothing has been
measured yet; it produces the same SBS-1 output, so switching would not change
`adsbd`'s interface.

## Consequences

### Positive

- One owner and one data path for the payload; no contention for the device.
- The payload can be tested without SDR hardware by replaying a recorded SBS-1
  stream into port 30003 (PLANO phase 3).
- The OBC loses a driver it would otherwise have had to maintain.

### Negative

- **CPU on the Pi Zero 2 W is the risk this decision accepts.** `dump1090` at
  2.4 MS/s competes with `ttcd` for four Cortex-A53 cores. It is unmeasured.
- **Timestamps are arrival times on the SBS socket, not RF reception times.**
  Adequate for HLR-ADS-07 at the resolution the downlink carries (100 ms); if
  a reviewer requires the receiver's own clock, `dump1090`'s Beast output
  carries a 12 MHz timestamp per message.
- A dependency on `dump1090-fa` being built and installed on the flight image.
- The OBC team must drop work already done on `feat/modulo-aocs`.

### Follow-up required

- Measure `dump1090` CPU and decode rate on the target and record the result
  in `docs/budgets/data-budget.md` (PLANO phase 3.4). If a core saturates,
  drop to 2.0 MS/s; if that is still not enough, revisit this record.
- State in the OBC↔TT&C ICD that the OBC receives ADS-B quantities only
  through `ttcd`.

## References

- `adsb_capture.c` — the prototype `adsbd` is built from
- `obc`, branch `feat/modulo-aocs`, `src/drivers/sdr.h` — the superseded design
- ADR-0003 — the processing split that places decoding onboard
- ADR-0005 — process architecture
- `docs/requisitos.md` — HLR-ADS-01 to 08 with their current status
