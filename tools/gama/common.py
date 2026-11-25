"""What every gama command shares: where the Pi is, how to reach it, how to
copy the tree there and the logs back, and how to tell the operator what is
right and what to do about what is not.

Messages to the operator are in Portuguese (the team's working language);
identifiers and comments in English (AGENTS.md).
"""

import glob
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# What a sync leaves alone on the Pi — the Makefile's PI_KEEP: its build
# trees, the evidence its bench runs produced (fetch brings it here), any
# other log and its local.mk. Everything else becomes identical to this tree.
SYNC_EXCLUDES = ['--exclude=/build*/', '--exclude=/docs/vv/bancada/', '--exclude=/local.mk',
                 '--exclude=*.jsonl', '--exclude=*.ndjson', '--exclude=__pycache__/']


# ---- configuration --------------------------------------------------------

def read_local_mk(path=None):
    """KEY = value lines of local.mk (also ?= and :=), as the Makefile reads
    them; anything else is ignored."""
    path = Path(path) if path else ROOT / 'local.mk'
    out = {}
    try:
        text = path.read_text()
    except OSError:
        return out
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        for op in (':=', '?=', '='):
            if op in line:
                key, value = line.split(op, 1)
                key = key.strip()
                if key and ' ' not in key:
                    out[key] = value.strip()
                break
    return out


def setting(name, default=None):
    """The environment first, then local.mk, then the default — the
    Makefile's order for PI_HOST and PI_DIR."""
    return os.environ.get(name) or read_local_mk().get(name) or default


def pi_host():
    return setting('PI_HOST')


def pi_dir():
    return setting('PI_DIR', 'ttec')


def on_pi(model=Path('/proc/device-tree/model')):
    """True on a Raspberry Pi, as ON_PI in the Makefile."""
    try:
        return 'Raspberry Pi' in model.read_text(errors='replace')
    except OSError:
        return False


def need_pi_host():
    host = pi_host()
    if not host:
        fail('PI_HOST não está definido',
             "echo 'PI_HOST = pi@gamapi.local' > local.mk  (na raiz do repositório)")
        sys.exit(2)
    return host


# ---- output ---------------------------------------------------------------

# Plain-text tags, the same width, so a log reads in columns and greps
# (grep '^\[ERRO' bancada.log); no symbols the terminal or the log viewer
# may not render. [SIM] and [REPLAY] mark what is not all real hardware
# (./gama sim ..., ./gama replay ...).
TAGS = {'ok': '[OK]    ', 'warn': '[AVISO] ', 'fail': '[ERRO]  ', 'say': '[INFO]  ',
        'sim': '[SIM]   ', 'replay': '[REPLAY]'}
HINT_INDENT = ' ' * (len(TAGS['ok']) + 1)

# What each kind of run is, said at its start and its end.
KIND = {
    'sim': 'simulação: nada físico, tudo neste computador; não é evidência de bancada',
    'replay': ('hardware real, aviões de um arquivo no lugar do SDR: valida o satélite, '
               'o enlace e a ground, não a recepção ADS-B'),
}


def banner(kind, detail=None):
    """The line that opens (and closes) a sim or replay run; nothing for a
    real one."""
    if kind in KIND:
        _line(TAGS[kind], KIND[kind] + (f'; {detail}' if detail else ''))


def _line(tag, text, hint=None):
    print(f'{tag} {text}', flush=True)
    if hint:
        for h in hint.splitlines():
            print(f'{HINT_INDENT}{h}', flush=True)


def ok(text, hint=None):
    _line(TAGS['ok'], text, hint)


def warn(text, hint=None):
    _line(TAGS['warn'], text, hint)


def fail(text, hint=None):
    _line(TAGS['fail'], text, hint)


def say(text):
    _line(TAGS['say'], text)


def clock():
    """The wall-clock time of a log line, HH:MM:SS."""
    return time.strftime('%H:%M:%S')


def show(level, text, hint=None):
    {'ok': ok, 'warn': warn, 'fail': fail}[level](text, hint)


# ---- running things -------------------------------------------------------

def run(cmd, check=False, capture=True, timeout=None, **kw):
    """subprocess.run with text output; a missing program is a result, not
    an exception (returncode 127)."""
    try:
        return subprocess.run(cmd, text=True, capture_output=capture, check=check,
                              timeout=timeout, **kw)
    except FileNotFoundError as exc:
        return subprocess.CompletedProcess(cmd, 127, '', str(exc))
    except subprocess.TimeoutExpired as exc:
        return subprocess.CompletedProcess(cmd, 124, exc.stdout or '', 'timeout')


def ssh_argv(host, remote, tty=False, timeout_s=6):
    """ssh that never asks for the login password (BatchMode) and gives up
    on an absent Pi quickly; -t only when there is a terminal for it, which
    sudo needs to ask for its own password."""
    argv = ['ssh', '-o', 'BatchMode=yes', '-o', f'ConnectTimeout={timeout_s}']
    if tty:
        argv.append('-t')
    return argv + [host, remote]


def ssh(host, remote, tty=False, capture=True, timeout=None):
    return run(ssh_argv(host, remote, tty=tty), capture=capture, timeout=timeout)


def reachable(host):
    return ssh(host, 'true', timeout=15).returncode == 0


def in_dir(directory, command):
    """A command run inside the tree on the Pi (relative to the user's home)."""
    return f'cd {shlex.quote(directory)} && {command}'


def sync(host, directory, quiet=False):
    """make pi-sync: the tree to the Pi, only what changed, uncommitted work
    included."""
    r = run(['rsync', '-az', '--delete', *SYNC_EXCLUDES, f'{ROOT}/', f'{host}:{directory}/'])
    # A directory the Pi filled with files of its own (a __pycache__) cannot
    # be deleted: harmless, and rsync says so with code 23.
    if r.returncode not in (0, 23):
        fail('não consegui copiar o código para a Pi', (r.stderr or r.stdout).strip()[-400:])
        return False
    if not quiet:
        ok(f'código copiado para {host}:{directory}')
    return True


def fetch(host, directory):
    """make pi-fetch: the Pi's bench logs into docs/vv/bancada/."""
    r = run(['rsync', '-a', '--ignore-missing-args', '--out-format=%n',
             f'{host}:{directory}/docs/vv/bancada/', f'{ROOT}/docs/vv/bancada/'])
    if r.returncode != 0:
        fail('não consegui trazer os logs da Pi', (r.stderr or '').strip()[-400:])
        return None
    return [n for n in r.stdout.splitlines() if n and not n.endswith('/')]


# ---- the ESP32 on this machine's USB --------------------------------------

def serial_ports():
    pattern = os.environ.get('GAMA_SERIAL_GLOB', '/dev/ttyUSB* /dev/ttyACM*')
    return sorted(p for g in pattern.split() for p in glob.glob(g))


USB_SERIAL_BRIDGES = ('10c4:ea60', '1a86:7523', '1a86:55d4')   # CP210x, CH340, CH9102


def esp32_port(given=None):
    """(port, problem): the given port, or the only serial port there is."""
    if given:
        return (given, None) if os.path.exists(given) else (None, f'{given} não existe')
    ports = serial_ports()
    if len(ports) == 1:
        return ports[0], None
    if len(ports) > 1:
        return None, 'mais de uma porta serial: ' + ', '.join(ports) + ' (escolha com --porta)'
    usb = run(['lsusb']).stdout
    if any(b in usb for b in USB_SERIAL_BRIDGES):
        return None, 'o conversor USB-serial aparece no lsusb, mas sem /dev/ttyUSB*: o módulo cp210x/ch341 não carregou?'
    return None, ('a ESP32 não aparece no USB (nem no lsusb): cabo só de carga, '
                  'mau contato ou outra porta USB')


# ---- the Pi, wherever this runs -------------------------------------------

class Pi:
    """Where a command for the Pi runs: right here when this is the Pi, over
    ssh from the PC otherwise. Commands run inside the tree (the repository
    here, PI_DIR there). Tests replace it."""

    def __init__(self, host=None, directory=None, local=None):
        self.local = on_pi() if local is None else local
        self.host = None if self.local else (host or need_pi_host())
        self.dir = str(ROOT) if self.local else (directory or pi_dir())

    @property
    def name(self):
        return 'esta Pi' if self.local else self.host

    def argv(self, command, tty=False):
        command = in_dir(self.dir, command)
        return ['bash', '-c', command] if self.local else ssh_argv(self.host, command, tty=tty)

    def run(self, command, tty=False, capture=True, timeout=None):
        return run(self.argv(command, tty=tty), capture=capture, timeout=timeout)

    def reachable(self):
        return self.local or reachable(self.host)

    def sync(self):
        return self.local or sync(self.host, self.dir, quiet=True)

    def copy_to(self, local_path, remote_path):
        if self.local:
            return run(['cp', str(local_path), remote_path]).returncode == 0
        return run(['scp', '-q', str(local_path), f'{self.host}:{remote_path}']).returncode == 0

    def fetch(self):
        return [] if self.local else fetch(self.host, self.dir)

    def typed(self, command):
        """What the operator types to run the command by hand."""
        return command if self.local else f'ssh -t {self.host} {shlex.quote(in_dir(self.dir, command))}'

    def sudo(self, command, why):
        """A command with sudo in it. sudo asks for its password, which needs
        a terminal: with one, the command runs with -t; without, the exact
        line to type is printed instead (make pi-service-stop's failure of
        20/11)."""
        if sys.stdin.isatty():
            say(f'{why}; o sudo da Pi pode pedir a senha')
            return self.run(command, tty=True, capture=False).returncode == 0
        fail('o sudo precisa de um terminal para pedir a senha', 'rode num terminal seu:\n' + self.typed(command))
        return False

    def probe(self, flags=''):
        """probe_pi.py's JSON (the tree must be synced), or None."""
        r = self.run(f'python3 tools/gama/probe_pi.py {flags}', timeout=60)
        try:
            return json.loads(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None

    def read_files(self, paths):
        """{path: text or None} of files on the Pi (probe_pi.py --cat)."""
        quoted = ' '.join(shlex.quote(p) for p in paths)
        r = self.run(f'python3 tools/gama/probe_pi.py --cat {quoted}', timeout=30)
        try:
            return json.loads(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None


def reachable_pi(pi):
    """The Pi object if it answers, else None after saying so."""
    if not pi.reachable():
        fail(f'a Pi ({pi.name}) não responde pelo ssh', 'está ligada? na mesma rede? ./gama net up')
        return False
    return True


def confirm(question, assume_yes=False):
    if assume_yes:
        return True
    if not sys.stdin.isatty():
        return False
    try:
        return input(f'[?]      {question} [s/N] ').strip().lower() in ('s', 'sim', 'y', 'yes')
    except EOFError:
        return False
