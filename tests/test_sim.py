"""make sim end to end: tools/bench/sim.sh with the real ttcd, adsbd and obcd
on the UDP radio, real traffic recorded from adsb.lol, and the ground's page
in front. A ping, a mission started from the page as the operator does it,
aircraft processed by the ground, and nothing left running afterwards.

    test_sim.py BUILD_DIR SOURCE_DIR
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

BUILD = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path('build').resolve()
SRC = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path('.').resolve()
sys.argv[1:] = []
sys.path.insert(0, str(SRC / 'tools/live_traffic'))
import live_traffic  # noqa: E402

# Ports of their own, so this test runs beside bench_step_udp and adsb_chain.
PORTS = {'SBS_PORT': '31023', 'SAT_PORT': '47021', 'GND_PORT': '47022', 'AVR_PORT': '31022'}


def traffic(path):
    """30 s of the Buenos Aires fixture, moved to now and repeated every 5 s."""
    point = json.loads((SRC / 'tools/live_traffic/fixtures/adsblol_point_unsam.json').read_text())
    base = live_traffic.records_from_response(point)
    shift = time.time_ns() - point['now'] * 1_000_000
    with open(path, 'w') as f:
        for k in range(6):
            for r in base:
                if 'lat' not in r:
                    continue
                r = dict(r, rx_epoch_ns=r['rx_epoch_ns'] + shift + k * 5_000_000_000)
                f.write(json.dumps(r) + '\n')


class TestSim(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = Path(self.tmp.name)
        traffic(d / 'traffic.ndjson')
        env = dict(os.environ, BUILD_DIR=str(BUILD), PYTHON=sys.executable, WEB_PORT='0', **PORTS)
        self.proc = subprocess.Popen(['tools/bench/sim.sh', str(d / 'traffic.ndjson'), str(d / 'out')],
                                     cwd=SRC, env=env, stdout=subprocess.PIPE, text=True,
                                     start_new_session=True)
        self.addCleanup(self.stop)
        self.base = None
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                break
            if ' on http://' in line:
                self.base = line.split(' on ')[1].strip()
                break
        self.assertIsNotNone(self.base, 'the page never came up')

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=40)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        self.proc.stdout.close()

    def state(self):
        with urllib.request.urlopen(self.base + '/api/state', timeout=5) as r:
            return json.load(r)

    def cmd(self, text):
        req = urllib.request.Request(self.base + '/api/cmd', data=json.dumps({'cmd': text}).encode(),
                                     headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.load(r)

    def wait(self, ready, seconds):
        deadline = time.monotonic() + seconds
        s = None
        while time.monotonic() < deadline:
            s = self.state()
            if ready(s):
                return s
            time.sleep(0.2)
        self.fail('never got there; last state: %s' % json.dumps(s)[:600])

    def test_ping_mission_and_aircraft_then_clean_exit(self):
        self.assertTrue(self.cmd('ping 1')['ok'])
        self.wait(lambda s: (s.get('acks') or {}).get('ok', 0) >= 1, 15)
        self.assertTrue(self.cmd('mode mission_adsb')['ok'])
        s = self.wait(lambda s: s.get('mode') == 'MISSION_ADSB' or
                      ((s.get('adsb') or {}).get('processed', 0) > 0), 30)
        s = self.wait(lambda s: (s.get('adsb') or {}).get('processed', 0) >= 5, 40)
        self.assertIsNone(s['adsb']['error'])
        self.assertEqual(s['adsb']['rejected'], 0)
        # ADR-0023: the category the replay sent on the AVR port, through
        # adsbd, the track's 2 bits and the radio, received by the ground.
        deadline, received = time.monotonic() + 30, []
        while time.monotonic() < deadline and not received:
            with urllib.request.urlopen(self.base + '/api/adsb', timeout=5) as r:
                aircraft = json.load(r).get('aircraft', [])
            received = [a for a in aircraft if a.get('category') and
                        a.get('category_source') in ('received', 'last_known')]
            time.sleep(0.5)
        self.assertTrue(received, 'no category arrived over the link')
        self.assertTrue(set(a['category'] for a in received) <= {'A1', 'A3', 'A7'})
        self.stop()
        self.assertEqual(self.proc.returncode in (0, 130, 143, -15), True, self.proc.returncode)
        left = subprocess.run(['pgrep', '-f', 'udp_port=' + PORTS['SAT_PORT']],
                              capture_output=True, text=True).stdout.strip()
        self.assertEqual(left, '', 'a daemon outlived sim.sh')


if __name__ == '__main__':
    unittest.main()
