"""gama: the configuration, the radio check in each case the bench has seen,
doctor's evaluation of a recorded probe, and a mission's order of steps —
all against fakes (no Pi, no radio, no ssh).

Run by CTest (gama_cli); also: python3 -m unittest test_gama
"""

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import cli      # noqa: E402
import common   # noqa: E402
import doctor   # noqa: E402
import mission  # noqa: E402
import probe_pi  # noqa: E402


def quiet(fn, *a, **kw):
    buf = io.StringIO()
    with redirect_stdout(buf):
        result = fn(*a, **kw)
    return result, buf.getvalue()


class LocalMk(unittest.TestCase):
    def test_the_makefiles_assignments_are_read(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'local.mk'
            p.write_text('# comment\nPI_HOST = pi@gamapi.local\nPI_DIR ?= ttec2  # trailing\nX:=1\nnot a line\n')
            self.assertEqual(common.read_local_mk(p), {'PI_HOST': 'pi@gamapi.local', 'PI_DIR': 'ttec2', 'X': '1'})

    def test_a_missing_file_is_empty(self):
        self.assertEqual(common.read_local_mk('/nonexistent/local.mk'), {})


class Esp32Port(unittest.TestCase):
    def test_the_only_port_is_taken(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'ttyUSB0').touch()
            os.environ['GAMA_SERIAL_GLOB'] = f'{d}/ttyUSB* {d}/ttyACM*'
            try:
                self.assertEqual(common.esp32_port(), (f'{d}/ttyUSB0', None))
                (Path(d) / 'ttyACM0').touch()
                port, problem = common.esp32_port()
                self.assertIsNone(port)
                self.assertIn('mais de uma', problem)
            finally:
                del os.environ['GAMA_SERIAL_GLOB']


class FakeHw:
    """RegVersion as the bench answered: first without, then with the
    MISO pull-up; records every pinctrl call."""

    def __init__(self, plain, pulled=None):
        self.plain, self.pulled, self.calls, self.pullup = plain, pulled, [], False

    def read_reg(self, spi_dev, reg):
        assert reg == 0x42
        return self.pulled if self.pullup else self.plain

    def pinctrl(self, *args):
        self.calls.append(tuple(str(a) for a in args))
        if args[:2] == ('set', 9):
            self.pullup = args[3] == 'pu'
        return True


CONF = {'spi_dev': '/dev/spidev0.0', 'reset_line': '20', 'dio0_line': '21'}


class RadioCheck(unittest.TestCase):
    def setUp(self):
        os.environ['GAMA_FAKE_SPI'] = '1'

    def tearDown(self):
        del os.environ['GAMA_FAKE_SPI']

    def test_a_radio_that_answers(self):
        hw = FakeHw(0x12)
        r = probe_pi.radio_check(CONF, hw)
        self.assertEqual(r['status'], 'ok')
        # reset released before reading, back to an input after
        self.assertEqual(hw.calls[0], ('set', '20', 'op', 'dh'))
        self.assertEqual(hw.calls[-1], ('set', '20', 'ip', 'pd'))

    def test_nothing_on_miso_as_on_20_11(self):
        hw = FakeHw(0x00, 0xff)
        r = probe_pi.radio_check(CONF, hw)
        self.assertEqual(r['status'], 'miso_floating')
        self.assertIn(('set', '9', 'a0', 'pd'), hw.calls)      # the pull-up is undone
        level, text, hint = doctor.radio_finding(r)
        self.assertEqual(level, 'fail')
        self.assertIn('pino 21', hint)

    def test_miso_held_low(self):
        self.assertEqual(probe_pi.radio_check(CONF, FakeHw(0x00, 0x00))['status'], 'miso_low')

    def test_a_wrong_answer(self):
        r = probe_pi.radio_check(CONF, FakeHw(0x55))
        self.assertEqual((r['status'], r['value']), ('unexpected', 0x55))
        self.assertIn('0x55', doctor.radio_finding(r)[1])

    def test_never_read_while_ttcd_holds_the_radio(self):
        hw = FakeHw(0x12)
        self.assertEqual(probe_pi.radio_check(CONF, hw, ttcd_running=True)['status'], 'busy')
        self.assertEqual(hw.calls, [])

    def test_no_configuration(self):
        self.assertEqual(probe_pi.radio_check(None, FakeHw(0x12))['status'], 'no_conf')

    def test_wirings(self):
        self.assertEqual(probe_pi.wiring('20', '21'), 'B1')
        self.assertEqual(probe_pi.wiring('25', '4'), 'B2')
        self.assertIsNone(probe_pi.wiring('25', '21'))


# What the Pi looked like on 20/11 before the cable: October's wiring in
# ttcd.conf, the clock on 16/11, no internet, radio A not answering.
PROBE_20_11 = {
    'ttcd_conf': CONF, 'wiring': 'B1', 'running': {}, 'services': {'ttcd': 'inactive', 'obcd': 'inactive', 'adsbd': 'inactive'},
    'clock': {'epoch': 1794867574.0, 'ntp_synchronized': False}, 'internet': False,
    'conf_files': ['adsbd.conf', 'obcd.conf', 'ttcd.conf'], 'w1_lines': ['dtoverlay=w1-gpio,gpiopin=17'],
    'sdr': True, 'radio': {'status': 'miso_floating', 'value': 0, 'pulled_up': 255},
}
PC_EPOCH_20_11 = 1795187382.0


class DoctorFindings(unittest.TestCase):
    def test_the_bench_of_20_11(self):
        f = doctor.pi_findings(PROBE_20_11, PC_EPOCH_20_11)
        by_text = {t: (lvl, h) for lvl, t, h in f}
        levels = [lvl for lvl, _, _ in f]
        self.assertEqual(levels.count('fail'), 1)                     # only the radio
        clock = next(t for t in by_text if t.startswith('relógio'))
        self.assertEqual(by_text[clock][0], 'warn')
        self.assertIn('-88.8 h', clock)                              # 16/11 19:19 against 20/11 12:09
        self.assertTrue(any('fiação de outubro' in t for t in by_text))
        self.assertTrue(any('sem internet' in t for t in by_text))

    def test_something_running_is_reported_with_the_way_out(self):
        p = dict(PROBE_20_11, running={'ttcd': [12]}, services={'adsbd': 'active'})
        f = doctor.pi_findings(p, PC_EPOCH_20_11)
        line = next(x for x in f if x[1].startswith('rodando na Pi'))
        self.assertIn('adsbd (serviço)', line[1])
        self.assertIn('./gama pi stop', line[2])

    def test_a_board_ready_pi(self):
        p = dict(PROBE_20_11, wiring='B2', ttcd_conf={'reset_line': '25', 'dio0_line': '4'},
                 clock={'epoch': PC_EPOCH_20_11 + 2, 'ntp_synchronized': True}, internet=True,
                 radio={'status': 'ok', 'value': 0x12})
        self.assertEqual([lvl for lvl, _, _ in doctor.pi_findings(p, PC_EPOCH_20_11)], ['ok'] * 7)


class FakeGround:
    def __init__(self, lives=2):
        self.lives, self.signals = lives, []

    def poll(self):
        self.lives -= 1
        return None if self.lives > 0 else 0

    def send_signal(self, s):
        self.signals.append(s)

    def wait(self, timeout=None):
        return 0


class FakeRemote:
    host, dir = 'pi@test', 'ttec'

    def __init__(self, probe, ready=True):
        self.calls, self.p, self.ready, self.alive = [], probe, ready, True

    def reachable(self):
        self.calls.append('reachable')
        return True

    def sync(self):
        self.calls.append('sync')
        return True

    def probe(self):
        self.calls.append('probe')
        return self.p

    def build(self):
        self.calls.append('build')
        return True

    def copy(self, local, remote_path):
        self.calls.append(('copy', remote_path))
        return True

    def start(self, make_args):
        self.calls.append(('start', make_args))
        return 4242

    def status(self, pgid):
        log = 'mission: MISSION_ADSB of 120 s …\n' if self.ready else 'mission: ttcd did not start\n'
        if not self.alive:
            log += 'mission: stopped; logs in docs/vv/bancada/2026-11-21/missao-120000\n'
        return log, self.alive and self.ready

    def stop(self, pgid):
        self.calls.append(('stop', pgid))
        self.alive = False

    def fetch(self):
        self.calls.append('fetch')
        return ['2026-11-21/missao-120000/ttcd.jsonl']


class FakeLocal:
    def __init__(self, port='/dev/ttyUSB0'):
        self.port, self.ground = port, None

    def esp32(self, given):
        return (self.port, None) if self.port else (None, 'a ESP32 não aparece no USB')

    def start_ground(self, port, secs, power, web_port):
        self.ground = FakeGround()
        self.started = (port, secs, power, web_port)
        return self.ground


def mission_args(**kw):
    base = dict(secs=120, input='capture.ndjson', sdr=False, avr=False, pc=False, port=None, power=2,
                tx_power=2, web_port=8080, no_build=False, skip_radio_check=False)
    base.update(kw)
    return SimpleNamespace(**base)


READY_PROBE = dict(PROBE_20_11, radio={'status': 'ok', 'value': 0x12})


class Mission(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.old_state = mission.STATE
        mission.STATE = Path(self.tmp.name) / 'gama-mission.json'

    def tearDown(self):
        mission.STATE = self.old_state
        self.tmp.cleanup()

    def run_pi(self, remote, local, **kw):
        return quiet(mission.run_pi, mission_args(**kw), remote, local, sleep=lambda s: None)

    def test_the_order_of_a_mission(self):
        remote, local = FakeRemote(READY_PROBE), FakeLocal()
        code, out = self.run_pi(remote, local)
        self.assertEqual(code, 0)
        self.assertEqual(remote.calls, [
            'reachable', 'sync', 'probe', 'build', ('copy', '/tmp/gama-mission.ndjson'),
            ('start', 'MISSION_S=120 TX_POWER=2 AVR= INPUT=/tmp/gama-mission.ndjson'),
            ('stop', 4242), 'fetch'])
        # the page gets the same length as the satellite
        self.assertEqual(local.started, ('/dev/ttyUSB0', 120, 2, 8080))
        self.assertIn('http://127.0.0.1:8080', out)
        self.assertIn('missao-120000', out)
        self.assertFalse(mission.STATE.exists())

    def test_the_sdr_and_the_category(self):
        remote = FakeRemote(READY_PROBE)
        self.run_pi(remote, FakeLocal(), input=None, sdr=True, avr=True, secs=60)
        self.assertIn(('start', 'MISSION_S=60 TX_POWER=2 AVR=1'), remote.calls)
        self.assertFalse(any(isinstance(x, tuple) and x[0] == 'copy' for x in remote.calls))

    def test_refused_when_something_holds_the_hardware(self):
        remote = FakeRemote(dict(READY_PROBE, running={'ttcd': [7]}))
        code, out = self.run_pi(remote, FakeLocal())
        self.assertEqual(code, 1)
        self.assertNotIn('build', remote.calls)
        self.assertIn('./gama pi stop', out)

    def test_refused_when_radio_a_does_not_answer(self):
        remote = FakeRemote(PROBE_20_11)
        code, out = self.run_pi(remote, FakeLocal())
        self.assertEqual(code, 1)
        self.assertIn('pino 21', out)
        self.assertEqual(remote.calls, ['reachable', 'sync', 'probe'])

    def test_the_radio_check_can_be_skipped(self):
        remote = FakeRemote(PROBE_20_11)
        code, _ = self.run_pi(remote, FakeLocal(), skip_radio_check=True)
        self.assertEqual(code, 0)

    def test_refused_without_the_esp32(self):
        remote = FakeRemote(READY_PROBE)
        code, out = self.run_pi(remote, FakeLocal(port=None))
        self.assertEqual(code, 1)
        self.assertEqual(remote.calls, ['reachable'])
        self.assertIn('ESP32', out)

    def test_a_satellite_that_does_not_come_up_is_stopped(self):
        remote = FakeRemote(READY_PROBE, ready=False)
        local = FakeLocal()
        code, out = self.run_pi(remote, local)
        self.assertEqual(code, 1)
        self.assertIsNone(local.ground)                       # no page for a dead satellite
        self.assertIn(('stop', 4242), remote.calls)
        self.assertIn('pi │ mission: ttcd did not start', out)


class Cli(unittest.TestCase):
    def test_help_lists_the_commands(self):
        _, out = quiet(lambda: cli.build_parser().print_help())
        for word in ('doctor', 'net', 'pi', 'radio', 'mission'):
            self.assertIn(word, out)

    def test_a_mission_needs_to_know_where_aircraft_come_from(self):
        code, out = quiet(cli.main, ['mission', '--secs', '30'])
        self.assertEqual(code, 2)
        self.assertIn('--sdr', out)

    def test_a_missing_capture_is_refused_before_anything(self):
        code, out = quiet(cli.main, ['mission', '--input', '/nonexistent.ndjson'])
        self.assertEqual(code, 2)
        self.assertIn('não existe', out)


if __name__ == '__main__':
    unittest.main()
