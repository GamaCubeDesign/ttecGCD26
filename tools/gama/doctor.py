"""gama doctor and gama radio check: the bench, end to end, as [OK]/[AVISO]/[ERRO] lines
with what to do about each one.

The Pi's side comes from probe_pi.py, run there over ssh after a sync; the
evaluation lives here, in pure functions (findings in, lines out), so the
tests feed it recorded probes.
"""

import json
import time
from pathlib import Path

import board as boardconf
import common as c

PINMAP = 'docs/relatorios/2026-11-20-pinagem-bancada.html'
CLOCK_SLACK_S = 60


def radio_finding(radio):
    """(level, text, hint) for probe_pi's radio check."""
    st = (radio or {}).get('status')
    v = radio.get('value') if radio else None
    if st == 'ok':
        return 'ok', 'rádio A responde pelo SPI (RegVersion 0x12)', None
    if st == 'miso_floating':
        return ('fail', 'rádio A não responde: nada dirige o MISO (0x00; 0xff com pull-up)',
                'com a Pi desligada: o RA-02 está conectado? 3,3 V no pino 1 e GND no 6?\n'
                f'MISO no pino 21; MOSI 19, SCK 23, NSS 24 ({PINMAP})')
    if st == 'miso_low':
        return ('fail', 'rádio A não responde: MISO preso em 0 (0x00 também com pull-up)',
                'o MISO (pino 21) em curto com o GND, ou o rádio sem 3,3 V puxando a linha')
    if st == 'unexpected':
        return ('fail', f'rádio A responde errado: 0x{v:02x} (esperado 0x12)' if v is not None
                else 'rádio A responde errado',
                f'fios do SPI trocados ou mau contato: MOSI 19, MISO 21, SCK 23, NSS 24 ({PINMAP})')
    if st == 'busy':
        return 'warn', 'o ttcd está rodando e segura o rádio A: não li o chip', './gama pi stop'
    if st == 'no_conf':
        return ('fail', 'não há /etc/gama/ttcd.conf na Pi',
                './gama board install')
    if st == 'no_spidev':
        return ('fail', f'{radio.get("spi_dev")} não existe na Pi',
                'dtparam=spi=on em /boot/firmware/config.txt e reiniciar')
    return 'fail', f'não consegui ler o rádio: {(radio or {}).get("error", "sem resposta")}', None


def pi_findings(p, pc_epoch=None):
    """(level, text, hint) lines for a full probe_pi result."""
    out = []
    # The clock: the Pi has no RTC; without internet it has no NTP either.
    clk = p.get('clock') or {}
    if pc_epoch is not None and clk.get('epoch') is not None:
        skew = clk['epoch'] - pc_epoch
        if abs(skew) > CLOCK_SLACK_S:
            out.append(('warn', f'relógio da Pi {skew / 3600:+.1f} h em relação ao PC',
                        'sem NTP: a Pi precisa de internet (cabo ethernet no PC, com a rede da bancada).\n'
                        'A missão funciona (a hora vem do SET_TIME), mas as pastas de log saem com a data errada.'))
        else:
            out.append(('ok', 'relógio da Pi certo' + (' (NTP)' if clk.get('ntp_synchronized') else ''), None))
    if p.get('internet') is False:
        out.append(('warn', 'a Pi está sem internet', 'ligue o cabo ethernet no PC (a rede da bancada repassa a dele)'))
    elif p.get('internet'):
        out.append(('ok', 'a Pi tem internet', None))

    run = p.get('running') or {}
    svc = {k: v for k, v in (p.get('services') or {}).items() if v == 'active'}
    if run or svc:
        names = sorted(set(run) | {f'{k} (serviço)' for k in svc})
        out.append(('warn', 'rodando na Pi: ' + ', '.join(names),
                    'para a bancada, nada pode segurar o rádio, o SDR ou o I²C: ./gama pi stop'))
    else:
        out.append(('ok', 'nenhum daemon nem serviço rodando na Pi', None))

    conf = p.get('ttcd_conf')
    files = p.get('conf_files')
    if conf is None:
        out.append(('fail', 'não há /etc/gama/ttcd.conf', './gama board install'))
    else:
        missing = [f for f in ('ttcd.conf', 'obcd.conf', 'adsbd.conf') if files is not None and f not in files]
        w = p.get('wiring')
        pins = f"RESET {conf.get('reset_line')}, DIO0 {conf.get('dio0_line')}"
        if w == 'B2':
            out.append(('ok', f'ttcd.conf na fiação da placa (B2: {pins})', None))
        elif w == 'B1':
            out.append(('warn', f'ttcd.conf na fiação de outubro (B1: {pins})',
                        'vale se os fios estão nos pinos 38/40; a da placa é 22/7 (B2): '
                        './gama board install'))
        else:
            out.append(('warn', f'ttcd.conf com pinos que não são nem B1 nem B2 ({pins})',
                        f'confira contra os fios ({PINMAP})'))
        if missing:
            out.append(('fail', 'faltam em /etc/gama: ' + ', '.join(missing),
                        './gama board install'))
    out += board_findings(p.get('installed'))

    w1 = p.get('w1_lines')
    if w1 is not None:
        if w1 == ['dtoverlay=w1-gpio,gpiopin=17']:
            out.append(('ok', '1-Wire só no GPIO17', None))
        else:
            out.append(('fail', '1-Wire mal configurado: ' + (', '.join(w1) or 'nenhuma linha w1-gpio'),
                        'uma só linha dtoverlay=w1-gpio,gpiopin=17 em /boot/firmware/config.txt '
                        '(./gama board install, depois ./gama pi reboot); '
                        'sem o gpiopin, o 1-Wire toma o GPIO4 (DIO0 do rádio na placa)'))

    if 'sdr' in p:
        out.append(('ok', 'SDR no USB da Pi', None) if p['sdr'] else
                   ('warn', 'o SDR não aparece no USB da Pi', 'NESDR na porta USB (a do meio), pelo adaptador OTG'))

    if 'radio' in p:
        out.append(radio_finding(p['radio']))
    return out


def board_findings(installed, board='obc-v2'):
    """obcd.conf against the board's: the pins and addresses the board sets
    (ttcd.conf's pins are the wiring line above). Extra keys are not
    differences: the board's file carries only what the board changes."""
    out = []
    conf = (installed or {}).get('obcd')
    src = c.ROOT / 'flight' / 'conf' / board / 'obcd.conf'
    if conf is None or not src.exists():
        return out
    want = boardconf.settings(src.read_text())
    diff = [f'{k} {conf.get(k) or "—"} ≠ {v}' for k, v in want.items() if not boardconf.same(conf.get(k), v)]
    if diff:
        out.append(('warn', f'obcd.conf difere do da placa {board}: ' + ', '.join(diff),
                    'vale para a bancada sem a placa; para a placa: ./gama board install'))
    else:
        out.append(('ok', f'obcd.conf com os pinos da placa {board}', None))
    return out


def pc_findings():
    out = []
    dev = c.run(['nmcli', '-t', '-f', 'TYPE,STATE,CONNECTION', 'device']).stdout
    wired = any(l.startswith('ethernet:connected') for l in dev.splitlines())
    ap = any(l.startswith('wifi:connected:') and l.endswith(':' + ap_connection()) for l in dev.splitlines())
    out.append(('ok', 'PC no cabo ethernet (tem internet com a rede da bancada no ar)', None) if wired else
               ('warn', 'PC sem cabo ethernet', 'com a rede da bancada no ar, o PC e a Pi ficam sem internet'))
    out.append(('ok', f'rede da bancada no ar ({ap_connection()})', None) if ap else
               ('warn', 'rede da bancada desligada', './gama net up'))
    port, problem = c.esp32_port()
    out.append(('ok', f'ESP32 em {port}', None) if port else ('warn', 'ESP32: ' + problem, None))
    built = (c.ROOT / 'build' / 'flight' / 'ttcd').exists()
    out.append(('ok', 'build/ compilado no PC', None) if built else ('warn', 'build/ não compilado no PC', 'make'))
    return out


def ap_connection():
    return c.setting('AP_CON', 'bancada-ap')


def remote_probe(host, directory, args=''):
    """probe_pi.py on the Pi (the tree must be synced); its JSON or None."""
    r = c.ssh(host, c.in_dir(directory, f'python3 tools/gama/probe_pi.py {args}'), timeout=60)
    if r.returncode != 0:
        return None
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return None


def local_probe(args=()):
    import probe_pi
    flags = list(args)
    return probe_pi.probe(radio='--no-radio' not in flags, others='--radio-only' not in flags)


def cmd_doctor(args):
    bad = 0
    if c.on_pi():
        c.say('na Pi')
        findings = pi_findings(local_probe())
    else:
        c.say('no PC')
        findings = pc_findings()
        host = c.pi_host()
        if not host:
            findings.append(('fail', 'PI_HOST não definido', "echo 'PI_HOST = pi@gamapi.local' > local.mk"))
        elif not c.reachable(host):
            findings.append(('fail', f'a Pi ({host}) não responde pelo ssh',
                             'está ligada? na mesma rede? ./gama net up'))
        else:
            findings.append(('ok', f'a Pi responde ({host})', None))
            if args.no_sync or c.sync(host, c.pi_dir(), quiet=True):
                pc_epoch = time.time()
                p = remote_probe(host, c.pi_dir())
                if p is None:
                    findings.append(('fail', 'não consegui rodar o probe na Pi', 'sem python3 na Pi?'))
                else:
                    findings += pi_findings(p, pc_epoch)
    for level, text, hint in findings:
        c.show(level, text, hint)
        bad += level == 'fail'
    print()
    c.say('tudo certo' if not bad else f'{bad} problema(s) a resolver')
    return 1 if bad else 0


def cmd_radio_check(args):
    if c.on_pi():
        radio = local_probe(['--radio-only'])['radio']
    else:
        host = c.need_pi_host()
        if not c.reachable(host):
            c.fail(f'a Pi ({host}) não responde pelo ssh', './gama net up')
            return 1
        if not args.no_sync and not c.sync(host, c.pi_dir(), quiet=True):
            return 1
        p = remote_probe(host, c.pi_dir(), '--radio-only')
        if p is None:
            c.fail('não consegui rodar o probe na Pi')
            return 1
        radio = p['radio']
    level, text, hint = radio_finding(radio)
    c.show(level, text, hint)
    return 0 if level == 'ok' else 1
