#!/usr/bin/env python3
"""sdr_check.py — is the 1090 MHz antenna delivering aircraft? A quick check of
the SDR and its antenna with dump1090, on the PC or on the Pi, no satellite.

    sdr_check.py run [--antenna NAME] [--secs 60] [--gain 49.6] [--out DIR]
                     [--lat -15.99 --lon -48.04] [--no-reference]
    sdr_check.py compare DIR...        one line per run found under the DIRs

`run` does three things and keeps everything in one folder, by default
docs/vv/bancada/<date>/antena/<antenna>-<time>/ (make sdr-check):

  1. the reference: with internet, the aircraft the OpenSky Network sees
     around the receiver right now (referencia.json). A run that hears
     nothing proves little unless something was there to hear;
  2. dump1090 for --secs seconds, at the given gain, Mode A/C on. Its output
     is kept compressed (dump1090.txt.gz); aircraft.json is copied every 5 s
     (aeronaves/), because dump1090 keeps only the latest;
  3. the summary (resumo.txt, resumo.json): the statistics blocks dump1090
     prints, the aircraft heard, and a verdict.

What decides is messages with a correct CRC and aircraft with a position. The
preamble and Mode A/C counts are not evidence on their own: at full gain the
receiver's own noise produces false preambles (18 CRC-valid messages out of
159 025 preambles in the first run, 18/11). There is no measurement with the
antenna removed: an unterminated input is not a meaningful reference.

Standard library only. Only the receiver's position (rounded box) leaves this
machine, to the OpenSky Network.
"""

import argparse
import datetime
import gzip
import json
import math
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = (-15.99, -48.04)          # the bench: UnB Gama (FGA), Brasília
OPENSKY = 'https://opensky-network.org/api/states/all?lamin={:.1f}&lomin={:.1f}&lamax={:.1f}&lomax={:.1f}'
REFERENCE_KM = 150.0              # reference aircraft within this range
NEAR_KM = 100.0                   # one this close, airborne, should be heard
SNAPSHOT_S = 5.0
EARTH_KM = 6371.0088


def distance_km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    h = (math.sin((p2 - p1) / 2) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2)
    return 2 * EARTH_KM * math.asin(math.sqrt(min(1.0, h)))


# ---- dump1090 ----

def find_dump1090(given=None):
    """(path, the flag that picks the SDR): the given program, else
    dump1090-fa (the Pi) or dump1090 (the PC's package), the same FlightAware
    program; the flag changed from --device-index to --device in version 10."""
    for name in ([given] if given else ['dump1090-fa', 'dump1090']):
        path = shutil.which(name) or (name if Path(name).is_file() else None)
        if path:
            text = subprocess.run([path, '--help'], capture_output=True, text=True).stdout
            return path, '--device-index' if '--device-index' in text else '--device'
    return None, None


STAT_FIELDS = {
    'samples_processed': r'(\d+) samples processed',
    'samples_dropped': r'(\d+) samples dropped',
    'modeac': r'(\d+) Mode A/C messages received',
    'preambles': r'(\d+) Mode-S message preambles received',
    'bad': r'(\d+) with bad message format or invalid CRC',
    'unknown_icao': r'(\d+) with unrecognized ICAO address',
    'accepted': r'(\d+) accepted with correct CRC',
    'noise_dbfs': r'(-?[\d.]+) dBFS noise power',
    'signal_dbfs': r'(-?[\d.]+) dBFS mean signal power',
    'peak_dbfs': r'(-?[\d.]+) dBFS peak signal power',
    'strong': r'(\d+) messages with signal power above -3dBFS',
    'usable': r'(\d+) total usable messages',
    'airborne_positions': r'(\d+) airborne position messages received',
    'tracks': r'(\d+) unique aircraft tracks',
}


def parse_stats(text):
    """The `Statistics:` blocks of dump1090's output (--stats-every), each a
    dict of STAT_FIELDS. Messages printed between blocks are ignored."""
    blocks = []
    for chunk in re.split(r'^Statistics: ', text, flags=re.M)[1:]:
        chunk = chunk.split('\nCPU load', 1)[0]
        b = {}
        for key, pattern in STAT_FIELDS.items():
            m = re.search(pattern, chunk)
            if m:
                b[key] = float(m.group(1)) if key.endswith('_dbfs') else int(m.group(1))
        if b:
            blocks.append(b)
    return blocks


def total_stats(blocks):
    """Counts summed over the blocks; noise and mean signal averaged; peak the
    highest."""
    if not blocks:
        return {}
    out = {}
    for key in STAT_FIELDS:
        values = [b[key] for b in blocks if key in b]
        if not values:
            continue
        if key in ('noise_dbfs', 'signal_dbfs'):
            out[key] = round(sum(values) / len(values), 1)
        elif key == 'peak_dbfs':
            out[key] = max(values)
        else:
            out[key] = sum(values)
    out['blocks'] = len(blocks)
    return out


def aircraft_seen(snapshots, receiver):
    """Every aircraft in the aircraft.json snapshots: messages, best RSSI,
    whether a position came, and its farthest distance from the receiver."""
    seen = {}
    for snap in snapshots:
        for a in snap.get('aircraft', []):
            s = seen.setdefault(a['hex'], {'hex': a['hex'], 'flight': '', 'messages': 0,
                                           'rssi_max': None, 'position': False, 'max_km': None})
            s['messages'] = max(s['messages'], int(a.get('messages', 0)))
            s['flight'] = (a.get('flight') or s['flight']).strip()
            if a.get('rssi') is not None:
                s['rssi_max'] = a['rssi'] if s['rssi_max'] is None else max(s['rssi_max'], a['rssi'])
            if a.get('lat') is not None and a.get('lon') is not None:
                s['position'] = True
                d = round(distance_km(receiver[0], receiver[1], a['lat'], a['lon']), 1)
                s['max_km'] = d if s['max_km'] is None else max(s['max_km'], d)
    return sorted(seen.values(), key=lambda s: -s['messages'])


# ---- the reference ----

def reference(receiver, timeout=15):
    """The airborne aircraft the OpenSky Network sees within REFERENCE_KM, or
    None without an answer (no internet, rate limit)."""
    lat, lon = receiver
    dlat = REFERENCE_KM / 111.0
    dlon = dlat / max(0.2, math.cos(math.radians(lat)))
    url = OPENSKY.format(lat - dlat, lon - dlon, lat + dlat, lon + dlon)
    req = urllib.request.Request(url, headers={'User-Agent': 'ttec-sdr-check/1'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            states = json.load(r).get('states') or []
    except (OSError, ValueError):
        return None
    out = []
    for s in states:
        if s[5] is None or s[6] is None:
            continue
        d = distance_km(lat, lon, s[6], s[5])
        if d <= REFERENCE_KM:
            out.append({'hex': s[0].upper(), 'flight': (s[1] or '').strip(), 'km': round(d, 1),
                        'on_ground': bool(s[8]), 'alt_ft': round((s[7] or 0) * 3.281)})
    return sorted(out, key=lambda a: a['km'])


# ---- the verdict ----

def verdict(stats, aircraft, ref):
    """(code, sentence). What proves the antenna is CRC-valid messages and
    positions; a silent run fails only if the reference had someone near."""
    with_pos = [a for a in aircraft if a['position']]
    accepted = stats.get('accepted', 0)
    if not stats:
        return 'ERRO', 'o dump1090 não imprimiu estatísticas: o SDR está conectado e livre? (na Pi: make adsbd-stop)'
    if with_pos:
        far = max(a['max_km'] for a in with_pos)
        return 'APROVADA', f'{len(with_pos)} aeronave(s) com posição, a até {far:.0f} km'
    if accepted or aircraft:
        return 'RECEBE', (f'{accepted} mensagens com CRC certo de {len(aircraft)} aeronave(s), mas nenhuma posição: '
                          'repita por mais tempo ou num lugar mais aberto')
    if ref is None:
        return 'INCONCLUSIVO', 'nenhuma mensagem e sem referência (sem internet): repita com rede para saber se havia aviões'
    near = [a for a in ref if not a['on_ground'] and a['km'] <= NEAR_KM]
    if near:
        return 'REPROVADA', (f'nenhuma mensagem, mas a OpenSky via {len(near)} aeronave(s) no ar a até {NEAR_KM:.0f} km '
                             f'(a mais próxima a {near[0]["km"]:.0f} km): confira a antena, o cabo e o conector')
    return 'INCONCLUSIVO', f'nenhuma mensagem, e a OpenSky não via aviões no ar a até {NEAR_KM:.0f} km'


def summary_text(r):
    st = r['stats']
    lines = [f"sdr_check · antena {r['antenna']} · {r['started']} · {r['secs']} s · ganho {r['gain']} dB",
             f"  {r['dump1090']} · receptor em {r['receiver'][0]}, {r['receiver'][1]}"]
    if st:
        lines.append(f"  ruído {st.get('noise_dbfs')} dBFS · sinal médio {st.get('signal_dbfs')} dBFS · pico "
                     f"{st.get('peak_dbfs')} dBFS · amostras descartadas {st.get('samples_dropped', 0)}")
        lines.append(f"  mensagens com CRC certo {st.get('accepted', 0)} · úteis {st.get('usable', 0)} · "
                     f"posições {st.get('airborne_positions', 0)} · preâmbulos {st.get('preambles', 0)} · "
                     f"Mode A/C {st.get('modeac', 0)}")
    lines.append(f"  aeronaves ouvidas: {len(r['aircraft'])} (com posição: {sum(a['position'] for a in r['aircraft'])})")
    for a in r['aircraft']:
        pos = f", posição a até {a['max_km']} km" if a['position'] else ''
        rssi = f", RSSI {a['rssi_max']} dBFS" if a['rssi_max'] is not None else ''
        lines.append(f"    {a['hex']} {a['flight']:<8} {a['messages']:5d} mensagens{rssi}{pos}")
    if r['reference'] is None:
        lines.append('  referência: sem resposta da OpenSky (sem internet?)')
    else:
        lines.append(f"  referência (OpenSky, até {REFERENCE_KM:.0f} km): {len(r['reference'])} aeronave(s)")
        heard = {a['hex'] for a in r['aircraft']}
        for a in r['reference'][:10]:
            where = 'no solo' if a['on_ground'] else f"{a['alt_ft']} ft"
            mark = '  ← ouvida' if a['hex'] in heard else ''
            lines.append(f"    {a['hex']} {a['flight']:<8} {a['km']:6.1f} km  {where}{mark}")
    lines.append(f"  VEREDITO: {r['verdict'][0]} — {r['verdict'][1]}")
    return '\n'.join(lines) + '\n'


# ---- the commands ----

def run(args):
    path, device_flag = find_dump1090(args.dump1090)
    if path is None:
        sys.exit('sdr_check: dump1090 not found (dump1090-fa on the Pi; the dump1090-fa package on the PC)')
    started = datetime.datetime.now()
    out = Path(args.out) / f"{args.antenna}-{started.strftime('%H%M%S')}"
    (out / 'aeronaves').mkdir(parents=True, exist_ok=True)
    json_dir = out / 'json'
    json_dir.mkdir(exist_ok=True)
    receiver = (args.lat, args.lon)
    print(f'sdr_check: antena {args.antenna}, {args.secs} s, em {out}', flush=True)

    ref = None if args.no_reference else reference(receiver)
    if ref is None:
        print('  referência: sem resposta da OpenSky' + ('' if args.no_reference else ' (sem internet?)'))
    else:
        print(f'  referência: {len(ref)} aeronave(s) da OpenSky a até {REFERENCE_KM:.0f} km')
        (out / 'referencia.json').write_text(json.dumps(ref, indent=1))

    cmd = [path, device_flag, '0', '--gain', str(args.gain), '--modeac', '--stats-every', '10',
           '--write-json', str(json_dir), '--write-json-every', str(int(SNAPSHOT_S)),
           '--lat', str(args.lat), '--lon', str(args.lon)]
    log = out / 'dump1090.txt'
    snapshots = []
    with open(log, 'w') as f:
        proc = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT)
        end = time.monotonic() + args.secs
        try:
            while time.monotonic() < end and proc.poll() is None:
                time.sleep(min(SNAPSHOT_S, max(0.0, end - time.monotonic())))
                src = json_dir / 'aircraft.json'
                try:
                    snap = json.loads(src.read_text())
                except (OSError, ValueError):
                    continue
                snapshots.append(snap)
                (out / 'aeronaves' / f"aircraft-{datetime.datetime.now().strftime('%H%M%S')}.json").write_text(json.dumps(snap))
                print(f"  {int(args.secs - max(0, end - time.monotonic())):3d} s: "
                      f"{len(snap.get('aircraft', []))} aeronave(s) no momento", flush=True)
        except KeyboardInterrupt:
            print('  interrompido')
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    text = log.read_text(errors='replace')
    with open(log, 'rb') as src, gzip.open(str(log) + '.gz', 'wb') as dst:
        shutil.copyfileobj(src, dst)
    log.unlink()
    shutil.rmtree(json_dir, ignore_errors=True)       # dump1090's own files: the copies are kept

    stats = total_stats(parse_stats(text))
    result = {'antenna': args.antenna, 'started': started.isoformat(timespec='seconds'), 'secs': args.secs,
              'gain': args.gain, 'dump1090': ' '.join(cmd[:1]), 'receiver': list(receiver), 'stats': stats,
              'aircraft': aircraft_seen(snapshots, receiver), 'reference': ref}
    result['verdict'] = list(verdict(stats, result['aircraft'], ref))
    (out / 'resumo.json').write_text(json.dumps(result, indent=1, ensure_ascii=False))
    summary = summary_text(result)
    (out / 'resumo.txt').write_text(summary)
    print(summary, end='')
    return 0 if result['verdict'][0] in ('APROVADA', 'RECEBE', 'INCONCLUSIVO') else 1


def compare(args):
    rows = []
    for d in args.dirs:
        for p in sorted(Path(d).rglob('resumo.json')):
            r = json.loads(p.read_text())
            st = r.get('stats', {})
            rows.append((r['started'], r['antenna'], r['secs'], st.get('noise_dbfs'), st.get('signal_dbfs'),
                         st.get('accepted', 0), len(r['aircraft']), sum(a['position'] for a in r['aircraft']),
                         max((a['max_km'] or 0 for a in r['aircraft'] if a['position']), default=0),
                         r['verdict'][0]))
    if not rows:
        sys.exit('sdr_check: no resumo.json under ' + ' '.join(args.dirs))
    print(f"{'início':19s} {'antena':10s} {'s':>4s} {'ruído':>6s} {'sinal':>6s} {'CRC ok':>7s} "
          f"{'aeron.':>6s} {'c/ pos':>6s} {'alcance':>8s}  veredito")
    for row in sorted(rows):
        print(f"{row[0]:19s} {row[1]:10s} {row[2]:4d} {row[3] if row[3] is not None else '—':>6} "
              f"{row[4] if row[4] is not None else '—':>6} {row[5]:7d} {row[6]:6d} {row[7]:6d} "
              f"{row[8]:6.0f} km  {row[9]}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run', help='one check of the SDR and its antenna')
    r.add_argument('--antenna', default='antena', help='a name for the antenna: cots, gama, ...')
    r.add_argument('--secs', type=int, default=60)
    r.add_argument('--gain', type=float, default=49.6, help='dB; 49.6 is the R820T maximum')
    r.add_argument('--out', default=str(ROOT / 'docs/vv/bancada' / datetime.date.today().isoformat() / 'antena'))
    r.add_argument('--lat', type=float, default=BENCH[0])
    r.add_argument('--lon', type=float, default=BENCH[1])
    r.add_argument('--no-reference', action='store_true', help='do not ask the OpenSky Network')
    r.add_argument('--dump1090', help='the program to run (default: dump1090-fa, else dump1090)')
    c = sub.add_parser('compare', help='one line per run under the given folders')
    c.add_argument('dirs', nargs='+')
    args = ap.parse_args()
    if args.cmd == 'run':
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', args.antenna) or args.secs <= 0:
            ap.error('--antenna: letters, digits, . _ - only; --secs must be positive')
        sys.exit(run(args))
    sys.exit(compare(args))


if __name__ == '__main__':
    main()
