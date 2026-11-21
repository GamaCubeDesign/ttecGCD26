#!/usr/bin/env python3
"""probe_pi.py — runs on the Pi and prints, as one JSON object, what gama
doctor, gama radio check and gama mission need to know about it: the clock,
the internet, what is running, the installed configuration and the wiring it
describes, the SDR, and whether radio A answers on SPI.

    python3 tools/gama/probe_pi.py [--radio-only] [--no-radio]
    python3 tools/gama/probe_pi.py --cat FILE...   {file: text or null}, for gama board

The radio check reads the SX1278's RegVersion (0x42), which is 0x12. When it
reads 0x00, it turns the Pi's pull-up on MISO and reads again: 0xff means
nothing drives MISO (radio not connected, no 3.3 V, or the MISO wire off
pin 21 — what the bench found on 20/11); 0x00 means MISO is held low. The
reset line of ttcd.conf is driven high first, as ttcd does, so a radio held
in reset by a floating line still answers. The pins go back to inputs with
their default pulls. Nothing is read while ttcd holds the radio.

Standard library plus python3-spidev and pinctrl, both on the bench Pi.
"""

import argparse
import json
import os
import re
import subprocess
import time
import urllib.request
from pathlib import Path

CONF_DIR = Path(os.environ.get('GAMA_CONF_DIR', '/etc/gama'))
BOOT_CONFIG = Path(os.environ.get('GAMA_BOOT_CONFIG', '/boot/firmware/config.txt'))
MISO_GPIO = 9
REG_VERSION = 0x42
SX1278_VERSION = 0x12

# The two wirings of radio A the bench has had (docs/relatorios/
# 2026-11-20-pinagem-bancada.html): October's and the new OBC board's.
WIRINGS = {(20, 21): 'B1', (25, 4): 'B2'}

DAEMONS = ('ttcd', 'obcd', 'adsbd', 'dump1090-fa', 'dump1090')
# Bracketed so that pgrep never matches the shell that runs it.
HELPERS = {'sbs_replay': 'tools/sbs_replay/sbs_repla[y].py',
           'pi_mission': 'tools/bench/pi_missio[n].sh'}


def sh(argv, timeout=10):
    try:
        r = subprocess.run(argv, text=True, capture_output=True, timeout=timeout)
        return r.returncode, r.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 127, ''


def read_conf(path):
    """key = value lines, comments dropped, as the daemons read them."""
    out = {}
    try:
        text = Path(path).read_text()
    except OSError:
        return None
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if '=' in line:
            k, v = line.split('=', 1)
            out[k.strip()] = v.strip()
    return out


def wiring(reset, dio0):
    """'B1', 'B2' or None for a (reset_line, dio0_line) pair."""
    try:
        return WIRINGS.get((int(reset), int(dio0)))
    except (TypeError, ValueError):
        return None


def default_pull(gpio):
    # BCM2835 reset state: GPIO0-8 pull up, the rest pull down.
    return 'pu' if gpio <= 8 else 'pd'


# ---- the radio ------------------------------------------------------------

class Hardware:
    """What the radio check touches; tests replace it."""

    def read_reg(self, spi_dev, reg):
        import spidev
        bus, cs = (int(x) for x in re.findall(r'(\d+)\.(\d+)$', spi_dev)[0])
        s = spidev.SpiDev()
        s.open(bus, cs)
        try:
            s.max_speed_hz = 500000
            s.mode = 0
            return s.xfer2([reg & 0x7f, 0])[1]
        finally:
            s.close()

    def pinctrl(self, *args):
        return sh(['pinctrl', *[str(a) for a in args]])[0] == 0


def radio_check(conf, hw=None, ttcd_running=False):
    """{'status': ..., 'value': ..., ...}. status: ok, miso_floating,
    miso_low, unexpected, busy, no_conf, no_spidev, error."""
    hw = hw or Hardware()
    if ttcd_running:
        return {'status': 'busy'}
    if conf is None:
        return {'status': 'no_conf'}
    spi_dev = conf.get('spi_dev', '/dev/spidev0.0')
    if not os.path.exists(spi_dev) and not os.environ.get('GAMA_FAKE_SPI'):
        return {'status': 'no_spidev', 'spi_dev': spi_dev}
    reset = conf.get('reset_line')
    out = {'spi_dev': spi_dev, 'reset_line': reset}
    try:
        if reset and reset.isdigit():
            hw.pinctrl('set', reset, 'op', 'dh')
            time.sleep(0.01)
        try:
            value = hw.read_reg(spi_dev, REG_VERSION)
            out['value'] = value
            if value == SX1278_VERSION:
                out['status'] = 'ok'
            elif value == 0x00:
                hw.pinctrl('set', MISO_GPIO, 'a0', 'pu')
                try:
                    pulled = hw.read_reg(spi_dev, REG_VERSION)
                finally:
                    hw.pinctrl('set', MISO_GPIO, 'a0', default_pull(MISO_GPIO))
                out['pulled_up'] = pulled
                out['status'] = 'miso_floating' if pulled == 0xff else 'miso_low' if pulled == 0 else 'unexpected'
            else:
                out['status'] = 'unexpected'
        finally:
            if reset and reset.isdigit():
                hw.pinctrl('set', reset, 'ip', default_pull(int(reset)))
    except Exception as exc:           # noqa: BLE001 — reported, not raised
        out.update(status='error', error=f'{type(exc).__name__}: {exc}')
    return out


# ---- everything else ------------------------------------------------------

def running():
    procs = {}
    for name in DAEMONS:
        code, out = sh(['pgrep', '-x', name])
        if code == 0:
            procs[name] = [int(p) for p in out.split()]
    for name, pattern in HELPERS.items():
        code, out = sh(['pgrep', '-f', pattern])
        if code == 0:
            procs[name] = [int(p) for p in out.split()]
    return procs


def services():
    out = {}
    for name in ('ttcd', 'obcd', 'adsbd'):
        code, text = sh(['systemctl', 'is-active', name])
        out[name] = text.strip() or 'unknown'
    return out


def clock():
    code, text = sh(['timedatectl', 'show', '-p', 'NTPSynchronized', '--value'])
    return {'epoch': time.time(), 'ntp_synchronized': text.strip() == 'yes' if code == 0 else None}


def internet():
    try:
        urllib.request.urlopen(urllib.request.Request('https://deb.debian.org', method='HEAD'), timeout=4)
        return True
    except Exception:                  # noqa: BLE001
        return False


def w1_lines():
    try:
        text = BOOT_CONFIG.read_text()
    except OSError:
        return None
    return [l.strip() for l in text.splitlines() if l.strip().startswith('dtoverlay=w1-gpio')]


def sdr_present():
    code, out = sh(['lsusb'])
    return '0bda:2838' in out or '0bda:2832' in out


def cat(path):
    try:
        return Path(path).read_text()
    except OSError:
        return None


def probe(radio=True, others=True, hw=None):
    result = {}
    procs = running()
    ttcd_conf = read_conf(CONF_DIR / 'ttcd.conf')
    result['ttcd_conf'] = ({k: ttcd_conf.get(k) for k in ('spi_dev', 'reset_line', 'dio0_line')}
                           if ttcd_conf is not None else None)
    if ttcd_conf is not None:
        result['wiring'] = wiring(ttcd_conf.get('reset_line'), ttcd_conf.get('dio0_line'))
    # What gama doctor compares with the board's configuration (gama board).
    result['installed'] = {d: read_conf(CONF_DIR / f'{d}.conf') for d in ('ttcd', 'obcd')}
    if others:
        result['running'] = procs
        result['services'] = services()
        result['clock'] = clock()
        result['internet'] = internet()
        result['conf_files'] = sorted(p.name for p in CONF_DIR.glob('*.conf')) if CONF_DIR.is_dir() else []
        result['w1_lines'] = w1_lines()
        result['sdr'] = sdr_present()
    if radio:
        result['radio'] = radio_check(ttcd_conf, hw=hw, ttcd_running='ttcd' in procs)
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--radio-only', action='store_true')
    ap.add_argument('--no-radio', action='store_true')
    ap.add_argument('--cat', nargs='+', metavar='FILE')
    args = ap.parse_args(argv)
    if args.cat:
        print(json.dumps({f: cat(f) for f in args.cat}))
        return 0
    print(json.dumps(probe(radio=not args.no_radio, others=not args.radio_only)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
