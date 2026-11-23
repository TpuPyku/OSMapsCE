"""The regions' outlines for the map of the PC app (the user 2026-10-29: a visual choice of the regions): each
region's source extract's .poly (openstreetmap.fr, Geofabrik - make_region.py's sources), the outer rings only,
simplified (OUTLINE_M) and rounded, east of 180 degrees kept on the east (Chukotka). The release's outlines.json:
{name: [[[lon, lat], ...], ...]}.

    python make_outlines.py <out.json>
"""
import json
import re
import sys
import urllib.error

import make_graph
import make_region

OUTLINE_M = 2000   # the simplification: ~2 km - a region's shape on a 600 px map of the CIS, ~100 KB in all


def poly_url(pbf_url):
    """An extract's .poly beside it: openstreetmap.fr's in its polygons/ tree, Geofabrik's next to the file."""
    url = re.sub(r'-latest\.osm\.pbf$', '.poly', pbf_url)
    return url.replace('download.openstreetmap.fr/extracts/', 'download.openstreetmap.fr/polygons/')


def parse_poly(text):
    """The outer rings of an Osmosis .poly (its name, then sections of 'lon lat' lines ending with END; a section
    named with '!' a hole - left out)."""
    rings, ring, hole = [], None, False
    for line in text.splitlines()[1:]:
        t = line.strip()
        if t == 'END':
            if ring is not None and not hole:
                rings.append(ring)
            ring = None
        elif ring is None and t:
            ring, hole = [], t.startswith('!')
        elif t:
            lon, lat = map(float, t.split()[:2])
            ring.append((lon, lat))
    return rings


def shape(ring):
    """A ring simplified, rounded to 0.001 degrees, past 180 degrees on the east."""
    pts = make_graph.simplify([(int(lat * 1e6), int(lon * 1e6)) for lon, lat in ring], OUTLINE_M)
    return [[round(lon / 1e6 + (360 if lon < -100e6 else 0), 3), round(lat / 1e6, 3)] for lat, lon in pts]


def outlines(regions, get):
    """{name: rings} of make_region.regions()'s {name: url or [urls]}; get(url) -> the .poly's text."""
    out = {}
    for name, urls in regions.items():
        out[name] = [shape(r) for u in (urls if isinstance(urls, list) else [urls]) for r in parse_poly(poly_text(u, get))]
    return out


def poly_urls(pbf_url):
    """Where an extract's .poly may be: beside it, then - Geofabrik's - in the region's own folder (Geofabrik moved its
    regions into folders 2026-11-23; the old .poly's redirect lost "europe/" - 404)."""
    url = poly_url(pbf_url)
    if 'download.geofabrik.de/' not in url:
        return [url]
    head, name = url.rsplit('/', 1)
    return [url, '%s/%s/%s' % (head, name[:-len('.poly')], name)]


def poly_text(pbf_url, get):
    """The first .poly of poly_urls there (a 404 tries the next; the last one's error raised)."""
    urls = poly_urls(pbf_url)
    for i, u in enumerate(urls):
        try:
            return get(u)
        except urllib.error.HTTPError as e:
            if e.code != 404 or i == len(urls) - 1:
                raise


def main(argv):
    with open(argv[0], 'w', encoding='utf-8') as f:
        json.dump(outlines(make_region.regions(), make_region.get), f, separators=(',', ':'))


if __name__ == '__main__':
    main(sys.argv[1:])
