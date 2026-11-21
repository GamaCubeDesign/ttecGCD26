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
    r = c.run(['nmcli', 'con', 'up', ap_connection()])
    if r.returncode != 0:
        c.fail(f'nmcli não subiu {ap_connection()}', (r.stderr or r.stdout).strip())
        return 1
    c.ok(f'rede da bancada no ar ({ap_connection()})')
    c.say(f'esperando a Pi ({host}) responder, até {WAIT_S} s (ela leva ~40 s depois de ligar)…')
    deadline = time.monotonic() + WAIT_S
    while time.monotonic() < deadline:
        r = c.ssh(host, 'hostname -I', timeout=8)
        if r.returncode == 0:
            c.ok(f'a Pi responde: {r.stdout.split()[0] if r.stdout.split() else host}')
            return 0
        time.sleep(3)
    c.fail('a Pi não respondeu', 'está ligada? entrou no hotspot do celular em vez desta rede? ./gama net check')
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
