"""gama: the configuration, the radio check in each case the bench has seen,
doctor's evaluation of a recorded probe, and a mission's order of steps —
and, since phase 2, a board's installation, the services, the link's
steps and the antenna check — all against fakes (no Pi, no radio, no ssh).

Run by CTest (gama_cli); also: python3 -m unittest test_gama
"""

import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import antenna  # noqa: E402
import board    # noqa: E402
import cli      # noqa: E402
import deploy   # noqa: E402
import ground   # noqa: E402
import common   # noqa: E402
import doctor   # noqa: E402
import link     # noqa: E402
import mission  # noqa: E402
import net      # noqa: E402
import probe_pi  # noqa: E402
import service  # noqa: E402


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
    def __init__(self, port='/dev/ttyUSB0', busy=()):
        self.port, self.ground, self.busy = port, None, list(busy)

    def esp32(self, given):
        return (self.port, None) if self.port else (None, 'a ESP32 não aparece no USB')

    def holders(self, port):
        return self.busy

    def start_ground(self, port, secs, power, web_port, prefix='mission-'):
        self.ground = FakeGround()
        self.started = (port, secs, power, web_port)
        self.prefix = prefix
        return self.ground


def mission_args(**kw):
    base = dict(kind='replay', secs=120, input='capture.ndjson', avr=False, port=None, power=2,
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
        self.assertEqual(local.prefix, 'replay-mission-')     # a capture: the session says replay

    def test_refused_while_another_program_holds_the_esp32(self):
        remote = FakeRemote(READY_PROBE)
        code, out = self.run_pi(remote, FakeLocal(busy=[(321, 'python3 ground/host/gs_web.py --serial /dev/ttyUSB0')]))
        self.assertEqual(code, 1)
        self.assertEqual(remote.calls, ['reachable'])
        self.assertIn('pid 321', out)

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
        for word in ('doctor', 'net', 'pi', 'radio', 'mission', 'board', 'service', 'link', 'antenna', 'esp32',
                     'replay', 'sim', 'ground'):
            self.assertIn(word, out)

    def test_the_prefix_names_the_kind(self):
        ap = cli.build_parser()
        self.assertEqual(ap.parse_args(['mission', '--secs', '30']).kind, 'real')
        r = ap.parse_args(['replay', 'mission', '--input', 'x.ndjson'])
        self.assertEqual((r.kind, r.input, r.action), ('replay', 'x.ndjson', None))
        self.assertEqual(ap.parse_args(['replay', 'mission', 'stop']).action, 'stop')
        sm = ap.parse_args(['sim', 'mission', '--input', 'x.ndjson'])
        self.assertEqual(sm.kind, 'sim')
        sl = ap.parse_args(['sim', 'link', 'ping'])
        self.assertEqual((sl.kind, sl.step), ('sim', 'ping'))
        self.assertEqual(ap.parse_args(['link', 'ping']).kind, 'real')
        with self.assertRaises(SystemExit), redirect_stdout(io.StringIO()), \
                unittest.mock.patch('sys.stderr', io.StringIO()):
            ap.parse_args(['sim', 'mission'])           # a simulation needs its aircraft

    def test_a_replay_needs_its_capture(self):
        code, out = quiet(cli.main, ['replay', 'mission', '--secs', '30'])
        self.assertEqual(code, 2)
        self.assertIn('./gama mission', out)

    def test_the_flags_before_the_prefixes_are_gone(self):
        for argv in (['mission', '--pc', '--input', 'x.ndjson'], ['mission', '--input', 'x.ndjson'],
                     ['mission', '--sdr'], ['link', 'ping', '--udp']):
            with self.assertRaises(SystemExit), redirect_stdout(io.StringIO()), \
                    unittest.mock.patch('sys.stderr', io.StringIO()):
                cli.build_parser().parse_args(argv)

    def test_a_sim_opens_and_closes_with_its_banner(self):
        with tempfile.NamedTemporaryFile(suffix='.ndjson') as f, \
                unittest.mock.patch.object(mission, 'run_pc', lambda a: 0):
            code, out = quiet(cli.main, ['sim', 'mission', '--input', f.name])
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertTrue(lines[0].startswith('[SIM]'))
        self.assertTrue(lines[-1].startswith('[SIM]') and lines[-1].endswith('; fim'))

    def test_a_missing_capture_is_refused_before_anything(self):
        code, out = quiet(cli.main, ['replay', 'mission', '--input', '/nonexistent.ndjson'])
        self.assertEqual(code, 2)
        self.assertIn('não existe', out)


    def test_the_new_groups_parse(self):
        ap = cli.build_parser()
        self.assertEqual(ap.parse_args(['service', 'enable']).daemons, [])
        self.assertEqual(ap.parse_args(['service', 'stop', 'adsbd']).daemons, ['adsbd'])
        self.assertEqual(ap.parse_args(['sim', 'link', 'lost-ack']).step, 'lost-ack')
        self.assertEqual(ap.parse_args(['board', 'install', '-y']).board, 'obc-v2')
        self.assertTrue(ap.parse_args(['antenna', 'check', '--pi']).pi)
        with self.assertRaises(SystemExit), redirect_stdout(io.StringIO()), \
                unittest.mock.patch('sys.stderr', io.StringIO()):
            ap.parse_args(['link', 'pong'])


# ---- phase 2 ---------------------------------------------------------------

class FakePi:
    """common.Pi without a Pi: records what would run there."""

    def __init__(self, files=None, probe=None, sudo_ok=True, answers=True):
        self.local, self.host, self.dir, self.name = False, 'pi@test', 'ttec', 'pi@test'
        self.files, self._probe, self.sudo_ok, self.answers = files or {}, probe, sudo_ok, answers
        self.ran, self.sudos, self.copies = [], [], []

    def reachable(self):
        return self.answers

    def sync(self):
        self.ran.append('<sync>')
        return True

    def read_files(self, paths):
        return {p: self.files.get(p) for p in paths}

    def probe(self, flags=''):
        return self._probe

    def run(self, command, tty=False, capture=True, timeout=None):
        self.ran.append(command)
        return SimpleNamespace(returncode=0, stdout=self.files.get('<stdout>', ''), stderr='')

    def copy_to(self, local_path, remote_path):
        self.copies.append((Path(local_path).read_text(), remote_path))
        return True

    def sudo(self, command, why):
        self.sudos.append(command)
        return self.sudo_ok

    def fetch(self):
        return []


# /etc/gama/ttcd.conf as the bench Pi had it on 20/11: October's pins under
# the new board's comments.
TTCD_20_11 = """spi_dev    = /dev/spidev0.0
reset_line = 20                # GPIO25 -> RA-02 NRESET, bus J1.29
dio0_line  = 21                # GPIO4 <- RA-02 DIO0
log_path   = /var/lib/gama/ttcd.jsonl
"""
BOOT_20_11 = """# Raspberry Pi OS
dtparam=audio=on
[cm4]
otg_mode=1
[all]
dtparam=spi=on
dtoverlay=w1-gpio
"""
OBC_V2 = HERE.parents[1] / 'flight' / 'conf' / 'obc-v2'


class Board(unittest.TestCase):
    def test_the_pins_of_20_11_differ_from_the_boards(self):
        ch = board.conf_changes(TTCD_20_11, (OBC_V2 / 'ttcd.conf').read_text())
        self.assertIn(('reset_line', '20', '25'), ch)
        self.assertIn(('dio0_line', '21', '4'), ch)
        self.assertIn(('log_path', '/var/lib/gama/ttcd.jsonl', None), ch)   # back to the code's default
        self.assertNotIn('spi_dev', [k for k, _, _ in ch])

    def test_hex_case_is_not_a_difference(self):
        self.assertEqual(board.conf_changes('aocs_addr = 0x2a\n', 'aocs_addr = 0x2A  # ICD\n'), [])

    def test_config_txt_gets_the_boards_lines_under_all(self):
        new, changes = board.plan_boot(BOOT_20_11, (OBC_V2 / 'config.txt').read_text(), 'T')
        lines = new.splitlines()
        self.assertIn('# gama board install T: dtoverlay=w1-gpio', lines)     # commented, not deleted
        self.assertNotIn('dtoverlay=w1-gpio', lines)
        tail = lines[len(lines) - 1 - lines[::-1].index('[all]'):]           # appended after [cm4], under [all]
        self.assertGreater(len(lines) - len(tail), lines.index('[cm4]'))
        self.assertEqual([l for l in tail if not l.startswith('#')][1:],
                         ['dtparam=i2c_arm=on', 'dtparam=i2c_arm_baudrate=100000', 'dtoverlay=w1-gpio,gpiopin=17'])
        self.assertEqual(lines.count('dtparam=spi=on'), 1)                     # already there
        self.assertEqual(len(changes), 4)
        self.assertEqual(board.plan_boot(new, (OBC_V2 / 'config.txt').read_text(), 'T2'), (None, []))

    def test_another_baudrate_is_replaced(self):
        new, changes = board.plan_boot('dtparam=i2c_arm_baudrate=400000\n', 'dtparam=i2c_arm_baudrate=100000\n', 'T')
        self.assertTrue(new.startswith('# gama board install T: dtparam=i2c_arm_baudrate=400000'))
        self.assertTrue(new.rstrip().endswith('dtparam=i2c_arm_baudrate=100000'))

    def files_20_11(self):
        return {'/etc/gama/ttcd.conf': TTCD_20_11, '/etc/gama/obcd.conf': (OBC_V2 / 'obcd.conf').read_text(),
                '/etc/gama/adsbd.conf': 'x = 1\n', '/boot/firmware/config.txt': BOOT_20_11}

    def test_the_plan_and_its_command(self):
        plan = board.make_plan(OBC_V2, self.files_20_11(), 'T')
        self.assertEqual({d: v['state'] for d, v in plan['confs'].items()},
                         {'ttcd': 'differs', 'obcd': 'same', 'adsbd': 'kept'})
        cmd = board.install_command('obc-v2', plan, 'T')
        self.assertIn('sudo cp -a /etc/gama/ttcd.conf /etc/gama/ttcd.conf.bak-T && sudo rm /etc/gama/ttcd.conf', cmd)
        self.assertNotIn('obcd.conf.bak', cmd)                   # the same as the board's: left alone
        self.assertLess(cmd.index('ttcd.conf.bak-T'), cmd.index('make install CONF_DIR=flight/conf/obc-v2'))
        self.assertIn('/boot/firmware/config.txt.bak-T && sudo cp /tmp/gama-config.txt', cmd)

    def test_install_backs_up_builds_and_installs(self):
        pi = FakePi(files=self.files_20_11())
        code, out = quiet(board.cmd_install, SimpleNamespace(board='obc-v2', yes=True, no_build=False), pi)
        self.assertEqual(code, 0, out)
        self.assertEqual(pi.ran[:2], ['<sync>', 'make build'])
        self.assertEqual(len(pi.sudos), 1)
        self.assertIn('dtoverlay=w1-gpio,gpiopin=17', pi.copies[0][0])
        self.assertIn('./gama pi reboot', out)

    def test_nothing_changes_without_a_yes(self):
        pi = FakePi(files=self.files_20_11())
        with unittest.mock.patch('sys.stdin', io.StringIO()):          # no terminal to ask on
            code, out = quiet(board.cmd_install, SimpleNamespace(board='obc-v2', yes=False, no_build=False), pi)
        self.assertEqual(code, 1)
        self.assertEqual(pi.sudos, [])
        self.assertIn('--yes', out)

    def test_check_says_whether_anything_is_pending(self):
        pi = FakePi(files=self.files_20_11())
        code, out = quiet(board.cmd_check, SimpleNamespace(board='obc-v2'), pi)
        self.assertEqual(code, 1)
        self.assertIn('reset_line', out)
        self.assertIn('acrescentada: dtoverlay=w1-gpio,gpiopin=17', out)

    def test_an_unknown_board(self):
        code, out = quiet(board.cmd_check, SimpleNamespace(board='obc-v9'), FakePi())
        self.assertEqual(code, 1)
        self.assertIn('obc-v2', out)

    def test_doctor_compares_obcd_with_the_board(self):
        f = doctor.board_findings({'obcd': {'burnwire_line': '27', 'heater_line': '23', 'i2c_dev': '/dev/i2c-1',
                                            'ina219_addr': '0x40', 'aocs_addr': '0x2a', 'w1_path': 'auto'}})
        self.assertEqual(f[0][0], 'warn')
        self.assertIn('burnwire_line 27 ≠ 22', f[0][1])
        self.assertNotIn('aocs_addr', f[0][1])


class Service(unittest.TestCase):
    def test_sudo_and_the_order_of_start(self):
        pi = FakePi()
        code, _ = quiet(service.cmd_action, SimpleNamespace(action='enable', daemons=['adsbd', 'ttcd']), pi)
        self.assertEqual(code, 0)
        self.assertEqual(pi.sudos, ['sudo systemctl enable --now ttcd adsbd'])      # ttcd first
        quiet(service.cmd_action, SimpleNamespace(action='stop', daemons=[]), pi)
        self.assertEqual(pi.sudos[-1], 'sudo systemctl stop ttcd obcd adsbd')

    def test_status_reads_each_daemon(self):
        pi = FakePi(files={'<stdout>': '@@ ttcd active enabled /var/lib/gama/ttcd.jsonl\n{"event":"hk"}\n'
                                       '@@ obcd inactive disabled /var/lib/gama/obcd.jsonl\n'
                                       '@@ adsbd failed enabled /var/lib/gama/adsbd.jsonl\n'})
        code, out = quiet(service.cmd_status, SimpleNamespace(), pi)
        self.assertEqual(code, 0)
        self.assertIn('[OK]     ttcd   active', out)
        self.assertIn('{"event":"hk"}', out)
        self.assertIn('[AVISO]  obcd   inactive  fora do boot', out)

    def test_link_ssh_puts_the_dropin_and_restarts_ttcd_alone(self):
        pi = FakePi(files={'<stdout>': '@link lora\n'})
        code, out = quiet(service.cmd_link, SimpleNamespace(link='ssh'), pi)
        self.assertEqual(code, 0)
        self.assertEqual(len(pi.sudos), 1)
        line = pi.sudos[0]
        self.assertIn('| sudo tee /etc/systemd/system/ttcd.service.d/link-ssh.conf', line)
        self.assertIn('-o radio=udp -o udp_bind=127.0.0.1 -o udp_port=47001', line)
        self.assertTrue(line.endswith('sudo systemctl daemon-reload && sudo systemctl restart ttcd'))
        self.assertNotIn('\n', line)                  # one line, to type by hand without a terminal
        self.assertIn('./gama ground --ssh', out)

    def test_link_lora_removes_the_dropin(self):
        pi = FakePi(files={'<stdout>': '@link ssh\n'})
        code, _ = quiet(service.cmd_link, SimpleNamespace(link='lora'), pi)
        self.assertEqual(code, 0)
        self.assertEqual(pi.sudos, ['sudo rm -f /etc/systemd/system/ttcd.service.d/link-ssh.conf && '
                                    'sudo systemctl daemon-reload && sudo systemctl restart ttcd'])

    def test_link_already_there_or_asked_changes_nothing(self):
        pi = FakePi(files={'<stdout>': '@link ssh\n'})
        self.assertEqual(quiet(service.cmd_link, SimpleNamespace(link='ssh'), pi)[0], 0)
        code, out = quiet(service.cmd_link, SimpleNamespace(link=None), pi)
        self.assertEqual(code, 0)
        self.assertEqual(pi.sudos, [])
        self.assertIn('enlace do ttcd: SSH', out)

    def test_status_says_when_ttcd_is_on_ssh(self):
        pi = FakePi(files={'<stdout>': '@link ssh\n@@ ttcd active enabled /var/lib/gama/ttcd.jsonl\n'})
        code, out = quiet(service.cmd_status, SimpleNamespace(), pi)
        self.assertEqual(code, 0)
        self.assertIn('[AVISO]  enlace do ttcd: SSH', out)
        self.assertIn('./gama service link lora', out)

    def test_without_a_terminal_sudo_says_what_to_type(self):
        pi = common.Pi(host='pi@test', directory='ttec', local=False)
        with unittest.mock.patch('sys.stdin', io.StringIO()):
            ok, out = quiet(pi.sudo, 'sudo systemctl stop ttcd', 'parando')
        self.assertFalse(ok)
        self.assertIn("ssh -t pi@test 'cd ttec && sudo systemctl stop ttcd'", out)


class Link(unittest.TestCase):
    def test_the_make_lines(self):
        root = str(common.ROOT)
        self.assertEqual(link.make_argv('ping', 'udp'), ['make', '-C', root, 'bench-ping', 'POWER=2', 'RADIO=udp'])
        self.assertEqual(link.make_argv('all', 'split', '/dev/ttyUSB0', 5),
                         ['make', '-C', root, 'bench', 'POWER=5', 'TTCD_CONF=/etc/gama/ttcd.conf',
                          'SPLIT=1', 'GS=esp32', 'ESP32_PORT=/dev/ttyUSB0'])
        self.assertEqual(link.make_argv('toa', 'pi', conf='/tmp/x.conf')[-1], 'TTCD_CONF=/tmp/x.conf')

    def args(self, **kw):
        base = dict(step='ping', port=None, power=2, conf=None, no_build=False, skip_radio_check=False)
        base.update(kw)
        return SimpleNamespace(**base)

    def test_from_the_pc_it_checks_builds_then_steps(self):
        remote, local, calls = FakeRemote(READY_PROBE), FakeLocal(), []
        with unittest.mock.patch.object(common, 'on_pi', lambda: False):
            code, out = quiet(link.cmd_link, self.args(), remote, local, lambda argv: calls.append(argv) or 0)
        self.assertEqual(code, 0, out)
        self.assertEqual([x for x in remote.calls if x in ('sync', 'probe', 'build')], ['sync', 'probe', 'build'])
        self.assertIn('ESP32_PORT=/dev/ttyUSB0', calls[0])

    def test_sim_link_is_udp_with_its_banner(self):
        calls = []
        code, out = quiet(link.cmd_link, self.args(kind='sim', no_build=True), None, None,
                          lambda argv: calls.append(argv) or 0)
        self.assertEqual(code, 0)
        self.assertEqual(calls[0][-1], 'RADIO=udp')
        self.assertTrue(out.startswith('[SIM]'))
        self.assertIn('build/bench-udp/', out)

    def test_refused_when_radio_a_does_not_answer(self):
        remote, calls = FakeRemote(PROBE_20_11), []
        with unittest.mock.patch.object(common, 'on_pi', lambda: False):
            code, out = quiet(link.cmd_link, self.args(), remote, FakeLocal(), lambda argv: calls.append(argv) or 0)
        self.assertEqual(code, 1)
        self.assertEqual(calls, [])
        self.assertIn('pino 21', out)


class Antenna(unittest.TestCase):
    def args(self, **kw):
        base = dict(pi=True, antenna='cots', secs=60, gain=49.6, lat=None, lon=None, no_reference=False)
        base.update(kw)
        return SimpleNamespace(**base)

    def test_the_folder_has_the_pcs_date(self):
        import datetime
        a = antenna.run_args(self.args(), datetime.date(2026, 11, 20))
        self.assertEqual(a[a.index('--out') + 1], 'docs/vv/bancada/2026-11-20/antena')
        self.assertNotIn('--lat', a)

    def test_refused_while_adsbd_holds_the_sdr(self):
        pi = FakePi(probe={'running': {'dump1090-fa': [9]}, 'services': {'adsbd': 'active'}, 'sdr': True})
        with unittest.mock.patch.object(common, 'on_pi', lambda: False):
            code, out = quiet(antenna.cmd_check, self.args(), pi)
        self.assertEqual(code, 1)
        self.assertIn('adsbd (serviço)', out)
        self.assertIn('./gama pi stop', out)
        self.assertNotIn('sdr_check', ' '.join(pi.ran))

    def test_on_the_pi(self):
        pi = FakePi(probe={'running': {}, 'services': {}, 'sdr': True, 'internet': True})
        with unittest.mock.patch.object(common, 'on_pi', lambda: False):
            code, _ = quiet(antenna.cmd_check, self.args(), pi)
        self.assertEqual(code, 0)
        self.assertTrue(pi.ran[-1].startswith('python3 tools/bench/sdr_check.py run --antenna cots --secs 60'))


DEPLOY_OUT = """\
{"mono_ms":100,"event":"local_command","signal":10,"uid":0,"pid":7}
{"mono_ms":100,"event":"mode","from":"PRE_TEST","to":"BASIC_INTERMEDIATE","event":"TC_BASIC_INTER","by":"signal","after_ms":5}
{"mono_ms":100,"event":"burnwire","on":true,"why":"deploy","driven":true}
{"mono_ms":5100,"event":"burnwire","on":false,"why":"pulse done","driven":true}
{"mono_ms":5100,"event":"mode","from":"BASIC_INTERMEDIATE","to":"PRE_TEST","event":"TASK_DONE","by":"burnwire","after_ms":5000}
RESULT done pid=7 log=/var/lib/gama/obcd.jsonl
"""


class NetUp(unittest.TestCase):
    """net up's wait for the Pi: one line per ssh attempt, with the attempt,
    the time since the network came up and ssh's reason; a summary at the
    end. Virtual time: each ssh takes 1 s, each pause 3 s."""

    def run_wait(self, results, wait_s=90):
        t = [0.0]
        replies = iter(results)

        def ssh():
            t[0] += 1
            code, out, err = next(replies)
            return SimpleNamespace(returncode=code, stdout=out, stderr=err)

        def sleep(s):
            t[0] += s
        return quiet(net.wait_for_pi, 'pi@gamapi.local', wait_s=wait_s, ssh=ssh,
                     now=lambda: t[0], sleep=sleep)

    def test_answers_on_the_third_attempt(self):
        refused = (255, '', 'ssh: connect to host gamapi.local port 22: Connection refused\n')
        unknown = (255, '', 'ssh: Could not resolve hostname gamapi.local: Name or service not known\n')
        code, out = self.run_wait([unknown, refused, (0, '10.42.0.17 \n', '')])
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertIn('tentativa 1, +1 s: falhou em 1.0 s: ssh: Could not resolve hostname', lines[1])
        self.assertIn('tentativa 2, +5 s: falhou em 1.0 s: ssh: connect to host gamapi.local port 22: '
                      'Connection refused', lines[2])
        self.assertTrue(lines[3].startswith('[OK]     '))
        self.assertIn('tentativa 3, +9 s: a Pi responde: 10.42.0.17', lines[3])

    def test_gives_up_with_a_summary(self):
        code, out = self.run_wait([(124, '', 'timeout')] * 10, wait_s=20)
        self.assertEqual(code, 1)
        self.assertIn('[ERRO]   ', out)
        self.assertIn('a Pi não respondeu: 5 tentativas em 20 s', out)
        self.assertIn('5x sem resposta em 8 s', out)


class Output(unittest.TestCase):
    def test_plain_text_tags_only(self):
        _, out = quiet(lambda: (common.ok('a'), common.warn('b', 'faça x'), common.fail('c'), common.say('d')))
        self.assertEqual(out, '[OK]     a\n[AVISO]  b\n         faça x\n[ERRO]   c\n[INFO]   d\n')
        self.assertTrue(all(ord(ch) < 0x2000 for ch in out))

    def test_the_banner_marks_sim_and_replay_only(self):
        _, out = quiet(lambda: (common.banner('sim'), common.banner('replay', 'aviões de x.ndjson'),
                                common.banner('real')))
        lines = out.splitlines()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith('[SIM]    simulação'))
        self.assertTrue(lines[1].startswith('[REPLAY] hardware real'))
        self.assertTrue(lines[1].endswith('; aviões de x.ndjson'))


class Ground(unittest.TestCase):
    def args(self, **kw):
        base = dict(kind='real', secs=600, web_port=8080, no_adsb=False, port=None, power=2,
                    session=None, speed=1.0, loop=False, no_build=True)
        base.update(kw)
        return SimpleNamespace(**base)

    def test_the_gs_web_lines(self):
        real = ground.gs_web_argv('real', self.args(), '/dev/ttyUSB0', 'pi@gamapi.local')
        self.assertEqual(real[2:], ['--port', '8080', '--mission-s', '600', '--session-prefix', 'mission-',
                                    '--adsb', '--serial', '/dev/ttyUSB0', '--power', '2',
                                    '--pi-ssh', 'pi@gamapi.local'])
        udp = ground.gs_web_argv('sim', self.args(kind='sim', no_adsb=True))
        self.assertEqual(udp[2:], ['--port', '8080', '--mission-s', '600', '--session-prefix', 'sim-mission-', '--udp'])
        rec = ground.gs_web_argv('sim', self.args(kind='sim', session='g.jsonl', speed=10.0, loop=True))
        self.assertEqual(rec[-5:], ['--replay', 'g.jsonl', '--speed', '10.0', '--loop'])

    def test_the_battery_calibration_passes_through(self):
        line = ground.gs_web_argv('real', self.args(battery_r_ohm=0.31, battery_diode_v=0.27),
                                  '/dev/ttyUSB0')
        self.assertIn('--battery-r-ohm', line)
        self.assertEqual(line[line.index('--battery-r-ohm') + 1], '0.31')
        self.assertEqual(line[line.index('--battery-diode-v') + 1], '0.27')
        self.assertNotIn('--battery-mah', line)

    def test_refused_while_the_esp32_is_held(self):
        r, out = quiet(ground.preflight, 'real', self.args(), esp32=lambda p: ('/dev/ttyUSB0', None),
                       held=lambda p: [(77, 'screen /dev/ttyUSB0 115200')], free=lambda p: True)
        self.assertIsNone(r)
        self.assertIn('pid 77 screen', out)

    def test_refused_when_the_web_port_is_taken(self):
        r, out = quiet(ground.preflight, 'real', self.args(), esp32=lambda p: ('/dev/ttyUSB0', None),
                       held=lambda p: [], free=lambda p: False)
        self.assertIsNone(r)
        self.assertIn('--web-port 8081', out)

    def test_a_session_must_exist(self):
        r, out = quiet(ground.preflight, 'sim', self.args(kind='sim', session='/nonexistent.jsonl'),
                       free=lambda p: True)
        self.assertIsNone(r)
        self.assertIn('não existe', out)

    def test_holders_reads_proc(self):
        with tempfile.TemporaryDirectory() as d:
            proc, dev = Path(d) / 'proc', Path(d) / 'ttyUSB0'
            dev.write_text('')
            (proc / '4242' / 'fd').mkdir(parents=True)
            (proc / '4242' / 'fd' / '3').symlink_to(dev)
            (proc / '4242' / 'cmdline').write_bytes(b'screen\0/dev/ttyUSB0\0')
            (proc / 'self').mkdir()
            self.assertEqual(ground.holders(str(dev), proc), [(4242, 'screen /dev/ttyUSB0')])

    def test_sim_ground_opens_and_closes_with_its_banner(self):
        calls = []
        with unittest.mock.patch.object(ground, 'port_free', lambda p, host='127.0.0.1': True):
            code, out = quiet(ground.cmd_ground, self.args(kind='sim'), lambda argv, **kw: calls.append(argv) or 0)
        self.assertEqual(code, 0)
        self.assertIn('--udp', calls[0])
        lines = out.rstrip().splitlines()
        self.assertTrue(lines[0].startswith('[SIM]') and 'rádio UDP' in lines[0])
        self.assertTrue(lines[-1].startswith('[SIM]') and lines[-1].endswith('; fim'))

    def test_the_ssh_gs_web_line(self):
        line = ground.gs_web_argv('real', self.args(ssh=True), pi_host='pi@test')
        self.assertEqual(line[2:], ['--port', '8080', '--mission-s', '600', '--session-prefix', 'ssh-mission-',
                                    '--adsb', '--ssh', 'pi@test', '--ssh-dir', common.pi_dir(),
                                    '--pi-ssh', 'pi@test'])

    def test_ssh_refused_while_ttcd_is_on_the_radio(self):
        pi = FakePi(files={'<stdout>': '@link lora\n'})
        ok, out = quiet(ground.preflight_ssh, self.args(ssh=True), pi, free=lambda p: True)
        self.assertFalse(ok)
        self.assertIn('./gama service link ssh', out)
        self.assertEqual(pi.ran, [service.link_probe_command()])    # nothing built for nothing

    def test_ssh_builds_on_the_pi_then_opens_the_page(self):
        pi, calls = FakePi(files={'<stdout>': '@link ssh\n'}), []
        with unittest.mock.patch.object(ground, 'port_free', lambda p, host='127.0.0.1': True):
            code, out = quiet(ground.cmd_ground_ssh, self.args(ssh=True, no_build=False),
                              lambda argv, **kw: calls.append(argv) or 0, pi)
        self.assertEqual(code, 0)
        self.assertEqual(pi.ran[1:], ['<sync>', 'make build'])
        self.assertIn('--ssh', calls[0])
        self.assertNotIn('--serial', calls[0])
        lines = out.rstrip().splitlines()
        self.assertTrue(lines[0].startswith('[AVISO]') and 'não pelo LoRa' in lines[0])
        self.assertTrue(lines[-1].startswith('[AVISO]') and lines[-1].endswith('; fim'))

    def test_the_parser(self):
        ap = cli.build_parser()
        self.assertTrue(ap.parse_args(['ground', '--ssh']).ssh)
        sl = ap.parse_args(['service', 'link', 'ssh'])
        self.assertEqual((sl.link, sl.func), ('ssh', service.cmd_link))
        self.assertIsNone(ap.parse_args(['service', 'link']).link)
        g = ap.parse_args(['ground', '--secs', '120'])
        self.assertEqual((g.kind, g.secs, g.func), ('real', 120, ground.cmd_ground))
        sg = ap.parse_args(['sim', 'ground', '--session', 'g.jsonl', '--speed', '10'])
        self.assertEqual((sg.kind, sg.session, sg.speed), ('sim', 'g.jsonl', 10.0))


class Deploy(unittest.TestCase):
    def test_a_done_pulse_is_success_with_its_length(self):
        pi = FakePi(files={'<stdout>': DEPLOY_OUT})
        code, out = quiet(deploy.cmd_deploy, SimpleNamespace(yes=True), pi)
        self.assertEqual(code, 0)
        self.assertEqual(pi.ran, ['sudo -n /usr/local/sbin/gama-deploy'])
        self.assertIn('após 5000 ms', out)

    def test_an_undriven_gpio_fails(self):
        pi = FakePi(files={'<stdout>': DEPLOY_OUT.replace('"driven":true', '"driven":false')})
        code, out = quiet(deploy.cmd_deploy, SimpleNamespace(yes=True), pi)
        self.assertEqual(code, 1)
        self.assertIn('nada foi acionado', out)

    def test_a_refusal_names_the_mode(self):
        text = ('{"mono_ms":1,"event":"local_command","signal":10}\n'
                '{"mono_ms":1,"event":"event_refused","event":"TC_BASIC_INTER","mode":"MISSION_ADSB","by":"signal"}\n'
                'RESULT refused pid=7 log=x\n')
        result, lines = deploy.summarize(text)
        self.assertEqual(result, 'refused')
        self.assertIn(('fail', 'recusado: o OBC está em MISSION_ADSB; a implantação só sai de PRE_TEST'), lines)

    def test_nothing_runs_without_confirmation(self):
        pi = FakePi()
        with unittest.mock.patch('sys.stdin.isatty', return_value=False):
            code, _ = quiet(deploy.cmd_deploy, SimpleNamespace(yes=False), pi)
        self.assertEqual(code, 1)
        self.assertEqual(pi.ran, [])


if __name__ == '__main__':
    unittest.main()
