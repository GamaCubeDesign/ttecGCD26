"""gama ground and gama sim ground: the ground's page (ground/host/gs_web.py)
on this machine, in place of make gs-web (24/11).

    gama ground [--secs 600] [--port P] [--power 2] [--web-port 8080]
        the ESP32 on this machine's USB, found by itself; the Pi of PI_HOST
        for the deployment over ssh (REG-07's fallback)
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

GS_WEB = c.ROOT / 'ground/host/gs_web.py'
SESSION_PREFIX = {'real': 'mission-', 'replay': 'replay-mission-', 'sim': 'sim-mission-'}


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
    if kind == 'sim':
        session = getattr(args, 'session', None)
        if session:
            argv += ['--replay', session, '--speed', str(args.speed)]
            if args.loop:
                argv.append('--loop')
        else:
            argv.append('--udp')
        return argv
    argv += ['--serial', serial, '--power', str(args.power)]
    if pi_host:
        argv += ['--pi-ssh', pi_host]
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


def cmd_ground(args, call=subprocess.call):
    kind = getattr(args, 'kind', 'real')
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
            c.warn('PI_HOST não definido: a implantação pelo ssh (plano B da REG-07) fica desligada na página')
        c.ok(f'página: http://127.0.0.1:{args.web_port}  (MISSION_ADSB de {args.secs} s; Ctrl-C fecha)')
        try:
            status = call(gs_web_argv(kind, args, port, host), cwd=c.ROOT)
        except KeyboardInterrupt:
            status = 0
        c.say(f'a sessão da ground fica em ground/host/sessions/{SESSION_PREFIX[kind]}*')
        return 0 if status in (0, 130, -2) else 1
    finally:
        c.banner(kind, 'fim')
