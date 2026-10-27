"""A region's vector tiles cut out of a PMTiles archive - Protomaps' daily planet build - into tiles.vtp.

    python pmextract.py <minLon> <minLat> <maxLon> <maxLat> <maxZoom> <out>/tiles.vtp [archive url]

Only the bytes the region needs are fetched (HTTP Range): the header, the directories met on the way (each once), the
tiles' data in merged ranges (neighbours in the archive's Hilbert order lie close). Nothing of the planet is stored.
The tiles come out ungzipped, keyed as OSMapsCE names them (make_tiles' tiles.vtp).
"""
import gzip
import json
import math
import sys
import urllib.request

from pmtiles.tile import Compression, deserialize_directory, deserialize_header, find_tile, zxy_to_tileid

import make_tiles

BUILDS = 'https://build-metadata.protomaps.dev/builds.json'
SERVER = 'https://build.protomaps.com/'
UA = {'User-Agent': 'OSMapsCE-maps (+https://github.com/TpuPyku/OSMapsCE)'}
kGap = 128 * 1024   # data ranges closer than this are fetched as one


def tile_range(bbox, z):
    """The x, y ranges of zoom z's tiles over (minLon, minLat, maxLon, maxLat)."""
    n = 1 << z

    def tx(lon):
        return min(n - 1, max(0, int((lon + 180) / 360 * n)))

    def ty(lat):
        la = math.radians(max(-85.05, min(85.05, lat)))
        return min(n - 1, max(0, int((1 - math.log(math.tan(la) + 1 / math.cos(la)) / math.pi) / 2 * n)))
    return range(tx(bbox[0]), tx(bbox[2]) + 1), range(ty(bbox[3]), ty(bbox[1]) + 1)


class Archive:
    """A PMTiles archive read through get_bytes(offset, length); its directories kept once read."""
    def __init__(self, get_bytes):
        self.get_bytes = get_bytes
        self.h = deserialize_header(get_bytes(0, 127))
        self.dirs = {}

    def directory(self, offset, length):
        if (offset, length) not in self.dirs:   # (deserialize_directory ungzips: Protomaps' internal compression)
            self.dirs[(offset, length)] = deserialize_directory(self.get_bytes(offset, length))
        return self.dirs[(offset, length)]

    def locate(self, tile_id):
        """(data offset, length) of a tile, None when the archive has none."""
        offset, length = self.h['root_offset'], self.h['root_length']
        for _ in range(4):   # (the spec's depth limit)
            e = find_tile(self.directory(offset, length), tile_id)
            if e is None:
                return None
            if e.run_length:
                return self.h['tile_data_offset'] + e.offset, e.length
            offset, length = self.h['leaf_directory_offset'] + e.offset, e.length
        return None


def merged(spans):
    """Sorted (offset, length) spans grouped into fetches: [(start, end, [spans])]."""
    groups = []
    for off, n in sorted(set(spans)):
        if groups and off - groups[-1][1] <= kGap:
            groups[-1][1] = max(groups[-1][1], off + n)
            groups[-1][2].append((off, n))
        else:
            groups.append([off, off + n, [(off, n)]])
    return groups


def fetch(archive, located, progress=None):
    """{span: tile bytes} for the located tiles' spans, a fetch per merged group; progress(done part 0..1) after each."""
    data = {}
    groups = merged(located.values())
    for i, (start, end, spans) in enumerate(groups):
        chunk = archive.get_bytes(start, end - start)
        for off, n in spans:
            data[(off, n)] = chunk[off - start:off - start + n]
        if progress:
            progress((i + 1) / len(groups))
    return data


def extract(get_bytes, bbox, maxzoom, out, progress=None):
    archive = Archive(get_bytes)
    located = {}
    for z in range(0, maxzoom + 1):
        xs, ys = tile_range(bbox, z)
        for x in xs:
            for y in ys:
                span = archive.locate(zxy_to_tileid(z, x, y))
                if span:
                    located[(z, x, y)] = span
    data = fetch(archive, located, progress)
    unzip = archive.h['tile_compression'] == Compression.GZIP
    make_tiles.write_pack(out, ((make_tiles.key(*t), gzip.decompress(data[s]) if unzip else data[s])
                                for t, s in located.items()))


def http_bytes(url):
    """get_bytes over HTTP Range requests."""
    def get(offset, length):
        req = urllib.request.Request(url, headers=dict(UA, Range='bytes=%d-%d' % (offset, offset + length - 1)))
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.read()
    return get


def http_get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
        return r.read()


def latest_build(get=http_get):
    """The URL of Protomaps' newest daily build."""
    return SERVER + json.loads(get(BUILDS))[-1]['key']


if __name__ == '__main__':
    a = sys.argv[1:]
    extract(http_bytes(a[6] if len(a) > 6 else latest_build()), tuple(map(float, a[0:4])), int(a[4]), a[5])
