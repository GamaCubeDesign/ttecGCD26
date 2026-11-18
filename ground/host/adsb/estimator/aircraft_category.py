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


# ---- the class inferred from kinematics (ADR-0022) ----
# The emitter category does not reach the ground: the SBS feed the satellite
# reads carries none, nor does the downlinked track record. Without it the
# origin estimate cannot tell a jet climbing out of Aeroparque from the
# heliports around it, and decides nothing (17/11: 0 origins, 5 of 6 right
# with the category). Only the "large" class is inferred, and only from
# evidence a light aircraft or a helicopter cannot produce; "light" is not:
# a helicopter at 120 kt and 2000 ft looks like one, and calling it fixed
# wing would rule out the heliports it flies to.
LARGE_SPEED_KT = 250.0          # sustained ground speed
LARGE_ALTITUDE_FT = 18000.0     # barometric
LARGE_CLIMB_FPM = 1800.0        # with LARGE_CLIMB_SPEED_KT
LARGE_CLIMB_SPEED_KT = 160.0
SUSTAINED = 2                   # observations, so one corrupt value never decides


def infer_class(history):
    """'A3' when the aircraft's own kinematics say "large", else None.

    history: the episode's observations (ground.models.Observation), any
    order. A received category is never replaced: the caller applies this
    only when none is known (missing, invalid or A0)."""
    fast = high = climbing = 0
    for o in history:
        f = o.fields
        if f.get('on_ground') == 1:
            continue
        gs, alt, vr = f.get('ground_speed_kt'), f.get('altitude_ft'), f.get('vertical_rate_fpm')
        if gs is not None and gs > LARGE_SPEED_KT:
            fast += 1
        if alt is not None and alt > LARGE_ALTITUDE_FT:
            high += 1
        if vr is not None and gs is not None and vr > LARGE_CLIMB_FPM and gs > LARGE_CLIMB_SPEED_KT:
            climbing += 1
        if max(fast, high, climbing) >= SUSTAINED:
            return 'A3'
    return None
