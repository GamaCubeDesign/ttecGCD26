#!/usr/bin/env python3
"""A missão simulada da cadeia B de 16/11: 20 aeronaves em 600 s em torno de SBBR.

6 chegadas e 6 saídas em SBBR, 8 em cruzeiro, com a classe Flight de
ground/host/adsb/validation/simular_trajetorias.py. Todas da categoria A3.
A geometria é sempre a mesma; só o rx_epoch_ns muda com a hora da geração.

    python3 gerar_sim20.py SAIDA.ndjson
"""
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / 'ground/host'))
from adsb.validation import simular_trajetorias as s  # noqa: E402
from adsb.estimator.config import Config  # noqa: E402


def flights():
    with sqlite3.connect(Config().reference_db.resolve().as_uri() + '?mode=ro', uri=True) as db:
        lat, lon, elev = db.execute(
            "SELECT latitude_deg,longitude_deg,elevation_ft FROM airports WHERE ident='SBBR'").fetchone()
    out = []

    def add(mode, start, dur, la, lo, hd, dist):
        n = len(out) + 1
        out.append(s.Flight('E4%04X' % n, 'GAM%03d' % n, mode, start, dur, la, lo, elev, hd, dist,
                            category='A3'))
    for k in range(6):
        add('arrival', 5 * k, 540, lat, lon, (30 + 60 * k) % 360, 60 + 5 * k)
        add('departure', 5 * k + 2, 540, lat, lon, (60 * k) % 360, 55 + 5 * k)
    for k in range(8):
        o_lat, o_lon = s.advance(lat, lon, 45 * k, 90)
        add('cruise', 3 * k, 560, o_lat, o_lon, (45 * k + 100) % 360, 120)
    return out


# Ímpares de E40001 a E4000B chegam a SBBR; pares até E4000C saem dela.
KIND = {**{'E4%04X' % n: 'chegada' for n in range(1, 13, 2)},
        **{'E4%04X' % n: 'saída' for n in range(2, 13, 2)},
        **{'E4%04X' % n: 'cruzeiro' for n in range(13, 21)}}

if __name__ == '__main__':
    out = Path(sys.argv[1])
    out.unlink(missing_ok=True)
    print('mensagens:', s.generate(out, flights(), duration=600, interval=1, speed=100))
