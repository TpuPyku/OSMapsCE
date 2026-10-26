#!/usr/bin/env python3
"""get_tiles.py - OpenFreeMap vector tiles (OpenMapTiles schema) around a point for VTileTest:
../VTileTest/tiles/<z>/<x>_<y>.pbf, z12 and z13 4 x 4, z14 6 x 6 (the view turned at z14 needs that much).

Usage: python OSMapsCE/native_vtest/get_tiles.py [lat lon]   (default: the centre of Samara)
"""
import json, math, os, sys, urllib.request

lat, lon = (float(sys.argv[1]), float(sys.argv[2])) if len(sys.argv) > 2 else (53.1955, 50.1100)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'VTileTest', 'tiles')
UA = {'User-Agent': 'OSMapsCE-VTileTest (+https://github.com/TpuPyku/OSMapsCE)'}


def get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30).read()


tmpl = json.loads(get('https://tiles.openfreemap.org/planet'))['tiles'][0]   # the current data version
n = 0
for z, r in ((12, 2), (13, 2), (14, 3)):
    k = 2 ** z
    cx = (lon + 180) / 360 * k
    cy = (1 - math.log(math.tan(math.radians(lat)) + 1 / math.cos(math.radians(lat))) / math.pi) / 2 * k
    os.makedirs(os.path.join(OUT, str(z)), exist_ok=True)
    for x in range(int(cx) - r, int(cx) + r):
        for y in range(int(cy) - r, int(cy) + r):
            try:
                data = get(tmpl.replace('{z}', str(z)).replace('{x}', str(x)).replace('{y}', str(y)))
            except Exception as e:
                print('  %d/%d/%d: %s' % (z, x, y, e))
                continue
            if data:
                open(os.path.join(OUT, str(z), '%d_%d.pbf' % (x, y)), 'wb').write(data)
                n += 1
print(n, 'tiles ->', os.path.normpath(OUT))
