"""gama ground and gama sim ground: the ground's page (ground/host/gs_web.py)
on this machine, in place of make gs-web (24/11).

    gama ground [--secs 600] [--port P] [--power 2] [--web-port 8080]
        the ESP32 on this machine's USB, found by itself; the Pi of PI_HOST
        for the deployment over ssh (REG-07's fallback)
    gama ground --ssh [--secs 600] [--web-port 8080]
        the satellite over ssh, no LoRa and no ESP32: the ground console on the
        Pi, next to ttcd on the UDP radio (./gama service link ssh first)

With both the ESP32 and PI_HOST, the page gets both ways to the satellite:
the Link tab's button moves ttcd between LoRa and ssh (flight/ttcd/gama-link)
and the page follows; a plain gama ground starts on whichever the satellite
is on.
    gama sim ground [--secs 600] [--web-port 8080]
        gs_cli on the UDP radio, against a satellite on this PC
        (make bench-ttcd RADIO=udp in another terminal)
    gama sim ground --session <ground.jsonl> [--speed 10] [--loop]
        a recorded ground log played on the page, no satellite at all

Before starting it says what would make the page fail later: the ESP32
absent or held by another program (a page left open, a link step), the web
port taken. ADS-B processing is on unless --no-adsb; the session folder
under ground/host/sessions/ carries sim-mission- for a simulation.
"""

import errno
import os
import socket
import subprocess
import sys
from pathlib import Path

import common as c
import service

GS_WEB = c.ROOT / 'ground/host/gs_web.py'
SESSION_PREFIX = {'real': 'mission-', 'replay': 'replay-mission-', 'sim': 'sim-mission-',
                  'ssh': 'ssh-mission-'}
SSH_WARNING = 'enlace pelo SSH, não pelo LoRa: os dados são do satélite, mas não é evidência de RF'


def holders(path, proc=Path('/proc')):
    """[(pid, command line)] of the processes with path open, from /proc
    (Linux); empty where /proc cannot tell."""
    out = []
    try:
        target = os.path.realpath(path)
    except OSError:
        return out
    try:
        entries = list(proc.iterdir())
    except OSError:
        return out
    for d in entries:
        if not d.name.isdigit() or int(d.name) == os.getpid():
            continue
        try:
            fds = list((d / 'fd').iterdir())
        except OSError:
            continue
        for fd in fds:
            try:
                if os.readlink(fd) == target:
                    cmd = (d / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace').strip()
                    out.append((int(d.name), cmd))
                    break
            except OSError:
                continue
    return out


def port_free(port, host='127.0.0.1'):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind((host, port))
        except OSError as exc:
            if exc.errno == errno.EADDRINUSE:
                return False
            raise
    return True


def gs_web_argv(kind, args, serial=None, pi_host=None, prefix=None):
    """The gs_web.py line for a kind ('real' or 'sim'); prefix overrides the
    session folder's (gama replay mission passes replay-mission-)."""
    argv = [sys.executable, str(GS_WEB), '--port', str(args.web_port), '--mission-s', str(args.secs),
            '--session-prefix', prefix or SESSION_PREFIX[kind]]
    if not getattr(args, 'no_adsb', False):
        argv.append('--adsb')
    for flag in ('battery_mah', 'battery_r_ohm', 'battery_diode_v'):
        value = getattr(args, flag, None)
        if value is not None:
            argv += ['--' + flag.replace('_', '-'), str(value)]
    if kind == 'sim':
        session = getattr(args, 'session', None)
        if session:
            argv += ['--replay', session, '--speed', str(args.speed)]
            if args.loop:
                argv.append('--loop')
        else:
            argv.append('--udp')
        return argv
    if getattr(args, 'ssh', False):
        argv[argv.index('--session-prefix') + 1] = prefix or SESSION_PREFIX['ssh']
        argv += ['--ssh', pi_host, '--ssh-dir', c.pi_dir(), '--pi-ssh', pi_host, '--link', 'ssh']
        if serial:                # the ESP32 too: the button can go back to LoRa
            argv += ['--serial', serial, '--power', str(args.power)]
        return argv
    argv += ['--serial', serial, '--power', str(args.power)]
    if pi_host:
        argv += ['--pi-ssh', pi_host, '--ssh', pi_host, '--ssh-dir', c.pi_dir()]
    return argv


def preflight(kind, args, esp32=None, held=None, free=None):
    """The ESP32 port ('' for a simulation), or None after saying what is
    wrong. esp32/held/free are for the tests."""
    esp32, held, free = esp32 or c.esp32_port, held or holders, free or port_free
    if args.secs <= 0:
        c.fail('--secs precisa ser positivo')
        return None
    if not free(args.web_port):
        c.fail(f'a porta {args.web_port} já está em uso: outra página aberta?',
               f'feche a outra (Ctrl-C nela) ou use --web-port {args.web_port + 1}')
        return None
    if kind == 'sim':
        session = getattr(args, 'session', None)
        if session and not Path(session).is_file():
            c.fail(f'{session} não existe')
            return None
        return ''
    port, problem = esp32(args.port)
    if not port:
        c.fail('ESP32 da ground: ' + problem)
        return None
    busy = held(port)
    if busy:
        c.fail(f'a ESP32 ({port}) está aberta por outro programa: ' +
               '; '.join(f'pid {pid} {cmd[:80]}' for pid, cmd in busy),
               'uma página ainda aberta, um ./gama link ou um monitor serial: Ctrl-C nele')
        return None
    c.ok(f'ESP32 em {port}')
    return port


def preflight_ssh(args, pi, free=None):
    """True when the page can reach the satellite over ssh: the web port
    free, the Pi answering, ttcd on the UDP radio, gs_cli built there."""
    free = free or port_free
    if args.secs <= 0:
        c.fail('--secs precisa ser positivo')
        return False
    if not free(args.web_port):
        c.fail(f'a porta {args.web_port} já está em uso: outra página aberta?',
               f'feche a outra (Ctrl-C nela) ou use --web-port {args.web_port + 1}')
        return False
    if not c.reachable_pi(pi):
        return False
    link = service.current_link(pi)
    if link != 'ssh':
        c.fail('o ttcd da Pi está no ' + (service.LINK_NAME[link] if link else 'enlace desconhecido') +
               ', não no rádio UDP', 'antes: ./gama service link ssh')
        return False
    c.ok(f'ttcd no enlace {service.LINK_NAME["ssh"]}')
    if not args.no_build:
        c.say('compilando na Pi (o gs_cli, a ground desta vez)…')
        if not pi.sync() or pi.run('make build', capture=False).returncode != 0:
            c.fail('a compilação na Pi falhou')
            return False
    elif pi.run('test -x build/tools/gs_cli/gs_cli', timeout=30).returncode != 0:
        c.fail('o gs_cli não está compilado na Pi', 'rode sem --no-build')
        return False
    return True


def spare_esp32(args, esp32=None, held=None):
    """The ESP32's port when it is here and free, for the way back to LoRa;
    None otherwise, without failing: ssh needs no ESP32."""
    esp32, held = esp32 or c.esp32_port, held or holders
    port, _ = esp32(args.port)
    return port if port and not held(port) else None


def cmd_ground_ssh(args, call=subprocess.call, pi=None, esp32=None, held=None):
    pi = pi or c.Pi()
    c.warn(SSH_WARNING)
    try:
        if not preflight_ssh(args, pi):
            return 1
        serial = spare_esp32(args, esp32, held)
        if serial:
            c.ok(f'ESP32 em {serial}: a aba Enlace volta ao LoRa pelo botão')
        else:
            c.say('sem a ESP32 livre: a página fica só no SSH')
        c.ok(f'página: http://127.0.0.1:{args.web_port}  (MISSION_ADSB de {args.secs} s; Ctrl-C fecha)')
        try:
            status = call(gs_web_argv('real', args, serial, pi.host), cwd=c.ROOT)
        except KeyboardInterrupt:
            status = 0
        c.say(f'a sessão da ground fica em ground/host/sessions/{SESSION_PREFIX["ssh"]}*')
        return 0 if status in (0, 130, -2) else 1
    finally:
        c.warn(SSH_WARNING + '; fim')


def cmd_ground(args, call=subprocess.call):
    kind = getattr(args, 'kind', 'real')
    if kind == 'real' and getattr(args, 'ssh', False):
        return cmd_ground_ssh(args, call)
    session = getattr(args, 'session', None)
    c.banner(kind, f'a página toca {session}, sem satélite' if session else
             ('a ground no rádio UDP: o satélite no PC é make bench-ttcd RADIO=udp' if kind == 'sim' else None))
    try:
        port = preflight(kind, args)
        if port is None:
            return 1
        if kind == 'sim' and not session and not args.no_build:
            c.say('compilando (o gs_cli do rádio UDP)…')
            if call(['make', '-C', str(c.ROOT), 'build']) != 0:
                c.fail('a compilação falhou')
                return 1
        host = c.pi_host() if kind == 'real' else None
        if kind == 'real' and not host:
            c.warn('PI_HOST não definido: a implantação e o enlace pelo ssh ficam desligados na página')
        c.ok(f'página: http://127.0.0.1:{args.web_port}  (MISSION_ADSB de {args.secs} s; Ctrl-C fecha)')
        try:
            status = call(gs_web_argv(kind, args, port, host), cwd=c.ROOT)
        except KeyboardInterrupt:
            status = 0
        c.say(f'a sessão da ground fica em ground/host/sessions/{SESSION_PREFIX[kind]}*')
        return 0 if status in (0, 130, -2) else 1
    finally:
        c.banner(kind, 'fim')
