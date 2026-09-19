# ADR-0006: AF_UNIX SOCK_SEQPACKET for inter-process communication

- **Status:** Accepted
- **Date:** 2026-09-19
- **Requirements:** HLR-COMM-01, HLR-COMM-02, HLR-GEN-03, HLR-SW-02
- **Deciders:** TT&C team; OBC team to review through the OBC↔TT&C ICD

## Context

Three processes on the Raspberry Pi must exchange messages (ADR-0005):
`ttcd` owns the radio, `adsbd` owns the ADS-B receiver (ADR-0008), and the
OBC — maintained by another team in another repository — owns the mission
modes. Telecommands reach the OBC only through `ttcd`; platform telemetry
reaches the ground only through `ttcd`.

The constraints come from the two sides:

- **The OBC runs a single-threaded cooperative loop at 1 Hz**
  (`obc/src/main.c`). It already has the right seam: `Event
  radio_poll_tc(void)` is called once per cycle and returns `EV_NONE` when
  nothing arrived. Whatever we provide must be pollable with a zero timeout;
  a blocking call stalls the whole on-board computer.
- **The OBC has no IPC of any kind today.** Nothing to stay compatible with,
  but also nothing to reuse.
- **The previous mission used files with fixed names as IPC**, and it failed
  in the way that approach tends to: the guard in `verifyFile()` checked
  `sensor_data.json` while `parseHealth()` read `HealthData.json`
  (`ultima_missao/satellite/Integration.cpp:110,173`).
- **The previous mission's byte-by-byte reassembly** of the radio stream meant
  a single lost byte desynchronised the parser permanently
  (`ultima_missao/satellite/Module.cpp:65-79`).

## Decision

1. **Transport:** an `AF_UNIX` socket of type `SOCK_SEQPACKET`. `ttcd`
   listens at a configured path (default `/run/gama/ttec.sock`); the other
   processes connect to it.
2. **Every datagram is exactly one GAMA frame** (`common/gama_frame.h`), with
   the same header and CRC as on the radio, using the `GAMA_FRAME_IPC_*`
   types. Payloads are defined in `common/gama_ipc.h`.
3. **The first frame on a connection is `IPC_HELLO`**, carrying the peer's
   role (`adsbd`, `obc`, `tool`) and the IPC version. Frames before it are
   rejected and counted; a version mismatch closes the connection.
4. **Every call is non-blocking** (`flight/libipc/ipc.h`). A send to a full
   queue returns without sending; the sender counts the drop instead of
   waiting (AGENTS.md, hard rule 6).

## Rationale

**Message boundaries are the deciding property.** With `SEQPACKET`, one send
is one receive: a frame arrives whole or not at all, and there is no stream to
resynchronise. That removes by construction the failure that broke the
previous mission's parser.

**It fits the OBC's loop without changing it.** A connected socket is a file
descriptor; `radio_poll_tc()` becomes one `recv(..., MSG_DONTWAIT)` that
returns `EV_NONE` when nothing is queued. The OBC team fills in a stub whose
signature they already wrote.

**One codec for the radio and the IPC.** The frames `ttcd` forwards to the OBC
are encoded and checked by the same code that every radio test exercises. An
IPC-specific format would be a second wire format to get wrong.

**The kernel reports a dead peer.** When a process exits, its socket closes
and the other end reads end-of-file at once. `ttcd` learns that `adsbd` died
without timeouts or heartbeats, and systemd restarts it (ADR-0005).

**No dependency and no broker.** The OBC is plain C11 with no libraries;
anything beyond the kernel's socket API would become their dependency too.

Measured on the development host: 10 000 frames each way between two
processes in 0.03 s, with no loss and no blocking call
(`tests/test_libipc.c`). To be re-measured on the Pi Zero 2 W.

## Alternatives considered

### Named pipes (FIFOs)

Need two per peer, one per direction. Reads are not message-oriented, so
frames must be delimited again — the reassembly problem this decision exists
to avoid. Opening a FIFO blocks until the other end opens it, and a writer
cannot tell a slow reader from a dead one.

### TCP on loopback

A byte stream, so it needs framing on top, and it exposes a network port on a
machine that also runs Wi-Fi during development. No property it has that
`AF_UNIX` lacks is useful here.

### POSIX message queues

Preserve boundaries, but have no notion of a connection: nothing tells the
reader that the writer died, and queues persist across process restarts with
stale content in them. Less familiar to both teams.

### Shared memory

Fastest, and irrelevant at our message rates — a few frames per second.
Requires a synchronisation protocol of its own, which is where the bugs would
be.

### D-Bus

A daemon and a library dependency on a 512 MB board, for three processes on
the same machine, and a dependency the OBC team would have to adopt.

### Files with fixed names

What the previous mission did. See Context.

## Consequences

### Positive

- One wire format across the radio and the IPC.
- Peer death is detected immediately, and each process can be restarted
  independently.
- Back-pressure is explicit: a full queue is reported, never waited on.

### Negative

- **Linux only.** The ESP32 cannot use it; it does not need to.
- **The OBC team must adopt a client** (~100 lines, provided as a reference
  implementation with the ICD). Until they do, telecommands cannot reach the
  OBC modes (HLR-COMM-01).
- **Access control is filesystem permissions** on the socket's directory.
  The systemd unit must create `/run/gama` with a group shared by the three
  services.
- **Dropped frames on a full queue are lost**, by design. They are counted
  and logged; at our rates a full queue means the peer has stopped reading,
  which is the problem to report.

### Follow-up required

- Write the OBC↔TT&C ICD (`docs/icd/obc-ttec-icd.md`) with the payloads of
  `common/gama_ipc.h`, and deliver the reference `radio.c` (PLANO phase 4.3).
- Re-measure throughput and latency on the Pi Zero 2 W.

## References

- `flight/libipc/ipc.h` — the transport
- `common/gama_ipc.h` — the payloads
- `tests/test_libipc.c` — the 10 000-frame acceptance test
- `obc/src/drivers/radio.h` — the OBC seam this fills
- ADR-0005 — process architecture
