"""gama link: the radio bench's steps (09/10, flight/ttcd/README.md) —
ttcd on radio A and the ground at the other end, one scripted step each,
the logs in docs/vv/bancada/<date>/.

    gama link ping|toa|per|rates|lost-ack|lbt|all [--port P] [--power 2]
    gama sim link ping|...|all [--power 2]

From the PC (the default): ttcd on the Pi with the installed configuration
(/etc/gama/ttcd.conf: the board's pins, ./gama board install) and the ESP32
ground station on this machine's USB — make bench-<step> SPLIT=1 GS=esp32,
after the same checks as gama mission (ssh, ESP32, nothing holding the
radio, radio A answering) and a build on the Pi. On the Pi: its two RA-02,
gs_cli on radio B. gama sim link <step> (before 24/11, --udp): both ends
on this machine, the UDP radio, no hardware — to see the steps work before
the bench.

The steps themselves stay in the Makefile and tools/bench/run_step.sh.
"""

import subprocess
from types import SimpleNamespace

import common as c
import mission

STEPS = ('ping', 'toa', 'per', 'rates', 'lost-ack', 'lbt')
WHAT = {
    'ping': '10 PINGs, todos com ACK',
    'toa': 'tempo no ar de cada perfil, dentro de 5% do modelo',
    'per': 'taxa de erro: 1000 PINGs por perfil (200 no SAFE) — demora',
    'rates': 'troca de perfil entre todos os pares',
    'lost-ack': 'o ACK do SET_RATE perdido: a volta automática tem de recuperar',
    'lbt': 'ouvir antes de falar, header contra preamble',
}
INSTALLED_CONF = '/etc/gama/ttcd.conf'


def target(step):
    return 'bench' if step == 'all' else f'bench-{step}'


def make_argv(step, mode, port=None, power=2, conf=None):
    """The make line for a step. mode: 'split' (ttcd on the Pi, the ESP32
    here), 'pi' (on the Pi, gs_cli on radio B) or 'udp'."""
    argv = ['make', '-C', str(c.ROOT), target(step), f'POWER={power}']
    if mode == 'udp':
        return argv + ['RADIO=udp']
    argv.append(f'TTCD_CONF={conf or INSTALLED_CONF}')
    if mode == 'split':
        argv += ['SPLIT=1', 'GS=esp32', f'ESP32_PORT={port}']
    return argv


def preflight(args, remote, local):
    """mission's checks, which are the same: the ESP32 port or None."""
    return mission.preflight(SimpleNamespace(port=args.port, skip_radio_check=args.skip_radio_check),
                             remote, local)


def cmd_link(args, remote=None, local=None, call=subprocess.call):
    sim = getattr(args, 'kind', None) == 'sim'
    if args.udp and not sim:
        c.warn('gama link --udp agora é ./gama sim link (o --udp some depois de 24/11)')
        sim = True
    if sim:
        c.banner('sim')
    try:
        return run_link(args, sim, remote, local, call)
    finally:
        if sim:
            c.banner('sim', 'fim')


def run_link(args, sim, remote, local, call):
    steps = STEPS if args.step == 'all' else (args.step,)
    for s in steps:
        c.say(f'{s}: {WHAT[s]}')
    if sim:
        mode, port = 'udp', None
        c.say('rádio UDP, as duas pontas neste computador (sem hardware)')
    elif c.on_pi():
        mode, port = 'pi', None
        c.say('na Pi: rádio A pelo ' + (args.conf or INSTALLED_CONF) + ', gs_cli no rádio B')
    else:
        mode = 'split'
        remote = remote or mission.Remote(c.need_pi_host(), c.pi_dir())
        local = local or mission.Local()
        port = preflight(args, remote, local)
        if port is None:
            return 1
        if not args.no_build:
            c.say('compilando na Pi…')
            if not remote.build():
                c.fail('a compilação na Pi falhou')
                return 1
            c.ok('compilado na Pi')
        c.say(f'ttcd na Pi com {args.conf or INSTALLED_CONF}, a ground na ESP32 ({port})')
    argv = make_argv(args.step, mode, port, args.power, args.conf)
    c.say(' '.join(argv[3:]))
    try:
        status = call(argv)
    except KeyboardInterrupt:
        status = 130
    print()
    if status == 0:
        c.ok('passo(s) concluído(s); os logs: ' +
             ('build/bench-udp/' if mode == 'udp' else 'docs/vv/bancada/<hoje>/'))
    else:
        c.fail(f'o passo falhou (código {status})', 'o resumo acima diz o que faltou; ./gama radio check')
    return 0 if status == 0 else 1
