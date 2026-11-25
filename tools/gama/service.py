"""gama service: ttcd, obcd and adsbd as systemd services on the Pi — the
flight configuration of the on-site tests — from the Pi itself or from the
PC, with sudo resolved (a terminal for its password, or the exact line).

    gama service status                  each one: active? at boot? its log's last line
    gama service start|stop|restart|enable|disable [ttcd obcd adsbd]
    gama service logs ttcd [-n 40] [-f] [--journal]
    gama service install                 make install: daemons and units (keeps /etc/gama)
    gama service link [lora|ssh]         ttcd's link: the RA-02 (flight) or ssh (the fallback)

enable starts them now and at every boot; disable stops them and takes them
out of the boot. With no daemon named, all three.

In place of make service-*, adsbd-* and obcd-*, which ran sudo over ssh
without a terminal and failed (20/11).

link ssh is the fallback for a LoRa link that fails (25/11: radio A's MISO
held at 0 on the assembled satellite): a systemd drop-in starts ttcd on the
UDP radio, on the Pi's loopback, and ./gama ground --ssh runs the ground
console next to it, over ssh. /etc/gama/ttcd.conf is not touched; link lora
removes the drop-in. Both restart ttcd only; obcd and adsbd see no change.
"""

import shlex

import common as c

DAEMONS = ('ttcd', 'obcd', 'adsbd')
ACTIONS = {
    'start': ('systemctl start', 'ligando'),
    'stop': ('systemctl stop', 'parando'),
    'restart': ('systemctl restart', 'reiniciando'),
    'enable': ('systemctl enable --now', 'ligando agora e em todo boot'),
    'disable': ('systemctl disable --now', 'parando e tirando do boot'),
}


LINK_DROPIN = '/etc/systemd/system/ttcd.service.d/link-ssh.conf'
# The installed unit's line plus the UDP radio, every key pinned so nothing
# in /etc/gama/ttcd.conf can point it elsewhere; the ports are gs_cli's
# defaults (tools/gs_cli/gs_cli.c).
LINK_SSH_UNIT = ('# ./gama service link ssh: ttcd on the UDP radio, the ground over ssh\n'
                 '# (./gama ground --ssh); ./gama service link lora removes this file.\n'
                 '[Service]\n'
                 'ExecStart=\n'
                 'ExecStart=/usr/local/bin/ttcd -c /etc/gama/ttcd.conf -o radio=udp '
                 '-o udp_bind=127.0.0.1 -o udp_port=47001 -o udp_peer=127.0.0.1 -o udp_peer_port=47002\n')
LINK_NAME = {'lora': 'LoRa (o RA-02)', 'ssh': 'SSH (o rádio UDP no loopback da Pi)'}


def link_command(link):
    """The sudo line that puts ttcd on a link and restarts it."""
    if link == 'ssh':
        put = (f'sudo mkdir -p {shlex.quote(LINK_DROPIN.rsplit("/", 1)[0])} && '
               # printf's format, its newlines as \\n: one line to type by hand.
               f"printf {shlex.quote(LINK_SSH_UNIT.replace(chr(10), chr(92) + 'n'))} "
               f'| sudo tee {LINK_DROPIN} >/dev/null')
    else:
        put = f'sudo rm -f {LINK_DROPIN}'
    return f'{put} && sudo systemctl daemon-reload && sudo systemctl restart ttcd'


def link_probe_command():
    return f'test -f {LINK_DROPIN} && echo "@link ssh" || echo "@link lora"'


def current_link(pi):
    """'lora' or 'ssh', as the Pi has it now; None when it cannot tell."""
    r = pi.run(link_probe_command(), timeout=30)
    for line in (r.stdout or '').splitlines():
        if line.startswith('@link '):
            return line.split()[1]
    return None


def log_path_command(d):
    """Shell that prints the daemon's log_path from /etc/gama/<d>.conf,
    /var/lib/gama/<d>.jsonl when it names none."""
    return (f"p=$(sed -n 's/^[[:space:]]*log_path[[:space:]]*=[[:space:]]*\\([^[:space:]#]*\\).*/\\1/p' "
            f"/etc/gama/{d}.conf 2>/dev/null | tail -n 1); echo \"${{p:-/var/lib/gama/{d}.jsonl}}\"")


def status_command():
    """Per daemon a marker line, '@@ name active enabled log', then the
    log's last line."""
    parts = [link_probe_command()]
    for d in DAEMONS:
        parts.append(f'l=$({log_path_command(d)}); '
                     f'echo "@@ {d} $(systemctl is-active {d} 2>/dev/null) $(systemctl is-enabled {d} 2>/dev/null) $l"; '
                     f'tail -n 1 "$l" 2>/dev/null; true')
    return '; '.join(parts)


def parse_status(text):
    """[(daemon, active, enabled, log path, last log line)]"""
    out = []
    for line in text.splitlines():
        if line.startswith('@@ '):
            f = line[3:].split() + ['?'] * 4
            out.append([f[0], f[1], f[2], f[3] if f[3] != '?' else '', ''])
        elif out and line.strip() and not out[-1][4]:
            out[-1][4] = line.strip()
    return [tuple(r) for r in out]


def names(args):
    return list(dict.fromkeys(getattr(args, 'daemons', None) or DAEMONS))


def cmd_status(args, pi=None):
    pi = pi or c.Pi()
    if not c.reachable_pi(pi):
        return 1
    r = pi.run(status_command(), timeout=30)
    rows = parse_status(r.stdout)
    if not rows:
        c.fail('não consegui ler o estado dos serviços', (r.stderr or '').strip()[-300:])
        return 1
    link = next((ln.split()[1] for ln in r.stdout.splitlines() if ln.startswith('@link ')), None)
    if link == 'ssh':
        c.warn(f'enlace do ttcd: {LINK_NAME["ssh"]}; a ground é ./gama ground --ssh',
               'de volta ao rádio: ./gama service link lora')
    elif link == 'lora':
        c.say(f'enlace do ttcd: {LINK_NAME["lora"]}')
    for d, active, enabled, log, last in rows:
        boot = 'no boot' if enabled == 'enabled' else 'fora do boot' if enabled == 'disabled' else f'boot: {enabled}'
        level = 'ok' if active == 'active' else 'warn' if active in ('inactive', 'failed', 'activating') else 'fail'
        c.show(level, f'{d:<6} {active:<9} {boot:<13} {log}')
        if last:
            print(f'      {last[:160]}')
    return 0


def cmd_action(args, pi=None):
    pi = pi or c.Pi()
    if not c.reachable_pi(pi):
        return 1
    ds = names(args)
    verb, why = ACTIONS[args.action]
    if args.action in ('start', 'enable', 'restart'):
        # ttcd first: adsbd connects to it (adsbd.service: After=ttcd.service).
        ds = [d for d in DAEMONS if d in ds]
    if not pi.sudo(f'sudo {verb} ' + ' '.join(ds), f'{why}: {", ".join(ds)}'):
        return 1
    cmd_status(args, pi)          # what it led to; the action itself succeeded
    return 0


def cmd_logs(args, pi=None):
    pi = pi or c.Pi()
    if not c.reachable_pi(pi):
        return 1
    d = args.daemon
    if args.journal:
        command = f'journalctl -u {d} -n {args.lines} --no-pager' + (' -f' if args.follow else '')
    else:
        command = f'tail -n {args.lines} {"-F " if args.follow else ""}"$({log_path_command(d)})"'
    try:
        return pi.run(command, tty=args.follow, capture=False).returncode
    except KeyboardInterrupt:
        return 0


def cmd_install(args, pi=None):
    pi = pi or c.Pi()
    if not c.reachable_pi(pi) or not pi.sync():
        return 1
    c.say('compilando na Pi…')
    if pi.run('make build', capture=False).returncode != 0:
        c.fail('a compilação na Pi falhou')
        return 1
    if not pi.sudo('make install', 'instalando os daemons e os serviços'):
        return 1
    c.ok('daemons e serviços instalados; /etc/gama mantido (a configuração da placa: ./gama board install)')
    return 0


def cmd_link(args, pi=None):
    """With no link named, says which one ttcd is on; with one, moves it."""
    pi = pi or c.Pi()
    if not c.reachable_pi(pi):
        return 1
    now = current_link(pi)
    if not args.link:
        if now is None:
            c.fail('não consegui ler o enlace do ttcd')
            return 1
        c.say(f'enlace do ttcd: {LINK_NAME[now]}')
        return 0
    if now == args.link:
        c.ok(f'o ttcd já está no enlace {LINK_NAME[args.link]}')
        return 0
    if not pi.sudo(link_command(args.link), f'passando o ttcd para {LINK_NAME[args.link]}'):
        return 1
    if args.link == 'ssh':
        c.ok('ttcd no rádio UDP: a ground é ./gama ground --ssh',
             'não é evidência de RF; de volta ao rádio: ./gama service link lora')
    else:
        c.ok('ttcd de volta ao RA-02 (a configuração de voo)')
    cmd_status(args, pi)
    return 0
