"""sdr_watch.py: the table of everything heard (counts across dump1090's
restarts, one position logged once, active against seen), and a whole run
against a fake dump1090 that dies once (no SDR).

Run by CTest (sdr_watch); also: python3 -m unittest test_sdr_watch
"""

import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sdr_watch as sw  # noqa: E402

RX = (-15.99, -48.04)


def ac(hexid, **kw):
    return dict({'hex': hexid, 'messages': 10, 'seen': 0.5, 'rssi': -30.0}, **kw)


class TableTest(unittest.TestCase):
    def test_messages_add_up_when_dump1090_drops_an_aircraft(self):
        t = sw.Table(RX)
        t.merge({'aircraft': [ac('e48011', messages=40)]}, 1000)
        t.merge({'aircraft': [ac('e48011', messages=5)]}, 1100)     # tracked afresh
        t.merge({'aircraft': [ac('e48011', messages=9)]}, 1101)
        self.assertEqual(t.aircraft['e48011']['messages'], 49)

    def test_a_position_is_logged_once_and_its_distance_kept(self):
        t = sw.Table(RX)
        a = ac('e4a54f', flight='GLO1458 ', lat=-16.956, lon=-48.408, seen_pos=0.5, alt_baro=35000)
        first = t.merge({'aircraft': [a]}, 1000)
        again = t.merge({'aircraft': [dict(a, seen_pos=1.5)]}, 1001)  # the same fix, a second older
        self.assertEqual(len(first), 1)
        self.assertEqual(again, [])
        s = t.aircraft['e4a54f']
        self.assertEqual((s['flight'], s['positions'], s['alt_ft']), ('GLO1458', 1, 35000))
        self.assertAlmostEqual(s['max_km'], 114.5, delta=1.0)

    def test_active_and_seen_and_no_tisb(self):
        t = sw.Table(RX)
        t.merge({'aircraft': [ac('aaaaaa'), ac('~123456')]}, 1000)
        t.merge({'aircraft': [ac('bbbbbb')]}, 1100)
        active, past = t.split(1100, 60)
        self.assertEqual([s['hex'] for s in active], ['bbbbbb'])
        self.assertEqual([s['hex'] for s in past], ['aaaaaa'])
        self.assertNotIn('~123456', t.aircraft)

    def test_the_screen_fits_the_terminal(self):
        t = sw.Table(RX)
        t.merge({'aircraft': [ac(f'e4{i:04x}', lat=-16.0, lon=-48.0 - i / 100) for i in range(30)]}, 1000)
        args = type('A', (), {'antenna': 'cots', 'gain': 49.6, 'active': 60})()
        text = sw.screen(t, 1000, args, 900, None, 'ok', 100, 24)
        self.assertLessEqual(len(text.splitlines()), 23)
        self.assertTrue(all(len(x) <= 100 for x in text.splitlines()))
        self.assertIn('30 no total', text)


FAKE = textwrap.dedent('''\
    #!/usr/bin/env python3
    # A dump1090 for the test: two aircraft; the first start dies after 1 s.
    import json, pathlib, sys, time
    if '--help' in sys.argv:
        print('--device <index|serial>  select device by index or serial number'); sys.exit(0)
    out = pathlib.Path(sys.argv[sys.argv.index('--write-json') + 1])
    flag = out.parent / 'started'
    first = not flag.exists()
    flag.write_text('1')
    ac = [{'hex': 'e48011', 'flight': 'TAM3015 ', 'lat': -16.77, 'lon': -47.94, 'seen': 0.2,
           'seen_pos': 0.2, 'messages': 21, 'rssi': -28.8}]
    if not first:
        ac.append({'hex': 'a37b10', 'messages': 3, 'seen': 0.1, 'rssi': -33.5})
    out.joinpath('aircraft.json').write_text(json.dumps({'now': time.time(), 'aircraft': ac}))
    out.joinpath('stats.json').write_text(json.dumps({'last1min': {'local': {'accepted': [70, 7], 'noise': -38.0}},
                                                      'total': {'local': {'accepted': [70, 7]}}}))
    print('rtlsdr: tuner R820T', flush=True)
    time.sleep(1 if first else 30)
    ''')


class Run(unittest.TestCase):
    def test_a_whole_run_survives_a_dump1090_restart(self):
        with tempfile.TemporaryDirectory() as d:
            fake = Path(d) / 'dump1090'
            fake.write_text(FAKE)
            fake.chmod(0o755)
            out = Path(d) / 'antena'
            p = subprocess.run([sys.executable, str(HERE / 'sdr_watch.py'), '--antenna', 'cots', '--secs', '9',
                                '--plain', '--dump1090', str(fake), '--out', str(out)],
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            (run,) = list(out.iterdir())
            self.assertTrue(run.name.startswith('vigia-cots-'))
            names = {x.name for x in run.iterdir()}
            self.assertTrue({'dump1090.txt.gz', 'posicoes.ndjson', 'vistas.json', 'resumo.txt'} <= names, names)
            seen = {s['hex'] for s in json.loads((run / 'vistas.json').read_text())}
            self.assertEqual(seen, {'e48011', 'a37b10'})
            text = (run / 'resumo.txt').read_text()
            self.assertIn('reiniciado 1 vez', text)
            self.assertIn('aeronaves ouvidas: 2 · com posição: 1', text)
            self.assertIn('CRC ok: 77 no total', text)


if __name__ == '__main__':
    unittest.main()
