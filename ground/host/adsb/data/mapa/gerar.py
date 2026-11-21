#!/usr/bin/env python3
"""Builds the map's detail layer (mapa.json) for the ground's page: the urban
areas' outlines, the cities' names and the province lines of the region the
basemap covers (../pares/geografia/america_latina.geojson), from Natural Earth
1:10m (public domain). The page has no network at the competition, so the
layer is a file in the repository; this script only regenerates it.

    python3 gerar.py DIR      # DIR holds the three Natural Earth GeoJSON files

The sources and their hashes are in fonte.json. Coordinates are rounded to
0.001 degree (~100 m), finer than the page ever draws a city.
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASEMAP = os.path.join(HERE, "..", "pares", "geografia", "america_latina.geojson")
OUT = os.path.join(HERE, "mapa.json")
SOURCES = {
    "urban": "ne_10m_urban_areas.geojson",
    "places": "ne_10m_populated_places_simple.geojson",
    "admin1": "ne_10m_admin_1_states_provinces_lines.geojson",
}
# The basemap's countries, as Natural Earth's ADM0_A3 for the province lines.
ADM0 = {"ARG", "BOL", "BRA", "CHL", "COL", "CRI", "CUB", "DOM", "ECU", "SLV", "GTM", "HTI",
        "HND", "MEX", "NIC", "PAN", "PRY", "PER", "URY", "VEN", "PRI"}


def bbox_of_basemap():
    geo = json.load(open(BASEMAP))
    xs, ys = [], []
    for f in geo["features"]:
        g = f["geometry"]
        polys = [g["coordinates"]] if g["type"] == "Polygon" else g["coordinates"]
        for poly in polys:
            for ring in poly:
                for lon, lat in ring:
                    xs.append(lon)
                    ys.append(lat)
    return min(xs), min(ys), max(xs), max(ys)


def inside(lon, lat, box):
    return box[0] <= lon <= box[2] and box[1] <= lat <= box[3]


def ring(coords):
    out = []
    for lon, lat in coords:
        p = [round(lon, 3), round(lat, 3)]
        if not out or out[-1] != p:
            out.append(p)
    return out


def main(src):
    box = bbox_of_basemap()
    load = {k: json.load(open(os.path.join(src, v))) for k, v in SOURCES.items()}

    # Urban areas: the outer rings, kept when their first point is in the box.
    urban = []
    for f in load["urban"]["features"]:
        g = f["geometry"]
        polys = [g["coordinates"]] if g["type"] == "Polygon" else g["coordinates"]
        for poly in polys:
            r = ring(poly[0])
            if len(r) >= 4 and inside(r[0][0], r[0][1], box):
                urban.append(r)

    # Cities: name, position, population, and Natural Earth's own rank of
    # how far out a label still belongs on the map (min_zoom).
    places = []
    for f in load["places"]["features"]:
        p = f["properties"]
        lon, lat = f["geometry"]["coordinates"]
        if inside(lon, lat, box):
            places.append([p["name"], round(lat, 3), round(lon, 3), int(p["pop_max"] or 0),
                           float(p["min_zoom"] or 0)])
    places.sort(key=lambda x: -x[3])

    # Province (state) lines of the basemap's countries.
    admin1 = []
    for f in load["admin1"]["features"]:
        if f["properties"].get("ADM0_A3") not in ADM0:
            continue
        g = f["geometry"]
        lines = [g["coordinates"]] if g["type"] == "LineString" else g["coordinates"]
        for line in lines:
            r = ring(line)
            if len(r) >= 2:
                admin1.append(r)

    with open(OUT, "w") as f:
        json.dump({"urban": urban, "places": places, "admin1": admin1}, f,
                  ensure_ascii=False, separators=(",", ":"))

    meta = {
        "provider": "Natural Earth",
        "release": "v5.1.2",
        "url": "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/v5.1.2/geojson/",
        "license": "public domain",
        "datasets": {k: {"file": v, "sha256": hashlib.sha256(
            open(os.path.join(src, v), "rb").read()).hexdigest()} for k, v in SOURCES.items()},
        "bbox": [round(x, 3) for x in box],
        "counts": {"urban": len(urban), "places": len(places), "admin1": len(admin1)},
    }
    with open(os.path.join(HERE, "fonte.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print(f"mapa.json: {os.path.getsize(OUT)} bytes, {meta['counts']}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
