"""The vector tiles of a region for OSMapsCE without the internet (vpack.cpp on the unit).

    python make_tiles.py <region>.mbtiles <out>/tiles.vtp

From planetiler's MBTiles (OpenMapTiles schema, the same tiles OpenFreeMap serves): no SQLite on the unit, so one
file with a sorted index and the tiles ungzipped. The file (little endian):

    header  'OSMT' u32 version, u32 count, i32 minLat, minLon, maxLat, maxLon (1e-6 degrees, the deepest zoom's
            tiles), u32 the index's offset
    index   count x (u32 key = z << 28 | x << 14 | y, u32 offset, u32 length), sorted by key (XYZ rows, as OSMapsCE
            names tiles; MBTiles keeps TMS rows)
    tiles   the Mapbox Vector Tiles
"""
import gzip
import math
import sqlite3
import struct
import sys

VERSION = 1
HEADER = '<4sII4iI'


def plain(data):
    return gzip.decompress(data) if data[:2] == b'\x1f\x8b' else data


def lat_of(z, y):
    return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / (1 << z)))))


def bbox(keys):
    """The deepest zoom's tiles' bounds, 1e-6 degrees."""
    z = max(k >> 28 for k in keys)
    xs = [(k >> 14) & 0x3FFF for k in keys if k >> 28 == z]
    ys = [k & 0x3FFF for k in keys if k >> 28 == z]
    n = 1 << z
    return (int(lat_of(z, max(ys) + 1) * 1e6), int((min(xs) / n * 360 - 180) * 1e6),
            int(lat_of(z, min(ys)) * 1e6), int(((max(xs) + 1) / n * 360 - 180) * 1e6))


def key(z, x, y):
    return z << 28 | x << 14 | y


def build(src, out):
    db = sqlite3.connect(src)
    rows = db.execute('select zoom_level, tile_column, tile_row, tile_data from tiles').fetchall()
    db.close()
    write_pack(out, ((key(z, x, (1 << z) - 1 - row), plain(data)) for z, x, row, data in rows))


def write_pack(out, items):
    """tiles.vtp from (key, MVT bytes) pairs (any order)."""
    tiles = sorted(items)
    head = struct.calcsize(HEADER)
    offset = head + len(tiles) * 12
    with open(out, 'wb') as f:
        f.write(struct.pack(HEADER, b'OSMT', VERSION, len(tiles), *bbox([k for k, _ in tiles]), head))
        for key, data in tiles:
            f.write(struct.pack('<III', key, offset, len(data)))
            offset += len(data)
        for _, data in tiles:
            f.write(data)
    print('tiles %d, %.1f MB' % (len(tiles), offset / 1e6))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2])
