"""Características derivadas; nunca altera as observações originais."""
import math
from statistics import median

from .geo import distance_km


def regression(samples):
    """Regressão robusta Theil–Sen, limitada a 30 amostras (tempo, valor)."""
    samples = sorted(dict(samples).items())
    if len(samples) > 30:
        samples = [samples[round(i * (len(samples)-1)/29)] for i in range(30)]
    if len(samples) < 3 or samples[-1][0]-samples[0][0] < 10:
        return None
    t0 = samples[0][0]
    points = [(t-t0, v) for t, v in samples]
    slopes = [(b[1]-a[1])/(b[0]-a[0]) for i,a in enumerate(points)
              for b in points[i+1:] if b[0]-a[0] >= 5]
    if not slopes:
        return None
    slope = median(slopes)
    intercept = median([v-slope*t for t,v in points])
    residual = median([abs(v-intercept-slope*t) for t,v in points])
    return {'slope': slope, 'residual': residual, 'count': len(points),
            'span_s': points[-1][0], 'last_time_s': samples[-1][0]}


def vertical_features(history, now, window_s=90):
    recent = [o for o in history if now-o.event_ns <= window_s*1e9]
    # Times relative to now avoid precision loss with nanosecond epoch integers.
    fit = regression([((o.event_ns-now)/1e9, o.fields['altitude_ft']) for o in recent
                      if o.fields.get('altitude_ft') is not None])
    received = {o.event_ns:o.fields['vertical_rate_fpm'] for o in recent
                if now-o.event_ns <= 30e9 and o.fields.get('vertical_rate_fpm') is not None}
    newest_received = max(received) if received else None
    measured = median([v for t,v in received.items() if newest_received-t<=10e9]) if received else None
    short_fit = regression([((o.event_ns-now)/1e9,o.fields['altitude_ft']) for o in recent
                            if now-o.event_ns<=30e9 and o.fields.get('altitude_ft') is not None])
    # Compare recent rates, not a 90-second average against a recent maneuver.
    if short_fit and short_fit['residual']<=200:
        fit=short_fit
    derived = fit['slope']*60 if fit and fit['last_time_s'] >= -30 and fit['residual'] <= 200 else None
    disagreement = measured is not None and derived is not None and abs(measured-derived)>600
    comparable = fit is not None and fit['span_s']<=45 and fit['last_time_s']>=-10 and newest_received is not None and now-newest_received<=10e9
    opposite = measured is not None and derived is not None and measured*derived<0 and min(abs(measured),abs(derived))>300
    conflict = bool(comparable and opposite)
    value = None if conflict else measured if measured is not None else derived
    if value is not None and abs(value)>12000:
        value=None
    evidence_ns = newest_received if received else (now+int(fit['last_time_s']*1e9) if fit else None)
    return {'value_fpm':value,'received_fpm':measured,'derived_fpm':derived,
            'source':'conflict' if conflict else 'received' if measured is not None else 'altitude_regression' if derived is not None else 'unavailable',
            'conflict':conflict,'magnitude_warning':bool(disagreement),'fit':fit,'evidence_ns':evidence_ns}


def quality(snapshot):
    if snapshot is None:
        return {'usable': False, 'score': 0.0, 'reason': 'SEM_POSICAO_RECENTE'}
    points = snapshot['trajectory']
    last = points[-1]['event_ns']
    points = [p for p in points if last-p['event_ns'] <= 120e9]
    unique = list({p['event_ns']:p for p in points}.values())
    span = (unique[-1]['event_ns']-unique[0]['event_ns'])/1e9
    gaps = [(b['event_ns']-a['event_ns'])/1e9 for a,b in zip(unique,unique[1:])]
    jumps = sum(distance_km(a['lat'],a['lon'],b['lat'],b['lon']) > 2 + dt*0.65
                for a,b,dt in zip(unique,unique[1:],gaps))
    conflicting = len({(p['event_ns'],p['lat'],p['lon']) for p in points})>len(unique)
    usable = not conflicting and len(unique)>=3 and span>=20 and not jumps and max(gaps, default=0)<=60
    score = min(1,len(unique)/6)*min(1,span/40)*math.exp(-snapshot['position_age_s']/60)
    if not usable:
        score = 0.0
    reason = ('POSICOES_CONFLITANTES' if conflicting else 'SALTO_DE_POSICAO' if jumps
              else 'POUCAS_POSICOES' if len(unique)<3 else 'TRECHO_OBSERVADO_CURTO' if span<20
              else 'LACUNA_ENTRE_POSICOES' if max(gaps,default=0)>60 else 'DADOS_UTILIZAVEIS')
    return {'usable': usable, 'score': round(score,4), 'positions':len(unique),
            'span_s':span, 'max_gap_s':max(gaps,default=0), 'position_jumps':jumps, 'conflicting_positions':conflicting,
            'reason':reason}


def approach_context(history, now):
    """Contexto de até dez minutos, separado da fase vertical instantânea."""
    rows = [o for o in history if now-o.event_ns<=600e9]
    altitude = [((o.event_ns-now)/1e9,o.fields['altitude_ft']) for o in rows
                if o.fields.get('altitude_ft') is not None]
    speed = [((o.event_ns-now)/1e9,o.fields['ground_speed_kt']) for o in rows
             if o.fields.get('ground_speed_kt') is not None]
    fit = regression(altitude)
    def change(samples):
        if len(samples)<3 or samples[-1][0]-samples[0][0]<20:
            return None
        start,end=samples[0][0],samples[-1][0]
        return median([v for t,v in samples if t<=start+10])-median([v for t,v in samples if t>=end-10])
    drop,slowing = change(altitude),change(speed)
    recent = [v for t,v in altitude if t>=-30]
    rebound = recent and min(v for _,v in altitude) < median(recent)-1000
    return {'span_s':(rows[-1].event_ns-rows[0].event_ns)/1e9 if rows else 0,
            'altitude_drop_ft':drop,'speed_drop_kt':slowing,
            'descent_context':bool(fit and fit['slope']*60 < -300 and drop is not None and drop>=1000 and not rebound),
            'long_vertical_rate_fpm':fit['slope']*60 if fit else None,
            'altitude_rebound':bool(rebound)}
