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
import doctor   # noqa: E402
import esp32    # noqa: E402
import link     # noqa: E402
import mission  # noqa: E402
import net      # noqa: E402
import pi       # noqa: E402
import service  # noqa: E402

EXAMPLES = """\
exemplos, na ordem de uma bancada:
  ./gama net up                     a rede da bancada (com o cabo ethernet no PC)
  ./gama doctor                     PC, rede, Pi, relógio, configuração, SDR e rádio A
  ./gama board install              a configuração da placa do OBC (obc-v2) na Pi
  ./gama radio check                o rádio A responde pelo SPI?
  ./gama pi stop                    para o que estiver segurando o rádio, o SDR e o I²C
  ./gama link ping                  10 PINGs pela ESP32; ./gama link all = os 6 passos
  ./gama antenna check --pi         o SDR da Pi ouve aviões?
  ./gama mission --secs 120 --input docs/vv/trafego-real/2026-11-17-unsam/capture_sem_categoria.ndjson
  ./gama mission --secs 120 --sdr   com aviões de verdade (ao ar livre)
  ./gama mission --pc --secs 120 --input <captura>   tudo no PC, sem hardware
  ./gama service enable             a configuração de voo: os três daemons em todo boot
  ./gama service status             cada serviço e a última linha do log
"""


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

    lk = sub.add_parser('link', help='os passos do enlace (09/10): ttcd na Pi, a ground na ESP32',
                        description=link.__doc__.split('\n\n')[0])
    lk.add_argument('step', choices=(*link.STEPS, 'all'),
                    help=('; '.join(f'{k}: {v}' for k, v in link.WHAT.items()) + '; all: os seis, em ordem').replace('%', '%%'))
    lk.add_argument('--udp', action='store_true', help='as duas pontas neste computador, sem hardware')
    lk.add_argument('--port', help='porta da ESP32 (padrão: a única ttyUSB/ttyACM)')
    lk.add_argument('--power', type=int, default=2, help='dBm, os dois rádios (padrão 2, a ~1 m)')
    lk.add_argument('--conf', help=f'o ttcd.conf na Pi (padrão {link.INSTALLED_CONF})')
    lk.add_argument('--no-build', action='store_true', help='não compila na Pi antes')
    lk.add_argument('--skip-radio-check', action='store_true', help='segue mesmo se o rádio A não responder')
    lk.set_defaults(func=link.cmd_link)

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

    m = sub.add_parser('mission', help='uma missão ADS-B pela página: satélite e página com o mesmo tempo',
                       description='Uma missão ADS-B operada pela página, num comando só: o satélite '
                                   '(na Pi, ou neste PC com --pc) e a página, com a mesma duração da '
                                   'MISSION_ADSB nos dois lados. Verifica antes (ssh, ESP32, nada '
                                   'rodando, rádio A), compila e sobe tudo; Ctrl-C encerra os dois, '
                                   'traz os logs e mostra o resumo.')
    m.add_argument('action', nargs='?', choices=['stop'],
                   help='stop: encerra uma missão que ficou rodando na Pi e traz os logs')
    m.add_argument('--secs', type=int, default=120, help='duração da MISSION_ADSB, s (padrão 120; 600 no voo)')
    src = m.add_mutually_exclusive_group()
    src.add_argument('--input', help='captura (.ndjson ou .sbs) tocada no lugar do SDR')
    src.add_argument('--sdr', action='store_true', help='o SDR de verdade (ao ar livre)')
    m.add_argument('--avr', action='store_true', help='a categoria pela porta AVR (Etapa B, ADR-0023), só nesta missão')
    m.add_argument('--pc', action='store_true', help='tudo no PC, rádio UDP, sem hardware (make sim)')
    m.add_argument('--port', help='porta da ESP32 (padrão: a única ttyUSB/ttyACM)')
    m.add_argument('--power', type=int, default=2, help='potência da ground, dBm (padrão 2)')
    m.add_argument('--tx-power', type=int, default=2, help='potência do rádio A, dBm (padrão 2)')
    m.add_argument('--web-port', type=int, default=8080, help='porta da página (padrão 8080)')
    m.add_argument('--no-build', action='store_true', help='não compila na Pi antes')
    m.add_argument('--skip-radio-check', action='store_true', help='sobe mesmo se o rádio A não responder')
    m.set_defaults(func=lambda a: mission.cmd_mission_stop(a) if a.action == 'stop' else mission.cmd_mission(a))
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
