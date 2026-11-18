"""Fusão heurística por extremidade, com abstenção e persistência temporal.

As notas são suporte limitado [0, 1], não probabilidades calibradas. Não
normalizamos pelo melhor candidato: uma lista fraca deve continuar fraca.
"""
from copy import deepcopy


def fusion_context(state):
    return deepcopy({key: state.get(key) for key in (
        'flight_state', 'data_quality', 'vertical_estimate', 'airport_method',
        'origin_candidates', 'destination_candidates', 'airport_summary',
        'tracking_episode', 'aircraft_category', 'snapshot')})


def support(value):
    return min(1., max(0., float(value or 0)))


class CombinedEstimator:
    def __init__(self):
        self.key = None
        self.memory = {}
        self.last_stamp = None

    def update(self, context, pairs, version, event_ns):
        context = context or {}
        phase = context.get('flight_state') or 'UNKNOWN'
        snapshot = context.get('snapshot') or {}
        trajectory = snapshot.get('trajectory') or []
        stamp = trajectory[-1]['event_ns'] if trajectory else event_ns
        key = (context.get('tracking_episode'), (context.get('aircraft_category') or {}).get('code'))
        if key != self.key or (self.last_stamp is not None and stamp-self.last_stamp > 60e9):
            self.memory.clear()
            self.last_stamp = None
        self.key = key
        fresh = self.last_stamp is None or stamp > self.last_stamp
        if fresh:
            self.last_stamp = stamp
        valid = (context.get('data_quality') or {}).get('usable', False) and phase not in ('GROUND', 'UNKNOWN')
        valid = valid and not (context.get('vertical_estimate') or {}).get('conflict', False)
        candidates = pairs.get('candidates', []) if pairs.get('status') == 'CANDIDATES' and valid else []
        straight = bool(candidates) and candidates[0].get('evidence', {}).get('direction_alignment', 0) >= .95
        out = {'source_version': version, 'source_event_ns': event_ns, 'phase': phase,
               'score_kind': 'heuristic_support_not_probability', 'confidence': 'BAIXA',
               'reason': 'Pesos por fase; estabilidade de 3 análises e 20 s; notas não calibradas.'}
        for kind in ('origin', 'destination'):
            decision = (context.get('airport_method') or {}).get(kind, {})
            relevant = phase == 'CLIMB' if kind == 'origin' else phase in ('DESCENT', 'APPROACH')
            local = {}
            if valid and relevant:
                local = {c['ident']: support(c.get('score')) for c in context.get(kind+'_candidates') or []}
                # Preferir a evidência temporal quando disponível.
                for c in decision.get('ranking', []):
                    local[c['airport']] = support(c.get('compatibility'))
            historical = (context.get('airport_summary') or {}).get('takeoff') if kind == 'origin' else None
            if valid and not local and historical and historical.get('status') == 'ESTIMATED':
                airport = historical.get('airport') or {}
                if airport.get('ident'):
                    local[airport['ident']] = support(airport.get('score'))
            selected = decision.get('candidate') or decision.get('best_candidate') or {}
            ev = selected.get('evidence', {})
            height = ev.get('initial_height_ft' if kind == 'origin' else 'height_above_airport_ft')
            near = ev.get('distance_km', float('inf')) <= 50 and height is not None and -300 <= height <= (6000 if kind == 'origin' else 10000)
            wl = .8 if relevant and near else .6 if phase in ('CLIMB', 'DESCENT', 'APPROACH') else .3 if phase == 'LEVEL' and straight else .5
            wp = 1-wl
            geometric = {}
            for c in candidates:
                ident = c[kind]['ident']
                geometric[ident] = max(geometric.get(ident, 0), support(c.get('score')))
            # Falha ou ausência de um método não é uma evidência contrária.
            if not geometric: wl, wp = 1., 0.
            elif not local: wl, wp = 0., 1.
            local_top = max(local, key=local.get) if local else None
            pair_top = max(geometric, key=geometric.get) if geometric else None
            agreement = local_top is not None and local_top == pair_top
            ranked = []
            for ident in local.keys() | geometric.keys():
                score = wl*local.get(ident, 0)+wp*geometric.get(ident, 0)
                # Concordância correlacionada: bônus pequeno e limitado.
                if agreement and ident == local_top: score = min(1., score+.05)
                ranked.append({'ident': ident, 'score': round(score, 6),
                               'local_support': local.get(ident, 0), 'pair_support': geometric.get(ident, 0)})
            ranked.sort(key=lambda c: (-c['score'], c['ident']))
            best = ranked[0] if ranked else None
            margin = best['score']-ranked[1]['score'] if len(ranked)>1 else best['score'] if best else 0
            enough = bool(best and best['score'] >= .6 and margin >= .08 and valid)
            if phase == 'LEVEL' and not straight and geometric: enough = False
            memory = self.memory.setdefault(kind, {})
            if not enough:
                memory.clear()
            elif fresh:
                contender = best['ident']
                incumbent = memory.get('chosen')
                scores = {c['ident']: c['score'] for c in ranked}
                if incumbent and contender != incumbent and best['score']-scores.get(incumbent, 0) < .12:
                    contender = incumbent
                if memory.get('pending') != contender:
                    memory.update(pending=contender, since=stamp, count=0)
                memory['count'] = memory.get('count', 0)+1
                if memory['count'] >= 3 and stamp-memory['since'] >= 20e9:
                    memory['chosen'] = contender
                if incumbent and memory.get('chosen') == incumbent and scores.get(incumbent, 0) < .6:
                    memory.pop('chosen', None)
            chosen = memory.get('chosen') if enough else None
            out[kind] = {'ident': chosen, 'status': 'ESTIMATED' if chosen else 'UNKNOWN',
                         'ranking': ranked, 'margin': round(margin, 6), 'agreement': agreement,
                         'weights': {'local': wl, 'pairs': wp},
                         'historical_local_origin': bool(historical and not relevant),
                         'reason': 'Hipótese estável' if chosen else 'Aguardando estabilidade' if enough else 'Evidência insuficiente ou candidatos próximos'}
        origin, destination = out['origin']['ident'], out['destination']['ident']
        supported_pair = any(c['origin']['ident']==origin and c['destination']['ident']==destination for c in candidates)
        out['route'] = {'origin': origin, 'destination': destination} if origin and destination and origin != destination and supported_pair else None
        out['status'] = 'ESTIMATED' if out['route'] else 'PARTIAL' if origin or destination else 'UNKNOWN'
        if origin and destination and not out['route']:
            out['reason'] = 'Extremidades inferidas separadamente; o par completo não foi sustentado.'
        return out
