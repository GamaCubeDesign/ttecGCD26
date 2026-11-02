#!/usr/bin/env python3
"""A real bench step on the UDP radio: tools/bench/run_step.sh with ping.txt.

The bench scripts and ground/host/tracks_to_ndjson.py read the ground's JSON
lines byte for byte, and those lines are produced by gs_cli on the bench and
by the ESP32 ground station on site. This runs the step exactly as
`make bench-ping RADIO=udp` does, on free ports, and checks what every
consumer relies on: every line a JSON object that starts with mono_ms and
wall_ms, and the step's verdict present and passing.

    test_bench_step.py BUILD_DIR SOURCE_DIR
"""

import json
import os
import socket
import subprocess
import sys
import tempfile


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    build, src = sys.argv[1], sys.argv[2]
    sat, gnd = free_udp_port(), free_udp_port()
    failures = []
    with tempfile.TemporaryDirectory(prefix="bench-step-") as tmp:
        sock = os.path.join(tmp, "ttec.sock")
        env = dict(os.environ)
        env["SOCK"] = sock
        env["TTCD_CMD"] = (
            f"{build}/flight/ttcd -c {src}/flight/ttcd/ttcd.conf.example -o radio=udp "
            f"-o ipc_path={sock} -o tx_power_dbm=2 -o lbt=header "
            f"-o udp_port={sat} -o udp_peer_port={gnd}")
        env["GS_CMD"] = f"{build}/tools/gs_cli/gs_cli --radio udp --port {gnd} --peer-port {sat}"
        prefix = os.path.join(tmp, "step")
        run = subprocess.run(
            [f"{src}/tools/bench/run_step.sh", f"{src}/tools/bench/ping.txt", prefix],
            env=env, capture_output=True, text=True, timeout=120)
        if run.returncode != 0:
            failures.append(f"run_step.sh exited {run.returncode}: {run.stderr.strip()}")
        lines = open(prefix + "-ground.jsonl", encoding="utf-8").read().splitlines()

    events = []
    for n, line in enumerate(lines, 1):
        try:
            obj = json.loads(line)
        except ValueError as e:
            failures.append(f"line {n} is not JSON ({e}): {line[:120]}")
            continue
        if not isinstance(obj, dict) or list(obj)[:3] != ["mono_ms", "wall_ms", "ev"]:
            failures.append(f"line {n} does not start mono_ms, wall_ms, ev: {line[:120]}")
            continue
        events.append(obj)

    kinds = [e["ev"] for e in events]
    for want in ("start", "command", "ack", "ping_summary", "summary"):
        if want not in kinds:
            failures.append(f"no '{want}' event")
    pings = [e for e in events if e["ev"] == "ping_summary"]
    if pings and (pings[0]["sent"] != 10 or pings[0]["acked"] != 10):
        failures.append(f"ping_summary: {pings[0]}")
    if kinds and kinds[-1] != "summary":
        failures.append("the last line is not the summary")

    for f in failures:
        print("  FAIL", f)
    print(f"test_bench_step: {len(lines)} lines, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
