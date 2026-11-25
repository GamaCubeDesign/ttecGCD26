"""On the Pi: how far behind real time the satellite's clock is, for the
last time anchors (SET_TIME) that obcd and adsbd logged. Real time is the
Pi's own clock, so this means something only while it is NTP-synchronised
(timedatectl: NTPSynchronized=yes; on 25/11, through the phone's hotspot).

    ssh pi@gamapi.local python3 - < ancora_ntp.py

Each anchor is unix_s + nsec at mono_ms; the Pi's clock at that mono_ms is
mono_ms plus (CLOCK_REALTIME - CLOCK_MONOTONIC) now, which holds as long as
NTP has not stepped the clock since."""
import json
import time

LOGS = ("/var/lib/gama/obcd.jsonl", "/var/lib/gama/adsbd.jsonl")
offset = time.clock_gettime(time.CLOCK_REALTIME) - time.clock_gettime(time.CLOCK_MONOTONIC)
for path in LOGS:
    anchors = []
    with open(path, "rb") as f:
        for raw in f:
            if b"time_anchor" not in raw:
                continue
            try:
                anchors.append(json.loads(raw))
            except ValueError:
                continue                  # a line cut by a power loss
    for a in anchors[-3:]:
        satellite = a["unix_s"] + a.get("nsec", 0) / 1e9
        real = a["mono_ms"] / 1000 + offset
        print("%s: anchor at mono %.1f s, satellite %.2f s behind real time"
              % (path.rsplit("/", 1)[-1], a["mono_ms"] / 1000, real - satellite))
