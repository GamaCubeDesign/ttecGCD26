"""gama mission: an ADS-B mission operated from the ground's page, in one
command — the satellite (on the Pi, or on this PC with --pc) and the page,
with the same MISSION_ADSB length on both sides.

On the Pi: checks first (ssh, the ESP32, nothing holding the hardware, radio
A answering), syncs and builds, starts tools/bench/pi_mission.sh there (make
mission-sat) detached in its own process group, waits for it to be ready,
then starts gs_web.py here. Ctrl-C stops both: a TERM to the Pi's process
group (pi_mission.sh cleans up and prints its summary), the page, then the
logs come back. A mission left behind by a lost terminal: gama mission stop.

With --pc it is make sim: the same satellite on this machine, UDP radio.
"""

import json
import signal
import subprocess
import sys
import time
from pathlib import Path

import common as c
import doctor

LOG = '/tmp/gama-mission.log'
STATE = c.ROOT / 'build' / 'gama-mission.json'
READY = 'mission: MISSION_ADSB of'
READY_TIMEOUT_S = 90


class Remote:
    """The Pi, over ssh. Tests replace it."""

    def __init__(self, host, directory):
        self.host, self.dir = host, directory

    def reachable(self):
        return c.reachable(self.host)

    def sync(self):
        return c.sync(self.host, self.dir, quiet=True)

    def probe(self):
        return doctor.remote_probe(self.host, self.dir)

    def build(self):
        return c.run(c.ssh_argv(self.host, c.in_dir(self.dir, 'make build')), capture=False).returncode == 0

    def copy(self, local, remote_path):
        return c.run(['scp', '-q', str(local), f'{self.host}:{remote_path}']).returncode == 0

    def start(self, make_args):
        cmd = (f'rm -f {LOG}; setsid nohup make mission-sat {make_args} > {LOG} 2>&1 < /dev/null & echo $!')
        r = c.ssh(self.host, c.in_dir(self.dir, cmd), timeout=30)
        try:
            return int(r.stdout.split()[-1])
        except (ValueError, IndexError):
            return None

    def status(self, pgid):
        """(the log so far, whether the process group is alive)."""
        r = c.ssh(self.host, f'cat {LOG} 2>/dev/null; kill -0 -- -{pgid} 2>/dev/null && echo __ALIVE__',
                  timeout=20)
        if r.returncode not in (0, 1):
            return None, True              # ssh hiccup: assume still running
        text = r.stdout
        alive = text.rstrip().endswith('__ALIVE__')
        return text.replace('__ALIVE__\n', '').replace('__ALIVE__', ''), alive

    def stop(self, pgid):
        c.ssh(self.host, f'kill -TERM -- -{pgid} 2>/dev/null; true', timeout=20)

    def fetch(self):
        return c.fetch(self.host, self.dir)


class Local:
    """This machine: the ESP32 and the page."""

    def esp32(self, given):
        return c.esp32_port(given)

    def start_ground(self, port, secs, power, web_port):
        argv = [sys.executable, str(c.ROOT / 'ground/host/gs_web.py'), '--serial', port,
                '--power', str(power), '--adsb', '--mission-s', str(secs), '--port', str(web_port)]
        return subprocess.Popen(argv, cwd=c.ROOT)


def make_args(args, pi_input):
    parts = [f'MISSION_S={args.secs}', f'TX_POWER={args.tx_power}', f'AVR={1 if args.avr else ""}']
    if pi_input:
        parts.append(f'INPUT={pi_input}')
    return ' '.join(parts)


class LogTail:
    """Prints the Pi's log lines not printed yet."""

    def __init__(self):
        self.seen = 0

    def feed(self, text):
        if text is None:
            return
        lines = text.splitlines()
        for line in lines[self.seen:]:
            print(f'  pi │ {line}', flush=True)
        self.seen = max(self.seen, len(lines))


def preflight(args, remote, local):
    """The ESP32 port, or None after saying what is wrong."""
    if not remote.reachable():
        c.fail(f'a Pi ({remote.host}) não responde pelo ssh', './gama net up')
        return None
    c.ok(f'a Pi responde ({remote.host})')
    port, problem = local.esp32(args.port)
    if not port:
        c.fail('ESP32 da ground: ' + problem)
        return None
    c.ok(f'ESP32 em {port}')
    if not remote.sync():
        return None
    c.ok('código copiado para a Pi')
    p = remote.probe()
    if p is None:
        c.fail('não consegui rodar o probe na Pi')
        return None
    run = p.get('running') or {}
    svc = [k for k, v in (p.get('services') or {}).items() if v == 'active']
    if run or svc:
        c.fail('rodando na Pi: ' + ', '.join(sorted(set(run) | {f'{s} (serviço)' for s in svc})),
               'nada pode segurar o rádio, o SDR ou o I²C: ./gama pi stop')
        return None
    c.ok('nada rodando na Pi')
    level, text, hint = doctor.radio_finding(p.get('radio'))
    if level != 'ok':
        if args.skip_radio_check:
            c.warn(text + ' (ignorado: --skip-radio-check)')
        else:
            c.fail(text, hint)
            return None
    else:
        c.ok(text)
    return port


def run_pi(args, remote, local, sleep=time.sleep, clock=time.monotonic):
    c.say(f'missão ADS-B de {args.secs} s na Pi, ' +
          (f'com {args.input} no lugar do SDR' if args.input else 'com o SDR de verdade'))
    port = preflight(args, remote, local)
    if port is None:
        return 1
    if not args.no_build:
        c.say('compilando na Pi (a primeira vez leva alguns minutos)…')
        if not remote.build():
            c.fail('a compilação na Pi falhou')
            return 1
        c.ok('compilado na Pi')
    pi_input = None
    if args.input:
        pi_input = '/tmp/gama-mission' + Path(args.input).suffix
        if not remote.copy(args.input, pi_input):
            c.fail(f'não consegui copiar {args.input} para a Pi')
            return 1
        c.ok(f'captura copiada para {pi_input}')

    pgid = remote.start(make_args(args, pi_input))
    if pgid is None:
        c.fail('não consegui iniciar o satélite na Pi')
        return 1
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps({'host': remote.host, 'dir': remote.dir, 'pgid': pgid}))
    tail = LogTail()
    ground = None
    try:
        deadline = clock() + READY_TIMEOUT_S
        while True:
            text, alive = remote.status(pgid)
            tail.feed(text)
            if text and READY in text:
                break
            if not alive or clock() > deadline:
                c.fail('o satélite não subiu na Pi' + ('' if alive else ': o processo terminou'),
                       'veja as linhas "pi │" acima; ./gama radio check')
                return finish(remote, pgid, ground, tail, failed=True)
            sleep(1)
        c.ok('satélite no ar na Pi')
        ground = local.start_ground(port, args.secs, args.power, args.web_port)
        print()
        c.ok(f'página: http://127.0.0.1:{args.web_port}  →  aba ADS-B  →  "Iniciar missão ADS-B"')
        c.say(f'a missão dura {args.secs} s depois do clique; repita quantas vezes quiser. Ctrl-C encerra tudo.')
        print()
        while ground.poll() is None:
            text, alive = remote.status(pgid)
            tail.feed(text)
            if not alive:
                c.fail('o satélite parou na Pi', 'veja as linhas "pi │" acima')
                break
            sleep(2)
    except KeyboardInterrupt:
        print()
        c.say('encerrando…')
    return finish(remote, pgid, ground, tail)


def finish(remote, pgid, ground, tail, failed=False):
    """Stop the Pi's group and the page, bring the logs back, show the summary."""
    # A second Ctrl-C here would leave the Pi running: ignore it until done.
    old = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        remote.stop(pgid)
        text = None
        for _ in range(20):
            text, alive = remote.status(pgid)
            if not alive:
                break
            time.sleep(1)
        tail.feed(text)
        if ground is not None and ground.poll() is None:
            ground.send_signal(signal.SIGINT)
            try:
                ground.wait(timeout=10)
            except subprocess.TimeoutExpired:
                ground.kill()
        got = remote.fetch()
        STATE.unlink(missing_ok=True)
        logs = [l.split('logs in ', 1)[1].strip() for l in (text or '').splitlines() if 'logs in ' in l]
        if logs:
            c.ok(f'logs da Pi em {logs[-1]}' + (f' ({len(got)} arquivo(s) trazido(s))' if got else ''))
        sessions = sorted((c.ROOT / 'ground/host/sessions').glob('mission-*'), key=lambda p: p.stat().st_mtime)
        if ground is not None and sessions:
            c.ok(f'sessão da ground em {sessions[-1].relative_to(c.ROOT)}')
    finally:
        signal.signal(signal.SIGINT, old)
    return 1 if failed else 0


def run_pc(args):
    if not args.input:
        c.fail('no PC a missão precisa de uma captura', '--input docs/vv/trafego-real/2026-11-17-unsam/capture_sem_categoria.ndjson')
        return 2
    c.say(f'missão ADS-B de {args.secs} s no PC (rádio UDP, sem hardware): make sim')
    argv = ['make', '-C', str(c.ROOT), 'sim', f'MISSION_S={args.secs}', f'INPUT={args.input}',
            f'WEB_PORT={args.web_port}']
    if not args.avr:
        argv.append('AVR=0')
    try:
        return subprocess.call(argv)
    except KeyboardInterrupt:
        return 130


def cmd_mission(args):
    if args.input and not Path(args.input).is_file():
        c.fail(f'{args.input} não existe')
        return 2
    if args.input and Path(args.input).suffix not in ('.ndjson', '.sbs'):
        c.fail('a captura precisa ser .ndjson ou .sbs')
        return 2
    if args.secs <= 0:
        c.fail('--secs precisa ser positivo')
        return 2
    if args.pc:
        return run_pc(args)
    if c.on_pi():
        c.fail('na Pi, use make mission-sat (a página roda no PC, onde está a ESP32)')
        return 2
    if not args.input and not args.sdr:
        c.fail('diga de onde vêm os aviões',
               '--input <captura.ndjson> (replay, sem SDR) ou --sdr (o SDR de verdade, ao ar livre)')
        return 2
    return run_pi(args, Remote(c.need_pi_host(), c.pi_dir()), Local())


def cmd_mission_stop(args):
    try:
        st = json.loads(STATE.read_text())
    except (OSError, ValueError):
        c.ok('nenhuma missão registrada como em andamento')
        st = None
    host = (st or {}).get('host') or c.need_pi_host()
    remote = Remote(host, (st or {}).get('dir') or c.pi_dir())
    if st:
        remote.stop(st['pgid'])
        c.ok(f'TERM enviado ao grupo {st["pgid"]} na Pi')
        STATE.unlink(missing_ok=True)
    got = remote.fetch()
    if got:
        c.ok(f'{len(got)} arquivo(s) trazido(s) para docs/vv/bancada/')
    return 0
