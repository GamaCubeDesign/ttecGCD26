# ADR-0007: Link protocol — framing, medium access, acknowledgement and rate change

- **Status:** Proposed — implemented, verified in simulation and on two RA-02 (bench of 2026-10-09); awaiting team review
- **Date:** 2026-09-19
- **Requirements:** HLR-COMM-01, HLR-COMM-02, HLR-COMM-03, HLR-ADS-07, HLR-ADS-08, HLR-GEN-03
- **Deciders:** TT&C team (LoRa sub-team)

## Context

ADR-0004 fixed the physical layer: three rate profiles on an SX1278 at
433 MHz. This record fixes what travels over it and when.

Four constraints shape the protocol:

1. **The radio is half-duplex.** While the satellite transmits it cannot hear
   a telecommand; if both ends transmit at once, both frames are lost. The
   previous mission never addressed this: it waited a fixed
   `sleep(Tpkt/1000 + 1)` after each transmission
   (`ultima_missao/satellite/Moden.cpp:91`) and slept ten seconds inside its
   command handlers (`Module.cpp:139`).
2. **HLR-COMM-01 requires a response time "defined by the team".** Until
   this record, none was defined; a fast system with no stated bound does not
   meet the requirement (`docs/requisitos.md`, HLR-COMM-01).
3. **HLR-ADS-08 requires loss and latency to be measured**, so every frame
   must be countable and every track record must carry enough timing to
   compute its latency.
4. **Loss tolerance comes first** (ADR-0003): no frame may depend on another
   frame having arrived.

Three gaps recorded in `docs/requisitos.md` are closed here: the undefined
telecommand bound, the missing rule for when the ground may transmit, and the
undefined meaning of the track record's `age_ds` field.

## Decision

### 1. Framing

The frame of `common/gama_frame.h`, unchanged: `ver | type | seq | len |
payload | crc16`, little-endian, at most 248 bytes of payload, pinned byte
for byte by `tests/test_vectors.c`. Sequence numbers count per direction.
The satellite numbers every frame it transmits; the ground numbers each
telecommand once and **reuses the number when it retransmits**.

### 2. Medium access

**Satellite** (`flight/ttcd/link.c`):

- Listens by default.
- After each of its own frames it stays silent for a **reply gap** long
  enough for the ground's reply to be decoded: 20 ms of ground turnaround, a
  header time and two symbols — **112 ms at NOMINAL**, 43 ms at FAST, 750 ms
  at SAFE. After a profile switch the gap grows by 100 ms, covering the
  ground's settle time.
- **Listens before it talks:** before every transmission it asks the modem
  whether a reception is in progress, and defers if so.
- A valid frame from the ground ends the gap at once, so the acknowledgement
  follows immediately.

**Ground** (`common/gama_gs_link.c`):

- A telecommand the operator is waiting for goes out **as soon as the modem is
  not receiving**, and immediately whenever a satellite frame ends.
- A **keepalive goes only in the reply gap**, which is guaranteed free.
- When a gap opens, a pending telecommand goes at once, whatever backoff it was
  waiting out.

### 3. Acknowledgement and retransmission

- **Every telecommand gets exactly one `TC_ACK`**, carrying a status, on every
  path: unknown command, wrong argument size, out-of-range argument, rejected
  in the current state, failed while executing.
- The ground is **stop-and-wait**: the next telecommand waits for the current
  one's ACK. ACK timeout: the ACK's time on air plus a header time plus
  100 ms — 328 ms at NOMINAL. Retransmission backoff: 250 ms, doubling, capped
  at 2 s. A telecommand is abandoned after 60 s, with an event.
- **At-most-once execution:** the satellite remembers the last telecommand it
  executed; a repeat with the same sequence number and payload within 60 s is
  answered by replaying the ACK, not by executing again.
- **Except `SET_RATE`, which is always executed.** It is idempotent ("be at
  profile p"), and it has to be: if the satellite switched and then reverted
  because the ground never heard the ACK, the ground's retransmission is the
  only thing that can bring the two ends back together. Treated as a
  duplicate, it would be answered and ignored, and the two ends would stay on
  different profiles.
- **`SET_TIME` is re-stamped on every attempt**, so a retransmission carries
  the current time and is executed as new.

### 4. Profile change and contact loss

- `SET_RATE` is **acknowledged at the old profile** and applied when that ACK
  has left.
- The ground follows when it hears the ACK, waits 50 ms for the satellite to
  reconfigure, and confirms with a PING at the new profile.
- The satellite **commits** on any ground frame heard at the new profile, and
  **reverts after 20 s** without one. The ground reverts after 30 s without an
  ACK at the new profile. The ground's timer is longer so that it does not
  give up while the satellite is still waiting to be confirmed.
- **Contact loss:** 120 s without a ground frame drops the satellite to SAFE,
  and 150 s without a satellite frame drops the ground to SAFE. When the ground
  hears the satellite at SAFE, it re-commands the profile it wanted, and the
  link recovers without an operator. The ground sends a keepalive every 30 s
  so that a quiet operator does not trigger this.

### 5. Telemetry schedule

At each transmission opportunity, in priority order: acknowledgements, then
housekeeping (every 10 s), statistics (every 30 s while streaming), the track
snapshot (every commanded period, 5 s by default), and the beacon.
**In SAFE there are no tracks** — one 20-aircraft snapshot would take 23 s of
air time at SF12 — only a beacon every 30 s and housekeeping every 60 s.
Streaming resumes by itself when the link leaves SAFE.

Only complete snapshots are sent; a snapshot is never sent twice.
Housekeeping fields with no data yet read `INT16_MIN`, `battery_mv` reads 0,
and the OBC mode reads `0xFF` while the OBC is not connected.

### 6. Track timing

**`age_ds` on the air is the time from the aircraft's last update to the
start of the transmission carrying it**, in deciseconds, saturating at 65535.
`ttcd` writes it immediately before transmitting, from the epoch `adsbd` puts
on each `IPC_TRACKS` snapshot; both processes read the same monotonic clock.

This makes each record self-contained in time. The ground stamps the arrival
of the frame with its own clock and computes the update time as
`arrival − time on air − 100 ms × age`: trajectories are reconstructed in the
ground's clock domain, with no clock synchronisation. The age is also, record
by record, the HLR-ADS-08 latency from reception to transmission. `ttcd`
reports its p95 in `TM_STAT`, counting each update only at its first
transmission, so that an aircraft left unheard does not look like a slow
pipeline.

### 7. The HLR-COMM-01 bound

**A telecommand is executed by `ttcd` within the time below from the moment
the operator submits it.** Commands executed by the OBC add one OBC cycle,
1 s.

| Profile | No frame lost (B0) | One frame lost (B1) |
|---|---|---|
| SAFE | 4.21 s | 10.89 s |
| **NOMINAL** | **1.40 s** | **3.37 s** |
| FAST | 0.44 s | 1.30 s |

- **B0** is the wait for the longest frame the satellite sends plus the
  telecommand's own time on air.
- **B1** adds one retry cycle: the ACK timeout, the first backoff, and B0
  again.
- Derived by `tools/analysis/lora_budget.py` and checked there by `--check`.
  The simulation checks the implementation against the same bound.

## Verification

`tests/test_link_sim.c` runs the real satellite core against the real ground
link on a simulated half-duplex channel. Each frame occupies the air for its
exact time on air. A collision destroys both frames. A radio sees the other's
transmission only after decoding its header, and only if it was listening
when that transmission began — the pessimistic assumption. Time is virtual
and runs are seeded, so the figures below reproduce exactly
(`./build/tests/test_link_sim`):

| Scenario, NOMINAL | Result |
|---|---|
| Idle link, 400 commands | p50 0.12 s, max 0.83 s; 6 collisions, 6 commands (1.5%) retried; none failed |
| Streaming 20 aircraft, 401 commands | p50 0.21 s, p95 1.78 s, max 2.40 s; first-attempt max 1.27 s (B0 = 1.40 s); 69 collisions, 48 commands (12%) retried; none failed |
| The same, if the modem detects a reception after 5 preamble symbols | 12 collisions, 9 commands (2%) retried; p95 1.11 s |
| Streaming, 10% frame loss each way | p95 2.34 s, max 6.25 s; none failed; every lost downlink frame counted exactly by the ground |
| Streaming, keepalives only, 30 min | 0 collisions, 0 contact losses |
| Onboard latency, reception to transmission | p95 3.2 s (HLR-ADS-08 target 5 s) |
| Rate change, ACK lost | both on FAST after 23.4 s; different profiles for 20.0 s, one satellite revert |
| Rate change, confirmation never heard | both back on NOMINAL; desynchronised 10.0 s |
| 200 s uplink blackout while streaming | back on NOMINAL 58 s after the uplink returned; tracks resumed |
| 300 s blackout both ways | both fell to SAFE; back on NOMINAL 32 s after it ended |
| `SET_MODE`, 100 commands, one ACK lost | exactly 100 OBC events; the retry was answered by ACK replay |
| `SET_TIME`, ACK lost | clock anchor within 0 ms after the retry |

`tests/test_ttcd_core.c` (188 checks) and `tests/test_gs_link.c` (39) pin each
rule on one side at a time.

### On the bench, two RA-02 (2026-10-09)

Both sides on one Raspberry Pi Zero 2 W, radios 0.8 m apart at 2 dBm, RSSI
about −28 dBm (`make bench`; logs and analysis in
`docs/vv/bancada/2026-10-09/`). Not the mission link: the distance and the
team's antennas are still to be measured.

| Step | Result |
|---|---|
| Time on air against `common/gama_lora.c`, 141 frames, 3 profiles | worst error +1.9% (FAST, 11 B); NOMINAL +0.4%, SAFE +0.1% |
| PING, NOMINAL / FAST / SAFE | 1000 / 1000 / 200 acknowledged at the first attempt; mean 276 ms / 81 ms / 2.70 s, inside B0 |
| Downlink | 2220 frames, none lost, none corrupt |
| Listen before talk during the PING runs | the satellite deferred 1174 transmissions while the ground spoke; no collision cost a command |
| Rate change, every pair of profiles | 7 of 7 acknowledged at the first attempt |
| Rate change, ACK lost (ground deaf 3 s) | satellite reverted after 20.000 s; both on FAST after 23.2 s, 12 attempts (simulation: 23.4 s) |
| 400 PINGs with HK every second, header vs preamble detection | 0% retried in both; header stays the default |

The bench found one driver defect the simulation could not: on the real
chip, RegModemStat's "RX on-going" bit stays set throughout RX continuous,
on an empty channel too (0x04 on both modules). Counted as busy, it held
every transmission back; the driver no longer counts it (commit
`72deaa5`).

## Rationale

Every rule above prevents a specific failure, and most were found by the
simulation rather than predicted:

- **The reply gap and listen-before-talk** keep the two ends from talking over
  each other. The first simulation run, before the two fixes below, had 170
  collisions across 401 telecommands while streaming.
- **Transmitting at once when a gap opens** removed most of those. A ground
  that waited out its listen-before-talk backoff started 30–80 ms into the gap,
  too late for the satellite to decode its header before the gap closed.
- **Withdrawing a pending keepalive** when the operator submits a command
  removed head-of-line blocking. Together, the two fixes took the collisions
  from 170 to 72 and the p95 from 2.44 s to 1.78 s.
- **Keepalives only in the gap** keep the most frequent telecommand from ever
  costing a telemetry frame: 0 collisions in 30 minutes.
- **Acknowledging at the old profile and reverting on silence** is what makes a
  rate change survivable when any single frame of the exchange is lost; both
  loss cases are simulated above.
- **Measuring age to the start of transmission** puts the HLR-ADS-08 latency in
  every record, and needs no clock synchronisation.

## Alternatives considered

### Transmit at any time and rely on retransmission (pure ALOHA)

Simplest. Rejected: while streaming, the satellite starts a frame about every
1.5 s (1 865 frames in 46 simulated minutes), and without a reply gap a
telecommand collides whenever it starts within a header time of one. Every
collision also destroys a telemetry frame.

### Every telecommand only in the reply gap

Collision-free, as the keepalives show. Rejected for operator commands: the
wait for the next gap is up to ~3 s while streaming, which would double B0.
Operator commands are rare; the 12% that need a retry stay within B1.

### Scheduled uplink slots announced by the satellite

The classic answer to half-duplex access. Rejected as more machinery than the
problem needs. The reply gap after every satellite frame already is a slot,
opened implicitly without any schedule to keep in agreement.

### No duplicate suppression

Rejected: a lost ACK would make the OBC receive the same mode event twice, and
a `SHUTDOWN` retried after a lost ACK would execute twice.

### Acknowledging `SET_RATE` at the new profile

Rejected: the ground has not switched yet, so it could not hear that ACK.

### An epoch in each `TM_TRACKS` frame, with ages relative to it

Considered first. It costs 4 bytes per frame and puts timestamps in the
satellite's clock domain, so every reconstructed time would depend on clock
synchronisation. Measuring to the start of transmission needs neither.

### Bulk download of the raw log over RF

Deferred. The complete log is 7.34 MiB. Even at FAST and 100% transmit time
that is about 3.4 hours of air time; at the 50% the link allows, about 6.9. It
is retrieved over USB after the mission (ADR-0003). `BULK_START` and
`BULK_ABORT` answer `FAILED` until a narrower use justifies building it.

## Consequences

### Positive

- **HLR-COMM-01 has a number**, derived and verified: 1.40 s without loss and
  3.37 s with one loss at NOMINAL.
- **The link recovers by itself** from a lost ACK, a lost confirmation, and
  outages in either direction, with no operator action.
- **HLR-ADS-08's loss and latency metrics** fall out of the protocol: sequence
  gaps give the loss count exactly, and each record's age gives its latency.

### Negative

- **12% of operator commands need a retry while streaming** under the
  pessimistic detection model (69 collisions in 401 commands). Each collision
  also destroys one telemetry frame. During a 10-minute mission with a handful
  of operator commands, that is one or two frames.
- **Recovery after an uplink outage is slow**: about a minute. The satellite
  drops to SAFE at 120 s, the ground only at 150 s, and then a beacon must be
  heard.
- **The protocol has timers that must agree across two code bases.** They live
  in shared code where possible (`common/`); the rest is pinned by the
  simulation.
- **A tick of 1 ms granularity is assumed.** The shell must deliver timer
  events accurately, which an `epoll` loop with `timerfd` does.

### Follow-up required

- ~~**Bench (PLANO 2.7):** measure how soon the SX1278 reports a reception in
  `RegModemStat`~~ — done 2026-10-09: with "signal detected" (preamble) or
  without it (header), 0% of commands needed a retry; header stays.
- ~~**Bench:** measure time on air against `common/gama_lora.c`~~ — done
  2026-10-09, within 1.9%. The modem reconfiguration time against the 50 ms
  settle is still unmeasured.
- **Bench at mission distance**, with the team's antennas: the frame loss
  rate at 0.8 m is zero and says nothing about the real link.
- Document the frames and payloads for the ground and the Design Package in
  `docs/icd/ota-protocol-icd.md`.
- Revisit bulk transfer only if a specific, smaller product is needed over RF.

## References

- `common/gama_frame.h`, `common/gama_tm.h`, `common/gama_tc.h` — wire format
- `common/gama_gs_link.c` — the ground side
- `flight/ttcd/link.c`, `tc_dispatch.c`, `tm_sched.c` — the satellite side
- `tests/test_link_sim.c` — the simulation behind every figure above
- `tools/analysis/lora_budget.py` — the HLR-COMM-01 bound
- ADR-0003, ADR-0004, ADR-0005
