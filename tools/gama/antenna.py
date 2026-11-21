"""gama antenna: is the 1090 MHz antenna delivering aircraft? —
tools/bench/sdr_check.py, on this machine or, with --pi, on the Pi.

    gama antenna check [--pi] [--antenna cots] [--secs 60] [--gain 49.6]
                       [--lat -15.99 --lon -48.04] [--no-reference]
    gama antenna compare [DIR...]      every check side by side

With --pi, nothing may hold the SDR (adsbd, dump1090: ./gama pi stop), and
the folder takes this PC's date, not the Pi's: without NTP the Pi's clock
put the checks of 20/11 in docs/vv/bancada/2026-11-16/. The logs come back
when it ends.
"""

import datetime
import shlex
import subprocess
import sys

import common as c

SDR_CHECK = 'tools/bench/sdr_check.py'


def out_dir(today=None):
    return f'docs/vv/bancada/{(today or datetime.date.today()).isoformat()}/antena'


def run_args(args, today=None):
    a = ['run', '--antenna', args.antenna, '--secs', str(args.secs), '--gain', str(args.gain),
         '--out', out_dir(today)]
    if args.lat is not None:
        a += ['--lat', str(args.lat)]
    if args.lon is not None:
        a += ['--lon', str(args.lon)]
    if args.no_reference:
        a.append('--no-reference')
    return a


def sdr_busy(probe):
    run = (probe or {}).get('running') or {}
    svc = [k for k, v in ((probe or {}).get('services') or {}).items() if v == 'active' and k == 'adsbd']
    return sorted({k for k in run if k in ('adsbd', 'dump1090-fa', 'dump1090')} | {f'{s} (serviço)' for s in svc})


def cmd_check(args, pi=None, call=subprocess.call):
    if args.secs <= 0:
        c.fail('--secs precisa ser positivo')
        return 2
    sub = run_args(args)
    if not args.pi or c.on_pi():
        c.say(f'o SDR deste computador por {args.secs} s; a pasta: {out_dir()}/')
        try:
            return call([sys.executable, str(c.ROOT / SDR_CHECK), *sub], cwd=c.ROOT)
        except KeyboardInterrupt:
            return 130
    pi = pi or c.Pi()
    if not c.reachable_pi(pi) or not pi.sync():
        return 1
    probe = pi.probe('--no-radio')
    if probe is None:
        c.fail('não consegui rodar o probe na Pi')
        return 1
    busy = sdr_busy(probe)
    if busy:
        c.fail('o SDR da Pi está ocupado: ' + ', '.join(busy), './gama pi stop')
        return 1
    if not probe.get('sdr'):
        c.fail('o SDR não aparece no USB da Pi', 'NESDR na porta USB (a do meio), pelo adaptador OTG')
        return 1
    if probe.get('internet') is False and not args.no_reference:
        c.warn('a Pi está sem internet: sem a referência da OpenSky',
               'ligue o cabo ethernet no PC, ou --no-reference')
    c.say(f'o SDR da Pi por {args.secs} s; a pasta: {out_dir()}/ (data deste PC)')
    command = 'python3 ' + SDR_CHECK + ' ' + ' '.join(shlex.quote(x) for x in sub)
    try:
        status = pi.run(command, tty=sys.stdin.isatty(), capture=False).returncode
    except KeyboardInterrupt:
        status = 130
    got = pi.fetch()
    if got:
        c.ok(f'{len(got)} arquivo(s) trazido(s) da Pi para docs/vv/bancada/')
    return status


def cmd_compare(args, call=subprocess.call):
    dirs = args.dirs or ['docs/vv/bancada']
    return call([sys.executable, str(c.ROOT / SDR_CHECK), 'compare', *dirs], cwd=c.ROOT)
