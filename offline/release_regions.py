"""The regions' offline data for GitHub Releases (the user 2026-10-27: the graph and the search there, built by
Actions - .github/workflows/maps.yml; the tiles are not ours: the PC app cuts them out of Protomaps' build).

    python release_regions.py list                 the regions' names, JSON (the workflow's matrix)
    python release_regions.py build <name> <dist>  <dist>/<name>.zip (route.grf, search.idx, region.txt) and
                                                   <dist>/<name>.json (its catalog entry)
    python release_regions.py catalog <dist> [previous.json]
                                                   <dist>/catalog.json of the entries there, the previous catalog's
                                                   regions not built this time kept (their zips stay in the release)

A catalog entry: name, date (the OSM extract's), zip, size, bbox (minLon, minLat, maxLon, maxLat: the road graph's
bounds, for the tiles' cut). The release: the tag "maps" (a pre-release: never the program's latest), its assets
replaced on each run - the app reads ASSETS + "catalog.json".
"""
import json
import os
import struct
import sys
import time
import zipfile

import make_graph
import make_region

ASSETS = 'https://github.com/TpuPyku/OSMapsCE/releases/download/maps/'
FILES = ('route.grf', 'search.idx', 'region.txt')


def graph_bbox(path):
    """(minLon, minLat, maxLon, maxLat), degrees, from route.grf's header."""
    with open(path, 'rb') as f:
        h = struct.unpack(make_graph.HEADER, f.read(struct.calcsize(make_graph.HEADER)))
    min_lat, min_lon, max_lat, max_lon = h[6:10]
    return [min_lon / 1e6, min_lat / 1e6, max_lon / 1e6, max_lat / 1e6]


def region_info(path):
    """region.txt's fields: {name, date, source}."""
    with open(path, encoding='utf-8') as f:
        return dict(line.split('=', 1) for line in f.read().splitlines() if '=' in line)


def build_region(name, url, dist, work):
    """The region's zip and catalog entry in dist (its data built in work; no tiles: no planetiler there)."""
    folder = make_region.build(name, url, work, os.path.join(work, 'data'))
    os.makedirs(dist, exist_ok=True)
    zpath = os.path.join(dist, name + '.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in FILES:
            z.write(os.path.join(folder, f), f)
    entry = {'name': name, 'date': region_info(os.path.join(folder, 'region.txt'))['date'], 'zip': name + '.zip',
             'size': os.path.getsize(zpath), 'bbox': graph_bbox(os.path.join(folder, 'route.grf'))}
    with open(os.path.join(dist, name + '.json'), 'w', encoding='utf-8') as f:
        json.dump(entry, f)
    return entry


def catalog(dist, previous=None):
    """catalog.json of the entries in dist and the previous catalog's others, by name."""
    entries = []
    for f in sorted(os.listdir(dist)):
        if f.endswith('.json') and f != 'catalog.json':
            with open(os.path.join(dist, f), encoding='utf-8') as e:
                entries.append(json.load(e))
    built = {e['name'] for e in entries}
    entries += [e for e in (previous or {'regions': []})['regions'] if e['name'] not in built]
    cat = {'version': 1, 'made': time.strftime('%Y-%m-%d', time.gmtime()),
           'regions': sorted(entries, key=lambda e: e['name'])}
    with open(os.path.join(dist, 'catalog.json'), 'w', encoding='utf-8') as f:
        json.dump(cat, f, ensure_ascii=False)
    return cat


def main(argv):
    if argv[0] == 'list':
        print(json.dumps(sorted(make_region.regions())))
    elif argv[0] == 'build':
        print(build_region(argv[1], make_region.regions()[argv[1]], argv[2], make_region.WORK))
    else:
        catalog(argv[1], json.load(open(argv[2], encoding='utf-8')) if len(argv) > 2 else None)


if __name__ == '__main__':
    main(sys.argv[1:])
