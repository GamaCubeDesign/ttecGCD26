# ADR-0002: C11 for flight and ground software, Python for analysis

- **Status:** Accepted
- **Date:** 2026-09-15
- **Requirements:** HLR-SW-01, HLR-SYS-01, HLR-COST-02
- **Deciders:** TT&C team

## Context

Three programs have to agree on one wire format:

| Component | Target | Why it must agree |
|---|---|---|
| `ttcd` | Raspberry Pi Zero 2 W, Linux | Encodes telemetry, decodes telecommands |
| ESP-IDF firmware | ESP32 | Decodes telemetry, encodes telecommands |
| OBC adapter | Raspberry Pi Zero 2 W, Linux | Decodes telecommand events over IPC |

The ESP-IDF target is fixed: the ground station is an ESP32 running ESP-IDF,
whose native language is C. The OBC is an existing C11 CMake project
(`obc/CMakeLists.txt`, `-Wall -Wextra`) written by a different team.

The previous mission demonstrates the failure mode this decision has to
prevent. `ultima_missao/satellite/Integration.cpp:149` transmits a C struct by
casting it to `uint8_t*`:

```cpp
tx_send((uint8_t*)&hd, hd.length);
```

The receiving ESP32 reassembles it byte by byte into a struct declared in a
*second, hand-maintained copy* of the header
(`ultima_missao/ground_station/CommunicationProtocol.h`). The two copies had
already diverged: the ground station's `Operation` enum defines
`SEND_CT_DATA = 13`, which the satellite's copy does not have at all, and the
`control` struct has six fields on one side and five on the other. The format
also depends on the compiler's padding and alignment choices for a struct that
mixes `float` and `uint8_t`, which nothing in the source states or checks.

That class of bug is invisible until the link is up, and on a competition
schedule it is discovered on the day of the test.

## Decision

All embedded software in this repository is **C11**: the flight daemons on the
Raspberry Pi, the ESP-IDF firmware on the ESP32, and the OBC-side IPC reference
implementation we hand to the OBC team. The packet codec lives in `common/`
and is compiled **from the same source files** into all three.

Python is used for ground-side analysis, report generation and the operator
CLI, where none of these constraints apply.

## Rationale

The selecting criterion is: *the codec must be compiled once, not implemented
twice.*

C11 is the only language that satisfies it, because it is the intersection of
what all three targets accept natively:

- ESP-IDF is a C framework. C++ is supported, but the codec would still have to
  be C-compatible to be linked from the IDF components that surround it.
- The OBC is C11 and owned by another team. Handing them a C file they can drop
  into their existing `add_executable` is a merge they can review in an hour.
  Handing them anything else is a negotiation.
- `dump1090-fa`, the SDR decoder, exposes a plain-text socket interface; the
  existing `adsb_capture.c` already speaks it in C.

With one shared source file, `tests/test_vectors.c` can be cross-compiled for
the ESP32 and run on the target. Passing on both is a *demonstration*, in the
HLR-VV-02 sense, that the two ends of the radio link produce identical bytes —
not an argument that they should.

The team already works fluently in C and C++, so no learning cost is being
paid for this.

## Alternatives considered

### C++17 on the Pi, C on the ESP32

This is what the previous mission did. Rejected: it forces the codec to be
written twice, which is precisely the failure documented above. The C++ that
was used also did nothing to prevent that failure — the raw struct cast, the
global mutable state, and the byte-by-byte reassembly are all constructs C++
permits as readily as C. The language was not the problem and would not have
been the solution.

### Rust for the flight software

Genuinely attractive for the parser, which is the component most exposed to
malformed input. Rejected on the shared-codec criterion: the ESP32 and OBC
sides would still need a C implementation, so the format would be expressed
twice and the central benefit of this decision would be lost. The team's
unfamiliarity is a secondary concern; twelve days to a documentation deadline
makes it a decisive one.

### Python for the flight software

Rejected on resources and on determinism. The Pi Zero 2 W has 512 MB of RAM and
four Cortex-A53 cores that `dump1090` already contends for at 2.4 MS/s. The
downlink path has a latency budget (HLR-ADS-08) that we must measure and
defend, and an interpreter with a garbage collector in that path makes the
p95 figure harder to explain than to achieve.

### C++ for the ESP32 application layer only, with the codec in C

Not rejected outright. If the ground-station team prefers C++ for the UI and
state handling above the codec, that is compatible with this decision, provided
`common/` stays C and is included through `extern "C"`. The headers in
`common/` already carry the necessary guards.

## Consequences

### Positive

- One wire format, one implementation, verified by golden byte vectors that run
  on both targets.
- The OBC-side deliverable is a file the other team can merge without adopting
  a toolchain.
- No dynamic allocation anywhere in the codec; it is safe to call from an
  interrupt-adjacent context on the ESP32.

### Negative

- C gives no help with memory safety in the parser, which is the highest-risk
  component. We accept this cost and pay it back with tooling rather than with
  the type system: the build runs `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion` and is kept at zero warnings, and the test suite runs
  under AddressSanitizer and UndefinedBehaviorSanitizer in CI. Every decode
  entry point validates its length before indexing.
- Discipline is required where a type system would have enforced: no struct may
  be cast to `uint8_t*`, and every multi-byte field goes through the explicit
  little-endian helpers in `common/gama_bytes.h`. This is a review checklist
  item, not a compiler guarantee.

### Follow-up required

- Add an ESP32 test target that builds `tests/test_vectors.c` for the xtensa
  toolchain, so the cross-target claim is continuously verified rather than
  checked once.

## References

- `common/` — the shared codec
- `tests/test_vectors.c` — the golden byte vectors that pin the format
- `ultima_missao/satellite/Integration.cpp:149` — the struct cast this replaces
- `ultima_missao/ground_station/CommunicationProtocol.h` — the divergent second copy
- `obc/CMakeLists.txt` — the OBC's C11 build
