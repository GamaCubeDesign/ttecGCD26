# ADR-0023: The satellite reads the emitter category from dump1090-fa's raw output and sends it in two spare bits of the track record

- **Status:** Proposed
- **Date:** 2026-11-18
- **Requirements:** competition rules 4.4.1 (estimate origin and destination, a
  primary objective), HLR-ADS-02 (onboard decoding), HLR-ADS-08 (every rejected
  message counted)
- **Deciders:** TT&C team

## Context

The ground's estimator filters candidate airports by the ADS-B emitter category,
and without it decided no origin on 1 h of real traffic (17/11, ADR-0022).
ADR-0022 recovers most of that on the ground, by inferring a large aircraft from
its kinematics; it cannot tell a light aircraft from a helicopter, and it knows
nothing until the aircraft has flown fast, high or steeply.

`dump1090-fa` decodes the category, but `adsbd` reads only its SBS output (port
30003), which has no category field. `dump1090-fa` also writes the raw messages
it received, one per line, in AVR format (`*8D4840D6202CC371C32CE0576098;`), on
its raw output port. `adsbd` turns that port off today (`--net-ro-port 0`,
`flight/adsbd/config.c`).

The category travels in the ADS-B identification message (DF17 or DF18, type
codes 1 to 4), which an aircraft sends every 5 s or so. The type code gives the
set (4: A, 3: B, 2: C, 1: D), and the 3-bit category field gives the class
within it (A1 light, A2 small, A3 large, A4 high-vortex, A5 heavy, A6 high
performance, A7 rotorcraft; B1 glider, …).

The track record (`common/gama_tm.h`) is 20 B, every byte a field. Its `flags`
byte uses bits 0 to 5 (`GAMA_TRACK_F_POSITION` … `GAMA_TRACK_F_CALLSIGN`); bits 6
and 7 are always 0.

The competition's ADS-B is simulated by the organisers (rules 4.4.4). Whether
their scenarios send identification messages with a category is not known; the
question is asked.

## Decision

1. **An optional second connection.** `adsbd.conf` gains `avr_port`, **0 by
   default**. With 0, nothing changes: no connection, no parsing, the same
   records. With a port (30002), `adsbd` connects to `dump1090-fa`'s raw output,
   on loopback, with the same reconnection as the SBS connection; the
   `dump1090_cmd` then sets `--net-ro-port 30002` instead of 0.
2. **A pure AVR parser** (`flight/adsbd/avr.c`, in the core: no system calls,
   ADR-0005). It takes one line and returns an ICAO address and a category, or a
   reason for refusal. It accepts only 28-hex-digit lines (112-bit messages) of
   DF17 or DF18, checks their CRC-24, and keeps only type codes 1 to 4. Every
   other line is counted by reason (rule 6, HLR-ADS-08): malformed, bad CRC, and
   not an identification message. The last is the common case, not an error.
3. **The category in the track table.** Each track keeps the last class received
   for its ICAO. The class lives and expires with the track; an identification
   message for an ICAO with no track is dropped (and counted), because the
   category alone is worth no record.
4. **Two bits in the track record's `flags`**, bits 6 and 7,
   `GAMA_TRACK_CLASS_*`:

   | Bits 6–7 | Class | ADS-B categories | The ground decodes it as |
   |---|---|---|---|
   | 00 | unknown | none received, A0, B0, sets C and D, B2–B7 | no category (as today) |
   | 01 | light | A1, A2, B1 (glider) | `A1` |
   | 10 | large | A3, A4, A5, A6 | `A3` |
   | 11 | rotorcraft | A7 | `A7` |

   These are exactly the distinctions the estimator uses
   (`ground/host/adsb/estimator/aircraft_category.py`).
5. **The ground** (`ground/host/adsb/tracks_to_ndjson.py`) turns the two bits
   into the `category` field the estimator already reads. A received class takes
   precedence over the kinematic inference of ADR-0022, which remains the
   fallback for 00.

`ttcd`, `obcd` and the ESP32 ground are unchanged: they carry the `flags` byte as
it is. The record stays 20 B and the protocol version stays 2.

## Rationale

- **Zero bytes on the radio.** The data budget (58 KB, `docs/budgets/data-budget.md`)
  and ADRs 0003/0004 are untouched.
- **Backwards compatible on the wire.** Bits 6–7 are 0 in every record sent
  today, and 00 means "unknown", so an old ground reading a new satellite, a new
  ground reading an old satellite, and every vector in `tests/test_vectors.c`
  stay valid. One new vector pins a record with the class set.
- **Off is today's flight.** With `avr_port = 0` the daemon and its output are
  byte for byte today's; `test_adsb_chain` checks that.
- **Measured need.** With ADR-0022 in place on both sides, the true category
  takes the origins decided on the 18/11 validation capture from 6 to 10 (all
  right), and leaves 17/11 at 7 of 7. Light aircraft and helicopters, which the
  inference leaves unknown, are the cases it helps most.

## Verification (2026-11-18, without hardware)

| What | Where | Result |
|---|---|---|
| The parser against the textbook message `8D4840D6202CC371C32CE0576098` (its CRC known from outside), every category's class, DF18 CF, other message kinds, bad CRC, malformed lines | `tests/test_avr.c` | 98 checks |
| The class kept for the track's life, the latest winning, gone with the track, never keeping it alive | `tests/test_tracks.c` | passes |
| Each AVR line counted by reason; the category line in the onboard record; with the port off, the class 0 | `tests/test_adsbd_core.c` | passes |
| The real `adsbd` with a fake dump1090 on both ports: the class in `IPC_TRACKS`, the record line, the AVR port closing and reconnecting; `avr_port` refused unless `dump1090_cmd` opens it | `tests/test_adsbd_integration.c`, `test_adsbd_config.c` | passes |
| `sbs_replay --avr-port` reproduces the textbook message byte for byte | `tools/sbs_replay/test_sbs_replay.py` | passes |
| `make sim` with real traffic: a category reaches the ground's page as received, over the UDP radio | `tests/test_sim.py` (`sim_udp`) | passes |
| 10 minutes of the 18/11 capture through `sbs_replay` → `adsbd` → `ttcd` → UDP radio → ground, with and without the port | `make adsb-chain`, `AVR=1` / `AVR=0` | the same 243 `TM_TRACKS` frames and 0 lost in both; 1 012 identifications applied, 0 rejected, 0 without a track; 1 411 of 1 430 records reached the ground with a class (the other 19: an aircraft's first second) |
| The whole 18/11 capture with the category reduced to the two bits (A2 → light, A5 → large) | the estimator, offline | origin 10 of 10, destination 10 of 12, combined 6 and 3: **identical** to the exact category. The two bits lose nothing the estimator uses |

Still to measure on the Pi: the same chain over LoRa, and the CPU of
`dump1090-fa` and `adsbd` with the raw output open
(`docs/vv/bancada/2026-11-20/roteiro.md`).

## Alternatives considered

### Read `dump1090-fa`'s `aircraft.json`

It has the category already decoded, but it is a file rewritten every second:
file-based IPC with a fixed name, rule 5, and polling a file from the event loop.

### Patch `dump1090-fa` to put the category in the SBS output

A fork to build and keep on the Pi, instead of the FlightAware package. Six days
before the competition, not worth the maintenance.

### The Beast binary output instead of AVR

The same messages with timestamps and signal level, in a binary framing with
escape bytes. AVR is one text line per message, the same shape as the SBS
reader already handles, and the extra fields are not needed.

### A new field or a new record

The category needs 2 bits and the record has them free. A new field would
change the record size, the budget and the wire version.

### Inference on the ground only (ADR-0022)

It stays, as the fallback. It cannot separate light aircraft from helicopters,
and it needs some flight before it decides.

## Consequences

### Positive

- The category reaches the ground when the scenario sends it, at no radio cost.
- The estimator gets light aircraft and helicopters, which the inference cannot
  give.
- One switch turns it off and restores today's flight software exactly.

### Negative

- A second TCP connection and parser in `adsbd`, six days before the
  competition. The CPU cost on the Pi is to be measured (`dump1090-fa` used
  21.9 % of one core on 2026-10-09).
- A3 and A5 become one class; the estimator weighs them almost the same.
- The class appears only after the first identification message, about 5 s
  after an aircraft is first heard.
- It helps only if the organisers' scenarios send identification messages with
  a category.

### Follow-up required

- `tools/sbs_replay`: serve AVR identification messages, with a correct CRC-24,
  from each aircraft's `category` field, so the chain can be tested without an SDR.
- Bench: `avr_port = 30002` in `/etc/gama/adsbd.conf` on the Pi, the chain with
  `sbs_replay` serving AVR and, if the antenna passes, the real SDR; the CPU of
  `dump1090-fa` and `adsbd`.
- If the bench does not close by 2026-11-20, the competition flies with
  `avr_port = 0`.
- The data-budget document notes the two bits; the ICD of the over-the-air
  format, when written, lists them.

## References

- `docs/relatorios/2026-11-18-proposta-categoria-adsb.html`: the proposal, for the team
- ADR-0022: the inference on the ground, the fallback
- `docs/vv/trafego-real/README.md`: the measurements with and without the category
- `common/gama_tm.h`: the track record and its flags
- ICAO Annex 10 Vol. IV and DO-260B: DF17/18, type codes 1–4, the emitter category
