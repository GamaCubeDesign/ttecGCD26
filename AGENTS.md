# AGENTS.md

Telemetry, telecommand and ADS-B payload software for the Gama Cube Design
CubeSat, CubeDesign 2026. Flight software runs on a Raspberry Pi Zero 2 W; the
ground station is an ESP32 running ESP-IDF; the link is a 433 MHz SX1278.

**Talk to the user in Portuguese.** Code, comments and DP-bound documents are
in English — see "Language" below.

## The one number that governs everything

The LoRa downlink carries **58 KB** over the 10-minute mission. The raw ADS-B
stream from 20 aircraft is **7.3 MiB**. That 129:1 gap forces onboard decoding and
aggregation, and it is the reason for most of the architecture. Before
proposing anything that moves bytes over the radio, check it against
`docs/budgets/data-budget.md`.

## Repository map

| Path | What | Target |
|---|---|---|
| `common/` | Shared packet codec: framing, CRC, TM/TC records | compiles on Pi **and** ESP32 |
| `flight/ttcd/` | Telecom daemon: radio, protocol, TC dispatch, TM scheduler | Pi |
| `flight/adsbd/` | Payload daemon: dump1090, track table, NDJSON log | Pi |
| `flight/libipc/` | AF_UNIX SOCK_SEQPACKET transport | Pi |
| `ground/esp32/` | Ground station firmware | ESP32 / ESP-IDF |
| `ground/host/` | Operator CLI, dashboard, report generator | PC, Python |
| `tools/analysis/` | Link and data budget calculator | PC |
| `tools/sim/` | Simulators for testing without hardware | PC |
| `docs/adr/` | Architecture Decision Records | — |
| `docs/icd/` | Interface contracts (OBC↔TT&C, over-the-air) | — |
| `ultima_missao/` | Previous competition's code, **frozen**, reference only | — |

The **OBC lives in a separate repository** (`../obc`), maintained by a
different team. Do not edit it. The contract is `docs/icd/obc-ttec-icd.md`; we
deliver a reference implementation and they merge it.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j4
(cd build && ctest --output-on-failure)

# before any merge
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DTTEC_SANITIZE=ON
cmake --build build-asan -j4 && (cd build-asan && ctest --output-on-failure)

python3 tools/analysis/lora_budget.py          # full budget tables
python3 tools/analysis/lora_budget.py --check  # verifies the ADRs' figures
python3 tools/analysis/check_requirements.py   # verifies docs/requisitos.md quotes the rules verbatim
```

The build must stay at **zero warnings** under `-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Wstrict-prototypes`. This is not cosmetic: the
bug classes this project can least afford — a truncated length field, a sign
lost on a latitude — appear first as implicit conversions.

## Hard rules

These come from concrete defects in `ultima_missao/`. Each cites where.

1. **Never serialise a struct by casting it to `uint8_t*`.** Every multi-byte
   field goes through `common/gama_bytes.h`. The previous mission did
   `tx_send((uint8_t*)&hd, hd.length)` (`Integration.cpp:149`) and its two
   hand-maintained copies of the header had already diverged. ADR-0002.
2. **Never block in an event loop.** No `sleep()`, no blocking `recv`. The
   previous mission had `sleep(10)` inside a command handler
   (`Module.cpp:139`), stalling the whole process. `ttcd` uses `epoll` with
   `timerfd`/`eventfd`/`signalfd`.
3. **The OBC's 1 Hz cooperative loop is single-threaded.** Anything we expose
   to it must be non-blocking and pollable, or it stalls the OBC entirely.
4. **No hardcoded paths.** Config from file plus argv override. The previous
   mission compiled `/home/pedro/adsb/...` into the binary
   (`adsb_capture.c:42-44`).
5. **No file-based IPC with fixed names.** The previous mission's guard checked
   `sensor_data.json` (`Integration.cpp:173`) while the reader opened
   `HealthData.json` (`Integration.cpp:110`).
6. **Every rejected frame is counted, never silently dropped.** HLR-ADS-08
   requires us to quantify the loss rate.
7. **No dynamic allocation in `common/`** and no platform headers there — that
   portability is what makes one wire format impossible to get wrong on only
   one side of the link.
8. **The Pi Zero 2 W has no RTC.** Use `CLOCK_MONOTONIC` with a single
   wall-clock anchor at boot, matching the convention already documented in
   `obc/docs/log_schema.md`.

## Wire format

One codec serves both the radio and the IPC transport, so the path carrying
telecommands to the OBC is exercised by every ground-link test.

```
 ver(1) │ type(1) │ seq(2) │ len(1) │ payload(0..248) │ crc16(2)
```

All little-endian. `tests/test_vectors.c` pins the actual bytes of every
record — **if you change a field's order, width, scaling or endianness, that
test fails, and that is the point.** Never edit a vector to make it pass;
regenerate with `tools/gen_vectors` only on a deliberate version bump, and
review the diff byte by byte.

Record sizes are load-bearing for the data budget: track 20 B, roster 11 B,
HK 25 B, stat 26 B, framing overhead 7 B. Changing one means revising
`docs/budgets/data-budget.md` and probably an ADR.

## Where decisions live

Read `docs/adr/README.md` first. ADR-0003 (processing split) and ADR-0004
(radio configuration) between them determine what the mission can and cannot
do.

The mission requirements, quoted verbatim with their status and gaps, are in
`docs/requisitos.md` — check it before claiming a requirement is met, and
update its status when one changes.

**Accepted ADRs are immutable.** To change a decision, write a new ADR and mark
the old one superseded — do not edit it. Write the record at the moment the
code materialises the decision, not afterwards: the Design Package is
assembled from them.

Cite the HLR requirement identifier (`HLR-ADS-07`, `HLR-COMM-01`, ...) in a
comment at the point of code that satisfies it. The traceability matrix is
built from those.

## Language

| Artifact | Language | Why |
|---|---|---|
| Code, identifiers, comments | English | — |
| ADRs, ICDs, budgets, V&V plan | English | Rules §2 require the DP in English; these feed it nearly verbatim |
| `README.md`, `PLANO.md`, working notes | Portuguese | Team's working language |
| Replies to the user | Portuguese | — |

## Current state

See `PLANO.md` for what is done, what is next, and the open risks. Phase 1
(shared codec, tests, ADRs 0001–0004) is complete. Three ADRs are cited by
accepted documents but not yet written — that debt is listed at the top of
`PLANO.md` and should be cleared before new code lands.

## Working style for this repo

- Numbers, not adjectives. If a decision rests on a calculation, put the script
  in `tools/analysis/` so a reviewer can rerun it. `lora_budget.py --check`
  runs in CTest so the docs cannot drift from the code in silence.
- The schedule is tight (DP 2026-09-27, on-site 2026-11-24) and all hardware is
  in hand: Pi Zero 2 W, two RA-02 modules, NooElec NESDR Nano 3, ESP32. Prefer
  the option that can be measured on the bench this week over the one that is
  theoretically better.
- When reusing `ultima_missao/` code, say explicitly what is kept and what is
  discarded, and why. Several of its defects are documented above and in the
  ADRs; do not reintroduce them.
