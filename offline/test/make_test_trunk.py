"""The trunk roads' test graph (4b) for native_osm/test_offline: tiny.osm's primary and tertiary streets - the residential
ones only the region's (route.grf), so a joined route shows where the region's part is.

    python make_test_trunk.py <tiny.osm> <out>/trunk.grf
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import make_graph  # noqa: E402

if __name__ == '__main__':
    make_graph.build(sys.argv[1], sys.argv[2], classes={'primary', 'tertiary'}, cell=200000)
