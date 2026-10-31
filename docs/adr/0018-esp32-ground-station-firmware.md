# ADR-0018: The ESP32 ground station runs the bench ground's console, and speaks its language over USB serial

- **Status:** Proposed
- **Date:** 2026-10-31
- **Requirements:** HLR-COMM-01, HLR-COMM-03, HLR-SW-02, HLR-ADS-08; CDR TTC-16
- **Deciders:** TT&C team

## Context

The ground station for the on-site tests is an ESP32 DevKit V1 with an RA-02
(CDR §11.3.8; PLANO D11: it sends the remote commands of the tests). Until
now the only ground was `tools/gs_cli` on a Raspberry Pi with a second RA-02,
which ran every bench step of 2026-10-09. Its output — one JSON object per
event — is what the bench scripts (`tools/bench/run_step.sh`), the downlink
adapter (`ground/host/tracks_to_ndjson.py`) and the analyses read, byte for
byte.

Constraints:
- The ESP32 has no wall clock, and the ground's clock is what anchors the
  satellite's (ADR-0009). A `SET_TIME` sent without one carried eight zero
  bytes, which `ttcd` accepted as 1970.
- The ground must answer the satellite within its reply gap, about 20 ms
  (ADR-0007). A serial line at 115200 baud is 11.5 kB/s; one TM_TRACKS line
  is about 2 kB.
- One person runs the whole TT&C; there is no time for a second ground
  implementation, nor for a second set of bench scripts.

## Decision

- **One console, two grounds.** Everything portable in `gs_cli` moves to
  `ground/console/gs_console.{h,c}`: the link's ground side, the command
  parser, the wait for each command, the PER measurement, and the JSON lines.
  `gs_cli` keeps only its radio, epoll and stdin; the ESP32 firmware
  (`ground/esp32`) keeps only the RA-02 over ESP-IDF's SPI and GPIO, and
  UART0. The SX1278 driver is the satellite's own (`flight/radio/sx1278.c`),
  listen-before-talk included (`sx1278_try_transmit`).
- **The serial protocol is the bench tool's**: the same commands, one per
  line, and the same JSON lines back. A setup phase comes first — `set clock
  MS`, `set profile|power|lbt|auto_time`, then `begin` — which `gs_cli` maps
  its options onto and the host sends over the serial port. `quit` prints the
  summary and restarts the board, so each session starts fresh, like a new
  `gs_cli` process.
- **The host side is a bridge, `ground/host/gs_serial.py`**, standard library
  only. It resets the board, sends this computer's clock and the options,
  feeds one command per echo, and prints only the JSON lines. With
  `GS=esp32 ESP32_PORT=...` every `make bench-*` step runs unchanged with the
  ESP32 as the ground. It is also the operator CLI of PLANO 3.2: typed
  commands go through it the same way.
- **No wall clock, no `SET_TIME`.** The console refuses `time` and
  `--auto-time` until a clock is set, and `ttcd` refuses a time before
  2026-01-01.
- **The ground never waits for the UART.** Lines go into the UART driver's
  16 KB ring buffer; one that does not fit is counted and dropped, never
  waited for.
- **Built with Docker**, in the `espressif/idf:release-v5.3` image, by
  `tools/esp32/idf.sh`; the shared code compiles by path, with the project's
  own warnings (`-Wconversion -Wpedantic -Werror`).
- **The portable test suites run on the ESP32** (`ground/esp32-tests`): in
  the image's QEMU now, on the board at the bench (TTC-16).

## Rationale

1. **Byte-identical output is a property of the code, not of care.** With one
   console there is nothing to keep in step; `tests/test_gs_console.c` pins
   the lines, and `bench_step_udp` runs a real bench step through
   `run_step.sh`.
2. **The serial link is fast enough with room to spare.** Continuous FAST
   streaming of 20 aircraft needs about 5.4 kB/s, 47% of 115200 baud; PER
   steps about 2.5 kB/s. The ring buffer absorbs bursts; a faster rate is a
   configuration change if the drop counter ever moves.
3. **The QEMU run is real evidence for the codec**: the same files compiled
   by the ESP32's GCC 13 for Xtensa, run on an emulated Xtensa CPU, with the
   same number of checks as on the PC (`make esp32-tests-qemu`, 2026-10-31:
   11 suites, 0 failed). The board run at the bench completes TTC-16.

## Alternatives considered

### A separate ground protocol (binary frames over serial, or a host program owning the link)

The host would run `gs_link` and the ESP32 would be a radio modem. Lost
because the ground's reply gap would then include the USB round trip, and
because the bench scripts would need a second ground backend.

### The ESP32 keeping its own clock (SNTP)

Would need Wi-Fi at the venue. The host's clock, sent at every session, is
simpler and is the clock the operator sees.

### Installing ESP-IDF on each machine

The Docker image pins the toolchain for everyone, the CI included.

## Consequences

### Positive

- The bench procedure, its scripts and its analyses work with either ground.
- Three defects of `gs_cli` were found and fixed in the move: the twelfth
  track record lost when fields were wide, unescaped JSON from the air or the
  serial port, and `SET_TIME` without a clock.
- The ground station firmware is about 230 KB, with 78% of its partition
  free.

### Negative

- The board resets at every session; a session cannot be resumed after the
  bridge exits.
- A line dropped on a full UART ring is gone (counted at the end of the
  session as `"ev":"uart"`).
- `release-v5.3` is a moving Docker tag; the `boot` line records the IDF
  version, and a reproducible build should pin the image digest.

### Follow-up required

- Bench: the six steps with `GS=esp32`, compared with the gs_cli runs of
  2026-10-09; the suites on the board (`make esp32-tests-flash`).
- On site: the ESP32 at 14 dBm (its Kconfig default), with the team's
  antenna.

## Verification

| What | Where | Result |
|---|---|---|
| Lines pinned byte for byte; 12 widest track records; escaping; setup phase; no `SET_TIME` without a clock | `tests/test_gs_console.c` | pass |
| A real bench step through `run_step.sh`, every ground line JSON | `bench_step_udp` (CTest) | pass |
| `ttcd` refuses a time before 2026 | `test_ttcd_core` | pass |
| The bridge's protocol against a fake board (echo flow, JSON only, reboot detected) | `ground/host/test_gs_serial.py` | pass |
| Firmware builds with the project's warnings, zero warnings | `make esp32-build` | pass |
| The 11 portable suites on an emulated ESP32 | `make esp32-tests-qemu` | 11 run, 0 failed |
| The suites on the DevKit; the six bench steps with the ESP32 as ground | bench | to do |

## References

- `ground/console/gs_console.h`, `ground/esp32/README.md`,
  `ground/host/gs_serial.py`.
- ADR-0002 (one codec), ADR-0007 (the link protocol), ADR-0009 (time).
- CDR §11.3.8 (ground station), TTC-16.
