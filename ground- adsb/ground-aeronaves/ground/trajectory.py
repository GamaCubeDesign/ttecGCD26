from .geo import distance_km, bearing_deg


def build_snapshot(observations, ttl_s):
    """Valores propagados têm idade/proveniência; observações brutas não mudam."""
    if not observations:
        return None
    now = observations[-1].event_ns
    positions = [o for o in observations if o.has_position]
    if not positions or (now - positions[-1].event_ns) / 1e9 > ttl_s:
        return None
    position = positions[-1]
    state = {'lat': position.fields['lat'], 'lon': position.fields['lon'],
             'position_age_s': (now - position.event_ns) / 1e9, 'fields': {},
             'trajectory': [{'event_ns': o.event_ns, 'lat': o.fields['lat'], 'lon': o.fields['lon'],
                             'altitude_ft': o.fields.get('altitude_ft')} for o in positions]}
    for key in ('altitude_ft', 'ground_speed_kt', 'track_deg', 'vertical_rate_fpm', 'on_ground'):
        for obs in reversed(observations):
            age = (now - obs.event_ns) / 1e9
            if age > ttl_s:
                break
            if key == 'on_ground' and obs.fields.get(key) == -1:
                # Desconhecido explícito não vira True e não reaproveita um GROUND antigo.
                break
            if obs.fields.get(key) is not None:
                state['fields'][key] = {'value': obs.fields[key], 'age_s': age,
                                       'source': 'received' if age == 0 else 'last_known'}
                break
    # Derivação de velocidade/direção apenas com posições recentes e distintas.
    for previous in reversed(positions[:-1]):
        dt = (position.event_ns - previous.event_ns) / 1e9
        if dt > ttl_s:
            break
        dist = distance_km(previous.fields['lat'], previous.fields['lon'], state['lat'], state['lon'])
        if dt >= 1 and dist >= .02:
            speed = dist / dt * 3600 / 1.852
            if speed <= 1500:
                for key, value in [('ground_speed_kt', speed), ('track_deg', bearing_deg(previous.fields['lat'], previous.fields['lon'], state['lat'], state['lon']))]:
                    state['fields'].setdefault(key, {'value': value, 'age_s': state['position_age_s'], 'source': 'derived_positions'})
            break
    return state
