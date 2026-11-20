"""sdr_check.py: dump1090's statistics read right, the verdict in each case,
and a whole run against a fake dump1090 (no SDR, no network).

The fixtures are from the first check, 18/11, with the COTS antenna outdoors.
Run by CTest (sdr_check); also: python3 -m unittest test_sdr_check
"""

import io
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sdr_check as sc  # noqa: E402

FIX = HERE / 'fixtures' / 'sdr_check'


def snapshot():
    return json.loads((FIX / 'aircraft.json').read_text())


class Stats(unittest.TestCase):
    def test_the_blocks_of_a_real_run(self):
        blocks = sc.parse_stats((FIX / 'dump1090.txt').read_text())
        self.assertEqual(len(blocks), 2)
        b = blocks[0]
        self.assertEqual((b['accepted'], b['preambles'], b['modeac']), (18, 159025, 356))
        self.assertEqual((b['noise_dbfs'], b['signal_dbfs'], b['peak_dbfs']), (-38.0, -26.3, -24.2))
        self.assertEqual((b['usable'], b['airborne_positions'], b['tracks']), (374, 6, 3))

    def test_totals_sum_counts_and_average_levels(self):
        t = sc.total_stats(sc.parse_stats((FIX / 'dump1090.txt').read_text()))
        self.assertEqual(t['blocks'], 2)
        self.assertEqual(t['accepted'], 77)
        self.assertEqual(t['noise_dbfs'], -37.8)
        self.assertEqual(t['peak_dbfs'], -20.9)
        self.assertEqual(sc.total_stats([]), {})

    def test_messages_between_blocks_are_ignored(self):
        self.assertEqual(sc.parse_stats('*8D4840D6202CC371C32CE0576098;\nDF:17\n'), [])


class Aircraft(unittest.TestCase):
    def test_positions_and_range_from_the_receiver(self):
        seen = sc.aircraft_seen([snapshot()], sc.BENCH)
        by = {a['hex']: a for a in seen}
        self.assertEqual(len(seen), 4)
        self.assertTrue(by['e4a54f']['position'])
        self.assertEqual(by['e4a54f']['flight'], 'GLO1458')
        self.assertFalse(by['a37b10']['position'])
        self.assertIsNone(by['a37b10']['max_km'])
        self.assertEqual(seen[0]['hex'], 'e4a54f')          # most messages first

    def test_the_best_of_several_snapshots(self):
        a = {'hex': 'abc123', 'messages': 3, 'rssi': -30.0}
        b = {'hex': 'abc123', 'messages': 9, 'rssi': -25.0, 'lat': -15.9, 'lon': -48.0}
        (s,) = sc.aircraft_seen([{'aircraft': [a]}, {'aircraft': [b]}], sc.BENCH)
        self.assertEqual((s['messages'], s['rssi_max'], s['position']), (9, -25.0, True))


class Verdict(unittest.TestCase):
    STATS = {'accepted': 77}

    def test_a_position_passes(self):
        code, why = sc.verdict(self.STATS, sc.aircraft_seen([snapshot()], sc.BENCH), None)
        self.assertEqual(code, 'APROVADA')
        self.assertIn('3 aeronave(s) com posição', why)

    def test_messages_without_a_position(self):
        seen = [{'hex': 'a', 'position': False, 'max_km': None}]
        self.assertEqual(sc.verdict(self.STATS, seen, None)[0], 'RECEBE')

    def test_silence_with_an_aircraft_near_fails(self):
        ref = [{'hex': 'E49295', 'flight': '', 'km': 25.0, 'on_ground': False, 'alt_ft': 30000}]
        code, why = sc.verdict({'accepted': 0}, [], ref)
        self.assertEqual(code, 'REPROVADA')
        self.assertIn('25 km', why)

    def test_silence_with_nobody_near_or_no_reference_is_inconclusive(self):
        far = [{'hex': 'X', 'flight': '', 'km': 140.0, 'on_ground': False, 'alt_ft': 38000}]
        parked = [{'hex': 'Y', 'flight': '', 'km': 5.0, 'on_ground': True, 'alt_ft': 0}]
        for ref in (None, [], far, parked):
            self.assertEqual(sc.verdict({'accepted': 0}, [], ref)[0], 'INCONCLUSIVO')

    def test_no_statistics_is_an_error(self):
        self.assertEqual(sc.verdict({}, [], None)[0], 'ERRO')


FAKE = textwrap.dedent('''\
    #!/usr/bin/env python3
    # A dump1090 for the test: one statistics block and an aircraft.json.
    import json, pathlib, sys, time
    if '--help' in sys.argv:
        print('--device <index|serial>  select device by index or serial number'); sys.exit(0)
    args = sys.argv
    assert args[1] == '--device' and args[2] == '0', args
    out = pathlib.Path(args[args.index('--write-json') + 1])
    out.joinpath('aircraft.json').write_text(pathlib.Path(%r).read_text())
    print(pathlib.Path(%r).read_text(), flush=True)
    time.sleep(30)
    ''')


class Run(unittest.TestCase):
    def test_a_whole_run_keeps_everything_in_one_folder(self):
        with tempfile.TemporaryDirectory() as d:
            fake = Path(d) / 'dump1090'
            fake.write_text(FAKE % (str(FIX / 'aircraft.json'), str(FIX / 'dump1090.txt')))
            fake.chmod(0o755)
            out = Path(d) / 'antena'
            p = subprocess.run([sys.executable, str(HERE / 'sdr_check.py'), 'run', '--antenna', 'cots',
                                '--secs', '6', '--no-reference', '--dump1090', str(fake), '--out', str(out)],
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            (run,) = list(out.iterdir())
            self.assertTrue(run.name.startswith('cots-'))
            names = {x.name for x in run.iterdir()}
            self.assertEqual(names, {'aeronaves', 'dump1090.txt.gz', 'resumo.json', 'resumo.txt'})
            self.assertTrue(list((run / 'aeronaves').glob('aircraft-*.json')))
            r = json.loads((run / 'resumo.json').read_text())
            self.assertEqual(r['verdict'][0], 'APROVADA')
            self.assertEqual(r['stats']['accepted'], 77)
            self.assertIsNone(r['reference'])
            self.assertIn('VEREDITO: APROVADA', p.stdout)

            buf = io.StringIO()
            with redirect_stdout(buf):
                sc.compare(type('A', (), {'dirs': [str(out)]})())
            self.assertIn('cots', buf.getvalue())
            self.assertIn('APROVADA', buf.getvalue())

    def test_a_bad_antenna_name_is_refused(self):
        p = subprocess.run([sys.executable, str(HERE / 'sdr_check.py'), 'run', '--antenna', '../x'],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 2)


if __name__ == '__main__':
    unittest.main()
