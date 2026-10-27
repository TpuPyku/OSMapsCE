"""The regions' maps as their user gets them: built for GitHub Releases (release_regions.py, the maps workflow), then
written to a flash drive by the PC app (maps_app.py) - the release's zip and the tiles cut out of Protomaps' build.

    python -m unittest discover -s offline/test      (quality.bat runs it under coverage)
"""
import io
import json
import os
import pathlib
import shutil
import sys
import tempfile
import unittest
import unittest.mock
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import maps_app          # noqa: E402
import release_regions   # noqa: E402
from test_tools import TINY, make_pmtiles, pack_tiles, read   # noqa: E402


class Maps(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.dist = os.path.join(self.tmp, 'dist')

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def release_tiny(self):
        return release_regions.build_region('tiny', pathlib.Path(TINY).as_uri(), self.dist,
                                            os.path.join(self.tmp, 'work'))

    def test_release_region(self):
        entry = self.release_tiny()
        with zipfile.ZipFile(os.path.join(self.dist, 'tiny.zip')) as z:
            self.assertEqual(sorted(z.namelist()), ['region.txt', 'route.grf', 'search.idx'])
        self.assertEqual(entry['name'], 'tiny')
        self.assertEqual(entry['title'], 'tiny')                          # (no Russian name: the name itself)
        self.assertEqual(release_regions.title('moscow_oblast'), 'Москва и Московская область')
        self.assertEqual(entry['zip'], 'tiny.zip')
        self.assertEqual(entry['size'], os.path.getsize(os.path.join(self.dist, 'tiny.zip')))
        self.assertRegex(entry['date'], r'^\d{4}-\d\d-\d\d$')
        lon0, lat0, lon1, lat1 = entry['bbox']                 # the graph's bounds: around the test town
        self.assertTrue(49.95 < lon0 <= 50.0 < lon1 < 50.05 and 52.95 < lat0 < 53.0 < lat1 < 53.05, entry['bbox'])
        self.assertEqual(json.loads(read(os.path.join(self.dist, 'tiny.json'))), entry)

    def test_catalog_of_the_regions(self):
        os.makedirs(self.dist)
        for name in ('b_oblast', 'a_krai'):
            with open(os.path.join(self.dist, name + '.json'), 'w') as f:
                json.dump({'name': name}, f)
        previous = {'regions': [{'name': 'c_old'}, {'name': 'a_krai', 'date': 'last week'}]}
        for _ in range(2):                                     # again: the catalog itself not an entry
            cat = release_regions.catalog(self.dist, previous, {'a_krai', 'b_oblast', 'c_old'})
        self.assertEqual(cat['version'], 1)
        self.assertEqual(cat['regions'], [{'name': 'a_krai'}, {'name': 'b_oblast'}, {'name': 'c_old'}])   # (kept)
        self.assertEqual(json.loads(read(os.path.join(self.dist, 'catalog.json'))), cat)

    def test_release_command_line(self):
        known = {'tiny': pathlib.Path(TINY).as_uri(), 'other': ''}
        prev = os.path.join(self.tmp, 'previous.json')
        with open(prev, 'w') as f:                                     # 'gone': no longer a region of the list
            json.dump({'regions': [{'name': 'other'}, {'name': 'gone'}]}, f)
        out = io.StringIO()
        with unittest.mock.patch.object(release_regions.make_region, 'regions', lambda: known), \
                unittest.mock.patch.object(release_regions.make_region, 'WORK', os.path.join(self.tmp, 'w')), \
                unittest.mock.patch('sys.stdout', out):
            release_regions.main(['list'])
            release_regions.main(['build', 'tiny', self.dist])
            release_regions.main(['catalog', self.dist])               # (no previous one: the first run)
            self.assertEqual(len(json.loads(read(os.path.join(self.dist, 'catalog.json')))['regions']), 1)
            release_regions.main(['catalog', self.dist, prev])
        self.assertEqual(json.loads(out.getvalue().splitlines()[0]), ['other', 'tiny'])   # the workflow's matrix
        cat = json.loads(read(os.path.join(self.dist, 'catalog.json')))
        self.assertEqual([r['name'] for r in cat['regions']], ['other', 'tiny'])

    def test_region_info(self):
        path = os.path.join(self.tmp, 'region.txt')
        with open(path, 'w', encoding='utf-8') as f:
            f.write('name=tiny\ndate=2026-10-27\nsource=http://x/?a=b\nno pair\n')
        self.assertEqual(release_regions.region_info(path),
                         {'name': 'tiny', 'date': '2026-10-27', 'source': 'http://x/?a=b'})

    def test_catalog_and_build_asked_where_published(self):
        asked = []

        def get(url):
            asked.append(url)
            return b'{"regions": []}' if url.endswith('catalog.json') else b'[{"key": "a.pmtiles"}, {"key": "b.pmtiles"}, {"key": "c.pmtiles"}]'
        self.assertEqual(maps_app.load_catalog(get), {'regions': []})
        self.assertEqual(maps_app.pmextract.latest_build(get), maps_app.pmextract.SERVER + 'c.pmtiles')   # the newest
        self.assertEqual(asked, [release_regions.ASSETS + 'catalog.json', maps_app.pmextract.BUILDS])

    def test_app_writes_a_region_to_the_drive(self):
        entry = self.release_tiny()
        zips = {release_regions.ASSETS + 'tiny.zip': read(os.path.join(self.dist, 'tiny.zip'))}
        planet = os.path.join(self.tmp, 'planet.pmtiles')
        town, far = (14, 10467, 5336), (14, 100, 100)             # the town's z14 tile; one far off
        make_pmtiles(planet, {town: b'town', far: b'far', (0, 0, 0): b'world'})
        data = read(planet)
        drive = os.path.join(self.tmp, 'E')
        steps = []
        maps_app.install(entry, drive, zips.__getitem__, lambda off, n: data[off:off + n], 15, steps.append, '20261027')
        folder = os.path.join(drive, 'OSMapsData', 'tiny')
        self.assertEqual(sorted(os.listdir(folder)), ['region.txt', 'route.grf', 'search.idx', 'tiles.vtp'])
        _, _, got = pack_tiles(os.path.join(folder, 'tiles.vtp'))
        self.assertEqual(got, {town: b'town', (0, 0, 0): b'world'})        # the region's tiles only
        self.assertEqual(maps_app.installed(drive), {'tiny': entry['date']})
        info = release_regions.region_info(os.path.join(folder, 'region.txt'))
        self.assertEqual(info['tiles'], '20261027')                        # the tiles' build: newer online ones refresh them
        self.assertTrue(steps and steps[-1] == 1.0, steps)                  # the progress, to the end

    def test_app_states(self):
        cat = {'regions': [{'name': 'a', 'date': '2026-10-27'}, {'name': 'b', 'date': '2026-10-27'},
                           {'name': 'c', 'date': '2026-10-27'}]}
        have = {'a': '2026-10-27', 'b': '2026-10-20'}
        self.assertEqual(maps_app.states(cat, have), {'a': 'ok', 'b': 'update', 'c': 'new'})

    def test_app_finds_the_removable_drives(self):
        kinds = {'C:\\': 3, 'E:\\': 2, 'F:\\': 2}                          # 2: removable (a card, a flash drive)
        mask = 1 << 2 | 1 << 4 | 1 << 5                                    # C:, E:, F:
        self.assertEqual(maps_app.removable_drives(lambda: mask, kinds.get), ['E:\\', 'F:\\'])


if __name__ == '__main__':
    unittest.main()
