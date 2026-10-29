"""The offline tests' toll region (native_osm/test_offline.bat; the user 2026-10-29: an option to keep off toll roads):
A - B straight on a toll road (~555 m), A - C - D - B round it, free (~1.7 km; dead ends off C and D: the way round
three edges); a toll road on from B far east (E, F), no free road near it; 60 degrees north, apart from the others.

    python make_test_toll.py <out.osm>
"""
import sys

NODES = {1: (60.000, 50.000), 2: (60.000, 50.010), 3: (60.005, 50.000), 4: (60.005, 50.010), 5: (60.008, 50.000),
         6: (60.000, 50.100), 7: (60.000, 50.120), 8: (60.008, 50.010)}
WAYS = [([1, 2], 'Платная дорога', {'highway': 'primary', 'toll': 'yes'}),
        ([1, 3, 4, 2], 'Объезд', {'highway': 'residential'}),
        ([3, 5], 'Тупик', {'highway': 'residential'}), ([4, 8], 'Тупик', {'highway': 'residential'}),
        ([2, 6, 7], 'Дальняя платная', {'highway': 'primary', 'toll': 'yes'})]


def osm():
    out = ['<?xml version="1.0" encoding="UTF-8"?>', '<osm version="0.6">']
    out += ['  <node id="%d" lat="%s" lon="%s" version="1"/>' % (i, la, lo) for i, (la, lo) in NODES.items()]
    for k, (ids, name, tags) in enumerate(WAYS, 1):
        out.append('  <way id="%d" version="1">%s%s<tag k="name" v="%s"/></way>' % (
            k, ''.join('<nd ref="%d"/>' % i for i in ids), ''.join('<tag k="%s" v="%s"/>' % kv for kv in tags.items()),
            name))
    out.append('</osm>')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    with open(sys.argv[1], 'w', encoding='utf-8') as f:
        f.write(osm())
