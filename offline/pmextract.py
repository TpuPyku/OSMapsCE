"""A region's vector tiles cut out of a PMTiles archive - Protomaps' daily planet build - into tiles.vtp.

    python pmextract.py <minLon> <minLat> <maxLon> <maxLat> <maxZoom> <out>/tiles.vtp [archive url]

Only the bytes the region needs are fetched (HTTP Range): the header, the directories met on the way (each once), the
tiles' data in merged ranges (neighbours in the archive's Hilbert order lie close). Nothing of the planet is stored.
The tiles come out ungzipped, keyed as OSMapsCE names them (make_tiles' tiles.vtp), slim: only what OSMapsCE draws.
"""
import gzip
import http.client
import json
import math
import sys
import threading
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

from pmtiles.tile import Compression, deserialize_directory, deserialize_header, find_tile, zxy_to_tileid

import make_tiles

BUILDS = 'https://build-metadata.protomaps.dev/builds.json'
SERVER = 'https://build.protomaps.com/'
UA = {'User-Agent': 'OSMapsCE-maps (+https://github.com/TpuPyku/OSMapsCE)'}
kGap = 128 * 1024          # data ranges closer than this are fetched as one
kMaxGroup = 2 * 1024 * 1024   # ... up to this much: a region's data is pieces, fetched kThreads at a time
kThreads = 8               # (each range of Protomaps' server takes seconds: 8 at a time ~8 times faster - 2026-10-28)
kDirsPart = 0.1            # the progress's part the directories take
kDataEnd = 0.6             # ... the data up to here; the rest - the tiles' processing (slim) and the pack's writing


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

    def prefetch(self, spans, pool):
        """The directories not read yet, read at once (kThreads at a time), each once."""
        need = sorted({d for d in spans if d not in self.dirs})
        for d, raw in zip(need, pool.map(lambda d: self.get_bytes(*d), need)):
            self.dirs[d] = deserialize_directory(raw)   # (it ungzips: Protomaps' internal compression)

    def locate_all(self, tile_ids, pool):
        """{tile id: (data offset, length)} of the tiles the archive has: a level of directories at a time."""
        pending = {t: (self.h['root_offset'], self.h['root_length']) for t in tile_ids}
        found = {}
        for _ in range(4):   # (the spec's depth limit)
            self.prefetch(pending.values(), pool)
            deeper = {}
            for t, d in pending.items():
                e = find_tile(self.dirs[d], t)
                if e is not None and e.run_length:
                    found[t] = (self.h['tile_data_offset'] + e.offset, e.length)
                elif e is not None:
                    deeper[t] = (self.h['leaf_directory_offset'] + e.offset, e.length)
            pending = deeper
        return found


def merged(spans):
    """Sorted (offset, length) spans grouped into fetches: [(start, end, [spans])]."""
    groups = []
    for off, n in sorted(set(spans)):
        if groups and off - groups[-1][1] <= kGap and off + n - groups[-1][0] <= kMaxGroup:
            groups[-1][1] = max(groups[-1][1], off + n)
            groups[-1][2].append((off, n))
        else:
            groups.append([off, off + n, [(off, n)]])
    return groups


def fetch(archive, located, pool, progress=None):
    """{span: tile bytes} for the located tiles' spans, the merged groups kThreads at a time; progress(part, text)
    after each."""
    data = {}
    groups = merged(located.values())
    total, done = sum(e - b for b, e, _ in groups) or 1, 0
    jobs = {pool.submit(archive.get_bytes, b, e - b): (b, e, spans) for b, e, spans in groups}
    for job in as_completed(jobs):
        start, end, spans = jobs[job]
        chunk = job.result()
        for off, n in spans:
            data[(off, n)] = chunk[off - start:off - start + n]
        done += end - start
        if progress:
            progress(kDirsPart + (kDataEnd - kDirsPart) * done / total,
                     'плитки: %.1f из %.1f МБ' % (done / 1e6, total / 1e6))
    return data


def processed(items, total, tell):
    """The (key, tile) pairs passed on, the progress told about every 2 % (the user 2026-10-29: the bar was full
    and the button still busy for minutes - the slimming of every tile, nothing told)."""
    step = max(1, total // 50)
    for i, item in enumerate(items, 1):
        yield item
        if i % step == 0 and i < total:
            tell(kDataEnd + (1 - kDataEnd) * i / total, 'обработка: %d из %d плиток' % (i, total))


def extract(get_bytes, bbox, maxzoom, out, progress=None, keep=lambda tile: tile):
    """The region's tiles into the pack out; keep: what of a tile is kept (slim); progress(part 0..1, text) from the
    start: the archive's directories first, then the data."""
    tell = progress or (lambda part, text: None)
    tell(0, 'каталог архива…')
    with ThreadPoolExecutor(kThreads) as pool:
        archive = Archive(get_bytes)
        keys = {zxy_to_tileid(z, x, y): (z, x, y) for z in range(0, maxzoom + 1)
                for xs, ys in [tile_range(bbox, z)] for x in xs for y in ys}
        located = {keys[t]: span for t, span in archive.locate_all(keys, pool).items()}
        tell(kDirsPart, 'каталог: %d плиток' % len(located))
        data = fetch(archive, located, pool, tell)
    unzip = archive.h['tile_compression'] == Compression.GZIP
    make_tiles.write_pack(out, processed(((make_tiles.key(*t), keep(gzip.decompress(data[s]) if unzip else data[s]))
                                          for t, s in located.items()), len(located), tell))
    tell(1.0, 'плитки записаны')


# ---- slim tiles: only what OSMapsCE draws (native_osm/vtile.cpp's kKinds) - no layers it never draws, no greenery
# (the user 2026-10-28, as Skoda Flow's map: woods, fields... a quarter of a pack), of the pois only its signs
DROP_LAYERS = {b'earth', b'boundaries'}
GREEN = {b'wood', b'forest', b'farmland', b'grassland', b'grass', b'meadow', b'scrub', b'wetland', b'heath',
         b'orchard', b'vineyard', b'allotments', b'nature_reserve', b'barren', b'glacier', b'sand'}
SIGNS = {b'fuel', b'parking'}


def varint(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if c < 0x80:
            return v, i


def spans(b):
    """A protobuf message's fields: (field, value - an int or bytes, the field's own bytes)."""
    i = 0
    while i < len(b):
        start = i
        tag, i = varint(b, i)
        wire = tag & 7
        if wire == 0:
            v, i = varint(b, i)
        elif wire == 2:
            n, i = varint(b, i)
            v, i = b[i:i + n], i + n
        else:   # (a double: 8 bytes, a float: 4)
            n = 8 if wire == 1 else 4
            v, i = b[i:i + n], i + n
        yield tag >> 3, v, b[start:i]


def fields(b):
    return ((f, v) for f, v, _ in spans(b))


def length_field(field, payload):
    out, v = bytearray(), len(payload)
    while v >= 0x80:
        out.append(v & 0x7f | 0x80)
        v >>= 7
    return bytes([field << 3 | 2]) + bytes(out) + bytes([v]) + payload


def unwanted(layer):
    """A layer's test of a feature's kinds (None: the layer kept whole)."""
    if layer in (b'landuse', b'landcover'):
        return lambda kinds: bool(kinds & GREEN)
    if layer == b'pois':
        return lambda kinds: not kinds & SIGNS
    return None


def feature_kinds(feature, keys, vals):
    tags, kinds = dict(fields(feature)).get(2, b''), set()
    i, pairs = 0, []
    while i < len(tags):
        v, i = varint(tags, i)
        pairs.append(v)
    for k, v in zip(pairs[::2], pairs[1::2]):
        if keys[k] in (b'kind', b'kind_detail'):
            kinds.add(vals[v])
    return kinds


def layer_head(layer):
    """A layer's name, keys and string values (a number's: None)."""
    fl = list(fields(layer))
    name = next((v for f, v in fl if f == 1), None)
    return name, [v for f, v in fl if f == 3], [dict(fields(v)).get(1) for f, v in fl if f == 4]


def slim_layer(layer, raw):
    """The layer's bytes slim (b'': none left)."""
    name, keys, vals = layer_head(layer)
    drop = unwanted(name)
    if name in DROP_LAYERS:
        return b''
    if not drop:
        return raw
    kept = [r for f, v, r in spans(layer) if f != 2 or not drop(feature_kinds(v, keys, vals))]
    left = any(f == 2 for f, _, _ in spans(b''.join(kept)))
    return length_field(3, b''.join(kept)) if left else b''


def slim(tile):
    return b''.join(slim_layer(v, r) if f == 3 else r for f, v, r in spans(tile))


def http_bytes(url):
    """get_bytes over HTTP Range requests: a kept connection a thread (a new one each: a round trip more each),
    opened again once when it broke."""
    u = urllib.parse.urlsplit(url)
    local = threading.local()

    def connection():
        if getattr(local, 'conn', None) is None:
            cls = http.client.HTTPSConnection if u.scheme == 'https' else http.client.HTTPConnection
            local.conn = cls(u.netloc, timeout=120)
        return local.conn

    def get(offset, length):
        for attempt in (0, 1):
            conn = connection()
            try:
                conn.request('GET', u.path, headers=dict(UA, Range='bytes=%d-%d' % (offset, offset + length - 1)))
                r = conn.getresponse()
                body = r.read()
                if r.status != 206:
                    raise OSError('HTTP %d' % r.status)
                return body
            except (http.client.HTTPException, OSError):
                conn.close()
                local.conn = None
                if attempt:
                    raise
    return get


def http_get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
        return r.read()


def latest_build(get=http_get):
    """The URL of Protomaps' newest daily build."""
    return SERVER + json.loads(get(BUILDS))[-1]['key']


def main(a):
    extract(http_bytes(a[6] if len(a) > 6 else latest_build()), tuple(map(float, a[0:4])), int(a[4]), a[5], keep=slim)


if __name__ == '__main__':
    main(sys.argv[1:])
