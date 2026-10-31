# ICD: OBC ↔ AOCS over I²C

- **Status:** DRAFT, version 1 — a proposal to the AOCS team. Nothing here is
  binding until they agree; every number is open to change.
- **Date:** 2026-10-31
- **Between:** the OBC (Raspberry Pi Zero 2 W, `flight/obcd`) and the AOCS
  board's ESP32 (firmware by the AOCS team).
- **Code:** `common/gama_aocs.{h,c}` encodes and decodes both frames; the AOCS
  firmware can compile the same two files (no platform headers, no
  allocation). Vectors: `tests/test_aocs_codec.c`.
- **Decision record:** ADR-0017.

## 1. What this interface is for

In `ADVANCED_AOCS` (CDR Figure 44) the OBC asks the ESP32 to run its task —
detumbling, then pointing to an azimuth — and later collects the result: the
final attitude, which the OBC reports in housekeeping (HLR-COMM-02), and
whether the task succeeded. The task ends the mode (`EV_TASK_DONE`, PLANO D2).

There is **no other wire** between the Pi and the ESP32: no UART, no GPIO.
Everything goes over the shared I²C bus.

## 2. The bus

| Item | Value |
|---|---|
| Bus | the stack's I²C, pins 10 (SDA) and 12 (SCL) of the 40-pin bus; the Pi's I²C1 (GPIO2/GPIO3, `/dev/i2c-1`) |
| Speed | 100 kHz, standard mode |
| Other devices | INA219 at 0x40 (OBC board); BMI088 at 0x18/0x19 (accelerometer) and 0x68/0x69 (gyroscope) (AOCS board) |
| ESP32 address | **0x2A** (7-bit), proposed; configurable on both sides. Clear of the INA219's strap range 0x40–0x4F, the BMI088 and EEPROMs at 0x50–0x57 |
| Masters | the Pi, and the ESP32 during its task — **never both at once** (§4) |

Two hardware facts shape everything below:

- **The Pi's I²C controller has no multi-master arbitration**, and the kernel
  marks it as unable to handle clock stretching. Two masters on this bus at
  the same time corrupt each other's transfers silently.
- **The classic ESP32 cannot stretch the clock as a slave**, so it cannot
  prepare data between a register-pointer write and the read that follows.
  Instead the ESP32 keeps a complete reply already loaded in its transmit
  buffer at all times (§3).

## 3. Frames

Two fixed-length frames, no register map. Every multi-byte field is
little-endian. Both frames end in a CRC-16/CCITT-FALSE (poly 0x1021, init
0xFFFF, no reflection, no final XOR) over all the bytes before it: the same
check as the radio frames.

### COMMAND — Pi writes, 16 bytes

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | magic | `0x47` ('G') |
| 1 | 1 | version | `1` |
| 2 | 1 | type | `0x01` |
| 3 | 1 | seq | the Pi's counter; echoed in the STATUS that answers it |
| 4 | 1 | cmd | `0x01` PING, `0x02` START |
| 5 | 1 | tasks | START only: bit 0 detumble, bit 1 point to azimuth |
| 6 | 2 | max_task_s | START only: 1 … 3600. The ESP32 is a slave again by then, whatever happens |
| 8 | 2 | azimuth_cdeg | START with bit 1: target azimuth, 0 … 35999 (0.01°) |
| 10 | 4 | reserved | 0 |
| 14 | 2 | crc16 | over bytes 0–13 |

### STATUS — Pi reads, 20 bytes

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | magic | `0x47` |
| 1 | 1 | version | `1` |
| 2 | 1 | type | `0x02` |
| 3 | 1 | seq | of the COMMAND this answers (0 before any) |
| 4 | 1 | state | 0 IDLE, 1 ACCEPTED, 2 REFUSED, 3 DONE, 4 FAILED |
| 5 | 1 | error | 0 none, 1 bad frame, 2 bad arguments, 3 sensor, 4 actuator, 5 timeout, 0xFF other |
| 6 | 2 | roll_cdeg | int16, 0.01°; `0x8000` = not measured |
| 8 | 2 | pitch_cdeg | int16, 0.01°; `0x8000` = not measured |
| 10 | 2 | yaw_cdeg | int16, 0.01° (the azimuth); `0x8000` = not measured |
| 12 | 2 | rate_cdps | residual angular rate, 0.01°/s; `0xFFFF` = not measured |
| 14 | 2 | task_s | how long the last task took, seconds |
| 16 | 1 | flags | reserved, 0 |
| 17 | 1 | reserved | 0 |
| 18 | 2 | crc16 | over bytes 0–17 |

Example bytes for each frame are in `tests/test_aocs_codec.c`.

### Rules for the ESP32 as a slave

1. Within **5 ms of every STOP** on its address, the transmit buffer holds one
   complete, current STATUS frame. It discards whatever a previous read left
   unread.
2. A read returns exactly that frame. The Pi always reads exactly 20 bytes,
   in a plain read: never a write followed by a repeated start.
3. A write of anything other than one valid 16-byte COMMAND is ignored, and
   the next STATUS says `error = 1` (bad frame). Zero-length writes (address
   probes such as `i2cdetect`) are ignored silently.
4. PING changes nothing but the seq echoed in the STATUS.
5. START is accepted (state ACCEPTED) or refused (state REFUSED, with the
   reason in `error`). A refused START does not take the bus.

## 4. Who owns the bus: one START, step by step

Outside `ADVANCED_AOCS` the ESP32 is **only a slave** and the Pi is the only
master. During the task the roles swap, for a bounded time, with no
handshake that could be lost: **every deadline counts from t0, the end of the
Pi's START write**, on both sides.

| Time from t0 | Pi | ESP32 |
|---|---|---|
| 0 | writes START | receives it; preloads STATUS = ACCEPTED or REFUSED within 5 ms |
| 20 – 100 ms | reads STATUS to learn the answer (up to 3 tries) | slave |
| 100 ms | **last transaction** before the lease | slave |
| 250 ms | silent | if it accepted: may become master, after clearing the bus (9 clocks and a STOP) |
| 250 ms + max_task_s | silent | **back to slave** at the latest, with the final STATUS (DONE or FAILED) preloaded, after clearing the bus again |
| 250 ms + max_task_s + 2 s | resumes: reads STATUS (up to 5 tries), then the INA219 | slave |

What the Pi concludes from the first STATUS read:

| Outcome | Meaning | Bus lent? |
|---|---|---|
| NACK on the START write | no ESP32 on the bus | no; the mode ends at once |
| STATUS = REFUSED | the ESP32 said no | no; the mode ends at once |
| STATUS = ACCEPTED | the task runs | yes, until 250 ms + max_task_s + 2 s |
| START acknowledged, but no valid STATUS by 100 ms | unknown | **yes** — the Pi assumes the worst and stays silent |

**The mode lasts the whole lease.** Without another wire the Pi cannot learn
that the task finished early: polling during the lease would put two masters
on the bus. So `ADVANCED_AOCS` always lasts 250 ms + max_task_s + 2 s, and
the result is read at the end.

**Consequence for telemetry:** during the lease the Pi does not read the
INA219, so battery voltage and current go as "no data" in housekeeping (a
recorded deviation from the CDR's OBC-7, ADR-0017). Battery temperature is
not affected: the DS18B20 is on 1-Wire.

## 5. Failure handling

- **The ESP32 resets during the task.** It must boot as a slave with SDA and
  SCL released, and preload a STATUS (IDLE). The Pi reads it at the end of
  the lease and reports the task as failed.
- **The ESP32 hangs while it is master.** The Pi cannot recover the bus: the
  Pi's driver does no bus recovery. The ESP32 must run a watchdog that
  resets it, and must clear the bus when it becomes master and before it
  becomes a slave again.
- **The Pi, or `obcd`, restarts during the lease.** `obcd` keeps the
  lease's end on the SD card from the moment the START is out, so after a
  restart, or a reboot of the Pi, it stays silent until the lease is over
  (after a reboot, for as long as was left when the file was written). The
  ESP32 needs to do nothing for this.
- **A frame fails its CRC.** The Pi retries the read (as in §4). The ESP32
  reports `error = 1` for a damaged COMMAND and does not start.

## 6. Open points for the AOCS team

1. The address, 0x2A, and the frame layout: do they fit your firmware?
2. A realistic `max_task_s` for detumbling plus azimuth pointing: the OBC
   uses it as the length of the mode. Today's placeholder is 300 s.
3. What the result should carry beyond roll, pitch, yaw and the residual
   rate.
4. Whether your I²C slave driver can preload the reply as §3 requires. With
   the ESP-IDF legacy driver this is `i2c_slave_write_buffer` after each
   transaction; please confirm on your side.
5. Pull-ups: which board carries them on the stack's SDA and SCL, and their
   value.

## 7. How to test it before integration

- `tests/test_aocs_codec.c` runs on the Pi, on a PC and on an ESP32
  (`ground/esp32-tests`): the same bytes on both ends.
- `flight/obcd` is tested against a simulated ESP32 that checks the Pi stays
  silent for the whole lease.
- On the bench: any ESP32 dev board playing the slave side of this document,
  wired to the Pi's pins 3 and 5, exercises the timing before the AOCS board
  exists.
