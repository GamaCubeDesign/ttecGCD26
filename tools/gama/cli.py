#!/usr/bin/env python3
"""gama — operar a bancada e o satélite.

O make compila, testa e analisa (make help); o gama opera: a rede da
bancada, a Pi, o diagnóstico, o rádio e a missão. Ele sabe se está no PC ou
na Pi e faz o ssh sozinho. PI_HOST vem do local.mk, como no Makefile.
Comandos de 21/11 (fase 1); docs/relatorios/2026-11-17-comandos-principais.html.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import doctor   # noqa: E402
import mission  # noqa: E402
import net      # noqa: E402
import pi       # noqa: E402

EXAMPLES = """\
exemplos:
  ./gama net up                     a rede da bancada (com o cabo ethernet no PC)
  ./gama doctor                     PC, rede, Pi, relógio, configuração, SDR e rádio A
  ./gama radio check                o rádio A responde pelo SPI?
  ./gama pi stop                    para o que estiver segurando o rádio, o SDR e o I²C
  ./gama mission --secs 120 --input docs/vv/trafego-real/2026-11-17-unsam/capture_sem_categoria.ndjson
  ./gama mission --secs 120 --sdr   com aviões de verdade (ao ar livre)
  ./gama mission --pc --secs 120 --input <captura>   tudo no PC, sem hardware
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
