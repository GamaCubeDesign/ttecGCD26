"""Geometria esférica de cabeceiras; rumo verdadeiro, distâncias em km."""

import math
from .geo import EARTH_KM, bearing_deg, distance_km, angle_delta


def advance(lat, lon, bearing, km):
    p, l, b = map(math.radians, (lat, lon, bearing))
    d = km / EARTH_KM
    p2 = math.asin(math.sin(p) * math.cos(d) + math.cos(p) * math.sin(d) * math.cos(b))
    l2 = l + math.atan2(
        math.sin(b) * math.sin(d) * math.cos(p),
        math.cos(d) - math.sin(p) * math.sin(p2),
    )
    return math.degrees(p2), (math.degrees(l2) + 180) % 360 - 180


def evidence(snapshot, runways, elevation, origin=False):
    fields = snapshot["fields"]
    track = fields.get("track_deg", {}).get("value")
    altitude = fields.get("altitude_ft", {}).get("value")
    if track is None:
        return None
    best = None
    for runway in runways:
        for end, other in [("le", "he"), ("he", "le")]:
            lat, lon = (
                runway.get(end + "_latitude_deg"),
                runway.get(end + "_longitude_deg"),
            )
            if lat is None or lon is None:
                continue
            heading = runway.get(end + "_heading_degT")
            if heading is None:
                olat, olon = (
                    runway.get(other + "_latitude_deg"),
                    runway.get(other + "_longitude_deg"),
                )
                if olat is None or olon is None:
                    continue
                heading = bearing_deg(lat, lon, olat, olon)
            # OurAirports endpoint coordinates are shifted by displaced threshold.
            lat, lon = advance(
                lat,
                lon,
                heading,
                (runway.get(end + "_displaced_threshold_ft") or 0) * 0.0003048,
            )
            distance = distance_km(lat, lon, snapshot["lat"], snapshot["lon"])
            if distance > 30:
                continue
            delta = math.radians(
                bearing_deg(lat, lon, snapshot["lat"], snapshot["lon"]) - heading
            )
            cross = (
                abs(
                    math.asin(
                        max(-1, min(1, math.sin(distance / EARTH_KM) * math.sin(delta)))
                    )
                )
                * EARTH_KM
            )
            along = (
                math.atan2(
                    math.sin(distance / EARTH_KM) * math.cos(delta),
                    math.cos(distance / EARTH_KM),
                )
                * EARTH_KM
            )
            if (origin and along < 0) or (not origin and along > 0.3):
                continue
            error = angle_delta(track, heading)
            score = math.exp(-0.5 * (cross / 1.5) ** 2 - 0.5 * (error / 20) ** 2)
            elev = runway.get(end + "_elevation_ft")
            elev = elevation if elev is None else elev
            height = (
                altitude - elev if altitude is not None and elev is not None else None
            )
            approach = (
                not origin
                and height is not None
                and 0 <= height <= 3500
                and 0.3 <= distance <= 20
                and cross <= 1.5
                and error <= 25
            )
            item = {
                "score": score,
                "runway": str(runway.get(end + "_ident") or end),
                "cross_track_km": round(cross, 3),
                "along_track_km": round(along, 3),
                "threshold_distance_km": round(distance, 3),
                "direction_error_deg": round(error, 2),
                "height_above_threshold_ft": height,
                "approach_compatible": approach,
            }
            if best is None or score > best["score"]:
                best = item
    return best
