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
                 ground's simulator (ground/host/ground-aeronaves/
                 simular_trajetorias.py). Each object becomes the SBS lines dump1090
                 would have written for it, paced by rx_epoch_ns.

The pacing keeps real time by default: adsbd stamps each message with its
own arrival, so --speed above 1 compresses the timeline too, and an
estimator downstream sees aircraft moving faster than their reported speed.

One client at a time. At the end of the input the connection stays open
until the client leaves (dump1090 does not hang up), unless --exit-at-end.

  sbs_replay.py --ndjson eventos.ndjson                  # 127.0.0.1:30003
  sbs_replay.py --sbs capture.sbs --port 31003 --exit-at-end
"""

import argparse
import datetime
import json
import signal
import socket
import sys
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


def ndjson_script(path):
    """(time in seconds, text) for every object of the file, in file order."""
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
            text = "".join(lines_from_record(rec))
            if text:
                yield rec.get("rx_epoch_ns", 0) / 1e9, text


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
            yield when, raw + "\r\n"


def serve(script, host, port, speed, exit_at_end):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"sbs_replay: waiting for a client on {host}:{port}", file=sys.stderr, flush=True)
    conn, peer = srv.accept()
    print(f"sbs_replay: {peer[0]}:{peer[1]} connected", file=sys.stderr, flush=True)
    start_wall, first, sent = time.monotonic(), None, 0
    try:
        for when, text in script:
            if first is None:
                first = when
            due = start_wall + (when - first) / speed
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            conn.sendall(text.encode("ascii"))
            sent += text.count("\n")
        print(f"sbs_replay: end of input, {sent} lines sent", file=sys.stderr, flush=True)
        if not exit_at_end:
            while conn.recv(1024):
                pass
    except (BrokenPipeError, ConnectionResetError):
        print(f"sbs_replay: client left after {sent} lines", file=sys.stderr)
    finally:
        conn.close()
        srv.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--sbs", help="SBS capture, one MSG line per message")
    src.add_argument("--ndjson", help="one JSON object per message")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=30003)
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
    script = ndjson_script(args.ndjson) if args.ndjson else sbs_script(args.sbs, args.rate)
    try:
        serve(script, args.host, args.port, args.speed, args.exit_at_end)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
