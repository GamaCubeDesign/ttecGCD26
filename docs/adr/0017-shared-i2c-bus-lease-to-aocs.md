# ADR-0017: The shared I²C bus is lent to the AOCS ESP32 for a bounded time; the INA219 is not read meanwhile

- **Status:** Proposed
- **Date:** 2026-10-31
- **Requirements:** HLR-SW-01, HLR-COMM-02, HLR-EPS-04 (deviation), HLR-GEN-03
- **Deciders:** TT&C/OBC team; the INA219's placement by the team, 2026-10-31

## Context

The INA219 (battery voltage and the satellite's current, OBC-7) and the AOCS
board (ESP32 and its BMI088) share one I²C bus: pins 10 and 12 of the stack,
the Pi's I²C1. During `ADVANCED_AOCS` the ESP32 must be master of that bus
to talk to its BMI088 (PLANO 2.4). On 2026-10-24 we proposed moving the
INA219 to the Pi's own I²C0, so that the Pi could keep reading it during the
AOCS task (`docs/relatorios/2026-10-24-proposta-i2c-ina219-obc.html`). On
2026-10-31 the team decided to keep the INA219 on the shared bus: the new OBC
board (`hardware/obc-v2`, KiCad of 2026-10-23) has it there.

Three facts constrain what can be done:

1. The Pi's I²C controller (BCM2835 family) has no multi-master arbitration,
   and its kernel driver declares that it cannot handle clock stretching.
   Two masters at once corrupt transfers without either noticing.
2. The classic ESP32 cannot stretch the clock as an I²C slave, so it cannot
   produce data between a register-pointer write and the read after it.
3. There is no other wire between the Pi and the ESP32 (finding A4): no
   UART, no GPIO. Whatever coordination there is goes over the same bus.

## Decision

- **Ownership by lease, timed from one event.** Outside `ADVANCED_AOCS` the
  Pi is the only master. On entering the mode the Pi writes a START; every
  later deadline on both sides counts from the end of that write (t0). The
  Pi's last transaction is at t0+100 ms; the ESP32 may master the bus from
  t0+250 ms and must be a slave again by t0+250 ms+max_task_s; the Pi resumes
  2 s after that. A START acknowledged on the bus but without a readable
  answer counts as a lease: the Pi assumes the worst.
- **Fixed-length frames, no register map**: a 16-byte COMMAND written by the
  Pi, a 20-byte STATUS the ESP32 keeps preloaded, both with the radio
  frames' CRC-16, both under the ESP32's 32-byte FIFO. The draft contract is
  `docs/icd/obc-aocs-i2c.md`; the codec is `common/gama_aocs.{h,c}`, which the
  AOCS firmware can compile too.
- **The mode lasts the whole lease.** The Pi cannot learn of an early finish
  without becoming a second master, so `ADVANCED_AOCS` lasts
  250 ms + max_task_s + 2 s and the result is read at the end. With no
  ESP32 answering, or a refused START, the mode ends at once. No ESP32
  answering means the START write is NACKed, or it fails otherwise and none
  of the STATUS reads after it goes through either: the Pi's driver reports
  an address nobody acknowledges as EIO, not only as ENXIO or EREMOTEIO
  (bench of 2026-11-05, kernel 6.18), and an ESP32 that took the START
  answers STATUS within 5 ms (ICD §3), long before it may take the bus.
- **During the lease the INA219 is not read**, and battery voltage and
  current go as "no data" (ADR-0013) from the START until the lease ends.
  This is a **recorded deviation** from the CDR's OBC-7, which promises them
  in every mode. Battery temperature is unaffected (1-Wire). We do not ask
  the AOCS team to read the INA219 for us.
- **One thread owns `/dev/i2c-1`** in `obcd`, for the INA219 polls and the
  AOCS frames alike, so the Pi's own accesses are serialised by
  construction; and every transfer is a single `I2C_RDWR` transaction, with
  a repeated start where a register is read.
- **The lease's end is kept on the SD card** (`i2c_lease_path`, in
  `/var/lib/gama`): written as soon as the START is out, synced once the
  STATUS window is over, removed with the result. An `obcd` restarted, or a
  Pi rebooted — which does not reset the ESP32 — stays silent until the
  lease is over. The monotonic clock does not survive a reboot, so a file
  from another boot (`boot_id`) counts as what was left of the lease when it
  was written, from now.

## Rationale

1. **A deadline both sides compute from the same instant cannot be
   misunderstood.** Had the ESP32 taken over after the Pi confirmed reading
   its answer, a confirmation lost to noise would leave the ESP32 believing
   it was confirmed while the Pi retried: two masters. Counting from the
   START, and treating an unclear outcome as a lease, makes the worst case a
   Pi that stays silent for nothing.
2. **Fixed frames are what the ESP32 slave can actually do.** Without clock
   stretching, a reply must already be in the slave's buffer when the read
   begins; a register map would need a pause between pointer and read that
   the Pi's driver does not offer within one transaction.
3. **Keeping the INA219 where it is costs only the AOCS window.** The
   deviation is bounded by the lease (today's placeholder for max_task_s is
   300 s), happens in one mode, and is visible: the ground sees "no data",
   never a stale or invented value.

## Alternatives considered

### The INA219 on the Pi's I²C0 (the proposal of 2026-10-24)

Keeps OBC-7 in every mode at the cost of two traces and two pull-ups on the
OBC board. Not taken by the team for the new board.

### The ESP32 reads the INA219 during its task and reports it with the result

Would cover the gap after the fact. Not requested: it adds work to another
team's firmware and a dependency on it for an EPS measurement.

### Polling the ESP32 during the lease, or agreed idle windows

Polling puts two masters on a bus without arbitration. Idle windows would
pause the attitude control loop at fixed intervals and depend on both
clocks agreeing. Both lost to the lease's simplicity.

### A register map

The usual I²C style. Lost on fact 2 above; kept as a fallback in the ICD if
the AOCS team insists, with a STOP and ≥ 5 ms between pointer and read.

## Consequences

### Positive

- The bus can never have two masters by design, whatever is lost on it.
- The contract is small enough to implement on both sides this week and is
  testable without the AOCS board (a simulated ESP32; any ESP32 dev board on
  the bench).

### Negative

- OBC-7 / HLR-EPS-04: no voltage or current for the duration of the AOCS
  lease. To be stated in the final documentation as a deviation.
- `ADVANCED_AOCS` always lasts the whole lease, even if the task finishes
  early.
- An ESP32 that hangs while holding the bus cannot be recovered from the Pi:
  its watchdog is the only remedy, and the contract requires one.
- ADR-0016's behaviour changes: with no ESP32 answering, `ADVANCED_AOCS` now
  ends at once instead of after a 300 s placeholder deadline.

### Follow-up required

- The AOCS team's answer to the draft (ICD §6): address, max_task_s, result
  fields, the preload behaviour of their slave driver.
- Bench: an ESP32 dev board playing the slave side against `obcd`, before
  the AOCS board is available.

## Verification

| What | Where | Result |
|---|---|---|
| Frame bytes against an independent CRC (Python `binascii.crc_hqx`) | `tests/test_aocs_codec.c` | pass |
| The Pi makes no transaction during the lease, in every outcome | `test_obcd_i2c_sched`, virtual time | pass |
| No ESP32, or a refused START: `ADVANCED_AOCS` ends at once; a lease: V and I "no data", re-entry refused as busy, the result's attitude in telemetry | `test_obcd_core` | pass |
| No ESP32 while the driver says EIO, not NACK: nothing lent; a START reported failed that reached the ESP32 is lent on its STATUS; a START written with no STATUS going through is lent | `test_obcd_i2c_sched` | pass |
| The Pi's driver on an address nobody acknowledges | bench 2026-11-05, `i2ctransfer`, kernel 6.18.34 | EIO, for 0x2A, 0x30 and 0x50, write or read |
| The real binary: a lease left on disk is kept, then its result read and the file removed; no ESP32 on the bus ends the mode at once | `test_obcd_integration` | pass |
| No data race between the I²C thread and the loop | `test_obcd_integration` under ThreadSanitizer | pass |
| A real ESP32 slave against the Pi | bench | to do |

## References

- `docs/icd/obc-aocs-i2c.md` (draft v1), `common/gama_aocs.h`.
- PLANO.md 2.4; ADR-0013 ("no data"), ADR-0016 (obcd).
- `docs/relatorios/2026-10-22-validacao-obc.html` (finding A4),
  `docs/relatorios/2026-10-24-proposta-i2c-ina219-obc.html`.
