#!/usr/bin/env python3
"""The ESP32 ground station, made to look like tools/gs_cli (ADR-0018).

Commands on stdin, one per line; the ground station's JSON lines on stdout,
the same as gs_cli prints. So every bench script runs unchanged with the
ESP32 as the ground: `make bench-ping GS=esp32 ESP32_PORT=/dev/ttyUSB0`.

    gs_serial.py --port /dev/ttyUSB0 [--power DBM] [--lbt header|preamble]
                 [--profile safe|nominal|fast] [--auto-time on|off] < commands
    gs_serial.py --port /dev/ttyUSB0 --suites      # read the test suites' run

What it does:
  1. resets the board through RTS (the DevKit's auto-reset circuit), so every
     session starts from a fresh boot, like a new gs_cli process; drops what
     the ROM bootloader prints, and waits for the firmware's `boot` line;
  2. sends this machine's clock (`set clock`, the ESP32 has none) and the
     options as `set` lines, then `begin`, which prints the `start` line;
  3. sends each command only once the previous one has been echoed — the
     console takes the next line only when the current command is done;
  4. at the end of stdin sends `quit` and exits after the `summary`.

Only lines that are JSON objects reach stdout; anything else (boot noise, a
stray log line) goes to stderr. If the board reboots in the middle, an
`error` line says so and the exit status is 1. Standard library only.
"""

import argparse
import fcntl
import json
import os
import re
import select
import sys
import termios
import time

BAUD = termios.B115200
BOOT_TIMEOUT_S = 8.0
SET_TIMEOUT_S = 3.0
SUITES_TIMEOUT_S = 120.0


class Board:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
        except OSError:
            pass
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0                                        # iflag: raw
        attrs[1] = 0                                        # oflag: raw
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                                        # lflag: no echo, no canon
        attrs[4] = attrs[5] = BAUD
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""

    def reset(self):
        """esptool's hard reset: DTR off (IO0 high: a normal boot), RTS
        pulsed (EN low). A port without modem lines — a pty — is left alone."""
        try:
            fcntl.ioctl(self.fd, termios.TIOCMSET, (0).to_bytes(4, sys.byteorder))
            fcntl.ioctl(self.fd, termios.TIOCMBIS, termios.TIOCM_RTS.to_bytes(4, sys.byteorder))
            time.sleep(0.1)
            fcntl.ioctl(self.fd, termios.TIOCMBIC, termios.TIOCM_RTS.to_bytes(4, sys.byteorder))
        except OSError:
            pass
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.buf = b""

    def send(self, line):
        data = (line + "\n").encode()
        while data:
            try:
                n = os.write(self.fd, data)
                data = data[n:]
            except BlockingIOError:
                select.select([], [self.fd], [], 1.0)

    def lines(self, timeout):
        """Complete lines received within `timeout` seconds (None: wait)."""
        r, _, _ = select.select([self.fd], [], [], timeout)
        if r:
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                chunk = b""
            self.buf += chunk
        out = []
        while b"\n" in self.buf:
            raw, self.buf = self.buf.split(b"\n", 1)
            out.append(raw.rstrip(b"\r").decode("utf-8", "replace"))
        return out


def as_event(line):
    """The JSON object a line holds, or None."""
    if not line.startswith("{"):
        return None
    try:
        obj = json.loads(line)
    except ValueError:
        return None
    return obj if isinstance(obj, dict) else None


def emit(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def noise(line):
    if line.strip():
        sys.stderr.write("esp32: " + line + "\n")


def fail(what, status=1):
    emit(json.dumps({"mono_ms": 0, "wall_ms": int(time.time() * 1000), "ev": "error",
                     "what": what}, separators=(",", ":")))
    sys.exit(status)


def wait_for(board, kinds, timeout, forward=False):
    """Reads until an event whose `ev` is in kinds; returns it, or None."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for line in board.lines(0.1):
            ev = as_event(line)
            if ev is None:
                noise(line)
            elif ev.get("ev") in kinds:
                return ev, line
            elif forward:
                emit(line)
    return None, None


def boot(board):
    board.reset()
    ev, line = wait_for(board, {"boot"}, BOOT_TIMEOUT_S)
    if ev is None:
        # No reset circuit, or a board already running a session: `quit`
        # makes the firmware restart.
        board.send("quit")
        ev, line = wait_for(board, {"boot"}, BOOT_TIMEOUT_S)
    if ev is None:
        fail("no boot line from the ground station: wrong port, or not flashed?")
    noise(line)
    if ev.get("radio") != "ok":
        fail("ground station radio: " + str(ev.get("why", "")))


def setup(board, settings):
    for key, value in settings:
        board.send(f"set {key} {value}")
        ev, line = wait_for(board, {"set", "error"}, SET_TIMEOUT_S)
        if ev is None or ev["ev"] == "error":
            fail(f"set {key} {value}: " + (ev or {}).get("what", "no answer"), 2)
    board.send("begin")
    ev, line = wait_for(board, {"start", "error"}, SET_TIMEOUT_S)
    if ev is None or ev["ev"] != "start":
        fail("begin: " + (ev or {}).get("what", "no answer"))
    emit(line)


def session(board):
    """Feeds stdin, one line per echo; quits at the end; returns at the summary."""
    stdin_open = True
    awaiting_echo = False
    echo_kinds = {"command"}            # what answers the line last sent
    quitting = False
    pending = b""
    while True:
        watch = [board.fd]
        if stdin_open and not awaiting_echo and not quitting:
            watch.append(sys.stdin.fileno())
        r, _, _ = select.select(watch, [], [], 0.5)
        if sys.stdin.fileno() in r:
            chunk = os.read(sys.stdin.fileno(), 4096)
            if not chunk:
                stdin_open = False
            pending += chunk
        while not awaiting_echo and not quitting and (b"\n" in pending or
                                                      (not stdin_open and pending)):
            if b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
            else:
                raw, pending = pending, b""
            text = raw.decode("utf-8", "replace").strip()
            if not text or text.startswith("#"):
                continue                    # not echoed by the console either
            board.send(text)
            awaiting_echo = True
            first = text.split()[0]
            echo_kinds = {"set", "error"} if first == "set" else {"command"}
            if first == "quit":
                quitting = True
        if not stdin_open and not pending and not awaiting_echo and not quitting:
            board.send("quit")
            awaiting_echo = quitting = True
            echo_kinds = {"command"}
        for line in board.lines(0):
            ev = as_event(line)
            if ev is None:
                noise(line)
                continue
            kind = ev.get("ev")
            if kind == "boot":
                fail("ground station rebooted")
            emit(line)
            if kind in echo_kinds:
                awaiting_echo = False
            if kind == "summary":
                return


SENTINEL = re.compile(r"^ESP32 SUITES: (\d+) run, (\d+) failed")


def suites(board):
    board.reset()
    end = time.monotonic() + SUITES_TIMEOUT_S
    while time.monotonic() < end:
        for line in board.lines(0.2):
            if line.startswith(("ESP32 SUITES", "==")) or "checks" in line or "FAIL" in line:
                emit(line)
            m = SENTINEL.match(line)
            if m:
                return 0 if m.group(2) == "0" else 1
    sys.stderr.write("gs_serial: no result line within %d s\n" % SUITES_TIMEOUT_S)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--power", type=int, help="this ground station's power, dBm")
    ap.add_argument("--lbt", choices=("header", "preamble"))
    ap.add_argument("--profile", choices=("safe", "nominal", "fast"))
    ap.add_argument("--auto-time", choices=("on", "off"))
    ap.add_argument("--suites", action="store_true",
                    help="read the result of the test suites image (ground/esp32-tests)")
    args = ap.parse_args()

    board = Board(args.port)
    if args.suites:
        return suites(board)

    boot(board)
    settings = [("clock", str(int(time.time() * 1000)))]
    if args.profile:
        settings.append(("profile", args.profile))
    if args.power is not None:
        settings.append(("power", str(args.power)))
    if args.lbt:
        settings.append(("lbt", args.lbt))
    if args.auto_time:
        settings.append(("auto_time", args.auto_time))
    setup(board, settings)
    session(board)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
