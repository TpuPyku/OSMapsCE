"""The region tools on the test map (tiny.osm): what OSMapsCE reads from their files, and the update's steps.

    python -m unittest discover -s offline/test      (quality.bat runs it under coverage)
"""
import math
import os
import pathlib
import shutil
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import make_graph    # noqa: E402
import make_region   # noqa: E402
import make_search   # noqa: E402
import make_tiles    # noqa: E402
import pmextract     # noqa: E402
import release_regions   # noqa: E402

TINY = os.path.join(HERE, 'tiny.osm')


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def graph(path):
    d = read(path)
    h = struct.unpack_from(make_graph.HEADER, d, 0)
    n_nodes, n_edges, off = h[2], h[3], h[-6:]
    nodes = [struct.unpack_from('<ii', d, off[0] + i * 8) for i in range(n_nodes)]
    first = struct.unpack_from('<%dI' % (n_nodes + 1), d, off[1])
    edges = []
    for a in range(n_nodes):
        for e in range(first[a], first[a + 1]):
            to, dm, ds, g, ng, cls, flags, nm = struct.unpack_from('<IIIIHBBI', d, off[2] + e * 24)
            name = d[off[4] + nm:d.index(b'\0', off[4] + nm)].decode().split('\t')[0]
            edges.append((nodes[a], nodes[to], dm, ds, name))
    return nodes, edges


def pack_tiles(path):
    """{(z, x, y): data} as the unit reads tiles.vtp: the header, the sorted index, the blobs."""
    d = read(path)
    magic, version, count, *_bbox, off_index = struct.unpack_from(make_tiles.HEADER, d, 0)
    out, keys = {}, []
    for i in range(count):
        key, off, length = struct.unpack_from('<QII', d, off_index + i * 16)
        keys.append(key)
        out[(key >> 32, (key >> 16) & 0xFFFF, key & 0xFFFF)] = d[off:off + length]
    return magic, keys, out


def make_mbtiles(path, tiles):
    """An MBTiles file as planetiler writes it: TMS rows, gzipped tile data."""
    import gzip
    import sqlite3
    db = sqlite3.connect(path)
    db.execute('create table tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob)')
    db.execute('create table metadata (name text, value text)')
    for (z, x, y), data in tiles.items():
        db.execute('insert into tiles values (?, ?, ?, ?)', (z, x, (1 << z) - 1 - y, gzip.compress(data)))
    db.commit()
    db.close()


def make_pmtiles(path, tiles):
    """A PMTiles archive as Protomaps' build is: tiles gzipped, in tile id order (identical neighbours: a run)."""
    import gzip
    from pmtiles.tile import zxy_to_tileid, TileType, Compression
    from pmtiles.writer import Writer
    with open(path, 'wb') as f:
        w = Writer(f)
        for key in sorted(tiles, key=lambda t: zxy_to_tileid(*t)):
            w.write_tile(zxy_to_tileid(*key), gzip.compress(tiles[key]))
        w.finalize({'tile_type': TileType.MVT, 'tile_compression': Compression.GZIP, 'min_lon_e7': -1800000000,
                    'min_lat_e7': -850000000, 'max_lon_e7': 1800000000, 'max_lat_e7': 850000000,
                    'center_zoom': 0, 'center_lon_e7': 0, 'center_lat_e7': 0}, {})


def pb(field, payload):
    """A protobuf length-delimited field (a varint field: payload an int)."""
    def var(v):
        out = b''
        while v >= 0x80:
            out += bytes([v & 0x7f | 0x80])
            v >>= 7
        return out + bytes([v])
    if isinstance(payload, int):
        return var(field << 3) + var(payload)
    return var(field << 3 | 2) + var(len(payload)) + payload


def mvt(layers):
    """A Mapbox Vector Tile: {layer: [kind, ...]} - each feature a point with its "kind" (Protomaps' class key)."""
    out = b''
    for name, kinds in layers.items():
        body = pb(1, name.encode()) + pb(3, b'kind')
        for i, k in enumerate(kinds):
            body += pb(4, pb(1, k.encode()))
        for i in range(len(kinds)):
            body += pb(2, pb(2, bytes([0, i])) + pb(3, 1) + pb(4, bytes([9, 2, 2])))
        out += pb(3, body + pb(15, 2))
    return out


def mvt_kinds(tile):
    """{layer: [kind, ...]} of a tile mvt() made (read back through pmextract's protobuf reader)."""
    import pmextract
    got = {}
    for f, layer in pmextract.fields(tile):
        name, vals, kinds = None, [], []
        for g, v in pmextract.fields(layer):
            if g == 1:
                name = v.decode()
            elif g == 4:
                vals.append(dict(pmextract.fields(v)).get(1, b'').decode())   # (a number: '')
            elif g == 2:
                tags = dict(pmextract.fields(v))[2]
                kinds.append(vals[tags[1]])
        got[name] = kinds
    return got


class Tools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_graph_roads_and_one_way(self):
        out = os.path.join(self.tmp, 'route.grf')
        make_graph.build(TINY, out)
        nodes, edges = graph(out)
        self.assertEqual(len(nodes), 14)              # the junctions and the ends (not the curve's inner points)
        self.assertEqual(len(edges), 25)              # 10 two-way pieces, the one-way, the roundabout's 4
        self.assertEqual(struct.unpack_from(make_graph.HEADER, read(out), 0)[4], 2)   # the curve's inner points
        ring = [(a, b) for a, b, _, _, n in edges if n == '']
        self.assertEqual(len(ring), 4)                # the roundabout: one way round
        one = [(a, b) for a, b, _, _, n in edges if n == 'Односторонняя улица']
        self.assertEqual(one, [((53000000, 50004000), (53002000, 50004000))])   # north only
        main = [dm for a, b, dm, _, n in edges if n == 'Главная улица' and a == (53000000, 50000000)]
        self.assertTrue(1300 < main[0] < 1400)        # ~134 m, in decimetres

    def test_trunk_graph(self):
        """4b (the user 2026-10-29): the trunk roads of the whole country - only the classes asked, the shapes
        simplified (the lengths still the true ones), the grid's cell coarse."""
        full, out = os.path.join(self.tmp, 'full.grf'), os.path.join(self.tmp, 'trunk.grf')
        make_graph.build(TINY, full)
        make_graph.build(TINY, out, classes={'primary'}, simplify_m=1000, cell=100000)
        nodes, edges = graph(out)
        self.assertEqual({n for _, _, _, _, n in edges}, {'Главная улица'})
        self.assertLess(len(nodes), len(graph(full)[0]))
        h = struct.unpack_from(make_graph.HEADER, read(out), 0)
        self.assertEqual(h[4], 0)                     # no inner points left within 1 km
        self.assertEqual(h[12], 100000)               # the cell
        main = sorted(dm for a, b, dm, _, n in graph(full)[1] if n == 'Главная улица')
        self.assertLessEqual(abs(sum(dm for _, _, dm, _, _ in edges) - sum(main)), 4)   # the same length, one piece
        self.assertEqual(len(edges), 2)               # (the side roads' junctions gone: one piece, both ways)

    def test_trunk_graph_west_cut(self):
        """Chukotka's east beyond 180 degrees (negative longitudes) stretched the grid round the globe (2026-10-29:
        3601 x 385 cells, 5.5 MB of 26): the nodes west of `west` left out."""
        out = os.path.join(self.tmp, 'cut.grf')
        make_graph.build(TINY, out, west=50002000)
        nodes, _ = graph(out)
        self.assertTrue(nodes and all(lon >= 50002000 for _, lon in nodes), nodes)
        self.assertEqual(make_graph.TRUNK['west'], 0)

    def test_graph_grid_bounded(self):
        """A big country at 0.005 degrees (2026-10-29: Kazakhstan's grid 2784 x 1696 cells - 19 MB of its 26.9 MB
        graph): the cell doubled until the grid is at most max_cells."""
        out = os.path.join(self.tmp, 'grid.grf')
        make_graph.build(TINY, out, cell=100, max_cells=40)
        h = struct.unpack_from(make_graph.HEADER, read(out), 0)
        gw, gh, cell = h[10], h[11], h[12]
        self.assertLessEqual(gw * gh, 40)
        self.assertGreater(cell, 100)
        self.assertEqual(cell % 100, 0)                     # (doubled)
        self.assertEqual(make_graph.kMaxCells, 1 << 20)

    def test_simplify(self):
        line = [(0, 0), (10, 1000), (0, 2000), (0, 3000), (500, 4000)]   # 1e-6 degrees: ~1 m of latitude per 9
        self.assertEqual(make_graph.simplify(line, 0), line)
        self.assertEqual(make_graph.simplify(line, 5), [(0, 0), (0, 3000), (500, 4000)])
        self.assertEqual(make_graph.simplify(line, 100), [(0, 0), (500, 4000)])
        self.assertEqual(make_graph.simplify(line[:2], 100), line[:2])

    def test_speed_and_access(self):
        self.assertEqual(make_graph.maxspeed_kmh('40 mph'), 64)
        self.assertIsNone(make_graph.maxspeed_kmh('none'))
        self.assertIsNone(make_graph.car_way({'highway': 'primary', 'access': 'private'}, [1, 2]))
        self.assertEqual(make_graph.car_way({'highway': 'primary', 'oneway': '-1'}, [1, 2]).oneway, -1)
        self.assertEqual(make_graph.car_way({'highway': 'track', 'tracktype': 'grade4'}, [1, 2]).speed, 8)

    def test_tile_pack_from_mbtiles(self):
        src, out = os.path.join(self.tmp, 'r.mbtiles'), os.path.join(self.tmp, 'tiles.vtp')
        tiles = {(14, 9907, 5118): b'samara z14', (14, 9908, 5118): b'east', (0, 0, 0): b'world',
                 (15, 19815, 10237): b'z15: x past 14 bits'}
        make_mbtiles(src, tiles)
        make_tiles.build(src, out)
        magic, keys, got = pack_tiles(out)
        self.assertEqual(magic, b'OSMT')
        self.assertEqual(keys, sorted(keys))          # the unit finds a tile by binary search
        self.assertEqual(got, tiles)                  # XYZ rows (not TMS), data ungzipped

    def test_slim_tiles(self):
        """Only what OSMapsCE draws (vtile.cpp): no layers it never draws (earth, boundaries), no greenery (the user
        2026-10-28: woods, fields - a quarter of a pack), of the pois only filling stations and car parks."""
        tile = mvt({'earth': ['earth'], 'landuse': ['wood', 'park', 'farmland', 'residential'],
                    'boundaries': ['country'], 'pois': ['cafe', 'fuel', 'parking'], 'roads': ['major_road'],
                    'landcover': ['forest', 'urban_area']})
        self.assertEqual(mvt_kinds(pmextract.slim(tile)), {'landuse': ['park', 'residential'], 'pois': ['fuel', 'parking'],
                                                          'roads': ['major_road'], 'landcover': ['urban_area']})
        roads = mvt({'roads': ['major_road']})
        self.assertEqual(pmextract.slim(roads), roads)              # nothing to drop: the same bytes
        self.assertEqual(pmextract.slim(mvt({'earth': ['earth']})), b'')
        import struct                                         # a height as a double, an area as a float (Protomaps)
        numbers = pb(4, bytes([3 << 3 | 1]) + struct.pack('<d', 12.5)) + pb(4, bytes([2 << 3 | 5]) + struct.pack('<f', 1.5))
        body = dict(pmextract.fields(mvt({'pois': ['cafe', 'fuel']})))[3]
        tile = pb(3, body[:-2] + numbers + body[-2:])   # (before the version, the layer's last 2 bytes)
        self.assertEqual(mvt_kinds(pmextract.slim(tile)), {'pois': ['fuel']})

    def test_slim_exact_bytes(self):
        """The slim layer byte for byte: the kept features as they were, the layer's length again (a long one: a
        varint of 2 bytes), "kind" found among other keys; a layer all dropped: gone."""
        def layer(kinds, keep):   # keys: name, kind; each feature: its name (value 0) and its kind
            body = pb(1, b'landuse') + pb(3, b'name') + pb(3, b'kind') + pb(4, pb(1, b'x' * 3))
            body += b''.join(pb(4, pb(1, k.encode())) for k in kinds)
            for i in keep:
                body += pb(2, pb(2, bytes([0, 0, 1, i + 1])) + pb(3, 3) + pb(4, bytes([9, 2, 2])))
            return pb(3, body + pb(15, 2))
        kinds = ['wood', 'park'] * 40                      # 80 features, the layer > 127 bytes
        got = pmextract.slim(layer(kinds, range(80)))
        self.assertEqual(got, layer(kinds, range(1, 80, 2)))
        self.assertGreater(len(got), 300)
        self.assertEqual(pmextract.slim(layer(['wood'], [0])), b'')
        self.assertEqual(pmextract.slim(mvt({'landuse': ['wood', 'farmland']})), b'')

    def test_protobuf_fields(self):
        import struct
        msg = (bytes([3 << 3 | 1]) + struct.pack('<d', 2.5) + bytes([4 << 3 | 5]) + struct.pack('<f', 1.5) +
               pb(5, 300) + pb(6, b'ab'))
        self.assertEqual(list(pmextract.fields(msg)), [(3, struct.pack('<d', 2.5)), (4, struct.pack('<f', 1.5)),
                                                        (5, 300), (6, b'ab')])
        self.assertEqual(pmextract.length_field(3, b'z' * 200), bytes([26, 200, 1]) + b'z' * 200)
        self.assertEqual(list(pmextract.fields(pb(7, 128))), [(7, 128)])                     # (0x80 0x01: 128 itself)
        self.assertEqual(pmextract.length_field(3, b'z' * 128), bytes([26, 0x80, 1]) + b'z' * 128)

    def test_pmextract_command_line(self):
        import unittest.mock
        asked = []
        patch = unittest.mock.patch.object
        with patch(pmextract, 'extract', lambda *a, **k: asked.append((a, k))), \
                patch(pmextract, 'http_bytes', lambda url: url), patch(pmextract, 'latest_build', lambda: 'newest'):
            pmextract.main(['1', '2', '3', '4', '15', 'out.vtp'])
            pmextract.main(['1', '2', '3', '4', '15', 'out.vtp', 'archive'])
        self.assertEqual(asked[0], (('newest', (1.0, 2.0, 3.0, 4.0), 15, 'out.vtp'), {'keep': pmextract.slim}))
        self.assertEqual(asked[1][0][0], 'archive')

    def test_pmtiles_extract_by_bbox(self):
        src = os.path.join(self.tmp, 'planet.pmtiles')
        inside = {(0, 0, 0): b'world', (5, 20, 10): b'samara', (5, 21, 10): b'east'}
        same = {(5, 20, 11): b'sea', (5, 21, 11): b'sea'}            # identical neighbours: one entry, a run
        outside = {(5, 2, 2): b'far', (6, 41, 21): b'deeper'}     # (z6: past the max zoom asked)
        filler = {(12, x, y): b'%d,%d' % (x, y) for x in range(100, 170) for y in range(100, 175)}   # leaf dirs
        make_pmtiles(src, {**inside, **same, **outside, **filler})
        data = read(src)
        asked = []

        def get_bytes(offset, length):                              # as over HTTP: ranges, counted
            asked.append(length)
            return data[offset:offset + length]
        out = os.path.join(self.tmp, 'tiles.vtp')
        # inside z5 tiles x 20..21, y 10..11 (lon 45..67.5, lat 41.0..55.8), zooms 0..5
        pmextract.extract(get_bytes, (45.1, 41.1, 67.4, 55.7), 5, out)
        _, _, got = pack_tiles(out)
        self.assertEqual(got, {**inside, **same})                 # ungzipped, the run's tiles both, nothing else
        self.assertLess(len(asked), 12)                             # header, directories, a few merged data ranges

    def test_pmtiles_extract_parallel_with_progress(self):
        """The user 2026-10-28: the maps came 'very slowly, if at all' - each range of Protomaps' server takes seconds:
        the directories and the data fetched 8 at a time, each directory once, the data in pieces of at most kMaxGroup;
        the progress told from the start, with its stage."""
        import threading
        import time
        import unittest.mock
        src = os.path.join(self.tmp, 'planet.pmtiles')
        tiles = {(12, x, y): b'%d,%d' % (x, y) * 20 for x in range(100, 170) for y in range(100, 175)}
        make_pmtiles(src, tiles)
        data = read(src)
        asked, lock, now = [], threading.Lock(), {'in': 0, 'max': 0}

        def get_bytes(offset, length):                              # slow, as Protomaps' server: concurrency counted
            with lock:
                asked.append((offset, length))
                now['in'] += 1
                now['max'] = max(now['max'], now['in'])
            time.sleep(0.02)
            with lock:
                now['in'] -= 1
            return data[offset:offset + length]
        steps = []
        out = os.path.join(self.tmp, 'tiles.vtp')
        n = 1 << 12
        lon = lambda x: x / n * 360 - 180                           # noqa: E731
        lat = lambda y: math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))   # noqa: E731
        with unittest.mock.patch.object(pmextract, 'kMaxGroup', 4000):
            pmextract.extract(get_bytes, (lon(100.5), lat(174.5), lon(169.5), lat(100.5)), 12, out,
                              lambda part, text: steps.append((part, text)))
        _, _, got = pack_tiles(out)
        self.assertEqual({k: v for k, v in got.items() if k[0] == 12}, tiles)
        self.assertEqual(len(asked), len(set(asked)))               # each directory, each piece: once
        self.assertGreater(now['max'], 1)                           # 8 at a time
        self.assertLessEqual(now['max'], 8)
        self.assertTrue(steps[0][0] == 0 and 'каталог' in steps[0][1], steps[:2])   # told before any data
        self.assertEqual(steps[-1][0], 1.0)
        self.assertTrue(all(a[0] <= b[0] for a, b in zip(steps, steps[1:])))
        self.assertTrue(any('МБ' in t for _, t in steps))
        got_mb = [p for p, t in steps if 'МБ' in t]
        self.assertEqual(got_mb[-1], pmextract.kDataEnd)            # the data's progress up to its end, then
        work = [(p, t) for p, t in steps if 'обработка' in t]       # (the user 2026-10-29: the bar was full long
        self.assertGreater(len(work), 3)                            # before the file) the tiles' processing told
        self.assertTrue(pmextract.kDataEnd < work[0][0] and work[-1][0] < 1.0, work[:2])
        self.assertTrue(work[-1][1].endswith('из %d плиток' % len(tiles)), work[-1])
        self.assertTrue(all(l <= 4000 + 200 for o, l in asked[1:] if l > 600), asked[:5])   # the data's pieces

    def test_pmtiles_extract_through_leaf_directories(self):
        """A big region: the root directory points to leaves (Protomaps' build: always), read a level at a time,
        each once."""
        import random
        from pmtiles.tile import deserialize_header
        random.seed(1)
        src = os.path.join(self.tmp, 'big.pmtiles')
        tiles = {(15, 16000 + random.randrange(300), 10000 + random.randrange(300)): b'%d' % i for i in range(60000)}
        make_pmtiles(src, tiles)
        data = read(src)
        self.assertGreater(deserialize_header(data[:127])['leaf_directory_length'], 0)
        asked = []

        def get_bytes(offset, length):
            asked.append((offset, length))
            return data[offset:offset + length]
        n = 1 << 15
        lon = lambda x: x / n * 360 - 180                           # noqa: E731
        lat = lambda y: math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))   # noqa: E731
        out = os.path.join(self.tmp, 'big.vtp')
        steps = []
        pmextract.extract(get_bytes, (lon(16100.5), lat(10199.5), lon(16199.5), lat(10100.5)), 15, out,
                          lambda part, text: steps.append((part, text)))
        _, _, got = pack_tiles(out)
        want = {k: v for k, v in tiles.items() if 16100 <= k[1] < 16200 and 10100 <= k[2] < 10200}
        self.assertEqual(got, want)
        self.assertEqual(len(asked), len(set(asked)))
        dirs = [(p, t) for p, t in steps if 'каталог архива:' in t]   # (the user 2026-10-29: no sign of life at the
        self.assertGreater(len(dirs), 1, steps[:4])                    # start) each directory read told
        self.assertTrue(0 < dirs[0][0] and dirs[-1][0] <= pmextract.kDirsPart, dirs)
        self.assertTrue(all(a[0] <= b[0] for a, b in zip(steps, steps[1:])))

    def test_merged_bounds(self):
        """Data ranges as one fetch up to kGap apart and kMaxGroup long - not a byte more."""
        g, m = pmextract.kGap, pmextract.kMaxGroup
        self.assertEqual(len(pmextract.merged([(0, 10), (10 + g, 5)])), 1)
        self.assertEqual(len(pmextract.merged([(0, 10), (11 + g, 5)])), 2)
        self.assertEqual(pmextract.merged([(1000, m - 5), (995 + m, 5)]), [[1000, 1000 + m, [(1000, m - 5), (995 + m, 5)]]])
        self.assertEqual(len(pmextract.merged([(1000, m - 5), (995 + m, 6)])), 2)
        self.assertEqual(pmextract.merged([(100, 10), (100, 10)]), [[100, 110, [(100, 10)]]])

    def test_http_bytes_reconnects(self):
        """A connection the server closed (or that broke): opened again, the range asked again."""
        import http.server
        import threading
        body = bytes(range(256)) * 40
        conns = []

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'   # (says it keeps the connection - and drops it after each answer)

            def setup(self):
                conns.append(1)
                super().setup()

            def do_GET(self):
                a, b = map(int, self.headers['Range'].split('=')[1].split('-'))
                self.send_response(206)
                self.send_header('Content-Length', str(b + 1 - a))
                self.end_headers()
                self.wfile.write(body[a:b + 1])
                self.close_connection = True

            def log_message(self, *a):
                pass
        srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            get = pmextract.http_bytes('http://127.0.0.1:%d/p.pmtiles' % srv.server_address[1])
            self.assertEqual(get(10, 100), body[10:110])
            self.assertEqual(get(5000, 300), body[5000:5300])
            self.assertEqual(len(conns), 2)
            bad = pmextract.http_bytes('http://127.0.0.1:%d/p.pmtiles' % srv.server_address[1])
            srv.shutdown()
            srv.server_close()
            with self.assertRaises(OSError):
                bad(0, 10)
        finally:
            srv.shutdown()

    def test_http_bytes_keeps_its_connection(self):
        """Ranges over one kept connection a thread (a new HTTPS connection each was a round trip more each)."""
        import http.server
        import threading
        body = bytes(range(256)) * 40
        conns = []

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def setup(self):
                conns.append(1)
                super().setup()

            def do_GET(self):
                a, b = map(int, self.headers['Range'].split('=')[1].split('-'))
                part = body[a:b + 1]
                self.send_response(206)
                self.send_header('Content-Length', str(len(part)))
                self.end_headers()
                self.wfile.write(part)

            def log_message(self, *a):
                pass
        srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            get = pmextract.http_bytes('http://127.0.0.1:%d/p.pmtiles' % srv.server_address[1])
            self.assertEqual(get(10, 100), body[10:110])
            self.assertEqual(get(5000, 300), body[5000:5300])
            self.assertEqual(len(conns), 1)
        finally:
            srv.shutdown()

    def test_search_index(self):
        out = os.path.join(self.tmp, 'search.idx')
        make_search.build(TINY, out)
        h = struct.unpack_from(make_search.HEADER, read(out), 0)
        self.assertEqual(h[2:6], (1, 9, 2, 1))        # places, streets (the ring has no name), houses, places to go
        self.assertEqual(make_search.norm('Улица 22-го Партсъезда, ё'), 'улица 22 го партсъезда е')

    def test_search_building_address(self):
        """A building's address (a way, not a node): its house at the building's centre."""
        osm = os.path.join(self.tmp, 'building.osm')
        with open(osm, 'w', encoding='utf-8') as f:
            f.write('<?xml version="1.0" encoding="UTF-8"?><osm version="0.6">'
                    '<node id="1" lat="53.000" lon="50.000" version="1"/>'
                    '<node id="2" lat="53.000" lon="50.002" version="1"/>'
                    '<node id="3" lat="53.002" lon="50.002" version="1"/>'
                    '<way id="1" version="1"><nd ref="1"/><nd ref="2"/><nd ref="3"/>'
                    '<tag k="building" v="yes"/><tag k="addr:street" v="Главная улица"/>'
                    '<tag k="addr:housenumber" v="3"/></way></osm>')
        r = make_search.Reader()
        r.apply_file(osm, locations=True)
        self.assertEqual(len(r.addrs), 1)
        la, lo, street, number, city = r.addrs[0]
        self.assertAlmostEqual(la, 53.000667, places=5)
        self.assertAlmostEqual(lo, 50.001333, places=5)
        self.assertEqual((street, number, city), ('Главная улица', '3', ''))

    def test_region_command_line(self):
        self.assertEqual(make_region.args(['samara_oblast', '--card', 'E:']), (['samara_oblast'], 'E:', False))
        self.assertEqual(make_region.args(['a', 'b']), (['a', 'b'], None, False))
        self.assertEqual(make_region.args(['--list']), ([], None, True))
        self.assertEqual(make_region.args([]), ([], None, True))

    def test_region_update_is_repeatable(self):
        url = pathlib.Path(TINY).as_uri()
        known = {'tiny': url}
        make_region.update(['tiny'], known, card=os.path.join(self.tmp, 'card'), work=self.tmp,
                           data=os.path.join(self.tmp, 'data'))
        folder = os.path.join(self.tmp, 'data', 'tiny')
        stamp = os.path.getmtime(os.path.join(folder, 'route.grf'))
        self.assertIn(b'name=tiny', read(os.path.join(folder, 'region.txt')))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, 'card', 'OSMapsData', 'tiny', 'search.idx')))
        make_region.update(['tiny'], known, work=self.tmp, data=os.path.join(self.tmp, 'data'))   # again: no rebuild
        self.assertEqual(os.path.getmtime(os.path.join(folder, 'route.grf')), stamp)
        with self.assertRaises(SystemExit):
            make_region.update(['nowhere'], known)

    def test_region_of_two_extracts(self):
        """A region made of two extracts (Moscow inside its oblast's hole): one graph and index of both; what both
        extracts have (the border's objects) once."""
        east = os.path.join(self.tmp, 'east.osm')
        with open(east, 'w', encoding='utf-8') as f:     # node 1: tiny's own (the shared border), a road to the east
            f.write('<?xml version="1.0" encoding="UTF-8"?><osm version="0.6">'
                    '<node id="1" lat="53.000" lon="50.000" version="1"/>'
                    '<node id="901" lat="53.000" lon="50.100" version="1"/>'
                    '<way id="901" version="1"><nd ref="1"/><nd ref="901"/><tag k="highway" v="primary"/></way></osm>')
        urls = [pathlib.Path(TINY).as_uri(), pathlib.Path(east).as_uri()]
        folder = make_region.build('two', urls, self.tmp, os.path.join(self.tmp, 'data'))
        lon0, _, lon1, _ = release_regions.graph_bbox(os.path.join(folder, 'route.grf'))
        self.assertTrue(lon0 < 50.001 and lon1 > 50.099, (lon0, lon1))     # both extracts' roads
        info = release_regions.region_info(os.path.join(folder, 'region.txt'))
        self.assertEqual(info['source'], ' '.join(urls))
        self.assertEqual(make_region.source_file('w', 'n', 'http://h/st.petersburg-latest.osm.pbf'),   # (its suffix only)
                         os.path.join('w', 'n.osm.pbf'))

    def test_region_tiles_with_planetiler(self):
        work, data = os.path.join(self.tmp, 'work'), os.path.join(self.tmp, 'data')
        os.makedirs(os.path.join(work, 'data', 'sources'))
        for name in make_region.SOURCES:                  # already fetched: nothing downloaded
            open(os.path.join(work, 'data', 'sources', name), 'wb').close()
        open(os.path.join(work, 'planetiler.jar'), 'wb').close()
        asked = []

        def planetiler(argv):                             # writes the MBTiles it is asked for, as planetiler does
            asked.append(argv)
            out = [a[len('--output='):] for a in argv if a.startswith('--output=')][0]
            make_mbtiles(out, {(14, 10467, 5226): b'pbf'})
        make_region.build('tiny', pathlib.Path(TINY).as_uri(), work, data, run=planetiler)
        _, _, got = pack_tiles(os.path.join(data, 'tiny', 'tiles.vtp'))
        self.assertEqual(got, {(14, 10467, 5226): b'pbf'})
        self.assertIn('--maxzoom=14', asked[0])
        self.assertFalse(os.path.exists(os.path.join(data, 'tiny', 'tiles.mbtiles')))   # the go-between removed
        self.assertIsNone(make_region.tile_builder(self.tmp))                           # no planetiler: no tiles

    def test_region_links(self):
        top = '<a href="volga_federal_district/">v</a><a href="ural_federal_district/">u</a>'
        page = '<a href="samara_oblast-latest.osm.pbf">s</a><a href="x.txt">x</a>'
        self.assertEqual(make_region.district_links(top), ['ural_federal_district', 'volga_federal_district'])
        self.assertEqual(make_region.region_links(page, 'B/'), {'samara_oblast': 'B/samara_oblast-latest.osm.pbf'})
        pages = {make_region.BASE: top, make_region.BASE + 'volga_federal_district/': page,
                 make_region.BASE + 'ural_federal_district/': ''}
        russia = lambda got: sorted(n for n in got if n not in make_region.COUNTRIES)   # noqa: E731
        self.assertEqual(russia(make_region.regions(get=pages.get)), ['crimea', 'donetsk', 'luhansk', 'samara_oblast'])
        pages[make_region.BASE + 'ural_federal_district/'] = '<a href="crimea_republic-latest.osm.pbf">c</a>'
        got = make_region.regions(get=pages.get)            # the disputed lands apart, by neutral names, whole
        self.assertEqual(russia(got), ['crimea', 'donetsk', 'luhansk', 'samara_oblast'])
        self.assertEqual(got['donetsk'], make_region.APART['donetsk'])
        self.assertEqual([u.rsplit('/', 1)[1] for u in got['crimea']],      # Sevastopol in Crimea, one region
                         ['crimea-latest.osm.pbf', 'sevastopol-latest.osm.pbf'])
        pages[make_region.BASE + 'ural_federal_district/'] = ('<a href="moscow-latest.osm.pbf">m</a>'
                                                              '<a href="moscow_oblast-latest.osm.pbf">o</a>')
        got = make_region.regions(get=pages.get)            # Moscow: inside its oblast (the oblast's extract: holes)
        self.assertNotIn('moscow', got)
        pages[make_region.BASE + 'ural_federal_district/'] = '<a href="saint_petersburg-latest.osm.pbf">p</a>'
        self.assertNotIn('saint_petersburg', make_region.regions(get=pages.get))   # (in leningrad_oblast's already)
        self.assertEqual(got['moscow_oblast'], [make_region.BASE + 'ural_federal_district/moscow_oblast-latest.osm.pbf',
                                                make_region.BASE + 'ural_federal_district/moscow-latest.osm.pbf'])

    def test_cis_regions(self):
        """The user 2026-10-29: the CIS too - Ukraine by its oblasts (openstreetmap.fr; Crimea, Donetsk, Luhansk stay
        apart, Kyiv inside its oblast), the other countries whole (Geofabrik); each with a Russian title."""
        ua = make_region.UKRAINE_DIR
        pages = {make_region.BASE: '', ua: ''.join('<a href="%s-latest.osm.pbf">x</a>' % n for n in (
            'lviv_oblast', 'kiev', 'kiev_oblast', 'crimea', 'sevastopol', 'donetsk_oblast', 'luhansk_oblast'))}
        got = make_region.regions(get=pages.get)
        self.assertEqual(got['lviv_oblast'], ua + 'lviv_oblast-latest.osm.pbf')
        self.assertEqual(got['kiev_oblast'], [ua + 'kiev_oblast-latest.osm.pbf', ua + 'kiev-latest.osm.pbf'])
        for gone in ('kiev', 'sevastopol', 'donetsk_oblast', 'luhansk_oblast'):
            self.assertNotIn(gone, got)
        self.assertEqual(got['crimea'], make_region.APART['crimea'])
        self.assertEqual(got['belarus'], 'https://download.geofabrik.de/europe/belarus-latest.osm.pbf')
        self.assertEqual(got['kazakhstan'], 'https://download.geofabrik.de/asia/kazakhstan-latest.osm.pbf')
        self.assertEqual(len(make_region.COUNTRIES), 10)
        for name in got:
            self.assertNotEqual(release_regions.title(name), name, name)   # every one titled


if __name__ == '__main__':
    unittest.main()
