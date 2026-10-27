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

    def test_search_index(self):
        out = os.path.join(self.tmp, 'search.idx')
        make_search.build(TINY, out)
        h = struct.unpack_from(make_search.HEADER, read(out), 0)
        self.assertEqual(h[2:6], (1, 9, 2, 1))        # places, streets (the ring has no name), houses, places to go
        self.assertEqual(make_search.norm('Улица 22-го Партсъезда, ё'), 'улица 22 го партсъезда е')

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
