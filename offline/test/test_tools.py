"""The region tools on the test map (tiny.osm): what OSMapsCE reads from their files, and the update's steps.

    python -m unittest discover -s offline/test      (quality.bat runs it under coverage)
"""
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
        key, off, length = struct.unpack_from('<III', d, off_index + i * 12)
        keys.append(key)
        out[(key >> 28, (key >> 14) & 0x3FFF, key & 0x3FFF)] = d[off:off + length]
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

    def test_speed_and_access(self):
        self.assertEqual(make_graph.maxspeed_kmh('40 mph'), 64)
        self.assertIsNone(make_graph.maxspeed_kmh('none'))
        self.assertIsNone(make_graph.car_way({'highway': 'primary', 'access': 'private'}, [1, 2]))
        self.assertEqual(make_graph.car_way({'highway': 'primary', 'oneway': '-1'}, [1, 2]).oneway, -1)
        self.assertEqual(make_graph.car_way({'highway': 'track', 'tracktype': 'grade4'}, [1, 2]).speed, 8)

    def test_tile_pack_from_mbtiles(self):
        src, out = os.path.join(self.tmp, 'r.mbtiles'), os.path.join(self.tmp, 'tiles.vtp')
        tiles = {(14, 9907, 5118): b'samara z14', (14, 9908, 5118): b'east', (0, 0, 0): b'world'}
        make_mbtiles(src, tiles)
        make_tiles.build(src, out)
        magic, keys, got = pack_tiles(out)
        self.assertEqual(magic, b'OSMT')
        self.assertEqual(keys, sorted(keys))          # the unit finds a tile by binary search
        self.assertEqual(got, tiles)                  # XYZ rows (not TMS), data ungzipped

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
        self.assertEqual(list(make_region.regions(get=pages.get)), ['samara_oblast'])


if __name__ == '__main__':
    unittest.main()
