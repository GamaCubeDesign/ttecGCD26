#!/usr/bin/env python3
"""gs_web_load.py — what the ground's page costs the PC during a mission.

Replays a recorded ground log through ground/host/gs_web.py (--replay, with
the ADS-B processing on), with N browsers following /events, and samples the
server from /proc: CPU (all its processes, pair workers included), resident
memory, bytes written to disk, and what the event stream sends.

    gs_web_load.py GROUND.jsonl [--speed 1] [--clients 1] [--pairs-db FILE] [--json OUT]

The figures are for one core: 100% is one core busy. Linux only (/proc).
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TICK = os.sysconf('SC_CLK_TCK')
PAGE = os.sysconf('SC_PAGE_SIZE')


def family(pid):
    """pid and all its descendants, from /proc."""
    children = {}
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            with open(f'/proc/{d}/stat') as f:
                ppid = int(f.read().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            continue
        children.setdefault(ppid, []).append(int(d))
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        out.append(p)
        todo.extend(children.get(p, []))
    return out


def sample(pids):
    """(cpu seconds, rss bytes, written bytes) summed over pids."""
    cpu = rss = wrote = 0
    for p in pids:
        try:
            with open(f'/proc/{p}/stat') as f:
                fields = f.read().rsplit(')', 1)[1].split()
            cpu += (int(fields[11]) + int(fields[12])) / TICK
            with open(f'/proc/{p}/statm') as f:
                rss += int(f.read().split()[1]) * PAGE
            with open(f'/proc/{p}/io') as f:
                for line in f:
                    if line.startswith('wchar:'):     # asked to write, page cache included
                        wrote += int(line.split()[1])
        except (OSError, ValueError, IndexError):
            continue
    return cpu, rss, wrote


class Browser(threading.Thread):
    """Follows /events like the page does, and counts what arrives."""

    def __init__(self, url):
        super().__init__(daemon=True)
        self.url, self.bytes, self.messages, self.stop = url, 0, 0, False

    def run(self):
        try:
            with urllib.request.urlopen(self.url, timeout=10) as r:
                while not self.stop:
                    line = r.readline()
                    if not line:
                        break
                    self.bytes += len(line)
                    if line.startswith(b'data:'):
                        self.messages += 1
        except OSError:
            pass


def mission_window(log, out, margin_ms=30_000):
    """The log cut to its tracks, with a margin: the mission, not the idle
    time around it. Returns the number of lines kept."""
    lines = [line for line in open(log) if line.strip()]
    t = [json.loads(line).get('mono_ms') for line in lines if '"ev": "tracks"' in line or '"ev":"tracks"' in line]
    t = [x for x in t if isinstance(x, (int, float))]
    if not t:
        raise SystemExit('gs_web_load: no tracks in ' + str(log))
    lo, hi = min(t) - margin_ms, max(t) + margin_ms
    kept = [line for line in lines if lo <= (json.loads(line).get('mono_ms') or lo) <= hi]
    Path(out).write_text(''.join(kept))
    return len(kept)


def span_s(log):
    t = [json.loads(line).get('mono_ms') for line in open(log) if line.strip()]
    t = [x for x in t if isinstance(x, (int, float))]
    return (max(t) - min(t)) / 1000.0 if t else 0.0


def pct(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(len(v) * p))] if v else None


def run(log, speed, clients, pairs_db, period=0.5):
    with tempfile.TemporaryDirectory() as tmp:
        argv = [sys.executable, str(ROOT / 'ground/host/gs_web.py'), '--replay', str(log),
                '--speed', str(speed), '--port', '0', '--adsb', '--session-dir', tmp]
        argv += ['--pairs-db', str(pairs_db)] if pairs_db else ['--no-pairs']
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, text=True)
        try:
            line = proc.stdout.readline()
            base = line.split(' on ')[1].strip()
            browsers = [Browser(base + '/events') for _ in range(clients)]
            for b in browsers:
                b.start()
            # The replay caps every pause at 2 s (gs_web.ReplaySource), so it
            # ends no later than the log's span at this speed.
            duration = span_s(log) / speed + 5.0
            t0 = time.monotonic()
            c0, _, w0 = sample(family(proc.pid))
            cpu_pct, rss, last = [], [], (t0, c0)
            while time.monotonic() - t0 < duration and proc.poll() is None:
                time.sleep(period)
                now = time.monotonic()
                c, r, _ = sample(family(proc.pid))
                cpu_pct.append(100.0 * (c - last[1]) / (now - last[0]))
                rss.append(r)
                last = (now, c)
            c1, _, w1 = sample(family(proc.pid))
            elapsed = time.monotonic() - t0
            with urllib.request.urlopen(base + '/api/state', timeout=5) as r:
                adsb = json.load(r).get('adsb') or {}
            for b in browsers:
                b.stop = True
            files = {p.name: p.stat().st_size for p in Path(tmp).rglob('*') if p.is_file()}
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=40)
            except subprocess.TimeoutExpired:
                proc.kill()
            proc.stdout.close()
    return {
        'log': str(log), 'speed': speed, 'clients': clients, 'pairs': bool(pairs_db),
        'seconds': round(elapsed, 1),
        'cpu_mean_pct': round(100.0 * (c1 - c0) / elapsed, 1),
        'cpu_p95_pct': round(pct(cpu_pct, .95), 1),
        'rss_max_mb': round(max(rss) / 1e6, 1),
        'written_mb': round((w1 - w0) / 1e6, 1),
        'session_files_mb': {k: round(v / 1e6, 2) for k, v in sorted(files.items())},
        'events_per_s': round(sum(b.messages for b in browsers) / max(1, clients) / elapsed, 1),
        'events_kb_per_s': round(sum(b.bytes for b in browsers) / max(1, clients) / elapsed / 1e3, 1),
        'adsb': {k: adsb.get(k) for k in ('received', 'processed', 'rejected', 'repeats', 'error')},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('log', type=Path, help="a ground log: a session's ground.jsonl")
    ap.add_argument('--speed', type=float, default=1.0)
    ap.add_argument('--clients', type=int, default=1, help='browsers following /events')
    ap.add_argument('--pairs-db', type=Path)
    ap.add_argument('--whole', action='store_true', help='replay the whole log, not only the mission')
    ap.add_argument('--json', type=Path, help='also write the result here')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        log = args.log
        if not args.whole:
            log = Path(tmp) / 'mission.jsonl'
            mission_window(args.log, log)
        result = run(log, args.speed, args.clients, args.pairs_db)
    result['log'] = str(args.log)
    text = json.dumps(result, indent=1, ensure_ascii=False)
    print(text)
    if args.json:
        args.json.write_text(text + '\n')


if __name__ == '__main__':
    main()
