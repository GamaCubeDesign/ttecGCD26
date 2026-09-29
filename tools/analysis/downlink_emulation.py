#!/usr/bin/env python3
"""What the ground receives, emulated from a full ADS-B capture.

The ground's estimator (ground/host/ground-aeronaves) was written
against the onboard record: every message, ~4 per aircraft per second. The
radio carries one record per aircraft per snapshot, every 5 s (ADR-0003).
This script turns a full capture into what the ground would rebuild from the
link, so the two can be compared on the same flights:

  - every PERIOD s, the latest state of each aircraft, by adsbd's rules
    (flight/adsbd/tracks.h): a position only if it is at most 10 s old, the
    record timed by that position, fields older than 60 s left out, at most
    24 aircraft, positions first;
  - quantised like the 20-byte track record (common/gama_tm.h): latitude
    1/93206 deg, longitude 1/46603 deg, altitude 25 ft, speed 0.1 kt, track
    0.01 deg, age 100 ms;
  - LOSS of the frames lost, 12 records to a frame;
  - the time rebuilt as the ground does, arrival - time on air - age, which
    comes back to the update time to within the 100 ms of the age.

The emulation is for studies on real captures. The reference for what the
chain really does is the chain itself: make adsb-chain INPUT=<capture>.

  downlink_emulation.py capture.ndjson > downlink.ndjson
  downlink_emulation.py capture.ndjson --loss 0.10 --seed 3 > lossy.ndjson

With --compare, runs the estimator on the capture and on the emulated
downlink and prints both conclusions side by side:

  downlink_emulation.py capture.ndjson --compare ground/host/ground-aeronaves \\
      --reference-db ground/host/banco-aeronaves/referencias.db
"""

import argparse
import contextlib
import io
import json
import random
import sys
import tempfile
from pathlib import Path

LAT_SCALE, LON_SCALE = 93206.0, 46603.0       # common/gama_tm.h
RECORDS_PER_FRAME = 12


def quantise(state):
    q = {}
    if "lat" in state:
        q["lat"] = round(round(state["lat"] * LAT_SCALE) / LAT_SCALE, 7)
        q["lon"] = round(round(state["lon"] * LON_SCALE) / LON_SCALE, 7)
    if "altitude_ft" in state:
        q["altitude_ft"] = int(state["altitude_ft"] / 25) * 25
    if "ground_speed_kt" in state and "track_deg" in state:
        q["ground_speed_kt"] = round(state["ground_speed_kt"], 1)
        q["track_deg"] = round(state["track_deg"] % 360.0, 2)
    if "vertical_rate_fpm" in state:
        q["vertical_rate_fpm"] = int(state["vertical_rate_fpm"])
    if state.get("on_ground") == 1:
        q["on_ground"] = 1
    return q


def emulate(messages, period_s=5.0, loss=0.0, seed=1, pos_max_age_s=10.0,
            field_ttl_s=60.0, expiry_s=60.0, max_records=24):
    """The records the ground would rebuild, from time-ordered messages."""
    rnd = random.Random(seed)
    msgs = sorted((m for m in messages if m.get("rx_epoch_ns")), key=lambda m: m["rx_epoch_ns"])
    if not msgs:
        return []
    tracks = {}              # icao -> {field: (value, t_ns)}, "last": t_ns
    out, i = [], 0
    snap = msgs[0]["rx_epoch_ns"] + int(period_s * 1e9)
    end = msgs[-1]["rx_epoch_ns"] + int(period_s * 1e9)
    while snap <= end:
        while i < len(msgs) and msgs[i]["rx_epoch_ns"] <= snap:
            m = msgs[i]
            i += 1
            k = tracks.setdefault(m["icao"].upper(), {})
            t = m["rx_epoch_ns"]
            k["last"] = t
            if m.get("lat") is not None and m.get("lon") is not None:
                k["pos"] = ((m["lat"], m["lon"]), t)
            for f in ("altitude_ft", "ground_speed_kt", "track_deg", "vertical_rate_fpm"):
                if m.get(f) is not None:
                    k[f] = (m[f], t)
            if m.get("on_ground") in (0, 1):
                k["on_ground"] = (m["on_ground"], t)
        for icao in [a for a, k in tracks.items() if snap - k["last"] > expiry_s * 1e9]:
            del tracks[icao]

        candidates = []
        for icao, k in tracks.items():
            fresh = lambda f, ttl: f in k and snap - k[f][1] <= ttl * 1e9
            has_pos = fresh("pos", pos_max_age_s)
            fields = [f for f in ("altitude_ft", "ground_speed_kt", "track_deg", "vertical_rate_fpm")
                      if fresh(f, field_ttl_s)]
            if not has_pos and not fields:
                continue
            at = k["pos"][1] if has_pos else max(k[f][1] for f in fields)
            state = {f: k[f][0] for f in fields}
            if has_pos:
                state["lat"], state["lon"] = k["pos"][0]
            if fresh("on_ground", field_ttl_s):
                state["on_ground"] = k["on_ground"][0]
            candidates.append((not has_pos, -at, icao, at, state))
        candidates.sort()
        records = candidates[:max_records]
        for f in range(0, len(records), RECORDS_PER_FRAME):
            if rnd.random() < loss:
                continue
            for _, _, icao, at, state in records[f:f + RECORDS_PER_FRAME]:
                age_ds = round((snap - at) / 1e8)
                o = {"icao": icao, "rx_epoch_ns": snap - age_ds * 100_000_000}
                o.update(quantise(state))
                o.update(source="downlink_emulation", age_ds=age_ds)
                out.append(o)
        snap += int(period_s * 1e9)

    # The ground writes a repeated update once (ground/host/tracks_to_ndjson.py).
    seen, unique = set(), []
    for o in out:
        key = (o["icao"], o["rx_epoch_ns"] // 250_000_000, o.get("lat"), o.get("altitude_ft"))
        if key not in seen:
            seen.add(key)
            unique.append(o)
    return unique


def read_capture(path):
    with open(path, encoding="utf-8") as f:
        for raw in f:
            raw = raw.strip()
            if raw:
                try:
                    yield json.loads(raw)
                except json.JSONDecodeError:
                    continue


def conclusions(ground_dir, reference_db, lines, t0):
    """Runs the ground's estimator (ground-aeronaves) on lines; returns its summary."""
    sys.path.insert(0, str(ground_dir))
    from ground.config import Config            # noqa: E402 (the estimator's own package)
    from ground.__main__ import run
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        cfg = Config(reference_db=Path(reference_db), telemetry_db=tmp / "t.db",
                     output=tmp / "s.json")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            run(cfg, io.StringIO("".join(json.dumps(x) + "\n" for x in lines)), 0)
        state = json.loads((tmp / "s.json").read_text())["aircraft"]
    result = {}
    for icao, s in state.items():
        summary = s.get("airport_summary") or {}
        def show(key):
            v = summary.get(key)
            if not v:
                return "—"
            ident = (v.get("airport") or {}).get("ident", "?")
            return f"{ident} {v['status']} @{(v['event_ns'] - t0) / 1e9:.0f}s"
        result[icao.upper()] = (show("takeoff"), show("landing"))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="full NDJSON: the onboard record, or a simulation")
    ap.add_argument("--period", type=float, default=5.0, help="snapshot period, s (5)")
    ap.add_argument("--loss", type=float, default=0.0, help="fraction of frames lost (0)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--compare", metavar="GROUND_DIR",
                    help="ground-aeronaves directory: run its estimator on both")
    ap.add_argument("--reference-db", help="the estimator's referencias.db (with --compare)")
    args = ap.parse_args()

    capture = list(read_capture(args.capture))
    downlink = emulate(capture, args.period, args.loss, args.seed)
    if not args.compare:
        for o in downlink:
            sys.stdout.write(json.dumps(o, separators=(",", ":")) + "\n")
        print(f"downlink_emulation: {len(capture)} messages -> {len(downlink)} records",
              file=sys.stderr)
        return
    if not args.reference_db:
        ap.error("--compare needs --reference-db")
    t0 = min(m["rx_epoch_ns"] for m in capture if m.get("rx_epoch_ns"))
    full = conclusions(args.compare, args.reference_db, capture, t0)
    link = conclusions(args.compare, args.reference_db, downlink, t0)
    print(f"capture: {len(capture)} messages; downlink: {len(downlink)} records "
          f"(period {args.period} s, loss {args.loss:.0%})")
    print(f"{'ICAO':<8}{'origin, capture':<26}{'origin, downlink':<26}"
          f"{'destination, capture':<26}{'destination, downlink':<26}")
    for icao in sorted(set(full) | set(link)):
        a, b = full.get(icao, ("—", "—")), link.get(icao, ("—", "—"))
        print(f"{icao:<8}{a[0]:<26}{b[0]:<26}{a[1]:<26}{b[1]:<26}")


if __name__ == "__main__":
    main()
