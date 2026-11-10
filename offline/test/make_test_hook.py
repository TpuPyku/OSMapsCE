"""The offline tests' hook region (native_osm/test_offline.bat; the user 2026-11-09, Moscow's Khodynka: at a fork the
route took a service loop bending left, then a hairpin right onto the street - the straight service road beside it was
a little slower with no cost for turns; and the fork was not told). 64 degrees north, apart from the others; metres
east / north of A:

    W (-200, 0) - A (0, 0): a street east.  At A two one-way service roads:
      straight: A - C (80, -3.5) - X (160, -5)                        (~160 m, 38.4 s)
      the loop: A - B1 (10, 2) - B2 (12, 25) - B3 (5, 32) - B (-5, 32): nearly straight at A, north, then curving to
                the west - into B as for a U-turn east (~53 m, 12.7 s); its chord A - B shows a turn of 115 degrees only
    The street M (tertiary): M0 (-100, 60) - B | B - X - T (300, -30) (B - X 168 m, 15.1 s); B - N (10, 240) north.
    Back to W (the graph keeps the part every node of it reaches): W - Z1 (-200, 200) - Z (-200, 400) - N, long.
    A branch of M as good as it (tertiary, another name): B - Y (77, -27) - T, ~20 degrees off M at B.
    W -> T: the straight road - the loop is 10.6 s quicker with turns free; by the nodes' chords its turns cost 9 s
    (the loop it is: hookold), by the roads' bearings 20.5 s (the straight road: the graphs with 'OSMB').
    W -> N: the loop - its fork at A told (the branch bends away within metres).
    W -> T at A: the loop beside (a service road like ours, 14 degrees off) - "keep right" (the user 2026-11-10: every
    fork of roads alike told); along M through B: M goes on by its name - the branch Y not told.
    A turn restriction (the user 2026-11-09: Khodynka's loop ends "only straight on"): from the loop at B not onto
    Y - from the loop to T by M (without it: Y, the quicker).

    python make_test_hook.py <out.osm> [lat]      (66: hookold, its graph cut before the bearings - an old one)
"""
import math
import sys

LAT, LON = 64.0, 50.0
M_LAT = 111195.0
M_LON = M_LAT * math.cos(math.radians(LAT))
PLACES = {'W': (-200, 0), 'A': (0, 0), 'C': (80, -3.5), 'X': (160, -5), 'B1': (10, 2), 'B2': (12, 25), 'B3': (5, 32),
          'B': (-5, 32), 'M0': (-100, 60), 'T': (300, -30), 'N': (10, 240), 'Z': (-200, 400), 'Y': (77, -27),
          'Z1': (-200, 200)}
IDS = {k: i for i, k in enumerate(PLACES, 1)}
SERVICE = {'highway': 'service', 'oneway': 'yes'}
WAYS = [(['W', 'A'], 'Улица Запад', {'highway': 'residential'}),
        (['A', 'C', 'X'], '', SERVICE),
        (['A', 'B1', 'B2', 'B3', 'B'], '', SERVICE),
        (['M0', 'B'], 'Улица Крюк', {'highway': 'tertiary'}),
        (['B', 'X', 'T'], 'Улица Крюк', {'highway': 'tertiary'}),   # (split at B: the restriction's way)
        (['B', 'N'], 'Улица Север', {'highway': 'residential'}),
        (['W', 'Z1', 'Z', 'N'], 'Улица Обход', {'highway': 'residential'}),
        (['B', 'Y', 'T'], 'Улица Ветка', {'highway': 'tertiary'})]
# turn restrictions: (its tags, from way, via node, to way) - the ways by their place in WAYS (ids from 1)
RULES = [({'type': 'restriction', 'restriction': 'no_left_turn'}, 3, 'B', 8)]


def lat_lon(name, lat=LAT):
    e, n = PLACES[name]
    return lat + n / M_LAT, LON + e / (M_LAT * math.cos(math.radians(lat)))


def osm(lat=LAT):
    out = ['<?xml version="1.0" encoding="UTF-8"?>', '<osm version="0.6">']
    for name, i in IDS.items():
        la, lo = lat_lon(name, lat)
        out.append('  <node id="%d" lat="%.7f" lon="%.7f" version="1"/>' % (i, la, lo))
    for k, (names, street, tags) in enumerate(WAYS, 1):
        tags = dict(tags, name=street) if street else tags
        out.append('  <way id="%d" version="1">%s%s</way>' % (
            k, ''.join('<nd ref="%d"/>' % IDS[n] for n in names),
            ''.join('<tag k="%s" v="%s"/>' % kv for kv in tags.items())))
    for k, (tags, fw, via, tw) in enumerate(RULES, 1):
        out.append('  <relation id="%d" version="1"><member type="way" ref="%d" role="from"/>'
                   '<member type="node" ref="%d" role="via"/><member type="way" ref="%d" role="to"/>%s</relation>' % (
                       k, fw, IDS[via], tw, ''.join('<tag k="%s" v="%s"/>' % kv for kv in tags.items())))
    out.append('</osm>')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'point':   # "lon,lat" of a place (the test's requests)
        la, lo = lat_lon(sys.argv[2])
        print('%.6f,%.6f' % (lo, la))
    else:
        with open(sys.argv[1], 'w', encoding='utf-8') as f:
            f.write(osm(float(sys.argv[2]) if len(sys.argv) > 2 else LAT))
