"""gama net: the bench network. The PC's NetworkManager connection bancada-ap
(created once, docs/vv/bancada/README.md) turns its Wi-Fi into the access
point gama-bancada and shares the internet of the ethernet cable; the Pi
knows that network. Without the cable, bringing it up leaves the PC and the
Pi with no internet (and the Pi with no NTP, so a wrong clock): up refuses
unless asked.
"""

import time

import common as c
from doctor import ap_connection

WAIT_S = 90


def ethernet_connected():
    dev = c.run(['nmcli', '-t', '-f', 'TYPE,STATE', 'device']).stdout
    return any(l == 'ethernet:connected' for l in dev.splitlines())


def cmd_up(args):
    host = c.need_pi_host()
    if not ethernet_connected():
        if not args.sem_internet:
            c.fail('o PC não está no cabo ethernet',
                   'com a rede da bancada no ar, o Wi-Fi do PC deixa de ser cliente: '
                   'o PC e a Pi ficam sem internet\n'
                   '(esta conversa cai, a Pi fica sem NTP e com o relógio errado).\n'
                   'Ligue o cabo, ou rode ./gama net up --sem-internet sabendo disso.')
            return 1
        c.warn('sem cabo ethernet: PC e Pi sem internet enquanto a rede da bancada estiver no ar')
    c.say(f'{c.clock()} subindo {ap_connection()} (nmcli)')
    t0 = time.monotonic()
    r = c.run(['nmcli', 'con', 'up', ap_connection()])
    if r.returncode != 0:
        c.fail(f'{c.clock()} nmcli não subiu {ap_connection()} ({time.monotonic() - t0:.1f} s)',
               (r.stderr or r.stdout).strip())
        return 1
    c.ok(f'{c.clock()} rede da bancada no ar ({ap_connection()}, {time.monotonic() - t0:.1f} s)')
    return wait_for_pi(host)


def ssh_reason(r):
    """One line on why an ssh attempt failed: its last stderr line, which
    tells a name not resolved (the Pi not on this network yet) from a
    refused port (up, sshd not yet) from a timeout."""
    if r.returncode == 124:
        return 'sem resposta em 8 s'
    lines = [l.strip() for l in (r.stderr or '').splitlines() if l.strip()]
    return lines[-1] if lines else f'ssh saiu com {r.returncode}'


def wait_for_pi(host, wait_s=WAIT_S, interval_s=3, ssh=None, now=time.monotonic, sleep=time.sleep):
    """ssh until the Pi answers or wait_s runs out, one log line per attempt:
    the time, the attempt, the seconds since the network came up and, on a
    failure, ssh's own reason. ssh/now/sleep are for the tests."""
    ssh = ssh or (lambda: c.ssh(host, 'hostname -I', timeout=8))
    c.say(f'{c.clock()} esperando a Pi ({host}) responder pelo ssh, até {wait_s} s '
          f'(ela leva ~40 s depois de ligar), uma tentativa a cada {interval_s} s')
    t0 = now()
    attempt = 0
    reasons = {}
    while now() - t0 < wait_s:
        attempt += 1
        t = now()
        r = ssh()
        took = now() - t
        elapsed = now() - t0
        if r.returncode == 0:
            addr = r.stdout.split()[0] if r.stdout.split() else host
            c.ok(f'{c.clock()} tentativa {attempt}, +{elapsed:.0f} s: a Pi responde: {addr} '
                 f'(ssh em {took:.1f} s)')
            return 0
        reason = ssh_reason(r)
        reasons[reason] = reasons.get(reason, 0) + 1
        c.say(f'{c.clock()} tentativa {attempt}, +{elapsed:.0f} s: falhou em {took:.1f} s: {reason}')
        sleep(interval_s)
    summary = '; '.join(f'{n}x {why}' for why, n in reasons.items())
    c.fail(f'{c.clock()} a Pi não respondeu: {attempt} tentativas em {now() - t0:.0f} s',
           f'{summary}\n'
           'está ligada? entrou no hotspot do celular em vez desta rede? ./gama net check')
    return 1


def cmd_down(args):
    r = c.run(['nmcli', 'con', 'down', ap_connection()])
    if r.returncode != 0:
        c.fail(f'nmcli não desligou {ap_connection()}', (r.stderr or r.stdout).strip())
        return 1
    c.ok('rede da bancada desligada; o Wi-Fi volta para a rede de costume')
    return 0


def cmd_check(args):
    print(c.run(['nmcli', '-f', 'DEVICE,TYPE,STATE,CONNECTION', 'device']).stdout.rstrip())
    c.show(*(('ok', 'cabo ethernet conectado') if ethernet_connected() else
             ('warn', 'sem cabo ethernet: com a rede da bancada no ar, não há internet')))
    host = c.pi_host()
    if not host:
        c.fail('PI_HOST não definido', "echo 'PI_HOST = pi@gamapi.local' > local.mk")
        return 1
    r = c.ssh(host, 'hostname -I; curl -sI --max-time 5 https://deb.debian.org >/dev/null && echo internet || echo sem-internet',
              timeout=20)
    if r.returncode != 0:
        c.fail(f'a Pi ({host}) não responde pelo ssh', './gama net up')
        return 1
    lines = r.stdout.split('\n')
    c.ok(f'a Pi responde: {lines[0].strip()}')
    c.show(*(('ok', 'a Pi tem internet') if 'internet' in lines[1:2] else
             ('warn', 'a Pi está sem internet (sem NTP: relógio pode estar errado)')))
    return 0
