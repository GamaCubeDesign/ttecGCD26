import json
import math
import re

from .models import Observation

LIMITS = {
    'lat': (-90, 90), 'lon': (-180, 180),
    'ground_speed_kt': (0, 5000), 'track_deg': (0, 360),
    'altitude_ft': (-3000, 200000), 'vertical_rate_fpm': (-100000, 100000),
}


def parse_line(raw: str, received_ns: int) -> Observation:
    def nonfinite(value):
        raise ValueError(f'Número JSON inválido: {value}')

    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'Campo JSON duplicado: {key}')
            result[key] = value
        return result

    data = json.loads(raw, parse_constant=nonfinite, object_pairs_hook=unique_object)
    if not isinstance(data, dict):
        raise ValueError('Esperado um objeto JSON por linha')
    icao = data.get('icao')
    if not isinstance(icao, str) or not re.fullmatch(r'[0-9a-fA-F]{6}', icao):
        raise ValueError('icao deve conter seis dígitos hexadecimais')
    stamp = data.get('rx_epoch_ns')
    if stamp is not None and (type(stamp) is not int or not 0 < stamp < 2**63):
        raise ValueError('rx_epoch_ns deve ser inteiro positivo de 64 bits')
    for key, (low, high) in LIMITS.items():
        value = data.get(key)
        if value is not None and (type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high):
            raise ValueError(f'{key} inválido')
    on_ground = data.get('on_ground')
    if on_ground is not None and (type(on_ground) not in (bool, int) or on_ground not in (-1, 0, 1)):
        raise ValueError('on_ground deve ser -1 (desconhecido), 0, 1, false ou true')
    return Observation(icao.upper(), stamp if stamp is not None else received_ns,
                       received_ns, 'rx_epoch_ns' if stamp is not None else 'ground_received_at', data, raw)
