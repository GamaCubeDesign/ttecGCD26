# ADR-0020: Require a ground time anchor before ADS-B mission startup

- **Status:** Proposed
- **Date:** 2026-11-12
- **Requirements:** HLR-ADS-07, HLR-ADS-08, HLR-SW-02

## Context

ADR-0009 supplies ground time on contact and after IPC reconnect. Previously,
SET_MODE could start capture before that anchor arrived. Reconnecting payloads
also received IPC_MODE before IPC_TIME_SET. The first onboard observations could
therefore carry the Pi's incorrect boot date.

## Decision

The shared ground console treats `mode mission_adsb` (including numeric event 3)
as an ordered operation: submit SET_TIME with the PC-supplied wall clock, wait
for its successful ACK, then submit the existing SET_MODE event. A missing clock,
negative ACK, exhausted retries or failed queue insertion aborts this operation.
Automatic time on contact remains independent and cannot complete this operation
with an unrelated ACK.

TT&C refuses a mission-start event before its first valid time anchor. For a
connected payload, it sends the current anchored time before forwarding the
start event to the OBC, refusing the request if that IPC send fails. On payload
reconnection it sends the time before the current mode. The payload independently
requires both the requested mission mode and a valid time anchor before launching
dump1090 or opening its SBS socket. The explicit `decoder = always` bench setting
retains standalone capture, including ADR-0009's unsynchronised fallback.

This extends startup orchestration in ADR-0014 using ADR-0009's unchanged time
representation: CLOCK_MONOTONIC plus an offset, not clock_settime. OBC mission
transitions, radio framing, track records, snapshot periods and packet sizes do
not change. One additional existing SET_TIME transaction is sent per user start;
there is no extra per-aircraft payload or stream. Existing clients can explicitly
send SET_TIME, wait for success, then SET_MODE.

## Consequences

The Pi cannot obtain accurate UTC from an un-timed start event alone; the ground
must supply a valid clock. The ESP32 obtains it from the PC serial bridge. Both
the ground console firmware and the flight daemons must be updated for the single
operator-command workflow. TT&C's mode ACK still acknowledges event delivery,
not proof that the OBC accepted a transition or that the SDR is receiving.
Housekeeping and track records confirm those subsequent stages.

A late payload can connect after the mission begins and receives time before
mode. The OBC timer still starts at its mode transition, not the first aircraft.
IPC acceptance is not a separate payload-level acknowledgement; the decoder's
local gate protects a new payload if its time frame was not delivered. Existing
UTC resynchronisation during capture can still step its offset and is logged.

## Verification

The normal 39-suite CTest run passes, including the three actual daemons through
UDP, rejection before time, time-aligned records, payload restart, and final
MISSION_DOWNLINK. Core tests cover failed IPC time delivery and reconnect order;
console tests cover successful sequencing, rejected time, missing clock and
retry exhaustion. The six affected core/integration suites also pass under
ASan and UBSan; compilation has no warnings. The requirement quotation check
passes 71/71. No wire-format golden vectors were changed.

This is software verification without an SDR or physical LoRa link. The updated
ESP32 firmware, Pi deployment and timestamp accuracy require a hardware run.
