"""OSMapsCE's offline data of regions: one command per update.

    python make_region.py samara_oblast [ulyanovsk_oblast ...] [--card E:] [--list]

For each region (openstreetmap.fr's extract name, Russia's regions: --list) it downloads the OSM extract (only when
the server has a newer one), builds route.grf (make_graph.py) and search.idx (make_search.py) into
..\\OSMapsData\\<region>\\ with region.txt (its name, the data's date), and with --card copies the region's folder to
<card>\\OSMapsData\\ (the SD card for the unit: \\SDMemory\\OSMapsData). The extracts and the tools' work: offline\\work.
Repeatable: a run after an interrupted one finishes it; nothing is rebuilt that is up to date.
"""
import email.utils
import os
import pathlib
import re
import shutil
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import make_graph
import make_search

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, 'work')
DATA = os.path.join(HERE, '..', 'OSMapsData')
BASE = 'http://download.openstreetmap.fr/extracts/russia/'
UA = {'User-Agent': 'OSMapsCE-make_region (+https://github.com/TpuPyku/OSMapsCE)'}


def get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60).read().decode()


def district_links(html):
    return sorted(set(re.findall(r'href="([a-z_]+_federal_district)/"', html)))


def region_links(html, base):
    """{name: url} of the extracts on a district's page."""
    return {f: base + f + '-latest.osm.pbf' for f in re.findall(r'href="([a-z_]+)-latest\.osm\.pbf"', html)}


def regions(get=get):
    """{name: url} of Russia's regions (the federal districts' folders)."""
    out = {}
    for d in district_links(get(BASE)):
        out.update(region_links(get(BASE + d + '/'), BASE + d + '/'))
    return out


def fetch(url, path):
    """Downloads when the source's copy is newer than ours (its Last-Modified against our file's time, which is set to
    the source's); True = a new file. The .part file never replaces a good one half written."""
    req = urllib.request.Request(url, headers=UA)
    have = os.path.getmtime(path) if os.path.exists(path) else None
    if have is not None:
        req.add_header('If-Modified-Since', email.utils.formatdate(have, usegmt=True))
    try:
        r = urllib.request.urlopen(req, timeout=120)
    except urllib.error.HTTPError as e:
        if e.code == 304:
            return False
        raise
    with r:
        stamp = email.utils.parsedate_to_datetime(r.headers['Last-Modified']).timestamp() if r.headers['Last-Modified'] else time.time()
        if have is not None and stamp <= have:
            return False
        with open(path + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
    os.replace(path + '.part', path)
    os.utime(path, (stamp, stamp))
    return True


def build(name, url, work=WORK, data=DATA):
    """The region's folder up to date: its extract, graph, index and region.txt. Returns the folder."""
    os.makedirs(work, exist_ok=True)
    out = os.path.join(data, name)
    os.makedirs(out, exist_ok=True)
    # (the extract's own suffix: .osm.pbf, or .osm for a plain XML one - pyosmium reads by it)
    pbf = os.path.join(work, name + ''.join(pathlib.PurePosixPath(urllib.parse.urlparse(url).path).suffixes[-2:]))
    new = fetch(url, pbf)
    for tool, file in ((make_graph, 'route.grf'), (make_search, 'search.idx')):
        target = os.path.join(out, file)
        if new or not os.path.exists(target):
            tool.build(pbf, target + '.part')   # (a half-built file never takes the place of a good one)
            os.replace(target + '.part', target)
    day = time.strftime('%Y-%m-%d', time.gmtime(os.path.getmtime(pbf)))
    with open(os.path.join(out, 'region.txt'), 'w', encoding='utf-8') as f:
        f.write('name=%s\ndate=%s\nsource=%s\n' % (name, day, url))
    return out


def copy_to(folder, card, name):
    dst = os.path.join(card + os.sep, 'OSMapsData', name)
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(folder):
        shutil.copy2(os.path.join(folder, f), os.path.join(dst, f))
    return dst


def update(names, known, card=None, work=WORK, data=DATA):
    """The regions built (and copied to the card); an unknown name stops it before any work."""
    unknown = [n for n in names if n not in known]
    if unknown:
        raise SystemExit('unknown region: %s (--list)' % ', '.join(unknown))
    for name in names:
        folder = build(name, known[name], work, data)
        print('%s: %s' % (name, copy_to(folder, card, name) if card else folder))


def args(argv):
    """The command line: (the region names, the card or None, the list of regions asked or no name given)."""
    card = argv[argv.index('--card') + 1] if '--card' in argv else None
    names = [a for a in argv if a not in ('--card', card, '--list')]
    return names, card, '--list' in argv or not names


def main(argv):
    names, card, listing = args(argv)
    known = regions()
    if listing:
        print('\n'.join(sorted(known)))
    else:
        update(names, known, card)


if __name__ == '__main__':
    main(sys.argv[1:])
