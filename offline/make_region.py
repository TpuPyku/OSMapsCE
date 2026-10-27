"""OSMapsCE's offline data of regions: one command per update.

    python make_region.py samara_oblast [ulyanovsk_oblast ...] [--card E:] [--list]

For each region (openstreetmap.fr's extract name, Russia's regions: --list) it downloads the OSM extract (only when
the server has a newer one), builds route.grf (make_graph.py) and search.idx (make_search.py) into
..\\OSMapsData\\<region>\\ with region.txt (its name, the data's date), and with --card copies the region's folder to
<card>\\OSMapsData\\ (the SD card for the unit: \\SDMemory\\OSMapsData). The extracts and the tools' work: offline\\work.
"""
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, 'work')
DATA = os.path.join(HERE, '..', 'OSMapsData')
BASE = 'http://download.openstreetmap.fr/extracts/russia/'
UA = {'User-Agent': 'OSMapsCE-make_region (+https://github.com/TpuPyku/OSMapsCE)'}


def regions():
    """{name: url} of Russia's regions (the federal districts' folders)."""
    out = {}
    top = urllib.request.urlopen(urllib.request.Request(BASE, headers=UA), timeout=60).read().decode()
    for d in sorted(set(re.findall(r'href="([a-z_]+_federal_district)/"', top))):
        page = urllib.request.urlopen(urllib.request.Request(BASE + d + '/', headers=UA), timeout=60).read().decode()
        for f in re.findall(r'href="([a-z_]+)-latest\.osm\.pbf"', page):
            out[f] = BASE + d + '/' + f + '-latest.osm.pbf'
    return out


def fetch(url, path):
    """Downloads when the server's copy is newer than ours; True = a new file."""
    req = urllib.request.Request(url, headers=UA)
    if os.path.exists(path):
        req.add_header('If-Modified-Since', time.strftime('%a, %d %b %Y %H:%M:%S GMT', time.gmtime(os.path.getmtime(path))))
    try:
        with urllib.request.urlopen(req, timeout=120) as r, open(path + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
    except urllib.error.HTTPError as e:
        if e.code == 304:
            return False
        raise
    os.replace(path + '.part', path)
    return True


def build(name, url):
    os.makedirs(WORK, exist_ok=True)
    pbf = os.path.join(WORK, name + '.osm.pbf')
    out = os.path.join(DATA, name)
    os.makedirs(out, exist_ok=True)
    new = fetch(url, pbf)
    print('%s: extract %s (%.0f MB)' % (name, 'downloaded' if new else 'up to date', os.path.getsize(pbf) / 1e6))
    for tool, file in (('make_graph.py', 'route.grf'), ('make_search.py', 'search.idx')):
        if new or not os.path.exists(os.path.join(out, file)):
            subprocess.check_call([sys.executable, os.path.join(HERE, tool), pbf, os.path.join(out, file)])
    day = time.strftime('%Y-%m-%d', time.gmtime(os.path.getmtime(pbf)))
    with open(os.path.join(out, 'region.txt'), 'w', encoding='utf-8') as f:
        f.write('name=%s\ndate=%s\nsource=%s\n' % (name, day, url))
    return out


def main():
    args = sys.argv[1:]
    card = None
    if '--card' in args:
        i = args.index('--card')
        card = args[i + 1]
        del args[i:i + 2]
    known = regions()
    if '--list' in args or not args:
        print('\n'.join(sorted(known)))
        return
    for name in args:
        if name not in known:
            sys.exit('unknown region: %s (--list)' % name)
        out = build(name, known[name])
        if card:
            dst = os.path.join(card + os.sep, 'OSMapsData', name)
            os.makedirs(dst, exist_ok=True)
            for f in os.listdir(out):
                shutil.copy2(os.path.join(out, f), os.path.join(dst, f))
            print('%s: copied to %s' % (name, dst))


if __name__ == '__main__':
    main()
