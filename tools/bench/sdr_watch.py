#!/usr/bin/env python3
"""sdr_watch.py — the SDR left on, in the terminal: every aircraft dump1090
receives, the active ones and those already seen, for as long as it runs.
For validating the reception with an antenna on the PC, no satellite.

    sdr_watch.py [--antenna NAME] [--gain 49.6] [--active 60] [--secs 0]
                 [--lat -15.99 --lon -48.04] [--out DIR] [--dump1090 PATH]

dump1090 runs with --write-json; aircraft.json is read every second and
merged into one table of everything heard since the start. "Active" is an
aircraft heard within --active seconds (60, as adsbd's track expiry,
flight/adsbd/core.c); the rest are "seen". If dump1090 exits (the SDR pulled
out, the USB reset) it is started again after RESTART_S, and the table
survives.

Everything stays in one folder, by default
docs/vv/bancada/<date>/antena/vigia-<antenna>-<time>/:
  dump1090.txt       dump1090's own output (compressed at the end)
  posicoes.ndjson    one line per new position: t, hex, flight, lat, lon, alt, km, rssi
  vistas.json        the table, rewritten every SAVE_S (survives a crash)
  resumo.txt         the summary, written when it stops (Ctrl+C or --secs)

dump1090 keeps an aircraft's message count only while it tracks it; the
count here adds up across those restarts. Standard library only; nothing
leaves this machine.
"""

import argparse
import datetime
import gzip
import json
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from sdr_check import BENCH, ROOT, distance_km, find_dump1090  # noqa: E402

POLL_S = 1.0
SAVE_S = 10.0
RESTART_S = 5.0
FT_PER_M = 3.281


# ---- the table ----

class Table:
    """Every aircraft heard, keyed by ICAO address. Pure: the time comes in."""

    def __init__(self, receiver):
        self.receiver = receiver
        self.aircraft = {}

    def merge(self, snap, now):
        """Merges one aircraft.json taken at `now` (epoch s). Returns the new
        positions, for the log."""
        fresh = []
        for a in snap.get('aircraft', []):
            hexid = a.get('hex', '').lower()
            if not hexid or hexid.startswith('~'):      # TIS-B / non-ICAO: not an aircraft heard
                continue
            seen = float(a.get('seen', 0.0))
            heard = now - seen
            s = self.aircraft.get(hexid)
            if s is None:
                s = self.aircraft[hexid] = {
                    'hex': hexid, 'flight': '', 'category': '', 'first': heard, 'last': heard,
                    'messages': 0, '_msg_base': 0, '_msg_last': 0, 'rssi_max': None, 'rssi': None,
                    'alt_ft': None, 'gs_kt': None, 'trk': None, 'lat': None, 'lon': None,
                    'km': None, 'max_km': None, 'positions': 0, '_pos_at': None}
            s['last'] = max(s['last'], heard)
            msgs = int(a.get('messages', 0))
            if msgs < s['_msg_last']:                   # dump1090 dropped and re-acquired it
                s['_msg_base'] += s['_msg_last']
            s['_msg_last'] = msgs
            s['messages'] = s['_msg_base'] + msgs
            if (a.get('flight') or '').strip():
                s['flight'] = a['flight'].strip()
            if a.get('category'):
                s['category'] = a['category']
            if a.get('rssi') is not None and seen < 2 * POLL_S + 1:
                s['rssi'] = a['rssi']
                s['rssi_max'] = a['rssi'] if s['rssi_max'] is None else max(s['rssi_max'], a['rssi'])
            alt = a.get('alt_baro', a.get('altitude'))
            if isinstance(alt, (int, float)):
                s['alt_ft'] = int(alt)
            elif alt == 'ground':
                s['alt_ft'] = 0
            if a.get('gs', a.get('speed')) is not None:
                s['gs_kt'] = round(a.get('gs', a.get('speed')))
            if a.get('track') is not None:
                s['trk'] = round(a['track'])
            if a.get('lat') is not None and a.get('lon') is not None:
                pos_at = now - float(a.get('seen_pos', seen))
                if s['_pos_at'] is None or pos_at > s['_pos_at'] + 0.05:
                    s['_pos_at'] = pos_at
                    s['lat'], s['lon'] = a['lat'], a['lon']
                    km = round(distance_km(self.receiver[0], self.receiver[1], a['lat'], a['lon']), 1)
                    s['km'] = km
                    s['max_km'] = km if s['max_km'] is None else max(s['max_km'], km)
                    s['positions'] += 1
                    fresh.append({'t': round(pos_at, 1), 'hex': hexid, 'flight': s['flight'],
                                  'lat': a['lat'], 'lon': a['lon'], 'alt_ft': s['alt_ft'],
                                  'km': km, 'rssi': a.get('rssi')})
        return fresh

    def split(self, now, active_s):
        """(active, seen): active by signal, strongest first; seen by last heard,
        most recent first."""
        rows = list(self.aircraft.values())
        active = [s for s in rows if now - s['last'] <= active_s]
        past = [s for s in rows if now - s['last'] > active_s]
        active.sort(key=lambda s: -(s['rssi'] if s['rssi'] is not None else -99))
        past.sort(key=lambda s: -s['last'])
        return active, past

    def public(self):
        return sorted(({k: v for k, v in s.items() if not k.startswith('_')} for s in self.aircraft.values()),
                      key=lambda s: s['first'])


# ---- the screen ----

def fmt_age(s):
    s = max(0, int(round(s)))
    if s < 60:
        return f'{s} s'
    if s < 3600:
        return f'{s // 60} min {s % 60:02d} s'
    return f'{s // 3600} h {(s % 3600) // 60:02d} min'


def dash(v, fmt='{}'):
    return '—' if v is None or v == '' else fmt.format(v)


HEAD = f"{'ICAO':6s} {'VOO':8s} {'CAT':3s} {'ALT ft':>6s} {'VEL kt':>6s} {'RUMO':>4s} {'DIST km':>7s} " \
       f"{'MÁX km':>6s} {'RSSI':>5s} {'MSGS':>6s} {'POS':>4s}  "


def row(s, now, last_col):
    return (f"{s['hex']:6s} {dash(s['flight']):8s} {dash(s['category']):3s} {dash(s['alt_ft']):>6s} "
            f"{dash(s['gs_kt']):>6s} {dash(s['trk']):>4s} {dash(s['km'], '{:.1f}'):>7s} "
            f"{dash(s['max_km'], '{:.0f}'):>6s} {dash(s['rssi'] if last_col == 'visto' else s['rssi_max'], '{:.1f}'):>5s} "
            f"{s['messages']:6d} {s['positions']:4d}  "
            + (f"há {fmt_age(now - s['last'])}" if last_col == 'visto'
               else f"{datetime.datetime.fromtimestamp(s['last']).strftime('%H:%M:%S')} "
                    f"(há {fmt_age(now - s['last'])}, ouvido por {fmt_age(s['last'] - s['first'])})"))


def stats_line(stats):
    """One line from dump1090-fa's stats.json: the last minute and the total."""
    if not stats:
        return 'estatísticas do dump1090: ainda nenhuma'
    m1 = stats.get('last1min', {})
    tot = stats.get('total', {})
    loc = m1.get('local', {})
    acc_1 = sum(loc.get('accepted', []) or [0])
    acc_t = sum(tot.get('local', {}).get('accepted', []) or [0])
    parts = [f"CRC ok: {acc_t} no total, {acc_1} no último minuto"]
    for key, name in (('noise', 'ruído'), ('signal', 'sinal médio'), ('peak_signal', 'pico')):
        if loc.get(key) is not None:
            parts.append(f'{name} {loc[key]:.1f} dBFS')
    dropped = loc.get('samples_dropped')
    if dropped:
        parts.append(f'[AVISO] {dropped} amostras descartadas no último minuto (USB?)')
    return ' · '.join(parts)


def screen(table, now, args, started, stats, status, width, height):
    active, past = table.split(now, args.active)
    with_pos = [s for s in table.aircraft.values() if s['positions']]
    far = max((s['max_km'] for s in with_pos), default=None)
    lines = [f"sdr_watch · antena {args.antenna} · ganho {args.gain} dB · rodando há {fmt_age(now - started)} "
             f"· {status}",
             '  ' + stats_line(stats),
             f"  aeronaves: {len(active)} ativas · {len(past)} vistas antes · {len(table.aircraft)} no total "
             f"· {len(with_pos)} com posição" + (f" · a mais distante a {far:.0f} km" if far else ''),
             '',
             f"ATIVAS (ouvidas nos últimos {args.active} s)",
             HEAD + 'VISTO']
    room = max(3, height - len(lines) - 6)
    n_act = min(len(active), max(3, room // 2 if past else room))
    lines += [row(s, now, 'visto') for s in active[:n_act]] or ['  (nenhuma agora)']
    if len(active) > n_act:
        lines.append(f'  … e mais {len(active) - n_act}')
    lines += ['', 'JÁ VISTAS (inativas, a mais recente primeiro)', HEAD.replace(' RSSI', 'RSSIm') + 'ÚLTIMA VEZ']
    n_past = max(0, height - len(lines) - 2)
    lines += [row(s, now, 'ultima') for s in past[:n_past]] or ['  (nenhuma ainda)']
    if len(past) > n_past:
        lines.append(f'  … e mais {len(past) - n_past} (todas em vistas.json)')
    return '\n'.join(line[:width] for line in lines[:height - 1])


# ---- the summary ----

def summary_text(table, args, started, ended, stats, restarts):
    rows = table.public()
    with_pos = [s for s in rows if s['positions']]
    lines = [f"sdr_watch · antena {args.antenna} · ganho {args.gain} dB · receptor em {args.lat}, {args.lon}",
             f"  de {datetime.datetime.fromtimestamp(started):%Y-%m-%d %H:%M:%S} a "
             f"{datetime.datetime.fromtimestamp(ended):%H:%M:%S} ({fmt_age(ended - started)}), "
             f"dump1090 reiniciado {restarts} vez(es)",
             '  ' + stats_line(stats),
             f"  aeronaves ouvidas: {len(rows)} · com posição: {len(with_pos)}"
             + (f" · a mais distante a {max(s['max_km'] for s in with_pos):.0f} km" if with_pos else ''),
             '', HEAD.replace(' RSSI', 'RSSIm') + 'PRIMEIRA–ÚLTIMA']
    for s in rows:
        s = dict(s, rssi=s['rssi_max'])
        lines.append(row(s, s['last'], 'visto').rsplit('há', 1)[0]
                     + f"{datetime.datetime.fromtimestamp(s['first']):%H:%M:%S}–"
                     f"{datetime.datetime.fromtimestamp(s['last']):%H:%M:%S}")
    if not rows:
        lines.append('  nenhuma: confira a antena, o cabo e o ganho; ./gama antenna check compara com a OpenSky')
    return '\n'.join(lines) + '\n'


# ---- the loop ----

class Dump1090:
    def __init__(self, cmd, log):
        self.cmd, self.log, self.proc, self.f = cmd, log, None, None

    def start(self):
        self.f = open(self.log, 'a')
        self.f.write(f'\n=== sdr_watch: dump1090 iniciado {datetime.datetime.now():%H:%M:%S} ===\n')
        self.f.flush()
        self.proc = subprocess.Popen(self.cmd, stdout=self.f, stderr=subprocess.STDOUT)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.alive():
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.f:
            self.f.close()
            self.f = None

    def last_line(self):
        try:
            lines = [x for x in self.log.read_text(errors='replace').splitlines() if x.strip()]
        except OSError:
            return ''
        return lines[-1] if lines else ''


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--antenna', default='antena', help='a name for the antenna: cots, gama, ...')
    ap.add_argument('--gain', type=float, default=49.6, help='dB; 49.6 is the R820T maximum')
    ap.add_argument('--active', type=int, default=60, help='heard within this many seconds = active')
    ap.add_argument('--secs', type=float, default=0, help='stop after this long (0: until Ctrl+C)')
    ap.add_argument('--lat', type=float, default=BENCH[0])
    ap.add_argument('--lon', type=float, default=BENCH[1])
    ap.add_argument('--out', default=str(ROOT / 'docs/vv/bancada' / datetime.date.today().isoformat() / 'antena'))
    ap.add_argument('--dump1090', help='the program to run (default: dump1090-fa, else dump1090)')
    ap.add_argument('--device', default='0', help='SDR index or serial')
    ap.add_argument('--plain', action='store_true', help='no screen redraw: one status line per SAVE_S')
    args = ap.parse_args()
    if not args.antenna.replace('-', '').replace('_', '').isalnum():
        sys.exit('sdr_watch: --antenna: letters, digits, - and _ only')

    path, device_flag = find_dump1090(args.dump1090)
    if path is None:
        sys.exit('sdr_watch: dump1090 not found (dump1090-fa on the Pi; the dump1090-fa package on the PC)')
    started = time.time()
    out = Path(args.out) / f"vigia-{args.antenna}-{datetime.datetime.now():%H%M%S}"
    json_dir = out / 'json'
    json_dir.mkdir(parents=True, exist_ok=True)
    cmd = [path, device_flag, args.device, '--gain', str(args.gain), '--quiet',
           '--write-json', str(json_dir), '--write-json-every', '1',
           '--lat', str(args.lat), '--lon', str(args.lon)]
    log = out / 'dump1090.txt'
    dump = Dump1090(cmd, log)
    table = Table((args.lat, args.lon))
    positions = open(out / 'posicoes.ndjson', 'a')
    stats, restarts, down_at, saved_at = None, 0, None, 0.0
    tty = sys.stdout.isatty() and not args.plain
    print(f'[INFO] sdr_watch: {path}, pasta {out}', flush=True)
    dump.start()
    try:
        while True:
            now = time.time()
            if args.secs and now - started >= args.secs:
                break
            if dump.alive():
                status = f'dump1090 ok, reinícios {restarts}'
                snap = read_json(json_dir / 'aircraft.json')
                if snap:
                    for p in table.merge(snap, now):
                        positions.write(json.dumps(p, ensure_ascii=False) + '\n')
                    positions.flush()
                stats = read_json(json_dir / 'stats.json') or stats
            else:
                down_at = down_at or now
                status = (f'[ERRO] dump1090 parou ({dump.last_line()[:80]}); '
                          f'reinicio em {max(0, RESTART_S - (now - down_at)):.0f} s')
                if now - down_at >= RESTART_S:
                    dump.stop()
                    restarts += 1
                    down_at = None
                    dump.start()
            if now - saved_at >= SAVE_S:
                (out / 'vistas.json').write_text(json.dumps(table.public(), indent=1, ensure_ascii=False))
                saved_at = now
                if not tty:
                    active, past = table.split(now, args.active)
                    print(f'{datetime.datetime.now():%H:%M:%S} {status} · {len(active)} ativas · '
                          f'{len(past)} vistas antes · {sum(1 for s in table.aircraft.values() if s["positions"])} '
                          'com posição', flush=True)
            if tty:
                size = shutil.get_terminal_size((120, 40))
                sys.stdout.write('\x1b[H\x1b[2J' + screen(table, now, args, started, stats, status,
                                                          size.columns, size.lines) + '\n')
                sys.stdout.flush()
            time.sleep(POLL_S)
    except KeyboardInterrupt:
        pass
    finally:
        dump.stop()
        positions.close()
        ended = time.time()
        (out / 'vistas.json').write_text(json.dumps(table.public(), indent=1, ensure_ascii=False))
        text = summary_text(table, args, started, ended, stats, restarts)
        (out / 'resumo.txt').write_text(text)
        if log.exists():
            with open(log, 'rb') as src, gzip.open(str(log) + '.gz', 'wb') as dst:
                shutil.copyfileobj(src, dst)
            log.unlink()
        shutil.rmtree(json_dir, ignore_errors=True)
        if tty:
            sys.stdout.write('\x1b[H\x1b[2J')
        print(text, end='')
        print(f'[OK] tudo em {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
