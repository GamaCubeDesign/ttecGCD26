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
| `flight/obcd/` | OBC daemon: mission modes (Figure 44), INA219 and DS18B20 in their own threads, burn-wire, heaters; pure core (`core.c`) + epoll shell (`main.c`) (ADR-0016). I²C link to the AOCS ESP32 **not yet (PLANO 2.4)** | Pi |
| `ground/esp32/` | Ground station firmware; sends the remote commands of the on-site tests — **not started (PLANO etapa 3)** | ESP32 / ESP-IDF |
| `ground/host/` | Downlink → estimator adapter (`tracks_to_ndjson.py`); origin/destination estimator and dashboard (`ground-aeronaves/`, a teammate's project, merged 2026-09-28); airport DB importer. Operator CLI not started | PC, Python |
| `tools/analysis/` | Budget calculator (`lora_budget.py`), time-on-air from logs (`toa_from_log.py`), requirement quote check, downlink emulation over full captures (`downlink_emulation.py`) | PC |
| `tools/gs_cli/` | Bench ground station: the real `gs_link` on a second RA-02 or the UDP radio | Pi / PC |
| `tools/bench/` | Bench procedure: one `gs_cli` command script per step, and `run_step.sh`, which starts `ttcd` afresh for each; `adsb_chain.sh` | Pi / PC |
| `tools/sbs_replay/` | Plays dump1090: serves SBS lines from a capture or a simulation, so `adsbd` runs without an SDR | Pi / PC |
| `Makefile` | Entry point for build, tests, bench, Pi install and PC→Pi runs (`make help`) | Pi / PC |
| `docs/adr/` | Architecture Decision Records | — |
| `docs/budgets/` | Data and link budgets (figures from `lora_budget.py`) | — |
| `docs/icd/` | Interface contracts (OBC↔TT&C, over-the-air, OBC↔AOCS I²C) — **empty, not written yet** | — |
| `docs/cdr/` | The delivered CDR (Design Package, v4, 2026-10-11): what the project promised. Its LaTeX source is a local, gitignored copy | — |
| `docs/relatorios/` | Dated status reports for the team, in Portuguese (HTML, self-contained) | — |
| `docs/vv/bancada/` | Hardware bench evidence: progress per roadmap stage and the Pi's setup (`README.md`), one folder per day with `notas.md` and the logs `make pi-fetch` brings back | — |
| `hardware/` | KiCad sources of the boards as fabricated (panels of 2026-10-08), one folder per board; `hardware/README.md` | — |
| `ultima_missao/` | Previous competition's code, **frozen**, reference only | — |

**The OBC software is ours since 2026-10-20** and is written here, in
`flight/obcd/`, in the same pure-core + epoll-shell pattern as `ttcd` and
`adsbd`. The other team's repository (`../obc`) is **frozen, reference
only**, like `ultima_missao/`: keep its mode and event enums (already pinned
in `common/gama_tc.h`) and the transitions of the CDR's Figure 44; do not
port its main loop, wiringPi calls or fixed paths. `docs/icd/obc-ttec-icd.md`
documents the interface between two of our own processes.

**The flight stack has known wiring defects.** The boards as fabricated
(`hardware/`) cannot run together: the 8 V path through the OBC's INA219, the
LoRa bus between the OBC and TT&C boards, the OBC's I²C and its TIP120 and
DS18B20 resistors are being fixed on a new OBC board (no date yet). Until it
arrives, pins come only from configuration files, and the `ttcd.conf` pins
(GPIO 20/21) are the bench wiring, not the flight stack. Details:
`docs/relatorios/2026-10-22-validacao-obc.html` and
`docs/relatorios/2026-10-24-proposta-i2c-ina219-obc.html`.

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

Keep the `ttcd`, `adsbd` and `obcd` cores free of system calls (ADR-0005):
no clock reads, no sleeps, no file descriptors in `link.c`, `tc_dispatch.c`,
`tm_sched.c`, nor in `sbs.c`, `tracks.c`, `ndjson.c`, `core.c`, nor in
`obcd/core.c`. Everything a core needs from the world goes through its ops
table (`ttcd_ops_t`, `adsbd_ops_t`, and the same for `obcd`).

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
3. **The OBC's main loop never waits.** It runs the mission modes; one
   blocking call stalls every mode transition and the heaters' thermostat.
   The single slow read, the DS18B20's ~750 ms conversion, runs in its own
   thread (PLANO D9). The previous OBC code slept 1 s per cycle.
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
HK 27 B (protocol v2, ADR-0013), stat 26 B, framing overhead 7 B. Changing
one means revising `docs/budgets/data-budget.md` and probably an ADR.

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

See `PLANO.md`: the decisions of 22–24/10 (D1–D12), the priority order with
dates, what is cut first if time runs short, the open questions and the
risks. As of 2026-10-24:

- **Done in software**: shared codec, IPC, `ttcd`, the ground side of the
  protocol, the SX1278 driver, the channel simulation, the bench tooling,
  `adsbd`, the ADS-B chain without hardware, the downlink adapter and the
  ground estimator; since 2026-10-24 also PLANO etapa 1: protocol version 2
  (ADR-0013), `ttcd` and `adsbd` driven by the OBC mode (ADR-0014),
  433.92 MHz and 14 dBm (ADR-0015); and of etapa 2, `obcd` without the AOCS
  link (ADR-0016). 27 CTest suites, zero warnings, clean under ASan and
  UBSan. None of this has been run on the bench yet.
- **Validated on the bench** (2026-10-07/09, `docs/vv/bancada/`, in
  Portuguese — **update it as each stage closes**): `make check` on the Pi
  (`pi@gamapi.local`, 64-bit Debian 13, reached through `local.mk`); the
  radio bench, all six steps at 0.8 m; dump1090-fa at 21.9% of one core;
  `adsbd` with the real SDR; the chain SDR → `adsbd` → `ttcd` → LoRa →
  ground, without track records (no aircraft position in view).
- **Priority now: finish the firmware before the hardware.** In order:
  (1) ~~protocol version 2, mode-driven `ttcd`/`adsbd`, 433.92 MHz and
  14 dBm~~ — done in software 2026-10-24; (2) `flight/obcd/` — core, shell
  and drivers done 2026-10-24, the AOCS I²C contract (2.4) waits for the
  AOCS firmware, which is being written; (3) the ESP32
  ground station, which sends the remote commands of the tests, and a
  minimal operator CLI; (4) CI and the flight image (`make provision`); then
  the new OBC board's bring-up and the validation of CDR Table 67.
- **Pending answers**: who writes the AOCS ESP32 firmware (the OBC↔AOCS
  contract is I²C); the PCB team's answer on the INA219 on its own I²C bus
  and the date of the new OBC board; the organisers' test order.
- **ADRs**: 0007 and 0012 *Proposed*, with the bench results in their
  verification sections; 0013, 0014 and 0015 *Proposed*, tested without
  hardware, bench pending; 0010 (onboard storage) still to write.
- **The CDR** (`docs/cdr/`) is the reference for what was promised: OBC
  criteria (Table 32), TT&C per mode (Table 55), derived requirements
  TTC-01..16 (Table 65), traceability (Table 66), remaining validation
  (Table 67).
- Requirement status and open gaps: `docs/requisitos.md` (§7 lists the
  actions) — its status is still that of 2026-09-19.

## Working style for this repo

- Numbers, not adjectives. If a decision rests on a calculation, put the script
  in `tools/analysis/` so a reviewer can rerun it. `lora_budget.py --check`
  runs in CTest so the docs cannot drift from the code in silence.
- The schedule is tight (CDR delivered 2026-10-11, team tests until
  2026-11-20, on-site 2026-11-24)
  and the electronics are in hand: Pi Zero 2 W, two RA-02 modules, NooElec
  NESDR Nano 3, ESP32. The antennas (433.92 MHz and 1090 MHz dipoles with LC
  baluns) are still being characterised and will be fabricated by the team;
  COTS antennas serve for the tests meanwhile. Prefer the option that can be
  measured on the bench this week over the one that is theoretically better.
- When reusing `ultima_missao/` code, say explicitly what is kept and what is
  discarded, and why. Several of its defects are documented above and in the
  ADRs; do not reintroduce them.
