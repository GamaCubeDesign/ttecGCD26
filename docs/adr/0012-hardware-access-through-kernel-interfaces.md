# ADR-0012: Radio hardware through the kernel's spidev and GPIO character device

- **Status:** Proposed — implemented; not yet run on the Raspberry Pi
- **Date:** 2026-09-19
- **Requirements:** HLR-COMM-01, HLR-COMM-03, HLR-GEN-03
- **Deciders:** TT&C team (LoRa sub-team)

## Context

The SX1278 hangs off the Raspberry Pi's SPI bus, with two GPIO lines: NRESET
(an output) and DIO0 (an input that rises when a packet has been received or
sent). The previous mission drove them through **pigpio**
(`ultima_missao/satellite/LoRa.c`): `spiOpen`/`spiXfer` for the bus and
`gpioSetISRFunc` for DIO0, whose callback runs on a pigpio-owned thread.
PLANO phase 2.2 carried the same assumption forward: "callback pigpio escreve
um byte num eventfd".

`ttcd` is a single-threaded `epoll` loop (ADR-0005, AGENTS.md hard rule 2),
and its core must see radio events in order with every other event.

## Decision

Use the kernel's own interfaces, with no library:

- **SPI through `spidev`** (`/dev/spidev0.0`): one `SPI_IOC_MESSAGE` ioctl
  per register access or FIFO burst.
- **GPIO through the character device, uAPI v2** (`/dev/gpiochip0`,
  `GPIO_V2_GET_LINE_IOCTL`). NRESET is requested as an output. DIO0 is
  requested as an input with rising-edge detection, and **its line descriptor
  goes directly into `ttcd`'s `epoll` set**.

The bus sits behind `sx1278_bus_t`, so the register logic
(`flight/radio/sx1278.c`) knows nothing of Linux, and is tested against an
emulated chip (`tests/test_sx1278.c`).

## Rationale

**An interrupt becomes a readable file descriptor.** The GPIO character
device reports each edge as an event readable from the line's descriptor, so
DIO0 is one more fd in the loop that already waits on the IPC sockets and the
timer. No thread, no callback, no `eventfd` bridge, and no ordering question
between a callback thread and the loop.

**No daemon, no root.** pigpio runs as a root daemon (`pigpiod`) or needs the
process itself to be root to map GPIO memory. `spidev` and the GPIO character
device need only membership of the `spi` and `gpio` groups, which Raspberry Pi
OS creates for this purpose. A flight process that does not run as root is one
fewer way to damage the system.

**Supported interfaces.** Both are mainline kernel APIs; GPIO uAPI v2 has been
in the kernel since 5.10, and current Raspberry Pi OS ships 6.x. The legacy
sysfs GPIO interface is deprecated, and pigpio does not support the newest Pi
hardware — not our board today, but a dependency with no future.

**Testability.** Separating the bus from the register logic is what allows the
driver's configuration — including the LowDataRateOptimize bug the legacy
driver had — to be checked on a laptop, bit by bit.

## Alternatives considered

### pigpio, as in the previous mission

Rejected for the thread-and-callback model, the root requirement and the
daemon. Keeping it would have meant a callback thread writing to an `eventfd`
just to get back into the loop — the bridge this decision removes.

### libgpiod

The official userspace library for the same character device. Rejected
because we need two lines and three ioctls, and libgpiod's API changed
incompatibly between v1 and v2. The versions differ across distributions, so
the flight image and the development machines would have to be kept in step.
The raw uAPI is stable and needs no dependency.

### sysfs GPIO (`/sys/class/gpio`)

Deprecated, slower, and its edge notification uses `poll` on an attribute
file with awkward semantics. Rejected.

## Consequences

### Positive

- One event loop, one thread, every event ordered.
- The flight binaries run without root.
- The register logic is tested on the host; only the thin Linux layer
  (`flight/radio/sx1278_linux.c`) needs the hardware to be exercised.

### Negative

- **The GPIO character device reports edges, and DIO0 is a level.** A flag
  that arrives while DIO0 is already high produces no new edge. The driver
  clears only the flags it read, so a late flag keeps DIO0 high and is found
  by the next service call, and the shell polls the chip periodically as a
  safety net. Without that, a missed edge could leave the radio deaf for good.
- **The Linux layer is untested until it runs on the Pi.** It compiles
  against the kernel headers on every build, but its first real execution is
  on the bench.
- Line offsets are board-specific: on the Pi Zero 2 W, `gpiochip0` offsets are
  the BCM numbers; other boards differ. They are configuration, not code.

### Follow-up required

- Bench: confirm the driver initialises the RA-02 (RegVersion 0x12), that DIO0
  edges arrive for both TxDone and RxDone, and measure the time from the edge
  to the service call.
- Provision the flight image with `ttcd` in the `spi` and `gpio` groups.

## References

- `flight/radio/sx1278.c`, `flight/radio/sx1278_linux.c`
- `tests/test_sx1278.c` — the driver against an emulated chip
- Linux kernel documentation, *GPIO Character Device Userspace API* (uAPI v2)
- ADR-0005 — the event loop this plugs into
