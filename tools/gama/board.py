"""gama board: a board's flight configuration (flight/conf/<board>/) on the
Pi — what differs from /etc/gama and /boot/firmware/config.txt, shown
before anything changes, then installed with a backup of every file it
replaces.

    gama board check [--board obc-v2]     the differences, nothing changed
    gama board install [--board obc-v2]   the daemons (make install) and the
                                          board's files, after a yes

make install keeps an existing /etc/gama/<d>.conf — on 20/11 that left
October's pins (RESET 20, DIO0 21) under comments of the new board's. Here
a conf that differs from the board's is copied to <d>.conf.bak-<time> and
replaced. config.txt gets the board's lines appended under [all] (a line
after [pi4] or [cm4] would apply only to those), and any other active line
for the same parameter — the w1-gpio without gpiopin=17, which would take
the radio's DIO0 — is commented out, not deleted.

The evaluation is pure (texts in, a plan out), so the tests feed it the
files the bench had.
"""

import sys
import time
from pathlib import Path

import common as c

CONF_ROOT = c.ROOT / 'flight' / 'conf'
CONF_DIR = '/etc/gama'
BOOT_CONFIG = '/boot/firmware/config.txt'
DAEMONS = ('ttcd', 'obcd', 'adsbd')
STAGED_BOOT = '/tmp/gama-config.txt'


# ---- pure ------------------------------------------------------------------

def settings(text):
    """key = value lines, comments dropped, as the daemons read them."""
    out = {}
    for line in (text or '').splitlines():
        line = line.split('#', 1)[0].strip()
        if '=' in line:
            k, v = line.split('=', 1)
            out[k.strip()] = v.strip()
    return out


def same(a, b):
    """Values equal as the daemons read them: 0x2A is 0x2a."""
    return (a or '').lower() == (b or '').lower() and (a is None) == (b is None)


def conf_changes(installed, board):
    """[(key, installed value, board value)] where they differ; None on
    one side means the key is absent there (the code's default)."""
    a, b = settings(installed), settings(board)
    return [(k, a.get(k), b.get(k)) for k in sorted(set(a) | set(b)) if not same(a.get(k), b.get(k))]


def boot_key(line):
    """What a config.txt line sets: 'dtparam=spi', 'dtoverlay=w1-gpio', ...
    Two active lines with the same key compete."""
    line = line.strip()
    if line.startswith('dtoverlay='):
        return line.split(',', 1)[0]
    if line.startswith('dtparam='):
        return line.split('=', 2)[0] + '=' + line.split('=', 2)[1]
    return line.split('=', 1)[0]


def active_lines(text):
    return [l.strip() for l in (text or '').splitlines() if l.strip() and not l.strip().startswith('#')]


def plan_boot(current, wanted, stamp):
    """(new text or None, [change descriptions]) to make config.txt carry
    the board's lines."""
    want = [l for l in active_lines(wanted) if not l.startswith('[')]
    keys = {boot_key(l): l for l in want}
    present = set(active_lines(current))
    changes, out = [], []
    for line in (current or '').splitlines():
        s = line.strip()
        k = boot_key(s) if s and not s.startswith(('#', '[')) else None
        if k in keys and s != keys[k]:
            out.append(f'# gama board install {stamp}: {s}')
            changes.append(f'comentada: {s}  (a placa usa {keys[k]})')
        else:
            out.append(line)
    missing = [l for l in want if l not in present]
    if missing:
        out += ['', f'# gama board install {stamp}: the board\'s lines (flight/conf)', '[all]'] + missing
        changes += [f'acrescentada: {l}' for l in missing]
    if not changes:
        return None, []
    return '\n'.join(out).rstrip('\n') + '\n', changes


def make_plan(board_dir, files, stamp):
    """files: {path: text or None} read on the Pi. The plan: per daemon
    'same', 'differs' (with its changes), 'missing' or 'kept'; and config.txt."""
    board_dir = Path(board_dir)
    plan = {'confs': {}, 'boot': (None, []), 'boot_unreadable': False}
    for d in DAEMONS:
        installed = files.get(f'{CONF_DIR}/{d}.conf')
        src = board_dir / f'{d}.conf'
        if not src.exists():
            # Not the board's to set (adsbd: the SDR is on the USB).
            plan['confs'][d] = {'state': 'missing' if installed is None else 'kept', 'src': None}
            continue
        if installed is None:
            plan['confs'][d] = {'state': 'missing', 'src': src}
            continue
        ch = conf_changes(installed, src.read_text())
        plan['confs'][d] = {'state': 'differs' if ch else 'same', 'src': src, 'changes': ch}
    boot_src = board_dir / 'config.txt'
    if boot_src.exists():
        current = files.get(BOOT_CONFIG)
        if current is None:
            plan['boot_unreadable'] = True
        else:
            plan['boot'] = plan_boot(current, boot_src.read_text(), stamp)
    return plan


def pending(plan):
    return (any(v['state'] in ('differs', 'missing') for v in plan['confs'].values())
            or plan['boot'][0] is not None)


def install_command(board, plan, stamp):
    """One shell line for the Pi, in the tree: back up and replace what
    differs, make install, config.txt from the staged copy."""
    steps = []
    for d, v in plan['confs'].items():
        if v['state'] == 'differs':
            f = f'{CONF_DIR}/{d}.conf'
            steps.append(f'sudo cp -a {f} {f}.bak-{stamp} && sudo rm {f}')
    steps.append(f'make install CONF_DIR=flight/conf/{board}')
    if plan['boot'][0] is not None:
        steps.append(f'sudo cp -a {BOOT_CONFIG} {BOOT_CONFIG}.bak-{stamp} && sudo cp {STAGED_BOOT} {BOOT_CONFIG}')
    return 'set -e; ' + ' && '.join(steps)


def show_plan(board, plan):
    for d, v in plan['confs'].items():
        f = f'{CONF_DIR}/{d}.conf'
        if v['state'] == 'same':
            c.ok(f'{f} igual ao da placa {board}')
        elif v['state'] == 'kept':
            c.ok(f'{f} mantido (não depende da placa)')
        elif v['state'] == 'missing':
            src = v['src'].relative_to(c.ROOT) if v['src'] else f'flight/{d}/{d}.conf.example'
            c.warn(f'{f} não existe: virá de {src}')
        else:
            c.warn(f'{f} difere do da placa {board}:')
            for k, old, new in v['changes']:
                print(f'      {k:<14} {old if old is not None else "(padrão do código)":<18} → '
                      f'{new if new is not None else "(padrão do código)"}')
    if plan['boot_unreadable']:
        c.fail(f'não consegui ler {BOOT_CONFIG} na Pi')
    elif plan['boot'][0] is None:
        c.ok(f'{BOOT_CONFIG} já tem as linhas da placa')
    else:
        c.warn(f'{BOOT_CONFIG}: (vale depois de reiniciar a Pi)')
        for ch in plan['boot'][1]:
            print(f'      {ch}')


# ---- commands --------------------------------------------------------------

def board_dir(board):
    d = CONF_ROOT / board
    if not d.is_dir() or board in ('', '.', '..'):
        c.fail(f'não há configuração da placa {board} em flight/conf/',
               'placas: ' + ', '.join(sorted(p.name for p in CONF_ROOT.iterdir() if p.is_dir())))
        return None
    return d


def read_and_plan(pi, board, stamp):
    bdir = board_dir(board)
    if bdir is None or not c.reachable_pi(pi) or not pi.sync():
        return None
    files = pi.read_files([f'{CONF_DIR}/{d}.conf' for d in DAEMONS] + [BOOT_CONFIG])
    if files is None:
        c.fail('não consegui ler a configuração na Pi', 'sem python3 na Pi?')
        return None
    return make_plan(bdir, files, stamp)


def cmd_check(args, pi=None):
    pi = pi or c.Pi()
    plan = read_and_plan(pi, args.board, time.strftime('%Y%m%d-%H%M%S'))
    if plan is None:
        return 1
    c.say(f'{pi.name} contra flight/conf/{args.board}/')
    show_plan(args.board, plan)
    if pending(plan):
        print()
        c.say(f'para instalar: ./gama board install --board {args.board}')
        return 1
    return 0


def cmd_install(args, pi=None):
    pi = pi or c.Pi()
    stamp = time.strftime('%Y%m%d-%H%M%S')
    plan = read_and_plan(pi, args.board, stamp)
    if plan is None:
        return 1
    c.say(f'{pi.name} ← flight/conf/{args.board}/')
    show_plan(args.board, plan)
    if plan['boot_unreadable']:
        return 1
    print()
    c.say('o make install também instala os daemons compilados e os serviços (sem ligá-los); '
          'cada arquivo trocado fica num .bak-' + stamp)
    if not c.confirm('aplicar?', args.yes):
        c.fail('nada mudou na Pi' + ('' if args.yes or sys.stdin.isatty() else ' (sem terminal: use --yes)'))
        return 1
    if not args.no_build:
        c.say('compilando na Pi…')
        if pi.run('make build', capture=False).returncode != 0:
            c.fail('a compilação na Pi falhou')
            return 1
    if plan['boot'][0] is not None:
        staged = c.ROOT / 'build' / 'gama-config.txt'
        staged.parent.mkdir(parents=True, exist_ok=True)
        staged.write_text(plan['boot'][0])
        if not pi.copy_to(staged, STAGED_BOOT):
            c.fail('não consegui copiar o config.txt novo para a Pi')
            return 1
    if not pi.sudo(install_command(args.board, plan, stamp), f'instalando a placa {args.board}'):
        c.fail('a instalação não terminou', 'nada foi trocado sem cópia: os .bak-' + stamp + ' estão em /etc/gama')
        return 1
    c.ok(f'placa {args.board} instalada em {pi.name}')
    if plan['boot'][0] is not None:
        c.warn('o config.txt só vale depois de reiniciar a Pi', './gama pi reboot')
    c.say('confira: ./gama doctor')
    return 0
