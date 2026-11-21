"""gama pi: the Pi from the PC — copy the tree, bring the logs back, open a
shell, and stop whatever would hold the radio, the SDR or the I²C bus.
"""

import os
import shlex
import sys

import common as c

# Manual leftovers die without sudo (they are the pi user's); the bracket
# keeps pkill from matching the remote shell that runs it.
KILL_LEFTOVERS = ('pkill -x ttcd; pkill -x obcd; pkill -x adsbd; pkill -x dump1090-fa; '
                  "pkill -f 'tools/bench/pi_missio[n].sh'; pkill -f 'tools/sbs_replay/sbs_repla[y].py'; true")
SERVICES = ('ttcd', 'obcd', 'adsbd')


def cmd_sync(args):
    host = c.need_pi_host()
    return 0 if c.sync(host, c.pi_dir()) else 1


def cmd_fetch(args):
    host = c.need_pi_host()
    got = c.fetch(host, c.pi_dir())
    if got is None:
        return 1
    if got:
        c.ok(f'{len(got)} arquivo(s) trazido(s) para docs/vv/bancada/')
        for name in got[:20]:
            print(f'    {name}')
        if len(got) > 20:
            print(f'    … e mais {len(got) - 20}')
    else:
        c.ok('nada novo na Pi')
    return 0


def cmd_ssh(args):
    host = c.need_pi_host()
    remote = c.in_dir(c.pi_dir(), 'exec bash -l')
    os.execvp('ssh', ['ssh', '-t', host, remote])


def active_services(host):
    r = c.ssh(host, 'systemctl is-active ' + ' '.join(SERVICES), timeout=20)
    return [s for s, state in zip(SERVICES, r.stdout.split()) if state == 'active']


def stop(host, directory=None, quiet=False):
    """Kill manual leftovers, then stop the services (sudo). True when
    nothing is left holding the hardware."""
    c.ssh(host, KILL_LEFTOVERS, timeout=20)
    if not quiet:
        c.ok('sobras manuais encerradas (ttcd, obcd, adsbd, dump1090, replay, pi_mission)')
    svc = active_services(host)
    if not svc:
        if not quiet:
            c.ok('nenhum serviço rodando')
        return True
    command = 'sudo systemctl stop ' + ' '.join(svc)
    if sys.stdin.isatty():
        c.say(f'parando os serviços ({", ".join(svc)}); o sudo da Pi pode pedir a senha')
        r = c.run(c.ssh_argv(host, command, tty=True), capture=False)
        if r.returncode == 0 and not active_services(host):
            c.ok('serviços parados (até o próximo boot)')
            return True
    c.fail('serviços ainda rodando: ' + ', '.join(svc),
           'num terminal seu (o sudo precisa perguntar a senha):\n'
           f'ssh -t {host} {shlex.quote(command)}')
    return False


def cmd_stop(args):
    host = c.need_pi_host()
    if not c.reachable(host):
        c.fail(f'a Pi ({host}) não responde pelo ssh', './gama net up')
        return 1
    return 0 if stop(host) else 1


def cmd_reboot(args):
    host = c.need_pi_host()
    if not c.reachable(host):
        c.fail(f'a Pi ({host}) não responde pelo ssh', './gama net up')
        return 1
    pi = c.Pi(host)
    # The ssh drops as the Pi goes down: that is not a failure.
    pi.sudo('sudo systemctl reboot', 'reiniciando a Pi')
    c.say('a Pi volta em ~40 s; ./gama net check, depois ./gama doctor')
    return 0
