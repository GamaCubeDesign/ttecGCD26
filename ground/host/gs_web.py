#!/usr/bin/env python3
"""gs_web.py — the ground station's browser interface.

Reads the JSON lines the ground prints (gs_serial.py on the ESP32, gs_cli on
the UDP radio, or a recorded log), keeps the satellite's state, serves one
page that follows it live, and sends the page's buttons back as the same text
commands one types in the console.

    gs_web.py --serial /dev/ttyUSB0 [--power 2]     the ESP32 ground station
    gs_web.py --udp                                 gs_cli on the UDP radio
    gs_web.py --ssh pi@gamapi.local                 the satellite over ssh, no LoRa
    gs_web.py --serial /dev/ttyUSB0 --ssh pi@...    both: the page's link button
                                                    moves between them (--link)
    gs_web.py --replay docs/vv/bancada/.../x.jsonl  a recorded log, no hardware
    gs_web.py --exec CMD ARGS...                    any program printing the lines

Then open http://127.0.0.1:8080. The state (GsState) knows nothing of sockets
or processes, so it is tested alone; the sources and the HTTP shell only move
lines. ADS-B processing is enabled with --adsb and requires NumPy.
"""

import argparse
import collections
import json
import os
import queue
import re
import signal
from pathlib import Path
import subprocess
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from battery import BatteryEstimator, BatteryModel, model_from

HERE = os.path.dirname(os.path.abspath(__file__))
# The page: plain files and ES modules, no build step (ground/host/web).
WEB_DIR = os.path.join(HERE, "web")
STATIC_TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
                ".js": "text/javascript; charset=utf-8", ".json": "application/json",
                ".png": "image/png", ".svg": "image/svg+xml", ".woff2": "font/woff2",
                ".txt": "text/plain; charset=utf-8"}
# The map's land: the 26 Latin American units of the pair database's region
# (Natural Earth, public domain), served as is.
BASEMAP_PATH = os.path.join(HERE, "adsb", "data", "pares", "geografia", "america_latina.geojson")
# The map's detail over the land, same region and source at 1:10m: urban
# areas, city names, province lines (adsb/data/mapa/gerar.py builds it).
MAPDETAIL_PATH = os.path.join(HERE, "adsb", "data", "mapa", "mapa.json")
# The repository's root: this file is ground/host/gs_web.py.
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
# The bench ground on the UDP radio, as the top-level build makes it.
GS_CLI = os.path.join(REPO, "build", "tools", "gs_cli", "gs_cli")
# The link when the LoRa fails (25/11, ./gama ground --ssh): ttcd on the UDP
# radio on the Pi's loopback (./gama service link ssh) and gs_cli, the ground
# console, on the Pi too, so only its lines and commands cross the network,
# inside ssh. The frames, the ACKs and the page are those of the radio link.
# gs_cli carries this machine's clock (--clock): the SET_TIME it sends on
# contact is the PC's, not the RTC-less Pi's (ADR-0009). A gs_cli left behind
# by a dropped ssh would hold the UDP port, so any is stopped first.
SSH_GS_CLI = "build/tools/gs_cli/gs_cli"

# What the satellite reports for "no data" (common/gama_tc.h).
NO_DATA = {"battery_mv": 65535, "current_ma": -32768, "temp_bat_ccel": -32768,
           "temp_ext_ccel": -32768, "temp_soc_ccel": -32768, "roll_cdeg": -32768,
           "pitch_cdeg": -32768, "yaw_cdeg": -32768, "adsb_msgs": 4294967295}

# The commands the page may send. Anything else is refused: no shutdown, no
# deafen, no console setup. The `armed` ones act on the mechanism or hand the
# bus to the AOCS, so they need the page's arm switch (checked here too).
COMMANDS = [
    (re.compile(r"ping [1-9][0-9]?"), False),
    (re.compile(r"hk|stat|roster|stop|time"), False),
    (re.compile(r"stream [0-9]{1,4}(\.[0-9])?"), False),
    (re.compile(r"rate (safe|nominal|fast)"), False),
    (re.compile(r"mode (mission_adsb|downlink|survival|task_done)"), False),
    (re.compile(r"mode (basic_inter|aocs)"), True),
]


# The deployment from the Pi itself, over ssh (flight/obcd/gama-deploy): the
# fallback for REG-07 when the radio cannot reach the satellite. It is not a
# radio command, so it is not in COMMANDS: the page needs --pi-ssh for it,
# and the arm switch, as for basic_inter. sudo -n: the sudoers rule that
# make install writes on the Pi, never a password typed here.
DEPLOY_REMOTE = "sudo -n /usr/local/sbin/gama-deploy"
DEPLOY_TIMEOUT_S = 30
OBCD_KIND = re.compile(r'^\{"mono_ms":\d+,"event":"([a-z_]+)"')


def ssh_deploy_argv(host):
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", host, DEPLOY_REMOTE]


def parse_deploy(returncode, stdout, stderr):
    """What gama-deploy's output says: {ok, result, why, burn_ms, steps}.
    result is its RESULT word (done, refused, timeout, nolog, noobcd,
    nosignal), or None when it never ran (ssh, sudo)."""
    result, events = None, []
    for line in stdout.splitlines():
        if line.startswith("RESULT "):
            result = line.split()[1] if len(line.split()) > 1 else None
            continue
        # The line's kind is its first "event": obcd's mode and refusal lines
        # carry a second one (the OBC event), which json.loads would keep.
        m = OBCD_KIND.match(line)
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if m and isinstance(obj, dict):
            events.append((m.group(1), obj))
    def first(kind, **want):
        return next((o for k, o in events if k == kind and all(o.get(f) == v for f, v in want.items())), None)
    on, off = first("burnwire", on=True), first("burnwire", on=False)
    steps = {
        "signal": first("local_command") is not None,
        "entered": first("mode", to="BASIC_INTERMEDIATE") is not None,
        "burn_on": on is not None,
        # False: obcd had no GPIO line for the burn-wire, nothing was driven.
        "driven": bool(on and on.get("driven")),
        "burn_off": off is not None,
        "back": first("mode", to="PRE_TEST") is not None,
    }
    refused = first("event_refused")
    err = (stderr or "").strip().splitlines()
    if result is None:
        if any("password is required" in l for l in err):
            why = "sudo asked for a password: run make install on the Pi (it writes /etc/sudoers.d/gama-deploy)"
        elif any("not found" in l for l in err):
            why = "gama-deploy is not installed on the Pi: make install there"
        else:
            why = err[-1] if err else "ssh exit %s" % returncode
    elif result == "refused":
        why = "refused by obcd: the mode is %s, deployment only from PRE_TEST" % (refused or {}).get("mode")
    else:
        why = {"done": "burn-wire pulse done, back in PRE_TEST",
               "timeout": "no return to PRE_TEST in 12 s",
               "nolog": "signal sent; obcd has no log file to follow",
               "noobcd": "obcd is not running (or more than one)",
               "nosignal": "the signal could not be sent"}.get(result, result)
    if result == "done" and not steps["driven"]:
        why += "; but the burn-wire's GPIO was not driven (no line in obcd)"
    burn_ms = off["mono_ms"] - on["mono_ms"] if on and off else None
    return {"ok": result == "done" and steps["driven"], "result": result, "why": why,
            "burn_ms": burn_ms, "steps": steps}


def static_file(url_path):
    """The file under web/ that a GET path names, or None. Nothing outside
    web/ is ever served, whatever the path says."""
    path = url_path.split("?", 1)[0]
    if path.endswith("/"):
        path += "index.html"
    rel = os.path.normpath(path.lstrip("/"))
    if rel.startswith("..") or os.path.isabs(rel):
        return None
    full = os.path.realpath(os.path.join(WEB_DIR, rel))
    if not full.startswith(os.path.realpath(WEB_DIR) + os.sep):
        return None
    if os.path.splitext(full)[1] not in STATIC_TYPES or not os.path.isfile(full):
        return None
    return full


def check_command(cmd, armed):
    """None if `cmd` may go to the ground, else the reason it may not."""
    if not isinstance(cmd, str):
        return "no command"
    for pattern, needs_arm in COMMANDS:
        if pattern.fullmatch(cmd):
            return "arm first" if needs_arm and not armed else None
    return "command not allowed"


class GsState:
    """What the page shows, from the ground's lines. Pure: no I/O, no clock
    but the one it is given."""

    LOG_MAX = 300
    SPARK_MAX = 90
    RUNS_MAX = 20
    HK_HIST_MAX = 180     # 30 min of housekeeping at one every 10 s
    HK_SPARSE_S = 9.0     # the chart and the log keep one HK in ~10 s
    # Modes that end before the next housekeeping (10 s) can show them: the
    # burn-wire pulse (burnwire_ms, 750 ms on obc-v2), the AOCS task without
    # its ESP32. A run of one is complete when SET_MODE was acknowledged and
    # a later HK finds the OBC back in PRE_TEST, where it came from.
    SHORT_MODES = ("BASIC_INTERMEDIATE", "ADVANCED_AOCS")
    INFER_AFTER_S = 1.0   # an HK this soon after the ACK may predate the change
    # The mode each `mode` command asks the OBC for (CDR Figure 44).
    MODE_OF = {"basic_inter": "BASIC_INTERMEDIATE", "aocs": "ADVANCED_AOCS",
               "mission_adsb": "MISSION_ADSB", "downlink": "MISSION_DOWNLINK",
               "survival": "ENV_SURVIVAL", "task_done": "PRE_TEST"}
    # GAMA_HK_F_* (common/gama_tm.h).
    HK_FLAGS = (("obc_linked", 1), ("adsb_linked", 2), ("time_synced", 4), ("dump1090_up", 8))

    def __init__(self, source="", now=time.time, battery=None):
        self.now = now
        self.source = source
        self.log = collections.deque(maxlen=self.LOG_MAX)
        self.rssi = collections.deque(maxlen=self.SPARK_MAX)
        self.snr = collections.deque(maxlen=self.SPARK_MAX)
        self.hk = None
        self.hk_at = None
        self.stat = None
        self.frame_at = None
        self.contact = None
        self.mode = None
        self.mode_at = None
        self.ground = {}
        self.error = None
        self.ack_ok = 0
        self.ack_bad = 0
        self.tc_failed = 0
        self.rx_bad = 0
        self.latencies = collections.deque(maxlen=50)
        self.tracks = None
        self.seq = 0
        # Every `mode` command, followed from its echo to the OBC leaving the
        # mode it asked for: the evidence of each test, step by step.
        self.runs = collections.deque(maxlen=self.RUNS_MAX)
        self.run = None
        # The OBC tab's series: [t, battery_mv, current_ma, temp_bat, temp_ext,
        # temp_soc, soc_pct], no-data sentinels as None.
        self.hk_hist = collections.deque(maxlen=self.HK_HIST_MAX)
        # The fast HK (FastHk): its ACKs out of the log, one HK line and one
        # chart point every ~10 s, as at NOMINAL; the estimate sees them all.
        self.quiet_hk = False
        self.hk_hist_t = None
        self.hk_note_t = None
        # The charge, autonomy and charging, from the HK alone (ADR-0024).
        self.battery = BatteryEstimator(battery)

    def _note(self, kind, text):
        self.seq += 1
        self.log.append({"n": self.seq, "t": self.now(), "kind": kind, "text": text})

    def note(self, kind, text):
        self._note(kind, text)

    def feed(self, obj):
        """One line from the ground, already parsed."""
        ev = obj.get("ev")
        if ev is None:
            return
        if "rssi" in obj and "snr" in obj:
            self.frame_at = self.now()
            self.rssi.append(obj["rssi"])
            self.snr.append(obj["snr"])
        if ev == "start":
            self.ground = {"radio": obj.get("radio"), "profile": obj.get("profile")}
            self.error = None
            self._note("info", "ground: %s, %s" % (obj.get("radio"), obj.get("profile")))
        elif ev == "hk":
            self._hk(obj)
        elif ev == "stat":
            self.stat = {k: obj.get(k) for k in (
                "msgs_received", "msgs_decoded", "aircraft", "tm_frames_sent",
                "tc_frames_rx", "tc_frames_bad", "latency_p95_ms", "dump1090_restarts")}
            self._note("tm", "stat: %s aircraft, %s messages" % (obj.get("aircraft"), obj.get("msgs_received")))
        elif ev == "contact":
            self.contact = bool(obj.get("up"))
            self._note("ok" if self.contact else "bad", "contact " + ("up" if self.contact else "lost"))
        elif ev == "ack":
            self._run_ack(obj)
            ok = obj.get("status") == "OK"
            if ok:
                self.ack_ok += 1
                self.latencies.append(obj.get("latency_ms", 0))
            else:
                self.ack_bad += 1
            if not (ok and self.quiet_hk and obj.get("cmd") == "REQ_HK"):
                self._note("ok" if ok else "bad", "%s: %s (%s ms, try %s)" % (
                    obj.get("cmd"), obj.get("status"), obj.get("latency_ms"), obj.get("attempts")))
        elif ev == "tc_failed":
            self._run_ack(dict(obj, status="NO_ACK"))
            self.tc_failed += 1
            self._note("bad", "%s failed after %s attempts" % (obj.get("cmd"), obj.get("attempts")))
        elif ev == "command":
            self._note("cmd", "> " + str(obj.get("text")))
            self._command(str(obj.get("text") or ""))
        elif ev == "error":
            run = self.run
            if run is not None and run["entered"] is None and run["failed"] is None and run["acks"] == [] \
                    and "not" in str(obj.get("what", "")):
                run["failed"] = str(obj.get("what"))      # e.g. no wall clock: nothing sent
            self.error = obj.get("what")
            self._note("bad", "error: %s" % obj.get("what"))
        elif ev == "link":
            self.contact = None           # the new path has not heard the satellite yet
            self.ground = {}
            self.error = None
            self._note("info", "link: now %s (%s)" % (obj.get("to"), obj.get("why")))
        elif ev == "rx_crc_error":
            self.rx_bad += 1
            self._note("bad", "frame with a bad CRC")
        elif ev == "loss":
            self._note("bad", "downlink loss: %s" % json.dumps({k: v for k, v in obj.items() if k not in ("mono_ms", "wall_ms", "ev")}))
        elif ev == "tracks":
            run = self.run
            if run is not None and run["target"] == "MISSION_ADSB" and run["left"] is None:
                run["tracks"] += 1
                if run["first_tracks"] is None:
                    run["first_tracks"] = self.now()
            self.tracks = obj.get("n", self.tracks)
            self._note("tm", "tracks: %s records" % obj.get("n"))
        elif ev in ("roster", "rate", "per", "ping_summary", "summary", "auto_time"):
            body = {k: v for k, v in obj.items() if k not in ("mono_ms", "wall_ms", "ev", "payload")}
            self._note("info", "%s %s" % (ev, json.dumps(body, separators=(",", ":"))))

    def _command(self, text):
        parts = text.split()
        if len(parts) != 2 or parts[0] != "mode":
            return
        self.run = {"n": self.seq, "cmd": text, "target": self.MODE_OF.get(parts[1]),
                    "sent": self.now(), "acks": [], "failed": None, "entered": None,
                    "left": None, "left_to": None, "dump1090": None,
                    "first_tracks": None, "tracks": 0, "from": self.mode, "inferred": False}
        self.runs.append(self.run)

    def _run_ack(self, obj):
        run = self.run
        if run is None or run["entered"] is not None or obj.get("cmd") not in ("SET_TIME", "SET_MODE"):
            return
        run["acks"].append({"cmd": obj.get("cmd"), "status": obj.get("status"),
                            "latency_ms": obj.get("latency_ms"), "attempts": obj.get("attempts"),
                            "t": self.now()})
        if obj.get("status") != "OK":
            run["failed"] = "%s %s" % (obj.get("cmd"), obj.get("status"))

    def _run_mode(self, mode, flags):
        run = self.run
        if run is None or run["target"] is None:
            return
        now = self.now()
        if run["entered"] is None and mode == run["target"]:
            run["entered"] = now
        elif run["entered"] is None and self._short_run_done(run, mode, now):
            # Entered and left between two HK: the ACK is when it entered.
            run["entered"] = next(a["t"] for a in run["acks"] if a["cmd"] == "SET_MODE")
            run["left"], run["left_to"], run["inferred"] = now, mode, True
        elif run["entered"] is not None and run["left"] is None and mode != run["target"]:
            run["left"], run["left_to"] = now, mode
        if (run["target"] == "MISSION_ADSB" and run["entered"] is not None and run["left"] is None
                and run["dump1090"] is None and isinstance(flags, int) and flags & 8):
            run["dump1090"] = now

    def _short_run_done(self, run, mode, now):
        if run["target"] not in self.SHORT_MODES or run["failed"] is not None:
            return False
        if mode != "PRE_TEST" or run["from"] != "PRE_TEST":
            return False
        ack = [a for a in run["acks"] if a["cmd"] == "SET_MODE" and a["status"] == "OK"]
        return bool(ack) and now - ack[-1]["t"] >= self.INFER_AFTER_S

    def _hk(self, obj):
        self.hk = {k: obj.get(k) for k in (
            "battery_mv", "current_ma", "temp_bat_ccel", "temp_ext_ccel", "temp_soc_ccel",
            "roll_cdeg", "pitch_cdeg", "yaw_cdeg", "adsb_msgs", "obc_mode", "link",
            "uptime_s", "flags", "rssi", "snr")}
        self.hk_at = self.now()
        mode = obj.get("obc_mode")
        if mode != self.mode:
            if self.mode is not None:
                self._note("mode", "mode %s → %s" % (self.mode, mode))
            self.mode = mode
            self.mode_at = self.now()
        self._run_mode(mode, obj.get("flags"))
        self.battery.feed(self.hk_at, obj.get("battery_mv"), obj.get("current_ma"), obj.get("uptime_s"))
        row = ([round(self.hk_at, 1)] + [
            None if NO_DATA.get(k) == obj.get(k) else obj.get(k)
            for k in ("battery_mv", "current_ma", "temp_bat_ccel", "temp_ext_ccel", "temp_soc_ccel")]
            + [self.battery.estimate()["soc_pct"]])
        if self.hk_hist_t is None or self.hk_at - self.hk_hist_t >= self.HK_SPARSE_S:
            self.hk_hist_t = self.hk_at
            self.hk_hist.append(row)
        parts = [str(mode)]
        for key, div, unit in (("battery_mv", 1000, "V"), ("current_ma", 1, "mA"),
                               ("temp_bat_ccel", 100, "°C"), ("temp_soc_ccel", 100, "°C SoC")):
            v = obj.get(key)
            if isinstance(v, (int, float)) and NO_DATA.get(key) != v:
                parts.append(("%.2f %s" if div == 1000 else "%.1f %s" if div == 100 else "%d %s") % (v / div, unit))
        if not self.quiet_hk or self.hk_note_t is None or self.hk_at - self.hk_note_t >= self.HK_SPARSE_S:
            self.hk_note_t = self.hk_at
            self._note("tm", "hk: " + " · ".join(parts))

    def snapshot(self, since=None):
        """The page's view. With `since`, only the log entries after that
        number (the event stream sends deltas); without, the last 120."""
        now = self.now()

        def age(t):
            return None if t is None else round(now - t, 1)

        hk = None
        if self.hk is not None:
            hk = {k: (None if NO_DATA.get(k) == v else v) for k, v in self.hk.items()}
            flags = self.hk.get("flags")
            if isinstance(flags, int):
                hk.update({name: bool(flags & bit) for name, bit in self.HK_FLAGS})
        lat = list(self.latencies)
        log = list(self.log)
        log = [e for e in log if e["n"] > since] if since is not None else log[-120:]
        return {
            "source": self.source, "ground": self.ground, "error": self.error,
            "contact": self.contact, "hk": hk, "hk_age": age(self.hk_at),
            "frame_age": age(self.frame_at), "mode": self.mode, "mode_age": age(self.mode_at),
            "stat": self.stat, "tracks": self.tracks,
            "rssi": list(self.rssi), "snr": list(self.snr),
            "acks": {"ok": self.ack_ok, "bad": self.ack_bad, "tc_failed": self.tc_failed,
                     "rx_bad": self.rx_bad,
                     "latency_ms": round(sum(lat) / len(lat)) if lat else None},
            "log": log, "log_n": self.seq, "full": since is None,
            "runs": [dict(r, acks=list(r["acks"])) for r in self.runs],
            "hk_hist": list(self.hk_hist),
            "battery": self.battery.estimate(),
            "now": now,
        }


class FastHk:
    """The page's fast housekeeping, for the EPS test (ADR-0024): one `hk`
    every PERIOD_S, so that the battery's state follows the light in ~6 s
    instead of 10-20 s. The next `hk` waits for the previous one's ACK (the
    console reads a command only after the last one is acknowledged, and
    over LoRa a retry takes seconds), and the whole thing turns itself off
    after MAX_S: over LoRa each cycle is ~0.5 s on air (REQ_HK, its ACK and
    the HK of 0.247 s), a quarter of the channel. Pure: the clock is given."""

    PERIOD_S = 2.0
    MAX_S = 900.0
    ACK_TIMEOUT_S = 15.0

    def __init__(self):
        self.until = None
        self.sent_at = None
        self.waiting = False

    def start(self, now):
        self.until, self.sent_at, self.waiting = now + self.MAX_S, None, False

    def stop(self):
        self.until, self.waiting = None, False

    def on(self, now):
        if self.until is not None and now >= self.until:
            self.until = None
        return self.until is not None

    def due(self, now):
        if not self.on(now):
            return False
        if self.waiting and now - self.sent_at < self.ACK_TIMEOUT_S:
            return False
        return self.sent_at is None or now - self.sent_at >= self.PERIOD_S

    def sent(self, now):
        self.sent_at, self.waiting = now, True

    def acked(self):
        self.waiting = False

    def snapshot(self, now):
        on = self.on(now)
        return {"on": on, "left_s": round(self.until - now) if on else None,
                "period_s": self.PERIOD_S}


class Hub:
    """The state, the clients following it, and the source that feeds it.

    Lines from the ground update the state at once; the pages hear about it
    at most every PUSH_MIN_S, and only the log entries they have not had
    yet: a burst of telemetry costs one message, not one per line."""

    PUSH_MIN_S = 0.5
    IDLE_PUSH_S = 2.0       # the ADS-B counters move on their own

    def __init__(self, state, source, pipeline=None, mission_s=600, pi_ssh=None,
                 runner=subprocess.run, links=None, link=None, link_host=None):
        self.pipeline = pipeline
        # The ways to the satellite the page can move between, {"lora": make
        # the ESP32's source, "ssh": make the ssh one}, when it has both; the
        # one in use; and the Pi that gama-link runs on.
        self.links = links or {}
        self.link = link
        self.link_host = link_host
        self.switching = threading.Lock()
        # The Pi for the deployment over ssh (--pi-ssh), None without one.
        self.pi_ssh = pi_ssh
        self.runner = runner
        self.deploying = threading.Lock()
        # How long obcd's MISSION_ADSB lasts (its adsb_timeout_ms, OBC-6): the
        # page counts down from it. 600 s in flight; a bench run may be
        # shorter, and then both sides are told the same number.
        self.mission_s = mission_s
        self.state = state
        self.source = source
        self.lock = threading.Lock()
        self.wake = threading.Condition(self.lock)
        self.clients = set()
        self.dirty = False
        self.closed = False
        self.pushed_n = 0
        self.pusher = threading.Thread(target=self._pusher, name="gs-web-push", daemon=True)
        self.pusher.start()
        self.fast_hk = FastHk()
        self.mono = time.monotonic
        threading.Thread(target=self._fast_hk_loop, name="gs-web-fasthk", daemon=True).start()

    def feed(self, obj):
        if self.pipeline:
            self.pipeline.feed(obj)
        with self.lock:
            if obj.get("ev") in ("ack", "tc_failed") and obj.get("cmd") == "REQ_HK":
                self.fast_hk.acked()
            self.state.feed(obj)
            self.dirty = True
            self.wake.notify()

    def note(self, kind, text):
        with self.lock:
            self.state.note(kind, text)
            self.dirty = True
            self.wake.notify()

    def snapshot(self):
        with self.lock:
            return self._snapshot()

    def _snapshot(self, since=None):
        result = self.state.snapshot(since)
        result["mission_s"] = self.mission_s
        result["fast_hk"] = self.fast_hk.snapshot(self.mono())
        result["link"] = {"current": self.link, "options": sorted(self.links),
                          "busy": self.switching.locked()}
        if self.pipeline:
            result["adsb"] = self.pipeline.snapshot()
        return result

    def subscribe(self):
        q = queue.Queue(maxsize=50)
        with self.lock:
            self.clients.add(q)
            q.put(json.dumps(self._snapshot()))
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.clients.discard(q)

    def close(self):
        with self.lock:
            self.closed = True
            self.wake.notify()

    def set_fast_hk(self, on):
        """The page's fast HK on or off: {ok, why}."""
        if on and isinstance(self.source, ReplaySource):
            return {"ok": False, "why": "replay: no hardware commands"}
        with self.lock:
            if on:
                self.fast_hk.start(self.mono())
                self.state.note("info", "fast HK on: one every %.0f s for %.0f min"
                                % (FastHk.PERIOD_S, FastHk.MAX_S / 60))
            else:
                self.fast_hk.stop()
                self.state.note("info", "fast HK off")
            self.state.quiet_hk = on
            self.dirty = True
            self.wake.notify()
        return {"ok": True, "why": "on" if on else "off"}

    def _fast_hk_loop(self):
        while True:
            time.sleep(0.25)
            with self.lock:
                if self.closed:
                    return
                now = self.mono()
                due = self.fast_hk.due(now)
                if due:
                    self.fast_hk.sent(now)
                elif self.state.quiet_hk and not self.fast_hk.on(now):
                    self.state.quiet_hk = False         # MAX_S ran out
                    self.state.note("info", "fast HK off (time)")
                    self.dirty = True
                    self.wake.notify()
            if due and not self.source.send("hk"):
                with self.lock:
                    self.fast_hk.stop()
                    self.state.quiet_hk = False
                    self.state.note("bad", "fast HK off: the ground is not running")
                    self.dirty = True
                    self.wake.notify()

    def _pusher(self):
        last = 0.0
        while True:
            with self.lock:
                while not self.dirty and not self.closed:
                    if not self.wake.wait(timeout=self.IDLE_PUSH_S) and self.pipeline is not None:
                        self.dirty = True
                if self.closed:
                    return
            wait = last + self.PUSH_MIN_S - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            with self.lock:
                self.dirty = False
                self._push_locked()
            last = time.monotonic()

    def _push_locked(self):
        text = json.dumps(self._snapshot(since=self.pushed_n))
        self.pushed_n = self.state.seq
        for q in list(self.clients):
            try:
                q.put_nowait(text)
            except queue.Full:
                self.clients.discard(q)      # a stalled page: it reconnects

    def send(self, cmd, armed):
        why = check_command(cmd, armed)
        if why is not None:
            return False, why
        if isinstance(self.source, ReplaySource):
            return False, "replay: no hardware commands"
        if cmd == "mode mission_adsb" and self.pipeline and self.pipeline.snapshot()["error"]:
            return False, "ADS-B processing unavailable"
        if not self.source.send(cmd):
            return False, "the ground is not running"
        return True, "sent"


    def switch_link(self, to):
        """The satellite's ttcd onto another link (gama-link over ssh), then
        the page's source after it. Blocks the request's thread up to
        LINK_TIMEOUT_S; one at a time."""
        if to not in self.links or len(self.links) < 2 or not self.link_host:
            return {"ok": False, "result": None, "why": "no link switch: start gs_web with --serial and --ssh"}
        if not self.switching.acquire(blocking=False):
            return {"ok": False, "result": None, "why": "a link switch is already running"}
        try:
            if to == self.link:
                return {"ok": True, "result": to, "why": "already on %s" % to}
            self.note("cmd", "> link %s (gama-link on %s)" % (to, self.link_host))
            try:
                r = self.runner(ssh_link_argv(self.link_host, to), capture_output=True, text=True,
                                timeout=LINK_TIMEOUT_S)
                out = parse_link(r.returncode, r.stdout, r.stderr)
            except subprocess.TimeoutExpired:
                out = {"ok": False, "result": None, "why": "ssh gave no answer in %d s" % LINK_TIMEOUT_S}
            except OSError as exc:
                out = {"ok": False, "result": None, "why": "ssh: %s" % exc}
            if out["result"] != to:
                self.note("bad", "link: " + out["why"])
                return out
            # The satellite moved (its ttcd may still be restarting): the page follows.
            self.source.stop()
            source = self.links[to]()
            with self.lock:
                self.source, self.link = source, to
                self.state.source = LINK_SOURCE[to]
                self.dirty = True
            self.feed({"ev": "link", "to": to, "ttcd": out.get("ttcd"), "why": out["why"]})
            source.start(self)
            return out
        finally:
            self.switching.release()

    def deploy_ssh(self, armed):
        """REG-07's fallback: gama-deploy on the Pi over ssh. Blocks the
        request's thread for the pulse (about 6 s); one at a time."""
        if not self.pi_ssh:
            return {"ok": False, "result": None, "why": "no Pi for ssh: start gs_web with --pi-ssh HOST"}
        if not armed:
            return {"ok": False, "result": None, "why": "arm first"}
        if not self.deploying.acquire(blocking=False):
            return {"ok": False, "result": None, "why": "a deployment over ssh is already running"}
        try:
            self.note("cmd", "> deploy over ssh (%s)" % self.pi_ssh)
            try:
                r = self.runner(ssh_deploy_argv(self.pi_ssh), capture_output=True, text=True,
                                timeout=DEPLOY_TIMEOUT_S)
                out = parse_deploy(r.returncode, r.stdout, r.stderr)
            except subprocess.TimeoutExpired:
                out = {"ok": False, "result": None, "why": "ssh gave no answer in %d s" % DEPLOY_TIMEOUT_S}
            except OSError as exc:
                out = {"ok": False, "result": None, "why": "ssh: %s" % exc}
            text = "deploy over ssh: " + out["why"]
            if out.get("burn_ms") is not None:
                text += " (pulse %d ms)" % out["burn_ms"]
            self.note("ok" if out["ok"] else "bad", text)
            return out
        finally:
            self.deploying.release()


# The satellite's link, from the page (25/11): flight/ttcd/gama-link on the
# Pi, over ssh, with the sudoers rule make install writes there (sudo -n),
# then the page's source follows. The satellite moves first: if it cannot,
# the page stays where it was.
LINK_HELPER = "/usr/local/sbin/gama-link"
LINK_DROPIN = "/etc/systemd/system/ttcd.service.d/link-ssh.conf"
LINK_TIMEOUT_S = 30
LINK_SOURCE = {"lora": "serial", "ssh": "ssh"}


def ssh_link_argv(host, to):
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", host, "sudo -n %s %s" % (LINK_HELPER, to)]


def parse_link(returncode, stdout, stderr):
    """What gama-link's output says: {ok, result, ttcd, why}. result is the
    link it put ttcd on, or None when it never ran (ssh, sudo)."""
    result = ttcd = None
    for line in stdout.splitlines():
        f = line.split()
        if len(f) >= 2 and f[0] == "RESULT":
            result, ttcd = f[1], (f[2] if len(f) > 2 else None)
    err = (stderr or "").strip().splitlines()
    if result in ("lora", "ssh"):
        why = "satellite on %s, ttcd %s" % ("LoRa" if result == "lora" else "ssh", ttcd)
        if returncode != 0:
            why += (": the RA-02 does not answer?" if result == "lora" else ": see ./gama service logs ttcd")
        return {"ok": returncode == 0, "result": result, "ttcd": ttcd, "why": why}
    if any("password is required" in l for l in err):
        why = "sudo asked for a password: run make install on the Pi (it writes /etc/sudoers.d/gama-link)"
    elif any("not found" in l for l in err):
        why = "gama-link is not installed on the Pi: ./gama service install"
    else:
        why = err[-1] if err else "ssh exit %s" % returncode
    return {"ok": False, "result": None, "ttcd": None, "why": why}


def query_link(host, runner=subprocess.run):
    """'lora' or 'ssh', as the Pi's ttcd is now; None when ssh fails."""
    try:
        r = runner(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", host,
                    "test -f %s && echo ssh || echo lora" % LINK_DROPIN],
                   capture_output=True, text=True, timeout=15)
    except (subprocess.TimeoutExpired, OSError):
        return None
    word = (r.stdout or "").strip()
    return word if r.returncode == 0 and word in ("lora", "ssh") else None


def ssh_ground_argv(host, directory, clock_ms):
    remote = (f"cd {directory} && {{ pkill -x gs_cli && sleep 0.5; true; }} && "
              f"exec {SSH_GS_CLI} --radio udp --auto-time on --clock {clock_ms}")
    return ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=6",
            "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3", host, remote]


class ProcessSource:
    """A program printing the ground's JSON lines and reading its commands."""

    def __init__(self, argv):
        self.argv = argv
        self.proc = None
        self.send_lock = threading.Lock()
        self.reader = None
        self.stopping = False         # stopped on purpose: its end is no error

    def start(self, hub):
        self.proc = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, args=(hub,), daemon=True)
        self.reader.start()

    def _pump(self, hub):
        """The process's lines into the hub until it ends; how many."""
        n = 0
        for raw in self.proc.stdout:
            raw = raw.strip()
            if not raw.startswith("{"):
                continue
            try:
                hub.feed(json.loads(raw))
                n += 1
            except ValueError:
                continue
        try:                          # reaped: a reconnecting source makes many
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        return n

    def _read(self, hub):
        self._pump(hub)
        if not self.stopping:
            hub.feed({"ev": "error", "what": "the ground process ended"})

    def send(self, cmd):
        if self.proc is None or self.proc.poll() is not None:
            return False
        try:
            with self.send_lock:
                self.proc.stdin.write(cmd + "\n")
                self.proc.stdin.flush()
            return True
        except OSError:
            return False

    def stop(self):
        self.stopping = True
        if self.proc is not None and self.proc.poll() is None:
            try:
                self.proc.stdin.write("quit\n")
                self.proc.stdin.flush()
                self.proc.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
        if self.reader:
            self.reader.join(timeout=3)


class ReplaySource:
    """A recorded log played back at its own pace, for rehearsing without
    hardware. Commands are not answered: the page says so."""

    def __init__(self, path, speed=1.0, loop=False):
        self.path, self.speed, self.loop = path, speed, loop
        self.hub = None
        self.alive = True
        self.reader = None

    def start(self, hub):
        self.hub = hub
        self.reader = threading.Thread(target=self._play, daemon=True)
        self.reader.start()

    def _play(self):
        while self.alive:
            last = None
            try:
                f = open(self.path, encoding="utf-8")
            except OSError as e:
                self.hub.feed({"ev": "error", "what": "replay: %s" % e.strerror})
                return
            with f:
                for raw in f:
                    if not self.alive:
                        return
                    try:
                        obj = json.loads(raw)
                    except ValueError:
                        continue
                    t = obj.get("mono_ms")
                    if last is not None and t is not None and t > last:
                        time.sleep(min((t - last) / 1000.0 / self.speed, 2.0))
                    if not self.alive:
                        return
                    last = t if t is not None else last
                    self.hub.feed(obj)
            if not self.loop:
                self.hub.note("info", "end of the recorded log")
                return

    def send(self, cmd):
        self.hub.note("info", "replay: '%s' not sent (no ground)" % cmd)
        return True

    def stop(self):
        self.alive = False
        if self.reader:
            self.reader.join(timeout=3)


def make_handler(hub):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def _send(self, code, body, ctype):
            data = body if isinstance(body, bytes) else body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/api/adsb":
                self._adsb()
            elif path == "/api/basemap":
                self._file(BASEMAP_PATH, "application/geo+json")
            elif path == "/api/mapdetail":
                self._file(MAPDETAIL_PATH, "application/json")
            elif path == "/api/airports":
                if hub.pipeline is None:
                    self._send(HTTPStatus.OK, "[]", "application/json")
                else:
                    self._send(HTTPStatus.OK, hub.pipeline.airports_json(), "application/json")
            elif hub.pipeline and path in ("/adsb/", "/adsb/data", "/adsb/sessions"):
                try:
                    body, ctype = hub.pipeline.page(self.path)
                    self._send(HTTPStatus.OK, body, ctype)
                except Exception as exc:
                    self._send(HTTPStatus.SERVICE_UNAVAILABLE, str(exc), "text/plain")
            elif path == "/api/deploy-ssh":
                self._send(HTTPStatus.OK, json.dumps({"host": hub.pi_ssh}), "application/json")
            elif path == "/api/link":
                self._send(HTTPStatus.OK, json.dumps(hub.snapshot()["link"]), "application/json")
            elif path == "/api/state":
                self._send(HTTPStatus.OK, json.dumps(hub.snapshot()), "application/json")
            elif path == "/events":
                self._events()
            else:
                full = static_file(path)
                if full is None:
                    self._send(HTTPStatus.NOT_FOUND, "not found", "text/plain")
                else:
                    self._file(full, STATIC_TYPES[os.path.splitext(full)[1]])

        def _file(self, full, ctype):
            try:
                with open(full, "rb") as f:
                    self._send(HTTPStatus.OK, f.read(), ctype)
            except OSError:
                self._send(HTTPStatus.NOT_FOUND, "not found", "text/plain")

        def _adsb(self):
            """The aircraft, as the page shows them; 304 while nothing changed."""
            if hub.pipeline is None:
                self._send(HTTPStatus.OK, '{"revision":0,"aircraft":[]}', "application/json")
                return
            revision, body = hub.pipeline.summary()
            tag = '"%d"' % revision
            if self.headers.get("If-None-Match") == tag:
                self.send_response(HTTPStatus.NOT_MODIFIED)
                self.send_header("ETag", tag)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("ETag", tag)
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)

        def _events(self):
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            q = hub.subscribe()
            try:
                while True:
                    try:
                        text = q.get(timeout=10)
                        self.wfile.write(("data: %s\n\n" % text).encode("utf-8"))
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except OSError:
                pass
            finally:
                hub.unsubscribe(q)

        def do_POST(self):
            if self.path not in ("/api/cmd", "/api/deploy-ssh", "/api/link", "/api/fast-hk"):
                self._send(HTTPStatus.NOT_FOUND, "not found", "text/plain")
                return
            try:
                n = min(int(self.headers.get("Content-Length", "0")), 1024)
                body = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                self._send(HTTPStatus.BAD_REQUEST, json.dumps({"ok": False, "why": "bad request"}),
                           "application/json")
                return
            if self.path == "/api/link":
                out = hub.switch_link(str(body.get("to")))
                self._send(HTTPStatus.OK if out["result"] else HTTPStatus.FORBIDDEN,
                           json.dumps(out), "application/json")
                return
            if self.path == "/api/fast-hk":
                out = hub.set_fast_hk(bool(body.get("on")))
                self._send(HTTPStatus.OK if out["ok"] else HTTPStatus.FORBIDDEN,
                           json.dumps(out), "application/json")
                return
            if self.path == "/api/deploy-ssh":
                out = hub.deploy_ssh(bool(body.get("armed")))
                self._send(HTTPStatus.OK if out["ok"] or out.get("result") else HTTPStatus.FORBIDDEN,
                           json.dumps(out), "application/json")
                return
            ok, why = hub.send(body.get("cmd"), bool(body.get("armed")))
            self._send(HTTPStatus.OK if ok else HTTPStatus.FORBIDDEN,
                       json.dumps({"ok": ok, "why": why}), "application/json")

    return Handler


class SshSource(ProcessSource):
    """gs_cli on the Pi over ssh. A dropped ssh is started again every
    RETRY_S until the source is stopped: on 25/11 the Pi's Wi-Fi went away
    for about a minute, ssh gave up after 15 s without an answer
    (ServerAliveInterval x CountMax) and the page stayed without a ground.
    ttcd runs on regardless; each new run carries the clock read then, and
    gs_cli's --auto-time sends SET_TIME again on contact."""

    RETRY_S = 5.0

    def __init__(self, host, directory, clock=time.time, retry_s=None):
        super().__init__(None)
        self.host, self.directory, self.clock = host, directory, clock
        self.retry_s = self.RETRY_S if retry_s is None else retry_s
        self.wakeup = threading.Event()

    def _spawn(self):
        self.argv = ssh_ground_argv(self.host, self.directory, int(self.clock() * 1000))
        self.proc = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)

    def start(self, hub):
        self._spawn()
        self.reader = threading.Thread(target=self._run, args=(hub,), daemon=True)
        self.reader.start()

    def _run(self, hub):
        down = False
        while True:
            if self._pump(hub) and down:
                down = False
            if self.stopping:
                return
            if not down:
                down = True
                hub.feed({"ev": "error", "what": "ssh to %s dropped: trying again every %g s"
                          % (self.host, self.retry_s)})
            if self.wakeup.wait(self.retry_s):
                return
            try:
                self._spawn()
            except OSError:
                continue

    def stop(self):
        self.stopping = True
        self.wakeup.set()
        super().stop()


def serial_source(args):
    argv = [sys.executable, os.path.join(HERE, "gs_serial.py"), "--port", args.serial,
            "--auto-time", "on"]
    if args.power is not None:
        argv += ["--power", str(args.power)]
    return ProcessSource(argv)


def build_links(args):
    """{link: a function making its source} for --serial (lora) and --ssh."""
    links = {}
    if getattr(args, "serial", None):
        links["lora"] = lambda: serial_source(args)
    if getattr(args, "ssh", None):
        links["ssh"] = lambda: SshSource(args.ssh, args.ssh_dir)
    return links


def initial_link(args, links, runner=subprocess.run):
    """With both links, --link, else the one the satellite is on (asked over
    ssh), else lora; with one, that one."""
    if len(links) == 1:
        return next(iter(links))
    if getattr(args, "link", None):
        return args.link
    return query_link(args.ssh, runner) or "lora"


def build_source(args, runner=subprocess.run):
    if args.replay:
        return ReplaySource(args.replay, args.speed, args.loop), "replay"
    if args.udp:
        return ProcessSource([GS_CLI, "--radio", "udp"]), "udp"
    if args.exec:
        return ProcessSource(args.exec), "exec"
    links = build_links(args)
    link = initial_link(args, links, runner)
    return links[link](), LINK_SOURCE[link]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group()
    ap.add_argument("--serial", metavar="PORT", help="the ESP32 ground station, e.g. /dev/ttyUSB0")
    src.add_argument("--udp", action="store_true", help="gs_cli on the UDP radio (build/ first)")
    src.add_argument("--replay", metavar="FILE", help="a recorded ground log (.jsonl)")
    ap.add_argument("--ssh", metavar="HOST",
                    help="the satellite over ssh, no LoRa: gs_cli on the Pi's UDP radio "
                         "(ttcd with radio=udp: ./gama service link ssh); with --serial too, "
                         "the page's link button moves between the two")
    ap.add_argument("--link", choices=("lora", "ssh"),
                    help="with --serial and --ssh, the one to start on (default: the satellite's)")
    src.add_argument("--exec", nargs=argparse.REMAINDER, help="any program printing the lines")
    ap.add_argument("--power", type=int, help="ground power, dBm (with --serial)")
    ap.add_argument("--ssh-dir", default="ttec", help="the repository on the Pi, from its home (with --ssh)")
    ap.add_argument("--speed", type=float, default=1.0, help="replay speed")
    ap.add_argument("--loop", action="store_true", help="replay again at the end")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--adsb", action="store_true", help="process received aircraft continuously")
    ap.add_argument("--mission-s", type=int, default=600,
                    help="MISSION_ADSB's length, s: obcd's adsb_timeout_ms / 1000 (600 in flight)")
    ap.add_argument("--pi-ssh", metavar="HOST",
                    help="the Pi, for the deployment over ssh (REG-07's fallback), e.g. pi@gamapi.local")
    ap.add_argument("--battery-mah", type=float, default=BatteryModel.capacity_mah,
                    help="the pack's capacity, mAh (NCR18650B 2S1P: 3200 minimum, 3350 typical)")
    ap.add_argument("--battery-r-ohm", type=float,
                    help="cells to INA219, Ohm, measured on the bench; default: measured from the HK")
    ap.add_argument("--battery-diode-v", type=float, default=BatteryModel.diode_v,
                    help="the EPS's D1 drop, V: P+ minus the INA219's reading at a light load")
    ap.add_argument("--session-dir", type=Path, default=Path(HERE)/"sessions")
    ap.add_argument("--session-prefix", default="mission-",
                    help="the session folder's prefix: sim-mission- or replay-mission- when not all real")
    from adsb.estimator.config import Config, add_pair_arguments, pair_options
    ap.add_argument("--reference-db", type=Path, default=Config().reference_db)
    add_pair_arguments(ap)
    # The pair method stays off unless asked for (ADR-0022): on real traffic
    # (17/11) and on straight synthetic flights it was wrong on 43 to 75 % of
    # its origins, and with it on the combined conclusion never decided.
    ap.add_argument("--pairs", action="store_true", dest="pairs_enabled",
                    help="also run the route-pair method of ADR-0019")
    ap.set_defaults(pairs_enabled=False)
    args = ap.parse_args(argv)
    pipeline = None
    if args.adsb:
        from adsb.pipeline import AdsbPipeline
        from dataclasses import replace
        try:
            pipeline = AdsbPipeline(args.session_dir, replace(Config(), reference_db=args.reference_db, **pair_options(ap,args)),
                                    prefix=args.session_prefix)
        except Exception as exc:
            ap.exit(1, f"ADS-B startup failed: {exc}\n")

    if bool(args.replay or args.udp or args.exec) == bool(args.serial or args.ssh):
        ap.error("one source: --serial and/or --ssh, or one of --udp, --replay, --exec")
    source, name = build_source(args)
    links = build_links(args) if name in ("serial", "ssh") else {}
    if len(links) < 2:
        links = {}
    if args.mission_s <= 0:
        ap.error("--mission-s must be positive")
    if args.battery_mah <= 0 or (args.battery_r_ohm or 0) < 0 or args.battery_diode_v < 0:
        ap.error("--battery-mah must be positive, --battery-r-ohm and --battery-diode-v not negative")
    model = model_from(args.battery_mah, args.battery_r_ohm, args.battery_diode_v)
    hub = Hub(GsState(source=name, battery=model), source, pipeline, mission_s=args.mission_s, pi_ssh=args.pi_ssh,
              links=links, link={"serial": "lora", "ssh": "ssh"}.get(name), link_host=args.ssh)
    server = None
    def interrupt(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupt)
    try:
        server = ThreadingHTTPServer((args.host, args.port), make_handler(hub))
        server.daemon_threads = True
        source.start(hub)
        print("gs_web: %s on http://%s:%d" % (name, args.host, server.server_address[1]), flush=True)
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        hub.source.stop()
        hub.close()
        if pipeline:
            pipeline.close()
        if server:
            server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
