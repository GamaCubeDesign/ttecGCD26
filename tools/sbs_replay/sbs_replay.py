#!/usr/bin/env python3
"""Serve SBS-1 lines on a TCP port, as dump1090-fa does on 30003.

Lets adsbd run with no SDR: on the bench, on a development PC, and in the
chain from a recorded or simulated mission to the ground's estimator
(make adsb-chain). Two kinds of input:

  --sbs FILE     an SBS capture, one "MSG,..." line per message, sent as is.
                 Paced by the "message generated" time in fields 6-7 when
                 present, otherwise at --rate lines per second.
  --ndjson FILE  one JSON object per message: the onboard record adsbd (or
                 the prototype adsb_capture.c) writes, or the output of the
                 ground's simulator (ground/host/adsb/validation/
                 simular_trajetorias.py). Each object becomes the SBS lines dump1090
                 would have written for it, paced by rx_epoch_ns.

The pacing keeps real time by default: adsbd stamps each message with its
own arrival, so --speed above 1 compresses the timeline too, and an
estimator downstream sees aircraft moving faster than their reported speed.

With --avr-port, it also plays dump1090's raw output (AVR, --net-ro-port):
for each aircraft whose NDJSON objects carry a "category" ("A3", "A7"...), an
ADS-B identification message (DF17, type code 1-4) with its callsign and a
correct CRC-24, about every 5 s as a transponder sends it, the first ~1 s
after the aircraft's first SBS line. That is the only thing adsbd reads on
that port (ADR-0023); an input without categories sends nothing there.

One client per port. At the end of the input the connections stay open
until the client leaves (dump1090 does not hang up), unless --exit-at-end.

  sbs_replay.py --ndjson eventos.ndjson                  # 127.0.0.1:30003
  sbs_replay.py --sbs capture.sbs --port 31003 --exit-at-end
  sbs_replay.py --ndjson capture.ndjson --avr-port 30002
"""

import argparse
import datetime
import json
import signal
import socket
import sys
import threading
import time

# Field order of a BaseStation MSG line (flight/adsbd/sbs.h).
FIELDS = 22


def sbs_bool(v):
    """BaseStation booleans: -1 is TRUE, 0 is false, empty is unknown."""
    if v in (1, True):
        return "-1"
    if v in (0, False):
        return "0"
    return ""


def msg(tt, icao, when, callsign="", alt="", gs="", trk="", lat="", lon="",
        vr="", squawk="", ground=""):
    stamp = datetime.datetime.fromtimestamp(when, tz=datetime.timezone.utc)
    d, t = stamp.strftime("%Y/%m/%d"), stamp.strftime("%H:%M:%S.%f")[:-3]
    f = ["MSG", str(tt), "1", "1", icao.upper(), "1", d, t, d, t, callsign,
         alt, gs, trk, lat, lon, vr, squawk, "", "", "", ground]
    assert len(f) == FIELDS
    return ",".join(str(x) for x in f) + "\r\n"


def fmt(v, digits=None):
    if v is None:
        return ""
    return f"{v:.{digits}f}" if digits is not None else str(int(v))


def lines_from_record(rec):
    """The SBS lines one NDJSON object stands for: identification, airborne
    position, velocity. A record carrying a transmission_type (adsbd's own
    record) becomes exactly one line of that type."""
    icao = rec.get("icao")
    if not isinstance(icao, str) or len(icao) != 6:
        return []
    when = rec.get("rx_epoch_ns", 0) / 1e9
    # On every line, as dump1090-fa writes it: taken from the transponder's
    # capability field in each message (mode_s.c) — ground for CA 4, airborne
    # for CA 5, empty when the transponder does not say (CA 0, 6, 7).
    ground = sbs_bool(rec.get("on_ground"))
    cs = rec.get("callsign") or ""
    pos = {"alt": fmt(rec.get("altitude_ft")),
           "lat": fmt(rec.get("lat"), 6), "lon": fmt(rec.get("lon"), 6)}
    vel = {"gs": fmt(rec.get("ground_speed_kt"), 1), "trk": fmt(rec.get("track_deg"), 1),
           "vr": fmt(rec.get("vertical_rate_fpm"))}
    tt = rec.get("transmission_type")
    if isinstance(tt, int) and 1 <= tt <= 8:
        return [msg(tt, icao, when, callsign=cs, squawk=rec.get("squawk") or "",
                    ground=ground, **pos, **vel)]
    out = []
    if cs:
        out.append(msg(1, icao, when, callsign=cs, ground=ground))
    if pos["lat"] and pos["lon"]:
        tt = 2 if rec.get("on_ground") == 1 else 3
        out.append(msg(tt, icao, when, ground=ground, **pos))
    elif pos["alt"]:
        out.append(msg(5, icao, when, alt=pos["alt"], ground=ground))
    if vel["gs"] or vel["vr"]:
        out.append(msg(4, icao, when, ground=ground, **vel))
    return out


# ---- AVR identification messages (ADR-0023) ----

# The 6-bit character set of the ADS-B callsign (ICAO Annex 10 Vol. IV).
AIS = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######"
IDENT_PERIOD_S = 5.0     # a transponder's identification rate
IDENT_FIRST_S = 1.0      # after the first SBS line: adsbd tracks it by then


def crc24(data):
    """Mode S CRC-24, generator 0xFFF409; 0 over a whole intact message."""
    c = 0
    for b in data:
        c ^= b << 16
        for _ in range(8):
            c <<= 1
            if c & 0x1000000:
                c ^= 0x1FFF409
    return c & 0xFFFFFF


def avr_ident(icao, category, callsign=""):
    """The AVR line of a DF17 identification message, or None for a
    category that is not set A-D, 0-7."""
    if not isinstance(category, str) or len(category) != 2:
        return None
    s, n = category[0].upper(), category[1]
    if s not in "ABCD" or n not in "01234567":
        return None
    tc = 4 - "ABCD".index(s)
    bits = 0
    for ch in (callsign or "").upper().ljust(8)[:8]:
        k = AIS.find(ch)
        bits = bits << 6 | (k if k > 0 and ch != "#" else 32)
    msg = bytes([0x8D]) + int(icao, 16).to_bytes(3, "big") + bytes([tc << 3 | int(n)]) \
        + bits.to_bytes(6, "big")
    return "*" + (msg + crc24(msg).to_bytes(3, "big")).hex().upper() + ";\n"


class IdentPacer:
    """Which records are due an identification message on the AVR port."""

    def __init__(self):
        self.due = {}

    def lines(self, rec, when):
        icao, cat = rec.get("icao"), rec.get("category")
        if not isinstance(icao, str) or len(icao) != 6 or not cat:
            return ""
        due = self.due.setdefault(icao, when + IDENT_FIRST_S)
        if when < due:
            return ""
        self.due[icao] = when + IDENT_PERIOD_S
        return avr_ident(icao, cat, rec.get("callsign") or "") or ""


def ndjson_script(path, avr=False):
    """(time in seconds, SBS text, AVR text) for every object of the file,
    in file order."""
    pacer = IdentPacer() if avr else None
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError:
                print(f"sbs_replay: line {n}: not JSON, skipped", file=sys.stderr)
                continue
            when = rec.get("rx_epoch_ns", 0) / 1e9
            text = "".join(lines_from_record(rec))
            raw_avr = pacer.lines(rec, when) if pacer else ""
            if text or raw_avr:
                yield when, text, raw_avr


def sbs_script(path, rate):
    """(time in seconds, text) for every line of an SBS capture."""
    t = 0.0
    with open(path, encoding="ascii", errors="replace") as f:
        for raw in f:
            raw = raw.rstrip("\r\n")
            if not raw:
                continue
            p = raw.split(",")
            when = None
            if len(p) > 7 and p[6] and p[7]:
                try:
                    when = datetime.datetime.strptime(
                        p[6] + " " + p[7], "%Y/%m/%d %H:%M:%S.%f").timestamp()
                except ValueError:
                    when = None
            if when is None:
                t += 1.0 / rate
                when = t
            yield when, raw + "\r\n", ""


class AvrPort:
    """dump1090's raw output: accepts a client whenever one comes, in the
    background, so the SBS replay never waits for it; lines with no client
    connected are dropped, as dump1090 drops them."""

    def __init__(self, host, port):
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((host, port))
        self.srv.listen(1)
        self.conn, self.sent, self.lock = None, 0, threading.Lock()
        print(f"sbs_replay: AVR on {host}:{port}", file=sys.stderr, flush=True)
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, peer = self.srv.accept()
            except OSError:
                return
            with self.lock:
                if self.conn is not None:
                    self.conn.close()
                self.conn = conn
            print(f"sbs_replay: AVR client {peer[0]}:{peer[1]}", file=sys.stderr, flush=True)

    def send(self, text):
        with self.lock:
            if self.conn is None:
                return
            try:
                self.conn.sendall(text.encode("ascii"))
                self.sent += text.count("\n")
            except OSError:
                self.conn.close()
                self.conn = None

    def close(self):
        with self.lock:
            if self.conn is not None:
                self.conn.close()
        self.srv.close()


def serve(script, host, port, speed, exit_at_end, avr=None):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"sbs_replay: waiting for a client on {host}:{port}", file=sys.stderr, flush=True)
    conn, peer = srv.accept()
    print(f"sbs_replay: {peer[0]}:{peer[1]} connected", file=sys.stderr, flush=True)
    start_wall, first, sent = time.monotonic(), None, 0
    try:
        for when, text, raw_avr in script:
            if first is None:
                first = when
            due = start_wall + (when - first) / speed
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            if text:
                conn.sendall(text.encode("ascii"))
                sent += text.count("\n")
            if raw_avr and avr is not None:
                avr.send(raw_avr)
        extra = f", {avr.sent} AVR lines" if avr is not None else ""
        print(f"sbs_replay: end of input, {sent} lines sent{extra}", file=sys.stderr, flush=True)
        if not exit_at_end:
            while conn.recv(1024):
                pass
    except (BrokenPipeError, ConnectionResetError):
        print(f"sbs_replay: client left after {sent} lines", file=sys.stderr)
    finally:
        conn.close()
        srv.close()
        if avr is not None:
            avr.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--sbs", help="SBS capture, one MSG line per message")
    src.add_argument("--ndjson", help="one JSON object per message")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=30003)
    ap.add_argument("--avr-port", type=int, default=0,
                    help="also serve identification messages in AVR, as dump1090's "
                         "--net-ro-port (ADR-0023); --ndjson only; 0: off")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="replay speed; 1 keeps real time (see above)")
    ap.add_argument("--rate", type=float, default=80.0,
                    help="lines per second for an SBS file without times")
    ap.add_argument("--exit-at-end", action="store_true",
                    help="close the connection and exit at the end of the input")
    args = ap.parse_args()
    if args.speed <= 0 or args.rate <= 0:
        ap.error("--speed and --rate must be positive")
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    if args.avr_port and not args.ndjson:
        ap.error("--avr-port needs --ndjson: an SBS capture carries no category")
    if args.avr_port == args.port:
        ap.error("--avr-port must differ from --port")
    script = (ndjson_script(args.ndjson, avr=bool(args.avr_port)) if args.ndjson
              else sbs_script(args.sbs, args.rate))
    avr = AvrPort(args.host, args.avr_port) if args.avr_port else None
    try:
        serve(script, args.host, args.port, args.speed, args.exit_at_end, avr)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
