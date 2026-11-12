"""Conservative facility compatibility from the received ADS-B emitter category.

Category is not aircraft performance, ownership or commercial/private operation.
The multipliers below are initial ranking heuristics, not probabilities or runway
requirements. A4/A6 depend on the protocol edition and stay neutral here.
"""
import re


FIXED_WING = frozenset(('A1', 'A2', 'A3', 'A5'))
LABELS = {
    'A0': 'Categoria não informada',
    'A1': 'Aeronave leve', 'A2': 'Aeronave de pequeno porte',
    'A3': 'Aeronave de grande porte', 'A5': 'Aeronave pesada',
    'A7': 'Helicóptero / rotorcraft',
}
SIZE_FACTORS = {
    'A2': {'small_airport': .95},
    'A3': {'small_airport': .85, 'medium_airport': .95},
    'A5': {'small_airport': .75, 'medium_airport': .90},
}
# A heliport is retained for an unknown category, but rotorcraft capability has
# not been observed. This weak prior disappears as soon as A7 is received.
UNCONFIRMED_ROTORCRAFT_FACTOR = .90


def normalize_category(value):
    # An integer 7 alone does not identify A7: the category set is required.
    if isinstance(value, str):
        value = value.strip().upper()
        if re.fullmatch(r'[A-D][0-7]', value):
            return value
    return None


def describe_category(snapshot):
    field = (snapshot or {}).get('fields', {}).get('category', {})
    code = normalize_category(field.get('value'))
    profile = 'fixed_wing' if code in FIXED_WING else 'rotorcraft' if code == 'A7' else 'unknown'
    return {'code': code, 'profile': profile,
            'label': LABELS.get(code, 'Categoria sem regra de compatibilidade' if code else 'Categoria indisponível'),
            'source': field.get('source', 'unavailable'), 'age_s': field.get('age_s')}


def airport_compatibility(category, airport_type):
    code = normalize_category(category)
    if code in FIXED_WING and airport_type == 'heliport':
        return {'allowed': False, 'factor': 0.0, 'reason': 'ASA_FIXA_EM_HELIPORTO'}
    if code != 'A7' and airport_type == 'heliport':
        return {'allowed': True, 'factor': UNCONFIRMED_ROTORCRAFT_FACTOR,
                'reason': 'CAPACIDADE_ROTORCRAFT_NAO_INFORMADA'}
    factor = SIZE_FACTORS.get(code, {}).get(airport_type, 1.0)
    reason = ('PREFERENCIA_HEURISTICA_DE_PORTE' if factor < 1 else
              'ROTORCRAFT_ADMITE_AEROPORTO_E_HELIPORTO' if code == 'A7' else
              'TIPO_COMPATIVEL' if code in FIXED_WING else 'CATEGORIA_SEM_RESTRICAO')
    return {'allowed': True, 'factor': factor, 'reason': reason}
