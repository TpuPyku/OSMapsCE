"""A graph as made before the speed limits (2026-11-11): route.grf without its 'OSMS' part - for the offline tests
(native_osm/test_offline.bat: the twin region), the unit's reading of an old region checked.

    python strip_limits.py <route.grf>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import make_graph   # noqa: E402


def strip(path):
    with open(path, 'rb') as f:
        d = f.read()
    h = struct.unpack_from(make_graph.HEADER, d, 0)
    end = h[-1] + (h[10] * h[11] + 1 + h[13]) * 4   # the grid's end: 'OSMB' and the bearings after it
    assert d[end:end + 4] == b'OSMB'
    end += 4 + h[3] * 2
    with open(path, 'wb') as f:
        f.write(d[:end])


if __name__ == '__main__':
    strip(sys.argv[1])
