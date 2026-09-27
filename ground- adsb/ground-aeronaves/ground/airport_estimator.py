import bisect
import math
import sqlite3

from .temporal import regression
from .runway import evidence as runway_evidence

from .geo import EARTH_KM, angle_delta, bearing_deg, distance_km


# Initial heuristics for direct-to-airport descent, not aircraft limits.
LEVEL_TREND_WEIGHT = .15
DESCENT_FULL_FEASIBILITY_FPM = 2500.0
DESCENT_ZERO_FEASIBILITY_FPM = 5000.0


class AirportEstimator:
    def __init__(self, config):
        self.config = config
        # Arquivo original aberto em modo somente leitura. Índice latitude em memória.
        con = sqlite3.connect(config.reference_db.resolve().as_uri() + '?mode=ro', uri=True)
        con.row_factory = sqlite3.Row
        try:
            marks = ','.join('?' for _ in config.airport_types)
            self.airports = [dict(r) for r in con.execute(f'''SELECT id, ident, icao_code, name,
                type, latitude_deg, longitude_deg, elevation_ft FROM airports
                WHERE type IN ({marks}) AND latitude_deg BETWEEN -90 AND 90
                AND longitude_deg BETWEEN -180 AND 180 ORDER BY latitude_deg''', config.airport_types)]
            self.runways = {}
            if con.execute("SELECT 1 FROM sqlite_master WHERE name='runways'").fetchone():
                for row in con.execute('SELECT * FROM runways WHERE closed=0'):
                    self.runways.setdefault(row['airport_ref'], []).append(dict(row))
        finally:
            con.close()
        self.latitudes = [a['latitude_deg'] for a in self.airports]
        if not self.airports:
            raise ValueError('Nenhum aeroporto disponível para os tipos selecionados')

    def estimate(self, snapshot, flight_state, origin=False):
        if snapshot is None or flight_state == 'GROUND' or (origin and flight_state != 'CLIMB'):
            return []
        fields = {k:v['value'] for k,v in snapshot['fields'].items()}
        lat,lon = snapshot['lat'],snapshot['lon']
        radius = self.config.radius_km
        delta = math.degrees(radius/EARTH_KM)
        lo,hi = bisect.bisect_left(self.latitudes,lat-delta),bisect.bisect_right(self.latitudes,lat+delta)
        positions = snapshot['trajectory']
        latest = positions[-1]['event_ns']
        long_positions = [p for p in positions if latest-p['event_ns']<=600e9]
        positions = [p for p in positions if latest-p['event_ns']<=120e9]
        context = snapshot.get('approach_context',{})
        terminal = not origin and context.get('descent_context',False)
        vertical = snapshot.get('vertical',{})
        vr = vertical.get('value_fpm', fields.get('vertical_rate_fpm'))
        phase = flight_state in ('CLIMB','DESCENT','APPROACH')
        candidates = []
        for airport in self.airports[lo:hi]:
            alat,alon = airport['latitude_deg'],airport['longitude_deg']
            distance = distance_km(lat,lon,alat,alon)
            if distance>radius:
                continue
            evidence = {'distance_km':round(distance,3)}
            anchor = snapshot.get('takeoff_anchor') if origin else None
            if anchor:
                anchor_distance = distance_km(anchor['lat'],anchor['lon'],alat,alon)
                evidence['takeoff_anchor_distance_km'] = round(anchor_distance,3)
                # Initial heuristics: favor the last observed ground position.
                parts = {'takeoff_anchor':(math.exp(-anchor_distance/10),.45),
                         'distance':(math.exp(-distance/120),.05)}
            else:
                parts = {'distance':(math.exp(-distance/(120 if phase else radius)),.15)}
            track = fields.get('track_deg')
            if track is not None:
                direction = (track+180)%360 if origin else track
                error = angle_delta(direction,bearing_deg(lat,lon,alat,alon))
                parts['direction'] = (math.exp(-.5*(error/90)**2),.05)
                evidence['direction_error_deg'] = round(error,2)
            fit = regression([((p['event_ns']-latest)/1e9,
                distance_km(p['lat'],p['lon'],alat,alon)) for p in positions])
            if fit:
                favorable = fit['slope'] if origin else -fit['slope']
                parts['trend'] = ((1+math.tanh(favorable/.05))/2,
                                  .4 if phase else LEVEL_TREND_WEIGHT if flight_state=='LEVEL' else .6)
                evidence['distance_slope_km_s'] = round(fit['slope'],5)
                evidence['trend_span_s'] = fit['span_s']
            altitude,elev = fields.get('altitude_ft'),airport['elevation_ft']
            speed = fields.get('ground_speed_kt')
            descent_feasibility = None
            if not origin and altitude is not None and elev is not None and speed is not None and speed>0:
                height_ft = max(0,altitude-elev)
                time_min = (distance/1.852)/speed*60
                if time_min>0:
                    required_descent_fpm = height_ft/time_min
                    descent_feasibility = max(0,min(1,
                        (DESCENT_ZERO_FEASIBILITY_FPM-required_descent_fpm)/
                        (DESCENT_ZERO_FEASIBILITY_FPM-DESCENT_FULL_FEASIBILITY_FPM)))
                    evidence['required_descent_fpm'] = round(required_descent_fpm,1)
                else:
                    # No horizontal travel time: positive height cannot be lost
                    # in this simplified model. Keep JSON finite at distance 0.
                    descent_feasibility = 0.0 if height_ft>0 else 1.0
                    evidence['required_descent_fpm'] = None if height_ft>0 else 0.0
                evidence['descent_feasibility'] = round(descent_feasibility,3)
            proper = vr is not None and (vr>150 if origin else vr< -150)
            if altitude is not None and elev is not None and (proper or terminal):
                height = altitude-elev
                evidence['height_above_airport_ft'] = height
                # Broad plausibility envelope; does not extrapolate a constant descent.
                vertical_score = math.exp(-max(0,height-(6000 if origin else 4000))/8000) if height>=-300 else 0
                parts['vertical'] = (vertical_score,.25 if phase else .05)
                first_alt = next((p['altitude_ft'] for p in positions if p.get('altitude_ft') is not None),None)
                evidence['initial_height_ft'] = first_alt-elev if first_alt is not None else None
            runway = runway_evidence(snapshot,self.runways.get(airport['id'],[]),elev,origin)
            if runway and (origin or flight_state=='APPROACH'):
                evidence['runway'] = runway
                parts['runway'] = (runway['score'],.5 if flight_state=='APPROACH' else .15 if phase else 0)
            if terminal and len(long_positions)>=3:
                first=long_positions[0]
                initial=distance_km(first['lat'],first['lon'],alat,alon)
                # A brief lateral maneuver near the minimum is not a lost approach.
                sampled=long_positions[::max(1,len(long_positions)//60)]+[long_positions[-1]]
                minimum=min(distance_km(p['lat'],p['lon'],alat,alon) for p in sampled)
                retreat=max(0,distance-minimum-5)
                convergence=max(0,min(1,(initial-distance)/max(initial,1)))*math.exp(-retreat/20)
                evidence.update(initial_distance_km=round(initial,3),minimum_distance_km=round(minimum,3),
                    cumulative_closure_km=round(initial-distance,3),retreat_from_minimum_km=round(distance-minimum,3),
                    approach_context=context)
                # Only positive approach evidence. Runway misalignment outside
                # final approach and the instantaneous track are not vetoes.
                parts={'distance':(math.exp(-distance/25),.45),'cumulative_convergence':(convergence,.30)}
                if altitude is not None and elev is not None:
                    height=altitude-elev
                    evidence['height_above_airport_ft']=height
                    parts['vertical']=(math.exp(-max(0,height-4000)/8000) if height>=-300 else 0,.15)
                if context.get('speed_drop_kt') is not None:
                    parts['deceleration']=(max(0,min(1,context['speed_drop_kt']/80)),.10)
                if runway and runway['approach_compatible']:
                    parts['runway']=(runway['score'],.10)
            total = sum(w for _,w in parts.values())
            expected = (1.1 if 'runway' in parts else 1) if terminal else 1.35 if flight_state=='APPROACH' else 1 if phase else .85
            score = sum(v*w for v,w in parts.values())/total
            # Direct distance is not remaining flight-path length during a
            # terminal maneuver. Preserve the established descent-context path.
            if not origin and flight_state=='LEVEL' and not terminal and descent_feasibility is not None:
                score *= descent_feasibility
            candidates.append({'airport_id':airport['id'],'ident':airport['ident'],
                'icao_code':airport['icao_code'],'name':airport['name'],'score':round(score,4),
                'evidence_coverage':round(min(1,total/expected),3),
                'components':{k:round(v,4) for k,(v,w) in parts.items() if w>0},'evidence':evidence})
        candidates.sort(key=lambda c:(-c['score'],c['evidence']['distance_km'],c['airport_id']))
        # Keep runners-up for ambiguity checks even with --top 1.
        return candidates
