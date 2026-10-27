"""The test map's tile pack (test_offline.bat): two tiles around the test town (53.001 N, 50.003 E) - its z14 tile
holding b'tile 14' and that tile's z13 parent holding b'tile 13' - built through planetiler's MBTiles shape by
make_tiles, as a region's real pack is.

    python make_test_tiles.py <out>/tiles.vtp
"""
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import make_tiles                 # noqa: E402
from test_tools import make_mbtiles   # noqa: E402


def tile_of(lat, lon, z):
    n = 1 << z
    y = (1 - math.log(math.tan(math.radians(lat)) + 1 / math.cos(math.radians(lat))) / math.pi) / 2 * n
    return int((lon + 180) / 360 * n), int(y)


def main(out):
    x, y = tile_of(53.001, 50.003, 14)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'tiny.mbtiles')
        make_mbtiles(src, {(14, x, y): b'tile 14', (13, x >> 1, y >> 1): b'tile 13'})
        make_tiles.build(src, out)


if __name__ == '__main__':
    main(sys.argv[1])
