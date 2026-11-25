#!/usr/bin/env python3
"""gama — operar a bancada e o satélite.

O make compila, testa e analisa (make help); o gama opera: a rede da
bancada, a Pi, o diagnóstico, a placa, os serviços, o rádio, o enlace, a
antena, a ESP32 e a missão. Ele sabe se está no PC ou na Pi e faz o ssh
sozinho. PI_HOST vem do local.mk, como no Makefile.
Desde 21/11; docs/relatorios/2026-11-17-comandos-principais.html.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import antenna  # noqa: E402
import board    # noqa: E402
import deploy   # noqa: E402
import doctor   # noqa: E402
import esp32    # noqa: E402
import ground   # noqa: E402
import link     # noqa: E402
import mission  # noqa: E402
import net      # noqa: E402
import pi       # noqa: E402
import service  # noqa: E402

EXAMPLES = """\
exemplos, na ordem de uma bancada:
  (sem prefixo: tudo real; replay: hardware real, aviões de um arquivo;
   sim: nada físico, tudo neste computador)
  ./gama net up                     a rede da bancada (com o cabo ethernet no PC)
  ./gama doctor                     PC, rede, Pi, relógio, configuração, SDR e rádio A
  ./gama board install              a configuração da placa do OBC (obc-v2) na Pi
  ./gama radio check                o rádio A responde pelo SPI?
  ./gama pi stop                    para o que estiver segurando o rádio, o SDR e o I²C
  ./gama link ping                  10 PINGs pela ESP32; ./gama link all = os 6 passos
  ./gama sim link ping              os mesmos passos sem hardware (rádio UDP no PC)
  ./gama antenna check --pi         o SDR da Pi ouve aviões?
  ./gama mission --secs 120         com aviões de verdade, pelo SDR (ao ar livre)
  ./gama replay mission --secs 120 --input docs/vv/trafego-real/2026-11-17-unsam/capture_sem_categoria.ndjson
                                    satélite e rádio reais, aviões de uma captura
  ./gama sim mission --secs 120 --input <captura>   tudo no PC, sem hardware
  ./gama ground                     só a página, com a ESP32 (o satélite já no ar: os serviços)
  ./gama sim ground --session <ground.jsonl> --speed 10   repete na página uma sessão gravada
  ./gama service enable             a configuração de voo: os três daemons em todo boot
  ./gama service status             cada serviço e a última linha do log
  ./gama deploy                     plano B da REG-07: o burn-wire disparado na Pi pelo SSH
"""


MISSION_DESCRIPTION = ('Uma missão ADS-B operada pela página, num comando só: o satélite e a página, '
                       'com a mesma duração da MISSION_ADSB nos dois lados. Na Pi: verifica antes (ssh, '
                       'ESP32, nada rodando, rádio A), compila e sobe tudo; Ctrl-C encerra os dois, traz '
                       'os logs e mostra o resumo.')


def add_mission_args(m, kind):
    """kind: 'real' (the SDR), 'replay' (the Pi, a capture) or 'sim' (this PC)."""
    m.set_defaults(kind=kind)
    if kind != 'sim':
        m.add_argument('action', nargs='?', choices=['stop'],
                       help='stop: encerra uma missão que ficou rodando na Pi e traz os logs')
    m.add_argument('--secs', type=int, default=120, help='duração da MISSION_ADSB, s (padrão 120; 600 no voo)')
    if kind != 'real':
        m.add_argument('--input', required=(kind == 'sim'), help='captura (.ndjson ou .sbs) tocada no lugar do SDR')
    m.add_argument('--avr', action='store_true', help='a categoria pela porta AVR (Etapa B, ADR-0023), só nesta missão')
    m.add_argument('--web-port', type=int, default=8080, help='porta da página (padrão 8080)')
    if kind != 'sim':
        m.add_argument('--port', help='porta da ESP32 (padrão: a única ttyUSB/ttyACM)')
        m.add_argument('--power', type=int, default=2, help='potência da ground, dBm (padrão 2)')
        m.add_argument('--tx-power', type=int, default=2, help='potência do rádio A, dBm (padrão 2)')
        m.add_argument('--no-build', action='store_true', help='não compila na Pi antes')
        m.add_argument('--skip-radio-check', action='store_true', help='sobe mesmo se o rádio A não responder')
    m.set_defaults(func=lambda a: mission.cmd_mission_stop(a) if getattr(a, 'action', None) == 'stop'
                   else mission.cmd_mission(a))


def add_link_args(lk, kind):
    """kind: 'real' (radio A and the ESP32, or the Pi's two RA-02) or 'sim'
    (the UDP radio on this computer)."""
    lk.set_defaults(kind=kind)
    lk.add_argument('step', choices=(*link.STEPS, 'all'),
                    help=('; '.join(f'{k}: {v}' for k, v in link.WHAT.items()) + '; all: os seis, em ordem').replace('%', '%%'))
    lk.add_argument('--power', type=int, default=2, help='dBm, os dois rádios (padrão 2, a ~1 m)')
    if kind == 'real':
        lk.add_argument('--port', help='porta da ESP32 (padrão: a única ttyUSB/ttyACM)')
        lk.add_argument('--conf', help=f'o ttcd.conf na Pi (padrão {link.INSTALLED_CONF})')
        lk.add_argument('--no-build', action='store_true', help='não compila na Pi antes')
        lk.add_argument('--skip-radio-check', action='store_true', help='segue mesmo se o rádio A não responder')
    else:
        lk.set_defaults(port=None, conf=None, no_build=True, skip_radio_check=False)
    lk.set_defaults(func=link.cmd_link)


def add_ground_args(g, kind):
    """kind: 'real' (the ESP32) or 'sim' (the UDP radio, or a recorded session)."""
    g.set_defaults(kind=kind, func=ground.cmd_ground)
    g.add_argument('--secs', type=int, default=600, help='duração da MISSION_ADSB que a página conta, s (padrão 600, o voo)')
    g.add_argument('--web-port', type=int, default=8080, help='porta da página (padrão 8080)')
    g.add_argument('--no-adsb', action='store_true', help='sem o processamento ADS-B (mapa e estimativas)')
    # The battery estimate's calibration (ADR-0024), measured on the bench
    # (docs/relatorios/2026-11-25-roteiro-teste-eps.html); absent: the page's defaults.
    g.add_argument('--battery-mah', type=float, help='capacidade do pack, mAh (padrão 3200, NCR18650B)')
    g.add_argument('--battery-r-ohm', type=float, help='R das células ao INA219, ohm (padrão 0,25; o "R medido" da aba OBC)')
    g.add_argument('--battery-diode-v', type=float, help='queda no D1 da EPS, V (padrão 0,30; P+ menos a leitura do INA219)')
    if kind == 'real':
        g.add_argument('--port', help='porta da ESP32 (padrão: a única ttyUSB/ttyACM)')
        g.add_argument('--power', type=int, default=2, help='potência da ground, dBm (padrão 2)')
    else:
        g.add_argument('--session', metavar='GROUND.JSONL',
                       help='repete uma sessão gravada da ground, sem satélite (sem isto: o rádio UDP)')
        g.add_argument('--speed', type=float, default=1.0, help='velocidade da repetição (com --session)')
        g.add_argument('--loop', action='store_true', help='repete de novo no fim (com --session)')
        g.add_argument('--no-build', action='store_true', help='não compila antes (o gs_cli do rádio UDP)')


def build_parser():
    ap = argparse.ArgumentParser(prog='gama', description=__doc__.split('\n\n')[1],
                                 epilog=EXAMPLES, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='group', metavar='<comando>')

    d = sub.add_parser('doctor', help='verifica tudo: PC, rede, Pi, relógio, configuração, SDR e rádio A')
    d.add_argument('--no-sync', action='store_true', help='não copia o código para a Pi antes')
    d.set_defaults(func=doctor.cmd_doctor)

    n = sub.add_parser('net', help='a rede da bancada (bancada-ap)')
    ns = n.add_subparsers(dest='action', metavar='<ação>', required=True)
    up = ns.add_parser('up', help='sobe a rede e espera a Pi responder')
    up.add_argument('--sem-internet', '--no-internet', dest='sem_internet', action='store_true',
                    help='sobe mesmo sem o cabo ethernet (PC e Pi ficam sem internet)')
    up.set_defaults(func=net.cmd_up)
    ns.add_parser('down', help='desliga a rede da bancada').set_defaults(func=net.cmd_down)
    ns.add_parser('check', help='a rede vista dos dois lados').set_defaults(func=net.cmd_check)

    p = sub.add_parser('pi', help='a Pi, a partir do PC')
    ps = p.add_subparsers(dest='action', metavar='<ação>', required=True)
    ps.add_parser('sync', help='copia o código para a Pi (o que mudou)').set_defaults(func=pi.cmd_sync)
    ps.add_parser('fetch', help='traz os logs da bancada da Pi').set_defaults(func=pi.cmd_fetch)
    ps.add_parser('ssh', help='abre um terminal na Pi, dentro do repositório').set_defaults(func=pi.cmd_ssh)
    ps.add_parser('stop', help='encerra daemons manuais e para os serviços (sudo)').set_defaults(func=pi.cmd_stop)
    ps.add_parser('reboot', help='reinicia a Pi (sudo): depois de mudar o config.txt').set_defaults(func=pi.cmd_reboot)

    b = sub.add_parser('board', help='a configuração de voo de uma placa (flight/conf/) na Pi')
    bs = b.add_subparsers(dest='action', metavar='<ação>', required=True)
    bc = bs.add_parser('check', help='o que difere entre a Pi (/etc/gama, config.txt) e a placa; nada muda')
    bi = bs.add_parser('install', help='instala os daemons e a configuração da placa, com backup do que trocar')
    for x in (bc, bi):
        x.add_argument('--board', default='obc-v2', help='a pasta em flight/conf/ (padrão obc-v2)')
    bi.add_argument('--yes', '-y', action='store_true', help='não pergunta antes de aplicar')
    bi.add_argument('--no-build', action='store_true', help='não compila na Pi antes')
    bc.set_defaults(func=board.cmd_check)
    bi.set_defaults(func=board.cmd_install)

    s = sub.add_parser('service', help='ttcd, obcd e adsbd como serviços (a configuração de voo)')
    ss = s.add_subparsers(dest='action', metavar='<ação>', required=True)
    ss.add_parser('status', help='cada serviço: ligado? no boot? a última linha do log').set_defaults(func=service.cmd_status)
    for action, text in (('start', 'liga agora'), ('stop', 'para (até o próximo boot)'),
                         ('restart', 'reinicia'), ('enable', 'liga agora e em todo boot (voo)'),
                         ('disable', 'para e tira do boot')):
        a = ss.add_parser(action, help=text + '; sem nome, os três')
        a.add_argument('daemons', nargs='*', choices=service.DAEMONS, metavar='daemon',
                       help='ttcd, obcd, adsbd (padrão: os três)')
        a.set_defaults(func=service.cmd_action)
    lg = ss.add_parser('logs', help='o log de um daemon')
    lg.add_argument('daemon', choices=service.DAEMONS)
    lg.add_argument('-n', '--lines', type=int, default=40, help='quantas linhas (padrão 40)')
    lg.add_argument('-f', '--follow', action='store_true', help='acompanha; Ctrl-C sai')
    lg.add_argument('--journal', action='store_true', help='o journal do systemd em vez do log do daemon')
    lg.set_defaults(func=service.cmd_logs)
    ss.add_parser('install', help='make install na Pi: daemons e serviços (mantém /etc/gama)').set_defaults(
        func=service.cmd_install)

    dp = sub.add_parser('deploy', help='o burn-wire disparado na Pi pelo SSH (plano B da REG-07)',
                        description=deploy.__doc__.split('\n\n')[0])
    dp.add_argument('--yes', '-y', action='store_true', help='não pergunta antes')
    dp.set_defaults(func=deploy.cmd_deploy)

    lk = sub.add_parser('link', help='os passos do enlace (09/10): ttcd na Pi, a ground na ESP32',
                        description=link.__doc__.split('\n\n')[0])
    add_link_args(lk, kind='real')

    an = sub.add_parser('antenna', help='a antena de 1090 MHz no SDR, sem o satélite')
    ans = an.add_subparsers(dest='action', metavar='<ação>', required=True)
    ac = ans.add_parser('check', help='o dump1090 por um tempo, contra o que a OpenSky vê')
    ac.add_argument('--pi', action='store_true', help='o SDR da Pi (padrão: o deste computador)')
    ac.add_argument('--antenna', default='antena', help='um nome para a antena: cots, gama, …')
    ac.add_argument('--secs', type=int, default=60, help='duração, s (padrão 60)')
    ac.add_argument('--gain', type=float, default=49.6, help='dB (padrão 49.6, o máximo do R820T)')
    ac.add_argument('--lat', type=float, help='latitude do receptor (padrão: a FGA)')
    ac.add_argument('--lon', type=float, help='longitude do receptor (padrão: a FGA)')
    ac.add_argument('--no-reference', action='store_true', help='não pergunta à OpenSky')
    ac.set_defaults(func=antenna.cmd_check)
    acmp = ans.add_parser('compare', help='todas as verificações lado a lado')
    acmp.add_argument('dirs', nargs='*', help='pastas (padrão: docs/vv/bancada)')
    acmp.set_defaults(func=antenna.cmd_compare)

    e = sub.add_parser('esp32', help='a ESP32 da ground, no USB deste computador')
    es = e.add_subparsers(dest='action', metavar='<ação>', required=True)
    for action, text, fn in (('port', 'em que porta ela está, ou por que não aparece', esp32.cmd_port),
                             ('flash', 'grava o firmware da ground (make esp32-flash)', esp32.cmd_flash)):
        x = es.add_parser(action, help=text)
        x.add_argument('--port', help='a porta (padrão: a única ttyUSB/ttyACM)')
        x.set_defaults(func=fn)

    r = sub.add_parser('radio', help='o rádio A (RA-02) da Pi')
    rs = r.add_subparsers(dest='action', metavar='<ação>', required=True)
    rc = rs.add_parser('check', help='lê o RegVersion pelo SPI e diz o que está errado')
    rc.add_argument('--no-sync', action='store_true', help='não copia o código para a Pi antes')
    rc.set_defaults(func=doctor.cmd_radio_check)

    gr = sub.add_parser('ground', help='a página da ground com a ESP32 deste computador',
                        description=ground.__doc__.split('\n\n')[0])
    add_ground_args(gr, kind='real')

    m = sub.add_parser('mission', help='uma missão ADS-B com aviões de verdade (o SDR), operada pela página',
                       description=MISSION_DESCRIPTION)
    add_mission_args(m, kind='real')

    rp = sub.add_parser('replay', help='hardware real, aviões de um arquivo (captura da API ou sintético)',
                        description='replay: o satélite e o rádio são os de verdade; os aviões vêm de um '
                                    'arquivo no lugar do SDR. Valida o satélite, o enlace e a ground, '
                                    'não a recepção ADS-B. Os logs da Pi vão para replay-missao-*.')
    rps = rp.add_subparsers(dest='group_cmd', metavar='<comando>', required=True)
    add_mission_args(rps.add_parser('mission', help='a missão ADS-B na Pi, com uma captura no lugar do SDR',
                                    description=MISSION_DESCRIPTION), kind='replay')

    sm = sub.add_parser('sim', help='simulação: nada físico, tudo neste computador',
                        description='sim: o satélite e a ground neste computador, ligados pelo rádio UDP, '
                                    'ou uma sessão gravada na página; nenhum hardware. Serve para ver o fluxo funcionar, nunca como '
                                    'evidência de bancada. Os logs ficam em build/.')
    sms = sm.add_subparsers(dest='group_cmd', metavar='<comando>', required=True)
    add_mission_args(sms.add_parser('mission', help='a missão ADS-B no PC, operada pela página (make sim)',
                                    description=MISSION_DESCRIPTION), kind='sim')
    add_link_args(sms.add_parser('link', help='os passos do enlace com as duas pontas neste computador',
                                 description=link.__doc__.split('\n\n')[0]), kind='sim')
    add_ground_args(sms.add_parser('ground', help='a página no rádio UDP, ou uma sessão gravada (--session)',
                                   description=ground.__doc__.split('\n\n')[0]), kind='sim')
    return ap


def main(argv=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, 'func', None):
        ap.print_help()
        return 0
    try:
        return args.func(args) or 0
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
