"""The region tools on the test map (tiny.osm): what OSMapsCE reads from their files, and the update's steps.

    python -m unittest discover -s offline/test      (quality.bat runs it under coverage)
"""
import gzip
import io
import json
import math
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from urllib.parse import parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import make_graph    # noqa: E402
import make_layers   # noqa: E402
import make_parklines   # noqa: E402
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


def limits(path):
    """Each edge's (name, speed limit) in the edges' order, from the 'OSMS' part after 'OSMB'; None: no such part."""
    d = read(path)
    h = struct.unpack_from(make_graph.HEADER, d, 0)
    n_edges, off = h[3], h[-6:]
    gw, gh, items = h[10], h[11], h[13]
    end = off[5] + (gw * gh + 1 + items) * 4
    assert d[end:end + 4] == b'OSMB'
    end += 4 + (n_edges * 2 + 3) // 4 * 4   # (to a multiple of 4: read in place on the unit)
    if d[end:end + 4] != b'OSMS':
        return None
    out = []
    for e in range(n_edges):
        nm = struct.unpack_from('<I', d, off[2] + e * 24 + 20)[0]
        out.append((d[off[4] + nm:d.index(b'\0', off[4] + nm)].decode().split('\t')[0], d[end + 4 + e]))
    return out


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
        self.assertEqual(len(nodes), 18)              # the junctions and the ends (not the curves' inner points)
        self.assertEqual(len(edges), 33)              # 13 two-way pieces, 2 one-way, the roundabout's 5
        self.assertEqual(struct.unpack_from(make_graph.HEADER, read(out), 0)[4], 3)   # the curves' inner points
        ring = [(a, b) for a, b, _, _, n in edges if n == '']
        self.assertEqual(len(ring), 5)                # the roundabout: one way round (r5: the south street again)
        one = [(a, b) for a, b, _, _, n in edges if n == 'Односторонняя улица']
        self.assertEqual(one, [((53000000, 50004000), (53002000, 50004000))])   # north only
        main = [dm for a, b, dm, _, n in edges if n == 'Главная улица' and a == (53000000, 50000000)]
        self.assertTrue(1300 < main[0] < 1400)        # ~134 m, in decimetres

    def test_speed_limit_of_tags(self):
        # the speed limit sign (design v3): the posted limit only - a number, mph, or a country's code for a kind of
        # road; 0 = not known (the class's typical speed is not a limit)
        lim = make_graph.limit_of
        self.assertEqual(lim({'maxspeed': '60'}), 60)
        self.assertEqual(lim({'maxspeed': '40 mph'}), 64)
        self.assertEqual(lim({'maxspeed': 'RU:urban'}), 60)
        self.assertEqual(lim({'maxspeed': 'RU:rural'}), 90)
        self.assertEqual(lim({'maxspeed': 'RU:motorway'}), 110)
        self.assertEqual(lim({'maxspeed': 'RU:living_street'}), 20)
        self.assertEqual(lim({'maxspeed': 'BY:urban'}), 60)
        self.assertEqual(lim({'maxspeed:type': 'RU:urban'}), 60)
        self.assertEqual(lim({'source:maxspeed': 'RU:rural'}), 90)
        self.assertEqual(lim({'zone:maxspeed': 'RU:20'}), 20)
        self.assertEqual(lim({'maxspeed': '50', 'maxspeed:type': 'RU:urban'}), 50)   # the sign's number wins
        self.assertEqual(lim({}), 0)
        self.assertEqual(lim({'maxspeed': 'none'}), 0)
        self.assertEqual(lim({'maxspeed': 'signals'}), 0)
        self.assertEqual(lim({'maxspeed': '300'}), 0)                                  # not a real one
        self.assertEqual(lim({'maxspeed': 'RU:unknown'}), 0)

    def test_graph_speed_limits(self):
        out = os.path.join(self.tmp, 'route.grf')
        make_graph.build(TINY, out)
        got = limits(out)
        self.assertIsNotNone(got)
        _, edges = graph(out)
        self.assertEqual(len(got), len(edges))
        self.assertEqual({v for n, v in got if n == 'Главная улица'}, {60})       # maxspeed=60
        self.assertEqual({v for n, v in got if n == 'Боковая улица'}, {60})       # RU:urban
        self.assertEqual({v for n, v in got if n not in ('Главная улица', 'Боковая улица')}, {0})

    def test_graph_nodes_by_place(self):
        # the user's Shturmann 2026-10-31: a 115 km way read most of the 96 MB graph into memory - its nodes numbered in
        # the order met, a place's neighbours all over the file. Numbered along a Hilbert curve: near on the map, near
        # in the file; the pieces after their first node, so their points and edges near too
        far, near = (55000000, 37000000), (55000100, 37000100)
        nodes = [far, (54000000, 36000000), near, (54000050, 36000050)]
        P = make_graph.Piece
        pieces = [P(0, 1, [], 1, 0, 0, False, 50, 0, 0, 0, False), P(2, 0, [], 1, 0, 0, False, 50, 0, 0, 0, False),
                  P(3, 1, [], 1, 0, 0, False, 50, 0, 0, 0, False)]
        pts, kept = make_graph.renumbered(nodes, pieces)
        self.assertEqual(sorted(pts), sorted(nodes))
        self.assertEqual(abs(pts.index(far) - pts.index(near)), 1)   # (met first and third)
        self.assertEqual(abs(pts.index(nodes[1]) - pts.index(nodes[3])), 1)
        self.assertEqual({(pts[p.a], pts[p.b]) for p in kept}, {(far, nodes[1]), (near, far), (nodes[3], nodes[1])})
        self.assertEqual([min(p.a, p.b) for p in kept], sorted(min(p.a, p.b) for p in kept))
        self.assertLess(make_graph.hilbert(0, 0, 4), make_graph.hilbert(0, 1, 4))
        self.assertEqual(sorted(make_graph.hilbert(x, y, 2) for x in range(4) for y in range(4)), list(range(16)))
        self.assertEqual([make_graph.hilbert(x, y, 1) for x, y in ((0, 0), (0, 1), (1, 1), (1, 0))], [0, 1, 2, 3])

    def layers(self, path):
        """As the unit reads layers.lay: the header, the sorted cells, each one's points."""
        d = read(path)
        magic, version, n_cells, n_points = struct.unpack_from(make_layers.HEADER, d, 0)
        self.assertEqual((magic, version), (b'OSML', 1))
        size = struct.calcsize(make_layers.HEADER)
        cells = [struct.unpack_from('<II', d, size + i * 8) for i in range(n_cells)]
        at = size + n_cells * 8
        self.assertEqual(len(d), at + n_points * 12)
        self.assertEqual([c[0] for c in cells], sorted(c[0] for c in cells))
        out = {}
        for i, (key, first) in enumerate(cells):
            last = cells[i + 1][1] if i + 1 < n_cells else n_points
            out[key] = [struct.unpack_from('<iiBBH', d, at + k * 12) for k in range(first, last)]
        return out

    def test_layers_what_is_kept(self):
        """Traffic lights, speed bumps, car parks (the user 2026-11-06): where, what, is it paid; the others left out."""
        self.assertEqual([make_layers.kind_of(t) for t in (
            {'highway': 'traffic_signals'}, {'highway': 'crossing'}, {'traffic_calming': 'bump'}, {'traffic_calming': 'hump'},
            {'traffic_calming': 'table'}, {'traffic_calming': 'cushion'}, {'traffic_calming': 'choker'}, {'traffic_calming': 'island'},
            {'amenity': 'parking'}, {'amenity': 'parking', 'access': 'private'}, {'amenity': 'parking', 'access': 'no'},
            {'amenity': 'parking', 'access': 'customers'}, {'amenity': 'fuel'}, {})],
            [make_layers.SIGNAL, None, make_layers.BUMP, make_layers.BUMP, make_layers.BUMP, make_layers.BUMP, None, None,
             make_layers.PARKING, None, None, make_layers.PARKING, None, None])
        self.assertEqual([make_layers.fee_of(t) for t in (
            {}, {'fee': 'yes'}, {'fee': 'no'}, {'fee': 'no', 'fee:conditional': 'yes @ (Mo-Fr 08:00-21:00)'},
            {'fee:conditional': 'yes @ (Mo-Fr)'}, {'fee': 'interval'}, {'fee': '200 руб/час'})],
            [make_layers.FEE_UNKNOWN, make_layers.FEE_PAID, make_layers.FEE_FREE, make_layers.FEE_PAID, make_layers.FEE_PAID,
             make_layers.FEE_UNKNOWN, make_layers.FEE_PAID])
        self.assertEqual([make_layers.places_of(t) for t in (
            {}, {'capacity': '12'}, {'capacity': '~30'}, {'capacity': '70000'}, {'capacity': '-1'}, {'capacity': '0'})],
            [0, 12, 0, 65535, 0, 0])

    def test_layers_file(self):
        osm = os.path.join(self.tmp, 'layers.osm')
        with open(osm, 'w', encoding='utf-8') as f:
            f.write('''<?xml version="1.0" encoding="UTF-8"?><osm version="0.6" generator="t">
<node id="1" lat="53.0000" lon="50.0000" version="1"><tag k="highway" v="traffic_signals"/></node>
<node id="2" lat="53.0050" lon="50.0050" version="1"><tag k="traffic_calming" v="bump"/></node>
<node id="3" lat="53.0200" lon="50.0000" version="1"><tag k="amenity" v="parking"/><tag k="fee" v="yes"/><tag k="capacity" v="40"/></node>
<node id="4" lat="53.0400" lon="50.0000" version="1"><tag k="amenity" v="fuel"/></node>
<node id="5" lat="53.0400" lon="50.0400" version="1"/><node id="6" lat="53.0400" lon="50.0420" version="1"/>
<node id="7" lat="53.0420" lon="50.0420" version="1"/><node id="8" lat="53.0420" lon="50.0400" version="1"/>
<way id="1" version="1"><nd ref="5"/><nd ref="6"/><nd ref="7"/><nd ref="8"/><nd ref="5"/><tag k="amenity" v="parking"/><tag k="fee" v="no"/></way>
<way id="2" version="1"><nd ref="5"/><nd ref="6"/><tag k="traffic_calming" v="hump"/></way>
<way id="3" version="1"><nd ref="5"/><nd ref="7"/><tag k="highway" v="residential"/></way>
</osm>''')
        out = os.path.join(self.tmp, 'layers.lay')
        make_layers.build(osm, out)
        cells = self.layers(out)
        key = make_layers.cell_of(53000000, 50000000)
        self.assertEqual(key, (14300 << 16) | 23000)
        self.assertEqual(sorted(cells[key]), [(53000000, 50000000, make_layers.SIGNAL, 0, 0),
                                              (53005000, 50005000, make_layers.BUMP, 0, 0)])
        self.assertEqual(cells[make_layers.cell_of(53020000, 50000000)],
                         [(53020000, 50000000, make_layers.PARKING, make_layers.FEE_PAID, 40)])
        pts = [p for c in cells.values() for p in c]
        self.assertEqual(len(pts), 5)   # (the fuel station and the road not; the closed outline's first node once)
        park = [p for p in pts if p[3] == make_layers.FEE_FREE]
        self.assertEqual(len(park), 1)
        self.assertEqual(park[0][:2], (53041000, 50041000))
        self.assertIn((53040000, 50041000, make_layers.BUMP, 0, 0), pts)

    def test_layers_empty_region(self):
        osm = os.path.join(self.tmp, 'none.osm')
        with open(osm, 'w', encoding='utf-8') as f:
            f.write('<?xml version="1.0" encoding="UTF-8"?><osm version="0.6" generator="t"></osm>')
        out = os.path.join(self.tmp, 'none.lay')
        make_layers.build(osm, out)
        self.assertEqual(self.layers(out), {})

    def test_layers_format(self):
        """What layers.h reads: the numbers are the file's, the cells' order holds whichever way the points lie."""
        self.assertEqual((make_layers.SIGNAL, make_layers.BUMP, make_layers.PARKING), (1, 2, 3))
        self.assertEqual((make_layers.FEE_UNKNOWN, make_layers.FEE_FREE, make_layers.FEE_PAID), (0, 1, 2))
        self.assertEqual(make_layers.cell_of(53009999, 50009999), (14300 << 16) | 23000)
        out = os.path.join(self.tmp, 'order.lay')
        make_layers.write([(53020000, 50000000, 1, 0, 0), (53000000, 50020000, 1, 0, 0), (53000100, 50010000, 1, 0, 0),
                           (53000200, 50000000, 1, 0, 0)], out)
        self.assertEqual(len(self.layers(out)), 4)

    def test_parklines_prices(self):
        """A car's rubles an hour, in tens: weekdays, the weekend x before 8, 8-21, after 21. The other vehicles and the
        holidays not read; a differentiated tariff by its price after the first minutes; of two rates the higher (never
        less than it may cost); a price not in tens up."""
        kCar = 'Легковой автомобиль'
        def row(period, hours, price=None, rest=None, car=kCar):
            return {'TariffPeriod': period, 'TimeRange': hours, 'HourPrice': price, 'RestOfTheDayPrice': rest, 'VehicleTypeForThisTariff': car}
        self.assertEqual(make_parklines.prices_of([]), (0, 0, 0, 0, 0, 0))
        self.assertEqual(make_parklines.prices_of([row('будни', 'круглосуточно', 40), row('выходные дни', 'круглосуточно', 0)]), (4, 4, 4, 0, 0, 0))
        self.assertEqual(make_parklines.prices_of([row('будни', '00:00-08:00', 60), row('будни', '08:00-21:00', 80), row('будни', '21:00-23:59', 70),
                                                   row('выходные дни', '00:00-08:00', 10), row('выходные дни', '08:00-21:00', 20), row('выходные дни', '21:00-23:59', 30)]),
                         (6, 8, 7, 1, 2, 3))
        self.assertEqual(make_parklines.prices_of([row('будни', '08:00-21:00', None, 150)]), (0, 15, 0, 0, 0, 0))
        self.assertEqual(make_parklines.prices_of([row('будни', '08:00-21:00', 380), row('будни', '08:00-21:00', 450), row('будни', '08:00-21:00', 200)]), (0, 45, 0, 0, 0, 0))
        self.assertEqual(make_parklines.prices_of([row('будни', '08:00-21:00', 41)]), (0, 5, 0, 0, 0, 0))
        self.assertEqual(make_parklines.prices_of([row('будни', '08:00-21:00', 9999)]), (0, 255, 0, 0, 0, 0))
        self.assertEqual(make_parklines.prices_of([row('будни', '08:00-21:00', None, None)]), (0, 0, 0, 0, 0, 0))
        self.assertEqual(make_parklines.prices_of([row('будни', 'круглосуточно', 600, None, 'Мотоцикл'), row('праздничные дни', 'круглосуточно', 900)]), (0, 0, 0, 0, 0, 0))

    def test_parklines_file(self):
        """The test feed: a line of two points, of three, one with an empty piece beside a real one; the bytes as layers.cpp reads them."""
        with open(os.path.join(HERE, 'parklines.json'), encoding='utf-8') as f:
            features = json.load(f)
        segs = make_parklines.segments_of(features)
        self.assertEqual(segs, [(53000500, 50001000, 0, 1000, 4, 4, 4, 0, 0, 0),
                                (53001000, 50011000, 0, 1000, 20, 38, 20, 0, 45, 0),
                                (53001000, 50012000, 200, 500, 20, 38, 20, 0, 45, 0),
                                (53010500, 50001000, 0, 1000, 0, 15, 0, 0, 0, 0)])
        out = os.path.join(self.tmp, 'parking.lay')
        make_parklines.write(segs, out)
        d = read(out)
        self.assertEqual(struct.unpack_from(make_layers.HEADER, d, 0), (b'OSMP', 1, 3, 4))   # (the two east pieces in one cell)
        cells = [struct.unpack_from('<II', d, 16 + i * 8) for i in range(3)]
        self.assertEqual(cells, [((14300 << 16) | 23000, 0), ((14300 << 16) | 23001, 1), ((14301 << 16) | 23000, 3)])
        at = 16 + 3 * 8
        self.assertEqual(len(d), at + 4 * 20)
        self.assertEqual(struct.unpack_from('<iihh6B2x', d, at), segs[0])

    def test_parklines_cell_by_the_middle(self):
        """A piece goes to the cell of its middle: cell_of of the point half way, whichever way it runs."""
        self.assertEqual(make_parklines.cell_key((53009000, 50000000, 2000, 0)), make_layers.cell_of(53010000, 50000000))
        self.assertEqual(make_parklines.cell_key((53000000, 50000000, 0, 2000)), make_layers.cell_of(53000000, 50001000))
        self.assertEqual(make_parklines.cell_key((53010000, 50001000, -2000, -2000)), make_layers.cell_of(53009000, 50000000))

    def test_parklines_download(self):
        """All the feed's pages, up to an empty one; a page again (the server's end) stops it, each once."""
        pages = {0: [{'id': 1}, {'id': 2}], 2: [{'id': 3}], 3: []}
        self.assertEqual(make_parklines.download(lambda skip: pages[skip]), [{'id': 1}, {'id': 2}, {'id': 3}])
        self.assertEqual(make_parklines.download(lambda skip: [{'id': 1}]), [{'id': 1}])

    def test_parklines_portal(self):
        """The key from .mosapi, a page by its offset, gzip or not; a failure never shows the url (the key is in it)."""
        class Reply(io.BytesIO):
            headers = {}

        class Zipped(Reply):
            headers = {'Content-Encoding': 'gzip'}

        class Refused(OSError):
            code = 403
        asked = []

        def portal(request, timeout):
            asked.append((request, timeout))
            return Zipped(gzip.compress(b'{"features": [{"id": 1}]}')) if len(asked) == 1 else Reply(b'{"features": []}')
        pathlib.Path(self.tmp, '.mosapi').write_text('KEY\n', encoding='utf-8')
        with mock.patch.object(make_parklines, 'HERE', os.path.join(self.tmp, 'offline')), mock.patch('urllib.request.urlopen', portal):
            self.assertEqual(make_parklines.features_from(['make_parklines.py', 'out.lay']), [{'id': 1}])
        request, timeout = asked[1]
        self.assertTrue(request.full_url.startswith(make_parklines.kUrl))
        self.assertEqual(parse_qs(urlsplit(request.full_url).query), {'$top': ['1000'], '$skip': ['1'], 'api_key': ['KEY']})
        self.assertEqual((request.get_header('Accept-encoding'), timeout), ('gzip', 300))
        with mock.patch('urllib.request.urlopen', side_effect=Refused), self.assertRaises(SystemExit) as failed:
            make_parklines.mos_page('KEY')(7)
        self.assertEqual(str(failed.exception), 'data.mos.ru: Refused 403 at 7')

    def test_parklines_command(self):
        """On a copy of the feed: the tool's arguments confused must not write over the test's source."""
        script = os.path.join(HERE, '..', 'make_parklines.py')
        src, out = os.path.join(self.tmp, 'feed.json'), os.path.join(self.tmp, 'cmd.lay')
        shutil.copy(os.path.join(HERE, 'parklines.json'), src)
        self.assertEqual(subprocess.run([sys.executable, script, out, src], stdout=subprocess.DEVNULL).returncode, 0)
        self.assertEqual(struct.unpack_from(make_layers.HEADER, read(out), 0)[3], 4)
        self.assertNotEqual(subprocess.run([sys.executable, script], stderr=subprocess.DEVNULL).returncode, 0)
        # without a feed it goes to the portal: here a copy of the tool with no .mosapi above it
        tool = os.path.join(self.tmp, 'tool')
        os.mkdir(tool)
        shutil.copy(script, tool)
        run = subprocess.run([sys.executable, os.path.join(tool, 'make_parklines.py'), out], capture_output=True, text=True,
                             env=dict(os.environ, PYTHONPATH=os.path.dirname(script)))
        self.assertIn('FileNotFoundError', run.stderr)

    def test_layers_command(self):
        """The line make_region's steps and test_offline.bat run."""
        script = os.path.join(HERE, '..', 'make_layers.py')
        osm, out = os.path.join(HERE, 'layers.osm'), os.path.join(self.tmp, 'cmd.lay')
        self.assertEqual(subprocess.run([sys.executable, script, osm, out]).returncode, 0)
        self.assertEqual(sum(len(c) for c in self.layers(out).values()), 7)
        self.assertNotEqual(subprocess.run([sys.executable, script, osm], stderr=subprocess.DEVNULL).returncode, 0)

    def test_graph_barriers(self):
        """The barriers on roads (the user 2026-10-29: a route through a hospital's grounds, closed by gates): a gate
        with no access tag - 10 minutes more (no way through, unless going there), a lift gate too (the user
        2026-10-31: a route through grounds behind two of them - only into them, never across), a block, a bollard, a
        private gate - no way past (the road cut there), a toll booth, a cattle grid, an open lift gate - passed."""
        self.assertEqual([make_graph.barrier_of(t) for t in (
            {}, {'barrier': 'gate'}, {'barrier': 'gate', 'access': 'private'}, {'barrier': 'gate', 'access': 'yes'},
            {'barrier': 'lift_gate'}, {'barrier': 'block'}, {'barrier': 'bollard'}, {'barrier': 'toll_booth'},
            {'barrier': 'cattle_grid'}, {'barrier': 'entrance'}, {'barrier': 'lift_gate', 'motor_vehicle': 'no'},
            {'barrier': 'lift_gate', 'access': 'yes'})],
            [None, 'gate', 'locked', None, 'gate', 'block', 'block', None, None, None, 'locked', None])
        # a gate closed by its access tag: passed for much more (the user 2026-11-08: a place behind one - the route
        # ended at it); a block, a bollard closed so: still no way past
        self.assertEqual(make_graph.barrier_of({'barrier': 'bollard', 'access': 'no'}), 'block')
        osm = os.path.join(self.tmp, 'barriers.osm')
        nodes = {1: (53.1, 50.000), 2: (53.1, 50.002), 3: (53.1, 50.004), 4: (53.1, 50.006),
                 5: (53.101, 50.002), 6: (53.102, 50.002), 7: (53.101, 50.004), 8: (53.102, 50.004),
                 9: (53.1, 50.008), 10: (53.101, 50.008), 11: (53.102, 50.008)}
        tags = {5: {'barrier': 'gate'}, 7: {'barrier': 'block'}, 10: {'barrier': 'gate', 'access': 'private'}}
        ways = [([1, 2, 3, 4, 9], 'Главная'), ([2, 5, 6], 'Воротная'), ([3, 7, 8], 'Блочная'), ([6, 8], 'Верхняя'),
                ([9, 10, 11], 'Дачная')]
        with open(osm, 'w', encoding='utf-8') as f:
            f.write('<?xml version="1.0" encoding="UTF-8"?>\n<osm version="0.6">\n')
            for i, (la, lo) in nodes.items():
                f.write('<node id="%d" lat="%s" lon="%s" version="1">%s</node>\n' % (i, la, lo, ''.join(
                    '<tag k="%s" v="%s"/>' % kv for kv in tags.get(i, {}).items())))
            for k, (ids, name) in enumerate(ways, 1):
                f.write('<way id="%d" version="1">%s<tag k="highway" v="residential"/><tag k="name" v="%s"/></way>\n'
                        % (k, ''.join('<nd ref="%d"/>' % i for i in ids), name))
            f.write('</osm>\n')
        out = os.path.join(self.tmp, 'barriers.grf')
        make_graph.build(osm, out)
        got, edges = graph(out)
        gate = [ds for a, b, dm, ds, n in edges if n == 'Воротная']
        plain = [ds for a, b, dm, ds, n in edges if n == 'Блочная']
        self.assertEqual(len(gate), 2)
        self.assertTrue(all(ds > 6000 for ds in gate) and all(ds < 1000 for ds in plain), (gate, plain))
        locked = [ds for a, b, dm, ds, n in edges if n == 'Дачная']
        self.assertEqual(len(locked), 2)                            # passed both ways, not cut
        self.assertTrue(all(ds > 3 * 6000 for ds in locked), locked)   # three gates' worth: never a way through
        at7 = (53101000, 50004000)
        self.assertEqual(got.count(at7), 2)                         # the block: each side's own end there
        self.assertFalse([1 for a, b, _, _, n in edges if n == 'Блочная' and a == at7 and b == at7])
        sides = {b for a, b, _, _, n in edges if n == 'Блочная' and a == at7}
        self.assertEqual(sides, {(53100000, 50004000), (53102000, 50004000)})   # up to it from both, no further

    def test_graph_toll(self):
        """A toll road's edges flagged (8): the unit keeps off them when asked (the user 2026-10-29)."""
        import make_test_toll
        osm, out = os.path.join(self.tmp, 'toll.osm'), os.path.join(self.tmp, 'toll.grf')
        with open(osm, 'w', encoding='utf-8') as f:
            f.write(make_test_toll.osm())
        make_graph.build(osm, out)
        flags = {}
        d = read(out)
        h = struct.unpack_from(make_graph.HEADER, d, 0)
        for e in range(h[3]):
            to, dm, ds, g, ng, cls, fl, nm = struct.unpack_from('<IIIIHBBI', d, h[-6:][2] + e * 24)
            name = d[h[-6:][4] + nm:d.index(b'\0', h[-6:][4] + nm)].decode().split('\t')[0]
            flags.setdefault(name, set()).add(fl & 8)
        self.assertEqual(flags, {'Платная дорога': {8}, 'Объезд': {0}, 'Тупик': {0}, 'Дальняя платная': {8}})
        self.assertTrue(make_graph.toll_of({'toll': 'yes'}) and make_graph.toll_of({'toll:motorcar': 'yes'}))
        self.assertFalse(make_graph.toll_of({'toll': 'no'}) or make_graph.toll_of({}))

    def test_graph_bearings(self):
        """Each edge's way out of its start and into its end (the user 2026-11-09, Khodynka: a turn's cost by the
        roads' true directions, not the nodes' chord): appended after the grid, 'OSMB' and 2 bytes an edge
        (256 = a full circle), the old readers' header untouched."""
        import make_test_hook
        osm, out = os.path.join(self.tmp, 'hook.osm'), os.path.join(self.tmp, 'hook.grf')
        with open(osm, 'w', encoding='utf-8') as f:
            f.write(make_test_hook.osm())
        make_graph.build(osm, out)
        d = read(out)
        h = struct.unpack_from(make_graph.HEADER, d, 0)
        self.assertEqual(h[1], 1)                                     # the version: the old programs still read it
        n_nodes, n_edges, off = h[2], h[3], h[-6:]
        end = off[5] + (h[10] * h[11] + 1 + h[13]) * 4
        self.assertEqual(d[end:end + 4], b'OSMB')
        lim = end + 4 + (n_edges * 2 + 3) // 4 * 4   # the speed limits after them (2026-11-11), 4-byte aligned
        self.assertEqual(d[lim:lim + 4], b'OSMS')
        self.assertEqual(len(d), lim + 4 + n_edges)
        nodes = [struct.unpack_from('<ii', d, off[0] + i * 8) for i in range(n_nodes)]
        first = struct.unpack_from('<%dI' % (n_nodes + 1), d, off[1])
        bear = {}
        for a in range(n_nodes):
            for e in range(first[a], first[a + 1]):
                to = struct.unpack_from('<I', d, off[2] + e * 24)[0]
                bear[(nodes[a], nodes[to])] = (d[end + 4 + e * 2], d[end + 5 + e * 2])

        def at(name):
            la, lo = make_test_hook.lat_lon(name)
            return round(la * 1e6), round(lo * 1e6)
        self.assertEqual(bear[(at('W'), at('A'))], (64, 64))          # east, straight: 90 degrees out and in
        self.assertEqual(bear[(at('A'), at('B'))], (30, 208))         # the loop: off A 42 degrees (20 m on: its first
        #                                                               metres east hide it), into B 292 (west)
        self.assertEqual(bear[(at('N'), at('W'))], (219, 128))        # backwards (its points read from the end): off N
        #                                                               to Z 307 degrees, into W from Z - south
        self.assertEqual(make_graph.bearing_byte((64000000, 50000000), (64000000, 49990000)), 192)   # west
        self.assertEqual(make_graph.bearing_byte((64000000, 50000000), (63990000, 50022800)), 96)    # south-east
        w, a = at('W'), at('A')
        self.assertEqual(make_graph.point_along([w, a]), (64000000, 49996307))   # 20 m on (the longitude's metres)
        self.assertEqual(make_graph.point_along([a, at('B1')]), at('B1'))         # shorter: its end
        self.assertEqual(make_graph.bearing_byte((64000000, 50000000), (64001000, 49999609)), 249)   # 350.3 degrees
        short = [(64000000, 50000000), (64000050, 50000000), (64000100, 50000000)]   # 11 m in all
        self.assertEqual(make_graph.point_along(short), short[-1])

    def test_graph_turn_rules(self):
        """Turn restrictions (the user 2026-11-09, Khodynka's "only straight on"): the restricted way in reaches a copy
        of its via node with only the turns allowed - the file and the router as they were."""
        import make_test_hook
        osm, out = os.path.join(self.tmp, 'hook.osm'), os.path.join(self.tmp, 'hook.grf')
        with open(osm, 'w', encoding='utf-8') as f:
            f.write(make_test_hook.osm())
        make_graph.build(osm, out)
        nodes, edges = graph(out)

        def at(name):   # (as the file has it: 7 decimals, then 1e-6 degrees)
            la, lo = make_test_hook.lat_lon(name)
            return round(round(la, 7) * 1e6), round(round(lo, 7) * 1e6)
        self.assertEqual(nodes.count(at('B')), 2)                     # B and its copy for the loop's way in
        outs = {}
        for a, b, _, _, name in edges:
            if a == at('B'):
                outs.setdefault(name, []).append(b)
        self.assertEqual(sorted(outs['Улица Крюк']), sorted([at('M0'), at('X')] * 2))   # B and its copy
        self.assertEqual(outs['Улица Ветка'], [at('T')])              # (by Y) B's only: not from the copy
        ins = [name for a, b, _, _, name in edges if b == at('B') and a == at('A')]
        self.assertEqual(ins, [''])                                   # the loop in: once (into the copy)
        P = make_graph.Piece   # the rules on lists as directed() makes them: node 1 -> 0 by way 1; 0 -> 2 way 2, 0 -> 3 way 3
        pieces = [P(1, 0, [], 1, 0, 1, False, 50, 0, 0, 0, False, wid) for wid in (1, 2, 3)]

        def graph_of():
            e = lambda to, pi: (to, 10, 10, 0, 0, 0, 0, 0, pi)   # noqa: E731
            return [(0, 0), (0, 1000), (1000, 0), (0, -1000)], [[e(2, 1), e(3, 2)], [e(0, 0)], [], []]
        for kind, left in (('only', 2), ('no', 3)):
            nodes, lst = graph_of()
            self.assertEqual(make_graph.turn_rules(nodes, lst, pieces, [(kind, {1}, 100, {2})], {100: (0, 0)}), 1)
            self.assertEqual((len(nodes), nodes[4], lst[1][0][0]), (5, (0, 0), 4))   # the way in: to the copy
            self.assertEqual([e[0] for e in lst[4]], [left])        # only: onto way 2 alone; no: all but way 2
            self.assertEqual([e[0] for e in lst[0]], [2, 3])        # the node itself: as it was
        nodes, lst = graph_of()
        self.assertEqual(make_graph.turn_rules(nodes, lst, pieces, [('no', {1}, 7, {2}), ('no', {1}, 100, {9}),
                                                                    ('no', {5}, 100, {2})], {100: (0, 0)}), 0)
        self.assertEqual(len(nodes), 4)                              # no via, no "to" way, no "from" way in: none
        self.assertEqual(make_graph.turn_rule({'type': 'restriction', 'restriction': 'only_straight_on'}), 'only')
        self.assertEqual(make_graph.turn_rule({'type': 'restriction', 'restriction:motorcar': 'no_left_turn'}), 'no')
        self.assertIsNone(make_graph.turn_rule({'type': 'restriction', 'restriction': 'no_left_turn',
                                                'except': 'bus;motorcar'}))
        self.assertIsNone(make_graph.turn_rule({'type': 'restriction', 'restriction': 'no_entry'}))   # (not a turn)
        self.assertIsNone(make_graph.turn_rule({'type': 'multipolygon'}))

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

    def test_numbers_tiles(self):
        """The deepest zoom's tiles (z15) in a pack: only the house numbers (Protomaps' buildings of kind "address",
        OpenMapTiles' housenumber layer) - the map drawn from z14 there (2026-11-11: z15 was 300 of the Moscow oblast's
        530 MB, the same roads and houses again)."""
        tile = mvt({'buildings': ['building', 'address', 'building_part', 'address'], 'roads': ['major_road'],
                    'housenumber': ['x'], 'water': ['water'], 'places': ['locality']})
        self.assertEqual(mvt_kinds(pmextract.numbers(tile)), {'buildings': ['address', 'address'], 'housenumber': ['x']})
        self.assertEqual(pmextract.numbers(mvt({'roads': ['major_road'], 'buildings': ['building']})), b'')
        only = mvt({'buildings': ['address']})
        self.assertEqual(pmextract.numbers(only), only)                 # nothing to drop: the same bytes

    def test_pmtiles_extract_deepest(self):
        """extract's deepest: what of the tiles at the zoom asked is kept (the others: keep)."""
        src = os.path.join(self.tmp, 'planet.pmtiles')
        make_pmtiles(src, {(0, 0, 0): b'world', (5, 20, 10): b'samara', (4, 10, 5): b'mid'})
        data = read(src)
        out = os.path.join(self.tmp, 'tiles.vtp')
        pmextract.extract(lambda o, n: data[o:o + n], (45.1, 41.1, 67.4, 55.7), 5, out, keep=lambda t: t.upper(),
                          deepest=lambda t: b'n' + t)
        self.assertEqual(pack_tiles(out)[2], {(0, 0, 0): b'WORLD', (4, 10, 5): b'MID', (5, 20, 10): b'nsamara'})

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

    def test_poly_ranges(self):
        """The tiles a region's outline touches (2026-11-03: Kazakhstan's box - twice the country, Russia's, China's,
        Uzbekistan's tiles in it - never cut in GitHub's 6 h): each row's x ranges, a tile round them."""
        z, n = 5, 32
        lon = lambda x: x / n * 360 - 180                           # noqa: E731
        lat = lambda y: math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))   # noqa: E731
        tri = [(lon(x), lat(y)) for x, y in ((10.5, 10.5), (20.5, 10.5), (10.5, 20.5), (10.5, 10.5))]
        r = pmextract.poly_ranges([tri], z)
        self.assertEqual(r[15], [(9, 18)])          # its rows 14..16 (x 10..17 on 14's top), a tile round
        self.assertEqual(r[9], [(9, 21)])           # the row over it: its neighbour's
        self.assertEqual(r[20], [(9, 13)])
        self.assertEqual(r[21], [(9, 12)])
        self.assertEqual(sorted(r), list(range(9, 22)))
        island = [(lon(x), lat(y)) for x, y in ((27.5, 14.5), (29.5, 14.5), (29.5, 15.5), (27.5, 15.5))]
        r = pmextract.poly_ranges([tri, island], z)
        self.assertEqual(r[15], [(9, 18), (26, 30)])  # two rings apart: two ranges
        near = [(lon(x), lat(y)) for x, y in ((18.5, 14.5), (19.5, 14.5), (19.5, 15.5), (18.5, 15.5))]
        self.assertEqual(pmextract.poly_ranges([tri, near], z)[15], [(9, 20)])   # close ones: one
        edge = [(lon(x), lat(y)) for x, y in ((0.2, 0.2), (31.8, 0.2), (31.8, 1.8), (0.2, 1.8))]
        self.assertEqual(pmextract.poly_ranges([edge], z)[0], [(0, 31)])   # never past the world's edge
        self.assertNotIn(-1, pmextract.poly_ranges([edge], z))
        bottom = [(lon(x), lat(y)) for x, y in ((3.5, 30.2), (5.5, 30.2), (5.5, 31.8), (3.5, 31.8))]
        self.assertEqual(sorted(pmextract.poly_ranges([bottom], z)), [29, 30, 31])   # (no row 32)
        u = [(x, y) for x, y in ((10.5, 10.5), (16.5, 10.5), (16.5, 16.5), (14.5, 16.5), (14.5, 12.5), (12.5, 12.5),
                                 (12.5, 16.5), (10.5, 16.5))]
        rows = pmextract.ring_rows([(lon(x), lat(y)) for x, y in u], n)
        self.assertEqual(pmextract.joined(rows[14]), [(10, 12), (14, 16)])   # a U: its two legs
        self.assertEqual(pmextract.joined(rows[11]), [(10, 16)])
        self.assertEqual(pmextract.joined([(5, 6), (1, 2), (3, 4)]), [(1, 6)])   # touching: one
        self.assertEqual(pmextract.joined([(1, 2), (4, 5), (3, 3)]), [(1, 5)])
        self.assertEqual(pmextract.joined([(1, 2), (4, 5)]), [(1, 2), (4, 5)])  # a tile between: two

    def test_pmtiles_extract_by_outline(self):
        """With the region's outline: only the tiles it touches (and their parents'), not its box's."""
        src = os.path.join(self.tmp, 'planet.pmtiles')
        n = 1 << 12
        tiles = {(12, x, y): b'%d,%d' % (x, y) for x in range(100, 140) for y in range(100, 140)}
        tiles[(11, 50, 50)] = b'parent'
        tiles[(11, 69, 69)] = b'far parent'
        make_pmtiles(src, tiles)
        data = read(src)
        lon = lambda x: x / n * 360 - 180                           # noqa: E731
        lat = lambda y: math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))   # noqa: E731
        sq = [(lon(x), lat(y)) for x, y in ((100.5, 100.5), (104.5, 100.5), (104.5, 104.5), (100.5, 104.5))]
        out = os.path.join(self.tmp, 'tiles.vtp')
        pmextract.extract(lambda o, l: data[o:o + l], (lon(100.5), lat(139.5), lon(139.5), lat(100.5)), 12, out,
                          rings=[sq])
        _, _, got = pack_tiles(out)
        self.assertEqual({k for k in got if k[0] == 12}, {(12, x, y) for x in range(99, 106) for y in range(99, 106)
                                                          if (12, x, y) in tiles})
        self.assertIn((11, 50, 50), got)
        self.assertNotIn((11, 69, 69), got)

    def test_region_rings(self):
        """A region's outline from its extracts' .poly (openstreetmap.fr's polygons/ tree); across 180 degrees
        (Chukotka) or none to be had: None - its box then."""
        polys = {'http://download.openstreetmap.fr/polygons/russia/a.poly': 'a\n1\n 40 50\n 42 50\n 42 52\nEND\nEND\n',
                 'http://download.openstreetmap.fr/polygons/russia/b.poly': 'b\n1\n 44 50\n 45 50\n 45 51\nEND\nEND\n',
                 'http://download.openstreetmap.fr/polygons/russia/c.poly': 'c\n1\n 170 60\n -170 60\n -170 62\nEND\nEND\n'}
        url = lambda n: 'http://download.openstreetmap.fr/extracts/russia/%s-latest.osm.pbf' % n   # noqa: E731

        def get(u):
            if u not in polys:
                raise OSError('404')
            return polys[u]
        self.assertEqual(release_regions.region_rings(url('a'), get), [[(40, 50), (42, 50), (42, 52)]])
        self.assertEqual(len(release_regions.region_rings([url('a'), url('b')], get)), 2)
        self.assertIsNone(release_regions.region_rings(url('c'), get))
        self.assertIsNone(release_regions.region_rings(url('x'), get))

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

    def test_fetch_stopped_drops_the_rest(self):
        """The progress raising (maps_app's "Отменить запись", 2026-10-30): the fetches not begun dropped, not waited
        for - the exception out at once."""
        from concurrent.futures import Future

        class Pool:   # the first fetch done, the others queued
            def __init__(self):
                self.jobs = []

            def submit(self, fn, *a):
                f = Future()
                if not self.jobs:
                    f.set_result(fn(*a))
                self.jobs.append(f)
                return f
        archive = type('A', (), {'get_bytes': lambda self, b, n: b'x' * n})()
        far = pmextract.kGap + pmextract.kMaxGroup
        located = {i: (i * far, 10) for i in range(3)}

        def progress(part, text):
            raise KeyboardInterrupt
        pool = Pool()
        with self.assertRaises(KeyboardInterrupt):
            pmextract.fetch(archive, located, pool, progress)
        self.assertEqual([f.cancelled() for f in pool.jobs], [False, True, True])

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
        self.assertEqual(h[2:6], (1, 12, 2, 1))       # places, streets (the ring has no name; Островная too:
                                                      # only the graph leaves it out; Западный, Дальняя),
                                                      # houses, places to go
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
