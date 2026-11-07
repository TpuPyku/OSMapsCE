"""make_parklines.py - Moscow's paid street parking as lines with their tariffs (the user 2026-11-07), for OSMapsCE's
parking.lay: data.mos.ru, set 623.

    python make_parklines.py data/moscow_parking.lay               (downloads; the key: .mosapi in the repo's root)
    python make_parklines.py <out> <features.json>

The result is kept in the repo: make_region.py puts it in the region's package, so the maps' build needs no key. Run it
when the tariffs change (a couple of times a year). The file: as make_layers.py's, a cell's piece of a line by its
middle, 20 bytes a piece: its start (1e-6 degrees), to its end, a car's price - tens of rubles an hour - on weekdays and
at the weekend, before 8, from 8 to 21, from 21. Holidays are not told apart: the unit would not know them.
"""
import gzip
import json
import os
import struct
import sys
import urllib.parse
import urllib.request

import make_layers

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = 1
kCar = 'Легковой автомобиль'
kDays = {'будни': 0, 'выходные дни': 3}
kHours = {'00:00-08:00': (0,), '08:00-21:00': (1,), '21:00-23:59': (2,), 'круглосуточно': (0, 1, 2)}
kUrl = 'https://apidata.mos.ru/v1/datasets/623/features?'


def tens(rub):
    return min((rub + 9) // 10, 255)


def prices_of(tariffs):
    """Two rates of one time (a zone's two tariffs in the set): the higher, never less than it may cost."""
    p = [0] * 6
    for t in tariffs:
        if t['VehicleTypeForThisTariff'] == kCar and t['TariffPeriod'] in kDays:
            rub = t['HourPrice'] if t['HourPrice'] is not None else t['RestOfTheDayPrice']   # (differentiated: after the first minutes)
            for hours in kHours[t['TimeRange']]:
                at = kDays[t['TariffPeriod']] + hours
                p[at] = max(p[at], tens(rub or 0))
    return tuple(p)


def segments_of(features):
    """(lat, lon, to lat, to lon, the prices...) of each piece of each line, 1e-6 degrees; no empty pieces."""
    out = []
    for f in features:
        prices = prices_of(f['properties']['attributes']['Tariffs'])
        for line in f['geometry']['coordinates']:
            pts = [(round(lat * 1e6), round(lon * 1e6)) for lon, lat in line]
            out += [(a[0], a[1], b[0] - a[0], b[1] - a[1]) + prices for a, b in zip(pts, pts[1:]) if a != b]
    return out


def cell_key(s):
    return make_layers.cell_of(s[0] + s[2] // 2, s[1] + s[3] // 2)


def write(segments, out):
    make_layers.write_cells(out, b'OSMP', VERSION, segments, cell_key, lambda s: struct.pack('<iihh6B2x', *s))


def download(page):
    """The set whole: page(skip) up to an empty page, or one with nothing new."""
    features, seen = [], set()
    while True:
        new = [f for f in page(len(features)) if json.dumps(f, sort_keys=True) not in seen]
        if not new:
            return features
        seen.update(json.dumps(f, sort_keys=True) for f in new)
        features += new


def mos_page(key):
    def page(skip):
        url = kUrl + urllib.parse.urlencode({'$top': 1000, '$skip': skip, 'api_key': key})
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={'Accept-Encoding': 'gzip'}), timeout=300) as r:
                d = r.read()
                return json.loads(gzip.decompress(d) if r.headers.get('Content-Encoding') == 'gzip' else d)['features']
        except OSError as e:   # (never the url: the key is in it)
            raise SystemExit('data.mos.ru: %s %s at %d' % (type(e).__name__, getattr(e, 'code', ''), skip)) from None
    return page


def features_from(argv):
    if len(argv) == 3:
        with open(argv[2], encoding='utf-8') as f:
            return json.load(f)
    with open(os.path.join(HERE, '..', '.mosapi'), encoding='utf-8') as f:
        return download(mos_page(f.read().strip()))


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    all_segments = segments_of(features_from(sys.argv))
    write(all_segments, sys.argv[1])
    print('%d pieces of lines' % len(all_segments))
