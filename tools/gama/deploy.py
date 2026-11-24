"""gama deploy: o burn-wire disparado na própria Pi, pelo SSH — o plano B da
REG-07, para quando o rádio não alcança o satélite.

    gama deploy [--yes]

Roda /usr/local/sbin/gama-deploy na Pi (instalado pelo make install, com a
regra do sudoers que dispensa a senha): ele manda SIGUSR1 ao obcd, que entra
em BASIC_INTERMEDIATE pelas mesmas transições do SET_MODE basic_inter — só a
partir de PRE_TEST (D1), um pulso por entrada (D3) — e imprime as linhas do
log do obcd até a volta a PRE_TEST. A página faz o mesmo (aba Implantação,
"Plano B"), com gs_web.py --pi-ssh.
"""

import json
import re

import common as c

REMOTE = '/usr/local/sbin/gama-deploy'
KIND = re.compile(r'^\{"mono_ms":(\d+),"event":"([a-z_]+)"')


def summarize(text):
    """(result, [(level, message)]) of gama-deploy's output."""
    result, lines, on = None, [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('RESULT '):
            result = (line.split() + [None])[1]
            continue
        m = KIND.match(line)
        if not m:
            continue
        try:
            o = json.loads(line)
        except ValueError:
            continue
        t, kind = int(m.group(1)), m.group(2)
        if kind == 'local_command':
            lines.append(('ok', 'sinal entregue ao obcd'))
        elif kind == 'mode':
            lines.append(('ok', f'modo {o.get("from")} → {o.get("to")}'))
        elif kind == 'event_refused':
            lines.append(('fail', f'recusado: o OBC está em {o.get("mode")}; a implantação só sai de PRE_TEST'))
        elif kind == 'burnwire' and o.get('on') is True:
            on = t
            if o.get('driven'):
                lines.append(('ok', 'burn-wire ligado (GPIO acionado)'))
            else:
                lines.append(('fail', 'burn-wire "ligado" sem GPIO: o obcd não tem a linha, nada foi acionado'))
        elif kind == 'burnwire' and o.get('on') is False:
            lines.append(('ok', 'burn-wire desligado' + (f' após {t - on} ms' if on is not None else '')))
    return result, lines


WHY = {
    'refused': 'o obcd recusou: ponha o OBC em PRE_TEST e repita',
    'timeout': 'sem volta a PRE_TEST em 12 s: veja ./gama service logs obcd',
    'nolog': 'sinal enviado, mas o obcd não tem arquivo de log para acompanhar',
    'noobcd': 'o obcd não está rodando (ou há mais de um): ./gama service status',
    'nosignal': 'o sinal não pôde ser enviado',
}


def cmd_deploy(args, pi=None):
    pi = pi or c.Pi()
    if not c.reachable_pi(pi):
        return 1
    if not c.confirm(f'acionar o burn-wire em {pi.name}, pelo SSH? é irreversível', args.yes):
        c.fail('nada foi feito', 'confirme com s, ou passe --yes')
        return 1
    c.say(f'disparando a implantação em {pi.name} (~6 s)…')
    r = pi.run(f'sudo -n {REMOTE}', timeout=40)
    if 'password is required' in (r.stderr or ''):
        c.fail('o sudo da Pi pediu senha: falta a regra do sudoers',
               'instale de novo, ela vem junto: ./gama service install')
        return 1
    if r.returncode == 127 or 'not found' in (r.stderr or ''):
        c.fail(f'{REMOTE} não está na Pi', './gama service install')
        return 1
    result, lines = summarize(r.stdout or '')
    for level, text in lines:
        c.show(level, text)
    if result == 'done' and all(level == 'ok' for level, _ in lines):
        c.ok('implantação feita, o OBC de volta a PRE_TEST')
        return 0
    if result == 'done':
        c.fail('o OBC fez o ciclo, mas o burn-wire não foi acionado de fato')
    else:
        c.fail(WHY.get(result, f'gama-deploy terminou com {r.returncode}'), (r.stderr or '').strip()[-300:] or None)
    return 1
