"""The offline tests' ramp region (native_osm/test_offline.bat; the user 2026-11-22, Moscow's MKAD at Щёлковское
шоссе: the exit 105A leaves the ring 15 degrees off it - told "straight"; a ramp joining the ring from the right just
before it - told "keep right", the driver moves left). 68 degrees north, apart from the others; metres east / north:

    The ring (trunk, both ways): S0 (0, -400) - J (0, 0) - K (0, 200) - S1 (0, 600), north.
    The exit at K (trunk_link, one way): K - R1 (10, 40 past K) - R2 (60, 120 past K) - R3 (150, 140 past K): 14
      degrees off the ring at K, to the right; then the road P (primary): R3 - P1 (400, 340).
    The entry at J (trunk_link, one way): Q0 (100, -200) - Q1 (8, -40) - J: from the right, 11 degrees left of north,
      onto the ring going north (it turns 11 degrees right to join it); from the street Q (primary): Q00 (300, -200) -
      Q0, and back to P: Q00 - P0 (400, -200) - P1 (the graph keeps the part every node of it reaches).
    The service road (the user 2026-11-22, Шоссе Энтузиастов's: a link back to the big road off it, the service road
      going on a class lower): D0 (-200, -400) - D1 (-200, 0) (primary, one way north) - D2 (-200, 400) - S1
      (secondary); the
      link at D1 (primary_link, one way): D1 - L1 (-209, 40) - L2 (-260, 120) - L3 (-350, 140): 13 degrees left of
      the service road; then the road W (primary): L3 - W1 (-600, 140) - W0 (-600, -400) - D0, W0 - S0.

    python make_test_ramp.py <out.osm>        python make_test_ramp.py point <place>  ("lon,lat")
"""
import math
import sys

LAT, LON = 68.0, 50.0
M_LAT = 111195.0
PLACES = {'S0': (0, -400), 'J': (0, 0), 'K': (0, 200), 'S1': (0, 600), 'R1': (10, 240), 'R2': (60, 320),
          'R3': (150, 340), 'P1': (400, 340), 'Q0': (100, -200), 'Q1': (8, -40), 'Q00': (300, -200), 'P0': (400, -200),
          'D0': (-200, -400), 'D1': (-200, 0), 'D2': (-200, 400), 'L1': (-209, 40), 'L2': (-260, 120),
          'L3': (-350, 140), 'W1': (-600, 140), 'W0': (-600, -400)}
IDS = {k: i for i, k in enumerate(PLACES, 1)}
LINK = {'highway': 'trunk_link', 'oneway': 'yes'}
WAYS = [(['S0', 'J', 'K', 'S1'], 'МКАД', {'highway': 'trunk'}),
        (['K', 'R1', 'R2', 'R3'], '', LINK),
        (['R3', 'P1'], 'Шоссе Восток', {'highway': 'primary'}),
        (['Q00', 'Q0'], 'Улица Въезд', {'highway': 'primary'}),
        (['Q0', 'Q1', 'J'], '', LINK),
        (['Q00', 'P0', 'P1'], 'Улица Обход', {'highway': 'primary'}),
        (['D0', 'D1'], 'Дублёр', {'highway': 'primary', 'oneway': 'yes'}),
        (['D1', 'D2', 'S1'], 'Дублёр', {'highway': 'secondary', 'oneway': 'yes'}),
        (['D1', 'L1', 'L2', 'L3'], '', {'highway': 'primary_link', 'oneway': 'yes'}),
        (['L3', 'W1', 'W0', 'D0'], 'Шоссе Запад', {'highway': 'primary'}),
        (['W0', 'S0'], 'Улица Запад', {'highway': 'primary'})]


def lat_lon(name):
    e, n = PLACES[name]
    return LAT + n / M_LAT, LON + e / (M_LAT * math.cos(math.radians(LAT)))


def osm():
    out = ['<?xml version="1.0" encoding="UTF-8"?>', '<osm version="0.6">']
    for name, i in IDS.items():
        la, lo = lat_lon(name)
        out.append('  <node id="%d" lat="%.7f" lon="%.7f" version="1"/>' % (i, la, lo))
    for k, (names, street, tags) in enumerate(WAYS, 1):
        tags = dict(tags, name=street) if street else tags
        out.append('  <way id="%d" version="1">%s%s</way>' % (
            k, ''.join('<nd ref="%d"/>' % IDS[n] for n in names),
            ''.join('<tag k="%s" v="%s"/>' % kv for kv in tags.items())))
    out.append('</osm>')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'point':
        la, lo = lat_lon(sys.argv[2])
        print('%.6f,%.6f' % (lo, la))
    else:
        with open(sys.argv[1], 'w', encoding='utf-8', newline='\n') as f:
            f.write(osm())
