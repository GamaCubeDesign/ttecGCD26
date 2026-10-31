# ADR-0013: Protocol version 2 — battery temperature, "no data" values, roster and mode over IPC

- **Status:** Proposed
- **Date:** 2026-10-24
- **Requirements:** HLR-COMM-02, HLR-EPS-04, HLR-ADS-05, HLR-ADS-08, HLR-GEN-03
- **Deciders:** TT&C team

## Context

Version 1 of the wire format (ADR-0007, pinned by `tests/test_vectors.c`)
left five gaps, all of them promised in the CDR as "protocol version 2"
(Tables of the IPC messages and of the telemetry records; TTC-06, OBC-7,
EPS-04.1, GEN-03.3):

1. **Battery temperature never reached the ground.** HLR-EPS-04 is verified
   by telemetry analysis, and the thermal test requires the battery above
   0 °C throughout (REG-26). The OBC already sends `temp_bat_ccel` in
   `IPC_TELEMETRY`, but `TM_HK` (25 B) had no field for it; it existed only
   in `ttcd`'s onboard log.
2. **"No data" was not defined.** `ttcd` reported a missing OBC as
   `INT16_MIN` in the signed fields, but as `0` in `battery_mv` and in
   `adsb_msgs` — both plausible readings. The OBC had no way to say that a
   single sensor had failed (PLANO D6). TTC-06: "a field without data reads
   as unknown".
3. **`GAMA_HK_F_DUMP1090_UP` was never set.** `ttcd` cannot see `dump1090`;
   only `adsbd` can, and `IPC_STAT` had no field to say it.
4. **`REQ_ROSTER` answered `FAILED`.** `adsbd` knows each aircraft's
   callsign, but no IPC message carried it to `ttcd`. The downlink phase
   (CDR Table 55) depends on it: the roster of the aircraft observed and the
   final statistics, 260 B and 1.4 s on the air.
5. **`adsbd` could not know the mission mode.** The OBC tells only `ttcd`
   (`IPC_MODE`), yet the decoder must run only in `MISSION_ADSB`
   (PLANO 1.2, TTC-07).

Every one of these changes a payload that both ends must agree on, so they go
together, in one version bump: one set of vectors to review, one moment at
which a version-1 peer stops being understood.

## Decision

`GAMA_FRAME_VERSION` and `GAMA_IPC_VERSION` both become **2**. The frame
header is unchanged. All fields little-endian.

**`TM_HK`, 27 B (was 25).** Bytes 0..13 are the OBC's block, in the order and
encoding of `IPC_TELEMETRY`; bytes 14..26 are `ttcd`'s.

| off | len | field | type | unit | no data |
|---|---|---|---|---|---|
| 0 | 2 | `battery_mv` | u16 | mV | `0xFFFF` |
| 2 | 2 | `current_ma` | i16 | mA, negative = charging | `0x8000` |
| 4 | 2 | `temp_bat_ccel` (new) | i16 | 0.01 °C | `0x8000` |
| 6 | 2 | `temp_ext_ccel` | i16 | 0.01 °C | `0x8000` |
| 8 | 2 | `roll_cdeg` | i16 | 0.01° | `0x8000` |
| 10 | 2 | `pitch_cdeg` | i16 | 0.01° | `0x8000` |
| 12 | 2 | `yaw_cdeg` | i16 | 0.01° | `0x8000` |
| 14 | 2 | `temp_soc_ccel` | i16 | 0.01 °C | `0x8000` |
| 16 | 4 | `adsb_msgs` | u32 | decoded, cumulative | `0xFFFFFFFF` |
| 20 | 1 | `obc_mode` | u8 | `gama_obc_state_t` | `0xFF` |
| 21 | 1 | `link_state` | u8 | `gama_link_state_t` | — |
| 22 | 4 | `uptime_s` | u32 | s | — |
| 26 | 1 | `flags` | u8 | `GAMA_HK_F_*` | — |

**"No data" values** (`common/gama_tm.h`): `GAMA_NO_DATA_I16 = INT16_MIN`,
`GAMA_NO_DATA_U16 = 0xFFFF`, `GAMA_NO_DATA_U32 = 0xFFFFFFFF`. The OBC writes
them field by field in `IPC_TELEMETRY` (its size is unchanged, 14 B) for any
sensor it could not read; `ttcd` copies them through, and writes them itself
for every field whose source is not connected.

**`IPC_STAT`, 13 B (was 12).** A `u8 flags` at offset 12; bit 0,
`GAMA_IPC_STAT_F_DUMP1090_UP`, means dump1090 is running and `adsbd`'s SBS
connection to it is open. `ttcd` reports it as `GAMA_HK_F_DUMP1090_UP` while
`adsbd` is connected.

**`IPC_ROSTER`, new, type `0x37`, `adsbd` → `ttcd`, 11·N B, 1 ≤ N ≤ 22.** N
roster records exactly as `TM_ROSTER` carries them (u24 ICAO, 8 bytes of
space-padded callsign), no header. `adsbd` sends a record when it learns a
callsign or the callsign changes, and every record it holds when the
connection to `ttcd` comes up. `ttcd` keeps a table of up to 128 aircraft,
updated by ICAO address, for the whole mission; `REQ_ROSTER` sends it in
frames of 22 records, or one frame with none when no callsign has been heard.

**`IPC_MODE`, type `0x33`, now also `ttcd` → `adsbd`.** The same 1-byte
payload. `ttcd` forwards each `IPC_MODE` from the OBC, sends the current mode
when `adsbd` connects, and sends `GAMA_OBC_ST_UNKNOWN` when the OBC
disconnects.

## Rationale

1. **The OBC's block is the `IPC_TELEMETRY` payload, byte for byte.** A
   version bump re-reads every offset anyway; putting the battery temperature
   where the OBC already sends it makes `TM_HK[0..13] == IPC_TELEMETRY` a
   property a test can check (`test_vectors`, `test_ttcd_core`), and leaves
   one field order for the OBC, `ttcd` and the ground to know instead of two.
   The only decoder of `TM_HK` today is `tools/gs_cli`; the ESP32 ground is
   not written yet.
2. **No air time is spent.** The housekeeping frame grows from 32 to 34 B.
   At SF9/BW125/CR4:5 both are 48 symbols, 0.247 s
   (`lora_budget.time_on_air`), so the allocation of `data-budget.md` §5 is
   unchanged; the volume grows by 2 B × 60 = 120 B in 10 minutes.
3. **Each "no data" value is one no real reading reaches.** 65.535 V on a 2S
   battery, −327.68 °C or ° on a temperature or an angle, 2³²−1 messages in
   10 minutes. Zero is not one: 0 mV is what a disconnected battery reads.
   `obc_mode` already used `0xFF`.
4. **A self-contained roster record needs no reassembly.** Each record names
   its own aircraft, so a record received twice, or out of order, does no
   harm. That is what lets `adsbd` resend everything on every reconnection
   without coordinating with `ttcd`, and a restarted `ttcd` recover the
   mission's roster in one second. The 260 B of the downlink phase
   (CDR Table 55): 20 records = 220 B, one 227 B frame, 1.13 s at SF9, plus
   the 33 B `TM_STAT`, 0.25 s.
5. **The mode is the OBC's message; its forwarding is not a new one.** CDR
   Table of the IPC messages already lists `IPC_MODE` as
   "obc → ttcd → adsbd". A second type would carry the same byte.

## Alternatives considered

### Append `temp_bat_ccel` at offset 25, keep the rest in place

Saves re-reading the offsets in `gs_cli`. Lost because the version changes
anyway, so the one decoder has to be reviewed regardless, and it would leave
the battery temperature apart from the voltage and current it is read with,
and the OBC's fields in a different order on the air from the order on IPC.

### Keep `0` as "no data" for `battery_mv` and `adsb_msgs`

The version-1 behaviour. Lost because a zero is a plausible reading and would
let a missing sensor pass for a real one, which is what TTC-06 forbids.

### A roster snapshot with `index`/`count`, like `IPC_TRACKS`

`adsbd` would send its whole table periodically and `ttcd` would replace its
copy when complete. Lost because the roster is cumulative, not a state in
time: an aircraft that left the track table must stay in it, so `adsbd` would
keep a mission-long list either way, and a snapshot of it re-sent every
period costs IPC traffic for no gain over sending what changed.

### A new IPC type for the mode forwarded to `adsbd`

Lost: same payload, same meaning, and the CDR already documents one message
on both hops.

## Consequences

### Positive

- HLR-EPS-04 can be verified from telemetry received on the ground, every
  10 s, in every mode (OBC-7, EPS-04.1).
- Every `TM_HK` field without data is unambiguous on the ground (TTC-06).
- The ground can tell "adsbd is up but dump1090 is not" — the SDR missing or
  failing — from a working payload, in housekeeping.
- `REQ_ROSTER` works, and the downlink phase of PLANO 1.2 has its data.
- `adsbd` knows the mission mode; PLANO 1.2 can act on it.

### Negative

- Version-1 and version-2 peers do not understand each other. Every frame is
  rejected by version and counted (`frames_rx_bad`), so a mixed bench fails
  visibly, not silently. The ground station and the Pi must be updated
  together; bench logs recorded before this change (`docs/vv/bancada/`) are
  version 1.
- `ttcd`'s state grows by 128 × 11 B for the roster table, and `adsbd`'s by
  128 entries of 16 B. Both are fixed and allocated at start.
- A roster longer than 128 aircraft is cut there. It is counted
  (`roster_full`, logged once) on both sides; the mission sizing is 20.

### Follow-up required

- PLANO 1.2: `adsbd` runs dump1090 only in `MISSION_ADSB`; `ttcd` sends the
  roster and the final statistics in `MISSION_DOWNLINK` and then
  `EV_TASK_DONE` to the OBC.
- The OBC (`flight/obcd/`, PLANO etapa 2) writes the "no data" values for
  failed sensors in `IPC_TELEMETRY`.
- The ESP32 ground (PLANO etapa 3) decodes version 2 and runs the same
  vectors.
- The ICDs (`docs/icd/`) are written from this layout.

## Verification

| What | Where | Result |
|---|---|---|
| Every v2 record and frame, byte by byte; a v1 frame is rejected | `tests/test_vectors.c` | pass; CRCs of every frame vector checked against an independent CCITT-FALSE implementation |
| `TM_HK[0..13]` equals the OBC's `IPC_TELEMETRY` | `test_vectors`, `test_ttcd_core` | pass |
| "No data" in every field without a source; a failed OBC sensor stays unknown | `test_ttcd_core` | pass |
| Dump1090 bit follows `IPC_STAT` and clears when `adsbd` leaves | `test_ttcd_core`, `test_adsbd_core` | pass |
| Roster: once per callsign, again on change, everything on reconnection, resent after a refused frame; 30 aircraft as 22 + 8 on the air | `test_adsbd_core`, `test_ttcd_core` | pass |
| Mode forwarded, sent on connection, `UNKNOWN` when the OBC leaves | `test_ttcd_core` | pass |
| Real `adsbd` and `ttcd`, UDP radio: callsigns reach the ground on `REQ_ROSTER`; HK says dump1090 is up | `test_adsb_chain` | pass |
| On the bench, with the RA-02 pair and the SDR | — | to do |

## References

- ADR-0007 — frame format, which this version keeps.
- `docs/budgets/data-budget.md` §5 — the allocation, revised for the 34 B
  housekeeping frame.
- CDR v4 (`docs/cdr/`), Tables of the IPC messages, of the telemetry records
  and 55; derived requirements TTC-06, TTC-07; OBC-7.
- PLANO.md, etapa 1.1 and decision D6.
