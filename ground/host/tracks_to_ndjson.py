#!/usr/bin/env python3
"""Downlinked tracks -> the ground estimator's NDJSON.

Reads the ground station's event log (one JSON object per line, as
tools/gs_cli writes it and as the ESP32 will send it over UART) and writes,
for every track record received in a TM_TRACKS frame, one line in the NDJSON
contract of the ground's ADS-B estimator (ground-aeronaves/ground/parser.py,
next to this file): the same fields adsbd records on board,
so the estimator reads the live link exactly as it reads a capture.

The time of each record is rebuilt in this machine's clock (ADR-0007):

    rx_epoch_ns = arrival - time on air - 100 ms x age_ds

with no clock synchronisation involved: arrival is the frame's wall_ms, the
time on air its toa_ms, both written by the ground station. Only the fields
the record's flags mark as valid are written; an absent field stays absent.

The satellite sends every aircraft in every snapshot. When an aircraft sent
nothing new between two snapshots, the same update arrives twice; it is
written once.

  gs_cli --radio udp ... | tracks_to_ndjson.py | python -m ground --input -
  tracks_to_ndjson.py < ground.jsonl > tracks.ndjson
"""

import json
import sys

# common/gama_tm.h
F_POSITION, F_ALTITUDE, F_VELOCITY, F_VRATE, F_ON_GROUND = 1, 2, 4, 8, 16

# Two receptions of one update rebuild the same instant to within the age's
# 100 ms step; a genuinely new update is at least a snapshot period later.
SAME_UPDATE_NS = 250_000_000


def records(event):
    """The ground-contract objects for one gs_cli event (none if not tracks)."""
    if event.get("ev") != "tracks":
        return []
    arrival_ns = int(event["wall_ms"]) * 1_000_000
    toa_ns = int(event.get("toa_ms", 0)) * 1_000_000
    out = []
    for r in event.get("records", []):
        flags = int(r.get("flags", 0))
        o = {"icao": str(r["icao"]).upper(),
             "rx_epoch_ns": arrival_ns - toa_ns - int(r.get("age_ds", 0)) * 100_000_000}
        if flags & F_POSITION:
            o["lat"], o["lon"] = r["lat"], r["lon"]
        if flags & F_ALTITUDE:
            o["altitude_ft"] = r["alt_ft"]
        if flags & F_VELOCITY:
            o["ground_speed_kt"], o["track_deg"] = r["gs_kt"], r["trk"]
        if flags & F_VRATE:
            o["vertical_rate_fpm"] = r["vr_fpm"]
        if flags & F_ON_GROUND:
            o["on_ground"] = 1
        if len(o) == 2:
            continue                       # nothing valid to say
        o.update(source="lora", seq=event.get("seq"), age_ds=r.get("age_ds"),
                 rssi=event.get("rssi"), snr=event.get("snr"))
        out.append(o)
    return out


def convert(lines, write):
    """Converts an iterable of gs_cli lines; returns (written, repeats)."""
    last = {}
    written = repeats = 0
    for raw in lines:
        raw = raw.strip()
        if not raw.startswith("{"):
            continue
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            continue
        for o in records(event):
            key = (o.get("lat"), o.get("lon"), o.get("altitude_ft"))
            prev = last.get(o["icao"])
            if prev is not None and prev[1] == key and abs(o["rx_epoch_ns"] - prev[0]) < SAME_UPDATE_NS:
                repeats += 1
                continue
            last[o["icao"]] = (o["rx_epoch_ns"], key)
            write(json.dumps(o, separators=(",", ":")) + "\n")
            written += 1
    return written, repeats


def main():
    def write(text):
        sys.stdout.write(text)
        sys.stdout.flush()                 # live: the estimator reads as it comes
    try:
        written, repeats = convert(sys.stdin, write)
    except KeyboardInterrupt:
        return
    except BrokenPipeError:
        return
    print(f"tracks_to_ndjson: {written} records written, {repeats} repeats dropped",
          file=sys.stderr)


if __name__ == "__main__":
    main()
