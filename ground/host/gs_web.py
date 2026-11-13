#!/usr/bin/env python3
"""gs_web.py — the ground station's browser interface.

Reads the JSON lines the ground prints (gs_serial.py on the ESP32, gs_cli on
the UDP radio, or a recorded log), keeps the satellite's state, serves one
page that follows it live, and sends the page's buttons back as the same text
commands one types in the console.

    gs_web.py --serial /dev/ttyUSB0 [--power 2]     the ESP32 ground station
    gs_web.py --udp                                 gs_cli on the UDP radio
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

HERE = os.path.dirname(os.path.abspath(__file__))
UI_PATH = os.path.join(HERE, "gs_web_ui.html")

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

    def __init__(self, source="", now=time.time):
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
            ok = obj.get("status") == "OK"
            if ok:
                self.ack_ok += 1
                self.latencies.append(obj.get("latency_ms", 0))
            else:
                self.ack_bad += 1
            self._note("ok" if ok else "bad", "%s: %s (%s ms, try %s)" % (
                obj.get("cmd"), obj.get("status"), obj.get("latency_ms"), obj.get("attempts")))
        elif ev == "tc_failed":
            self.tc_failed += 1
            self._note("bad", "%s failed after %s attempts" % (obj.get("cmd"), obj.get("attempts")))
        elif ev == "command":
            self._note("cmd", "> " + str(obj.get("text")))
        elif ev == "error":
            self.error = obj.get("what")
            self._note("bad", "error: %s" % obj.get("what"))
        elif ev == "rx_crc_error":
            self.rx_bad += 1
            self._note("bad", "frame with a bad CRC")
        elif ev == "loss":
            self._note("bad", "downlink loss: %s" % json.dumps({k: v for k, v in obj.items() if k not in ("mono_ms", "wall_ms", "ev")}))
        elif ev == "tracks":
            self.tracks = obj.get("n", self.tracks)
            self._note("tm", "tracks: %s records" % obj.get("n"))
        elif ev in ("roster", "rate", "per", "ping_summary", "summary", "auto_time"):
            body = {k: v for k, v in obj.items() if k not in ("mono_ms", "wall_ms", "ev", "payload")}
            self._note("info", "%s %s" % (ev, json.dumps(body, separators=(",", ":"))))

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
        self._note("tm", "hk: %s, %s mV, %s mA, %s cdeg" % (
            mode, obj.get("battery_mv"), obj.get("current_ma"), obj.get("temp_bat_ccel")))

    def snapshot(self):
        now = self.now()

        def age(t):
            return None if t is None else round(now - t, 1)

        hk = None
        if self.hk is not None:
            hk = {k: (None if NO_DATA.get(k) == v else v) for k, v in self.hk.items()}
        lat = list(self.latencies)
        return {
            "source": self.source, "ground": self.ground, "error": self.error,
            "contact": self.contact, "hk": hk, "hk_age": age(self.hk_at),
            "frame_age": age(self.frame_at), "mode": self.mode, "mode_age": age(self.mode_at),
            "stat": self.stat, "tracks": self.tracks,
            "rssi": list(self.rssi), "snr": list(self.snr),
            "acks": {"ok": self.ack_ok, "bad": self.ack_bad, "tc_failed": self.tc_failed,
                     "rx_bad": self.rx_bad,
                     "latency_ms": round(sum(lat) / len(lat)) if lat else None},
            "log": list(self.log)[-120:],
        }


class Hub:
    """The state, the clients following it, and the source that feeds it."""

    def __init__(self, state, source, pipeline=None):
        self.pipeline = pipeline
        self.state = state
        self.source = source
        self.lock = threading.Lock()
        self.clients = set()

    def feed(self, obj):
        if self.pipeline:
            self.pipeline.feed(obj)
        with self.lock:
            self.state.feed(obj)
            self._push_locked()

    def note(self, kind, text):
        with self.lock:
            self.state.note(kind, text)
            self._push_locked()

    def snapshot(self):
        with self.lock:
            return self._snapshot()

    def _snapshot(self):
        result = self.state.snapshot()
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

    def _push_locked(self):
        text = json.dumps(self._snapshot())
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


class ProcessSource:
    """A program printing the ground's JSON lines and reading its commands."""

    def __init__(self, argv):
        self.argv = argv
        self.proc = None
        self.send_lock = threading.Lock()
        self.reader = None

    def start(self, hub):
        self.proc = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, args=(hub,), daemon=True)
        self.reader.start()

    def _read(self, hub):
        for raw in self.proc.stdout:
            raw = raw.strip()
            if not raw.startswith("{"):
                continue
            try:
                hub.feed(json.loads(raw))
            except ValueError:
                continue
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
            if self.path in ("/", "/index.html"):
                with open(UI_PATH, "rb") as f:
                    self._send(HTTPStatus.OK, f.read(), "text/html; charset=utf-8")
            elif hub.pipeline and self.path.split("?")[0] in ("/adsb/", "/adsb/data", "/adsb/sessions"):
                try:
                    body, ctype = hub.pipeline.page(self.path)
                    self._send(HTTPStatus.OK, body, ctype)
                except Exception as exc:
                    self._send(HTTPStatus.SERVICE_UNAVAILABLE, str(exc), "text/plain")
            elif self.path == "/api/state":
                self._send(HTTPStatus.OK, json.dumps(hub.snapshot()), "application/json")
            elif self.path == "/events":
                self._events()
            else:
                self._send(HTTPStatus.NOT_FOUND, "not found", "text/plain")

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
            if self.path != "/api/cmd":
                self._send(HTTPStatus.NOT_FOUND, "not found", "text/plain")
                return
            try:
                n = min(int(self.headers.get("Content-Length", "0")), 1024)
                body = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                self._send(HTTPStatus.BAD_REQUEST, json.dumps({"ok": False, "why": "bad request"}),
                           "application/json")
                return
            ok, why = hub.send(body.get("cmd"), bool(body.get("armed")))
            self._send(HTTPStatus.OK if ok else HTTPStatus.FORBIDDEN,
                       json.dumps({"ok": ok, "why": why}), "application/json")

    return Handler


def build_source(args):
    if args.replay:
        return ReplaySource(args.replay, args.speed, args.loop), "replay"
    if args.serial:
        argv = [sys.executable, os.path.join(HERE, "gs_serial.py"), "--port", args.serial,
                "--auto-time", "on"]
        if args.power is not None:
            argv += ["--power", str(args.power)]
        return ProcessSource(argv), "serial"
    if args.udp:
        gs_cli = os.path.join(HERE, "..", "..", "..", "build", "tools", "gs_cli", "gs_cli")
        return ProcessSource([gs_cli, "--radio", "udp"]), "udp"
    return ProcessSource(args.exec), "exec"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--serial", metavar="PORT", help="the ESP32 ground station, e.g. /dev/ttyUSB0")
    src.add_argument("--udp", action="store_true", help="gs_cli on the UDP radio (build/ first)")
    src.add_argument("--replay", metavar="FILE", help="a recorded ground log (.jsonl)")
    src.add_argument("--exec", nargs=argparse.REMAINDER, help="any program printing the lines")
    ap.add_argument("--power", type=int, help="ground power, dBm (with --serial)")
    ap.add_argument("--speed", type=float, default=1.0, help="replay speed")
    ap.add_argument("--loop", action="store_true", help="replay again at the end")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--adsb", action="store_true", help="process received aircraft continuously")
    ap.add_argument("--session-dir", type=Path, default=Path(HERE)/"sessions")
    sys.path.insert(0, os.path.join(HERE, "ground-aeronaves"))
    from ground.config import Config, add_pair_arguments, pair_options
    ap.add_argument("--reference-db", type=Path, default=Config().reference_db)
    add_pair_arguments(ap)
    args = ap.parse_args(argv)
    pipeline = None
    if args.adsb:
        from adsb_pipeline import AdsbPipeline
        from dataclasses import replace
        try:
            pipeline = AdsbPipeline(args.session_dir, replace(Config(), reference_db=args.reference_db, **pair_options(ap,args)))
        except Exception as exc:
            ap.exit(1, f"ADS-B startup failed: {exc}\n")

    source, name = build_source(args)
    hub = Hub(GsState(source=name), source, pipeline)
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
        source.stop()
        if pipeline:
            pipeline.close()
        if server:
            server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
