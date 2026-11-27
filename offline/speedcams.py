"""speedcams.py - speed cameras and road dangers for a region (the user 2026-11-22: only through the PC loader), from
openspeedcam.net's export for Navitel (cp1251 CSV: IDX,X,Y,TYPE,SPEED,DirType,Direction; X lon, Y lat, degrees).

The region's folder gets speedcam.txt (the cameras, the same CSV - VestaHome's CamLine reads it) and danger.lay (the
speed bumps and the dangers as OSMapsCE's layers file, make_layers.py's format). TYPE, Navitel's: 1, 4 a static
camera, 3 a red-light one, 5 a mobile ambush; 100 a pedestrian crossing, 102 a speed bump, 103 a bad road, 104 a
dangerous turn, 105 a dangerous crossroads, 106 another danger. The export is New Navitel's: a kind's subtype in
SPEED (newnavitel.blogspot.com, "Различие между speedcam Navitel и New Navitel" - the user 2026-11-27): 101 with 155
(a radar complex) or 25 (a lane camera) is a camera, 101's others (a limit sign, a police post, a town, railway
crossings, video control) dropped; an ambush (5) with x5: speed control at x5 - 5, its 125..155 other kinds.
"""
import os
import time

import make_layers

URL = 'https://drive.google.com/uc?export=download&id=%s'
# Each country's own file (newnavitel.blogspot.com's open ones, the user 2026-11-27: downloaded only for its regions
# chosen); they do not overlap - Crimea, Donetsk, Luhansk have points in both Russia's and Ukraine's
IDS = {'RU': '1h1H9k4t4mbnKy1WKs5Y9VBApGuW6qwFV', 'BY': '1FauoFsG535LdzRbEHTnVBO6yjEh53KB5',
       'UA': '1mpV3eOKR4JTtxOiEbPZMzixV4kNl6y-W', 'KZ': '1dJRH4uyQ2vNtpPPXqcaMOp5nyhs0rYvN'}
UKRAINE = ('cherkasy_oblast', 'chernihiv_oblast', 'chernivtsi_oblast', 'dnipropetrovsk_oblast', 'kharkiv_oblast',
           'kherson_oblast', 'khmelnytskyi_oblast', 'kiev_oblast', 'kirovohrad_oblast', 'lviv_oblast',
           'mykolaiv_oblast', 'odessa_oblast', 'poltava_oblast', 'rivne_oblast', 'sumy_oblast', 'ternopil_oblast',
           'vinnytsia_oblast', 'volyn_oblast', 'zakarpattia_oblast', 'zaporizhia_oblast', 'zhytomyr_oblast')
# (release_regions' names)
COUNTRY = dict(dict.fromkeys(UKRAINE, ('UA',)), belarus=('BY',), kazakhstan=('KZ',),
               **dict.fromkeys(('crimea', 'donetsk', 'luhansk'), ('RU', 'UA')))   # any other region: Russia's
HEADER = 'IDX,X,Y,TYPE,SPEED,DirType,Direction'
CAMS = (1, 3, 4, 5)
LIMIT_MIN, LIMIT_MAX = 10, 160   # openspeedcam.net's fillers, no limits: its red-light cameras' 5, the "no limit" 160


CAMS_101 = (25, 155)   # 101's cameras: a lane camera, a radar complex (written as static cameras, 1)
AMBUSH = 5


def limit(speed):
    """The camera's limit as VestaHome takes it: a filler is 0, unknown (VestaHome reads old files the same way)."""
    return speed if LIMIT_MIN <= speed < LIMIT_MAX else 0


def camera(kind, speed):
    """(TYPE, limit) of a camera as written to speedcam.txt; None - not a camera."""
    if kind == 101:
        return (1, 0) if speed in CAMS_101 else None
    if kind not in CAMS:
        return None
    if kind == AMBUSH and speed % 10 == 5:   # (New Navitel's ambush subtypes)
        return kind, speed - 5 if speed < 120 else 0
    return kind, limit(speed)
BUMP = 102
DANGERS = (100, 103, 104, 105, 106)   # danger.lay: kind DANGER, fee = TYPE - 100
LAY_BUMP, LAY_DANGER = make_layers.BUMP, 4
CACHE_FILE = 'OpenSpeedcam_%s.txt'
RUN = time.time()   # the loader's start: the cached file of this run is used again, an older one fetched anew


def url(country):
    return URL % IDS[country]


def countries(names):
    """The countries (IDS' keys) whose files these regions need, sorted."""
    return sorted({c for n in names for c in COUNTRY.get(n, ('RU',))})


def parse(raw):
    """(idx, lon, lat, type, speed, dirtype, direction) of each good line; the header, blank or bad lines skipped."""
    rows = []
    for line in raw.decode('cp1251', 'replace').splitlines():
        f = line.replace(';', ',').split(',')
        try:
            rows.append((int(f[0]), float(f[1]), float(f[2])) + tuple(int(float(v)) for v in f[3:7]))
        except (ValueError, IndexError):
            continue
    return [r for r in rows if len(r) == 7]   # (a line of 3..6 fields: too short)


def cut(rows, bbox):
    """The rows inside bbox (minLon, minLat, maxLon, maxLat - the catalog's)."""
    lon0, lat0, lon1, lat1 = bbox
    return [r for r in rows if lon0 <= r[1] <= lon1 and lat0 <= r[2] <= lat1]


def drop(path):
    if os.path.exists(path):
        os.remove(path)


def write_speedcam(path, rows):
    """The cameras as the source has them (ASCII, CRLF, its header), the fillers' limits 0; none: no file (an old one
    off). Their count."""
    cams = [(r, camera(r[3], r[4])) for r in rows]
    cams = [(r, c) for r, c in cams if c]
    if not cams:
        drop(path)
        return 0
    with open(path, 'w', newline='\r\n') as f:
        f.write(HEADER + '\n')
        for r, c in cams:
            f.write('%d,%r,%r,%d,%d,%d,%d\n' % (r[:3] + c + r[5:]))
    return len(cams)


def danger_point(r):
    """layers.lay's point (lat, lon 1e-6 degrees, kind, fee, places) of a bump or a danger; None: neither."""
    kind = LAY_BUMP if r[3] == BUMP else LAY_DANGER if r[3] in DANGERS else None
    return kind and (round(r[2] * 1e6), round(r[1] * 1e6), kind, r[3] - 100 if kind == LAY_DANGER else 0, 0)


def write_danger(path, rows):
    """The bumps and dangers as OSMapsCE's points file; none: no file (an old one off). Their count."""
    pts = [p for p in map(danger_point, rows) if p]
    if not pts:
        drop(path)
        return 0
    make_layers.write(pts, path)
    return len(pts)


FILES = (('speedcam.txt', write_speedcam), ('danger.lay', write_danger))   # a region's, and what writes each


def put(folder, rows):
    """A region's speedcam.txt and danger.lay of its rows: how many points written."""
    return sum(write(os.path.join(folder, name), rows) for name, write in FILES)


def download(cache_dir, fetch, country):
    """A country's export bytes: fetch(its url) once a run, kept in cache_dir; not the CSV (Google's page instead) -
    ValueError."""
    path = os.path.join(cache_dir, CACHE_FILE % country)
    if os.path.exists(path) and os.path.getmtime(path) >= RUN:
        with open(path, 'rb') as f:
            return f.read()
    raw = fetch(url(country))
    if not raw.lstrip(b'\xef\xbb\xbf \r\n').startswith(b'IDX,'):
        raise ValueError('openspeedcam.net: not the cameras file')
    os.makedirs(cache_dir, exist_ok=True)
    with open(path + '.part', 'wb') as f:
        f.write(raw)
    os.replace(path + '.part', path)
    return raw
