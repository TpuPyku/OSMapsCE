"""speedcams.py - speed cameras and road dangers for a region (the user 2026-11-22: only through the PC loader), from
openspeedcam.net's export for Navitel (cp1251 CSV: IDX,X,Y,TYPE,SPEED,DirType,Direction; X lon, Y lat, degrees).

The region's folder gets speedcam.txt (the cameras, the same CSV - VestaHome's CamLine reads it) and danger.lay (the
speed bumps and the dangers as OSMapsCE's layers file, make_layers.py's format). TYPE, Navitel's: 1, 4 a static
camera, 3 a red-light one, 5 a mobile ambush; 100 a pedestrian crossing, 102 a speed bump, 103 a bad road, 104 a
dangerous turn, 105 a dangerous crossroads, 106 another danger; anything else (101 "speed limit": mostly video
control) dropped.
"""
import os
import time

import make_layers

URL = 'https://drive.google.com/uc?export=download&id=1h1H9k4t4mbnKy1WKs5Y9VBApGuW6qwFV'
HEADER = 'IDX,X,Y,TYPE,SPEED,DirType,Direction'
CAMS = (1, 3, 4, 5)
BUMP = 102
DANGERS = (100, 103, 104, 105, 106)   # danger.lay: kind DANGER, fee = TYPE - 100
LAY_BUMP, LAY_DANGER = make_layers.BUMP, 4
CACHE_FILE = 'OpenSpeedcam_Russia.txt'
RUN = time.time()   # the loader's start: the cached file of this run is used again, an older one fetched anew


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
    """The cameras as the source has them (ASCII, CRLF, its header); none: no file (an old one off). Their count."""
    cams = [r for r in rows if r[3] in CAMS]
    if not cams:
        drop(path)
        return 0
    with open(path, 'w', newline='\r\n') as f:
        f.write(HEADER + '\n')
        for r in cams:
            f.write('%d,%r,%r,%d,%d,%d,%d\n' % r)
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


def download(cache_dir, fetch):
    """The export's bytes: fetch(URL) once a run, kept in cache_dir; not the CSV (Google's page instead) - ValueError."""
    path = os.path.join(cache_dir, CACHE_FILE)
    if os.path.exists(path) and os.path.getmtime(path) >= RUN:
        with open(path, 'rb') as f:
            return f.read()
    raw = fetch(URL)
    if not raw.lstrip(b'\xef\xbb\xbf \r\n').startswith(b'IDX,'):
        raise ValueError('openspeedcam.net: not the cameras file')
    os.makedirs(cache_dir, exist_ok=True)
    with open(path + '.part', 'wb') as f:
        f.write(raw)
    os.replace(path + '.part', path)
    return raw
