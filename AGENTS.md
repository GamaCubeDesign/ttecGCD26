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
| `flight/ttcd/` | Telecom daemon: pure core (`link.c`, `tc_dispatch.c`, `tm_sched.c`) + epoll shell (`main.c`) | Pi |
| `flight/radio/` | SX1278 driver over an abstract bus; Linux bus (spidev + GPIO chardev); radio backends (sx1278, udp) | Pi |
| `flight/adsbd/` | Payload daemon: supervises dump1090-fa, parses SBS, track table, NDJSON onboard record, snapshots to `ttcd`; pure core (`sbs.c`, `tracks.c`, `ndjson.c`, `core.c`) + epoll shell (`main.c`) | Pi |
| `flight/libipc/` | AF_UNIX SOCK_SEQPACKET transport | Pi |
| `ground/esp32/` | Ground station firmware — **not started (PLANO phase 4.1)** | ESP32 / ESP-IDF |
| `ground/host/` | Downlink → estimator adapter (`tracks_to_ndjson.py`); origin/destination estimator and dashboard (`ground-aeronaves/`, a teammate's project, merged 2026-09-28); airport DB importer. Operator CLI not started | PC, Python |
| `tools/analysis/` | Budget calculator (`lora_budget.py`), time-on-air from logs (`toa_from_log.py`), requirement quote check, downlink emulation over full captures (`downlink_emulation.py`) | PC |
| `tools/gs_cli/` | Bench ground station: the real `gs_link` on a second RA-02 or the UDP radio | Pi / PC |
| `tools/bench/` | Bench procedure: one `gs_cli` command script per step, and `run_step.sh`, which starts `ttcd` afresh for each; `adsb_chain.sh` | Pi / PC |
| `tools/sbs_replay/` | Plays dump1090: serves SBS lines from a capture or a simulation, so `adsbd` runs without an SDR | Pi / PC |
| `Makefile` | Entry point for build, tests, bench, Pi install and PC→Pi runs (`make help`) | Pi / PC |
| `docs/adr/` | Architecture Decision Records | — |
| `docs/budgets/` | Data and link budgets (figures from `lora_budget.py`) | — |
| `docs/icd/` | Interface contracts (OBC↔TT&C, over-the-air) — **empty, not written yet** | — |
| `docs/relatorios/` | Dated status reports for the team, in Portuguese (HTML, self-contained) | — |
| `docs/vv/bancada/` | Hardware bench evidence: progress per roadmap stage and the Pi's setup (`README.md`), one folder per day with `notas.md` and the logs `make pi-fetch` brings back | — |
| `ultima_missao/` | Previous competition's code, **frozen**, reference only | — |

The **OBC lives in a separate repository** (`../obc`), maintained by a
different team. Do not edit it. The contract will be `docs/icd/obc-ttec-icd.md`
(not written yet: it waits on a meeting with the OBC team, questions P1–P14 of
`docs/relatorios/2026-10-02-integracao-obc-ttec.html`); we deliver a reference
implementation (`radio.c`) and they merge it.

## Build and test

```bash
make                  # configure + build in build/ (RelWithDebInfo); -j2 on the Pi
make test             # ctest --output-on-failure; T=regex runs a subset
make check            # before any merge: test, asan (ASan+UBSan, warnings as errors), requirements
                      # on the Pi, asan is UBSan alone: ASan cannot start in its 39-bit address space

make budget           # full budget tables (lora_budget.py)
make budget-check     # verifies the ADRs' figures (lora_budget.py --check, also in ctest)
make requirements     # verifies docs/requisitos.md quotes the rules verbatim

# the whole link without hardware: ttcd on the UDP radio, gs_cli as the ground
make bench-ping RADIO=udp   # one scripted bench step; ttcd is started and stopped for it
make bench-ttcd RADIO=udp   # by hand: the satellite in one terminal...
make bench-gs RADIO=udp     # ...the ground in another: ping 5, rate fast, ...

# a recorded or simulated ADS-B mission through adsbd, ttcd, the UDP radio and the ground
make adsb-chain INPUT=<capture.sbs | record.ndjson>   # real time: 10 min of mission = 10 min
```

The `Makefile` only strings these commands together — `cmake`, `ctest`, the
scripts in `tools/` — and each still works on its own; keep it that way, so
any failure can be reproduced without make. It configures on every `make`
because that refreshes `TTEC_VERSION` (`git describe`), which every `ttcd`
log records. The bench targets drive the two RA-02 on the Pi and the UDP
radio anywhere else, and `make pi-<target>` runs any target on the Pi over
rsync and ssh; both are described in `flight/ttcd/README.md`.

`ctest` includes `test_link_sim` (both protocol sides on a simulated
half-duplex channel, virtual time), `test_ttcd_integration` and
`test_adsbd_integration` (the real binaries, real time) and
`test_adsb_chain` (`adsbd` and `ttcd` together with the ground on the UDP
radio). The protocol figures in ADR-0007 come from `test_link_sim`; if you
change link behaviour, rerun it and update the ADR's verification table.

Keep the `ttcd` and `adsbd` cores free of system calls (ADR-0005): no clock
reads, no sleeps, no file descriptors in `link.c`, `tc_dispatch.c`,
`tm_sched.c`, nor in `sbs.c`, `tracks.c`, `ndjson.c`, `core.c`. Everything a
core needs from the world goes through `ttcd_ops_t` or `adsbd_ops_t`.

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
3. **Anything we expose to the OBC must be non-blocking and pollable.** Its
   `master` runs a single-threaded 1 Hz cooperative loop (its CDR text
   describes threads instead); either way, one blocking call stalls the OBC.
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
8. **The Pi Zero 2 W has no RTC.** Stamp with `CLOCK_MONOTONIC`; wall-clock
   time is monotonic plus an anchor that the ground sends (`SET_TIME`, on every
   contact) and `ttcd` forwards (`IPC_TIME_SET`). Nothing sets the system
   clock (ADR-0009, extending `obc/docs/log_schema.md`).

## Wire format

One codec serves both the radio and the IPC transport, so the path carrying
telecommands to the OBC is exercised by every ground-link test.

```
 ver(1) │ type(1) │ seq(2) │ len(1) │ payload(0..248) │ crc16(2)
```

All little-endian. `tests/test_vectors.c` pins the actual bytes of every
record — **if you change a field's order, width, scaling or endianness, that
test fails, and that is the point.** Never edit a vector to make it pass;
regenerate with `make vectors` (`tools/gen_vectors`) only on a deliberate
version bump, and review the diff byte by byte.

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

See `PLANO.md` for what is done, what is next, and the open risks; the
latest overview is `docs/relatorios/2026-10-06-panorama-ttec-cdr.html`. As of
2026-10-07:

- **Phases 1–3 are done in software**: shared codec, IPC, `ttcd`, the ground
  side of the protocol, the SX1278 driver, the channel simulation, the bench
  tooling, `adsbd`, the ADS-B chain without hardware, the downlink adapter and
  the ground estimator. 23 CTest suites, 3406 checks in C plus 56 Python
  tests, zero warnings, clean under ASan and UBSan.
- **Hardware tests started on 2026-10-07**, following
  `docs/relatorios/2026-10-05-roteiro-testes-raspberry.html` (stages 1–8).
  Progress, the bench Pi's configuration and each day's notes are in
  `docs/vv/bancada/` (`README.md`, `AAAA-MM-DD/notas.md`) — **update them as
  each stage closes**, in Portuguese. Stages 1 and 2 are done: the Pi
  (`pi@gamapi.local`, reached over ssh from this PC through `local.mk`, on
  the PC's own Wi-Fi access point — NetworkManager *shared*, described in
  that README) runs **64-bit** Debian 13 with gcc 14.2, SPI enabled (the OBC
  team chose 64 bits on 2026-10-07; the 2026-10-05 decision of 32 bits is
  superseded), and `make check` passes there with zero warnings. Next: wiring
  the two RA-02 (stage 3), then the phase-2 radio bench (`make bench`,
  procedure in `flight/ttcd/README.md`) and the phase-3 measurements
  (dump1090-fa CPU, gain, decode rate).
- **ADRs**: 0009 accepted 2026-10-05; 0007 and 0012 *Proposed*, to accept
  after the bench; 0010 (onboard storage) still to write.
- **Decided on 2026-10-05, not implemented** (PLANO, "Decisões de 05/10"): one
  protocol version bump (battery temperature in `TM_HK`, dump1090-alive in
  `IPC_STAT`, an ICAO→callsign IPC message for `REQ_ROSTER`); `ttcd` and
  `adsbd` following the OBC mode; a `MISSION_DOWNLINK` proposal to the OBC; CI
  running `make check`; the link ICD now, the OBC↔TT&C ICD after the meeting.
- **Not started**: `ground/esp32/`, the operator CLI, `docs/icd/`, the V&V
  plan in `docs/vv/` (only the bench evidence exists), `docs/conops.md`.
- **The CDR is the competition's Design Package, due 2026-10-11.** Its draft
  chapter is `cdr_tmp/07-eletronics.tex` (untracked); its OBC and payload
  sections still describe a different design, and the TT&C software
  subsection is empty (report of 2026-10-06, §12).
- Requirement status and open gaps: `docs/requisitos.md` (§7 lists the
  actions) — its status is still that of 2026-09-19.

## Working style for this repo

- Numbers, not adjectives. If a decision rests on a calculation, put the script
  in `tools/analysis/` so a reviewer can rerun it. `lora_budget.py --check`
  runs in CTest so the docs cannot drift from the code in silence.
- The schedule is tight (CDR / Design Package 2026-10-11, on-site 2026-11-24)
  and the electronics are in hand: Pi Zero 2 W, two RA-02 modules, NooElec
  NESDR Nano 3, ESP32. The antennas (433.92 MHz and 1090 MHz dipoles with LC
  baluns) are still being characterised and will be fabricated by the team;
  COTS antennas serve for the tests meanwhile. Prefer the option that can be
  measured on the bench this week over the one that is theoretically better.
- When reusing `ultima_missao/` code, say explicitly what is kept and what is
  discarded, and why. Several of its defects are documented above and in the
  ADRs; do not reintroduce them.
