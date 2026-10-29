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
from test_tools import TINY, make_pmtiles, mvt, mvt_kinds, pack_tiles, read   # noqa: E402


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
        # a kept entry gets today's title (the user 2026-10-29: "Украина:" off the oblasts - not only when rebuilt)
        previous = {'regions': [{'name': 'kiev_oblast', 'title': 'Украина: Киев и Киевская область'}]}
        cat = release_regions.catalog(self.dist, previous, {'kiev_oblast'})
        self.assertIn({'name': 'kiev_oblast', 'title': 'Киев и Киевская область'}, cat['regions'])
        self.assertFalse([t for t in release_regions.TITLES.values() if t.startswith('Украина')])

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
        tile = mvt({'roads': ['major_road'], 'landuse': ['wood', 'park']})
        make_pmtiles(planet, {town: tile, far: b'far', (0, 0, 0): mvt({'earth': ['earth'], 'water': ['ocean']})})
        data = read(planet)
        drive = os.path.join(self.tmp, 'E')
        steps = []

        def get_file(url, tell):                                           # the release's zip, its download told
            tell(0.5, 'half')
            tell(1.0, 'all')
            return zips[url]
        maps_app.install(entry, drive, get_file, lambda off, n: data[off:off + n], 15,
                         lambda part, text: steps.append((part, text)), '20261027')
        folder = os.path.join(drive, 'OSMapsData', 'tiny')
        self.assertEqual(sorted(os.listdir(folder)), ['region.txt', 'route.grf', 'search.idx', 'tiles.vtp'])
        _, _, got = pack_tiles(os.path.join(folder, 'tiles.vtp'))
        self.assertEqual(sorted(got), sorted([town, (0, 0, 0)]))           # the region's tiles only,
        self.assertEqual(mvt_kinds(got[town]), {'roads': ['major_road'], 'landuse': ['park']})   # slim (no wood)
        self.assertEqual(mvt_kinds(got[(0, 0, 0)]), {'water': ['ocean']})
        self.assertEqual(maps_app.installed(drive), {'tiny': entry['date']})
        info = release_regions.region_info(os.path.join(folder, 'region.txt'))
        self.assertEqual(info['tiles'], '20261027')                        # the tiles' build: newer online ones refresh them
        self.assertEqual(steps[-1][0], 1.0)                                  # the progress, to the end
        parts = [part for part, _ in steps]
        self.assertEqual(parts, sorted(parts))
        tiles_end = [p for p, t in steps if t == 'плитки записаны'][0]          # (the user 2026-10-29: the bar full
        graph = [(p, t) for p, t in steps if t.startswith('граф и поиск')]      # while the zip still came) the zip's
        self.assertEqual(tiles_end, maps_app.kTilesPart)                       # download after the tiles, told
        self.assertEqual(graph, [(maps_app.kTilesPart + (1 - maps_app.kTilesPart) * 0.5, 'граф и поиск: half'),
                                 (1.0, 'граф и поиск: all')])

    def test_app_writes_the_trunk_graph(self):
        """4b (2026-10-29): the trunk roads' graph of Russia and the CIS - with any regions written, once a build."""
        def trunk_zip(date):
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, 'w') as z:
                z.writestr('trunk.grf', b'OSMG' + date.encode())
                z.writestr('region.txt', 'name=trunk\ndate=%s\n' % date)
            return buf.getvalue()
        drive = os.path.join(self.tmp, 'T')
        assets = {release_regions.ASSETS + 'trunk.zip': trunk_zip('2026-10-29')}
        told = []
        get = lambda url, tell: (tell(1.0, 'x'), assets[url])[1]          # noqa: E731
        self.assertEqual(maps_app.install_trunk(drive, get, lambda part, text: told.append(part)), 'written')
        self.assertEqual(told, [1.0])
        folder = os.path.join(drive, 'OSMapsData', 'trunk')
        self.assertEqual(sorted(os.listdir(folder)), ['region.txt', 'trunk.grf'])
        self.assertEqual(maps_app.install_trunk(drive, get), 'same')
        assets[release_regions.ASSETS + 'trunk.zip'] = trunk_zip('2026-11-02')
        self.assertEqual(maps_app.install_trunk(drive, get), 'written')
        self.assertEqual(read(os.path.join(folder, 'trunk.grf')), b'OSMG2026-11-02')
        self.assertEqual(maps_app.install_trunk(drive, lambda url, tell: {}[url]), 'none')   # no graph in the release yet
        self.assertNotIn('trunk', maps_app.installed(drive))                     # not a region of the list
        self.assertEqual(maps_app.trunk_date(drive), '2026-11-02')                # its own row's date
        self.assertIsNone(maps_app.trunk_date(self.tmp))

    def test_app_states(self):
        cat = {'regions': [{'name': 'a', 'date': '2026-10-27'}, {'name': 'b', 'date': '2026-10-27'},
                           {'name': 'c', 'date': '2026-10-27'}]}
        have = {'a': '2026-10-27', 'b': '2026-10-20'}
        self.assertEqual(maps_app.states(cat, have), {'a': 'ok', 'b': 'update', 'c': 'new'})

    def test_app_choosing(self):
        """The list's logic (the user 2026-10-28: a click picks a region, no Ctrl): a click toggles one, all / none /
        the outdated ones, the search by the Russian name or the latin one, the size chosen."""
        cat = {'regions': [{'name': 'samara_oblast', 'title': 'Самарская область', 'size': 17, 'date': 'd2'},
                           {'name': 'moscow_oblast', 'title': 'Москва и Московская область', 'size': 58, 'date': 'd2'},
                           {'name': 'crimea', 'title': 'Крым и Севастополь', 'size': 11, 'date': 'd2'}]}
        self.assertEqual(maps_app.toggled(set(), 'crimea'), {'crimea'})
        self.assertEqual(maps_app.toggled({'crimea', 'samara_oblast'}, 'crimea'), {'samara_oblast'})
        self.assertEqual(maps_app.chosen_size(cat, {'crimea', 'moscow_oblast'}), 69)
        self.assertEqual(maps_app.chosen_size(cat, set()), 0)
        self.assertEqual([r['name'] for r in maps_app.matching(cat, '')],
                         ['crimea', 'moscow_oblast', 'samara_oblast'])                    # by the title
        self.assertEqual([r['name'] for r in maps_app.matching(cat, 'мос')], ['moscow_oblast'])
        self.assertEqual([r['name'] for r in maps_app.matching(cat, 'SAMARA')], ['samara_oblast'])
        self.assertEqual(maps_app.outdated(cat, {'crimea': 'd1', 'samara_oblast': 'd2'}), {'crimea'})

    def test_app_window(self):
        """The window itself (Tk, the catalog given): a click on a row picks it (lit), again lets it go; all / none /
        the outdated; the search narrows the list; the status tells the size chosen; nothing chosen - nothing written."""
        import tkinter
        import tkinter.ttk
        import unittest.mock
        cat = {'regions': [{'name': 'a_oblast', 'title': 'Альфа', 'size': 2e6, 'date': 'd2'},
                           {'name': 'b_oblast', 'title': 'Бета', 'size': 3e6, 'date': 'd2'}]}
        drive = os.path.join(self.tmp, 'E')
        os.makedirs(os.path.join(drive, 'OSMapsData', 'b_oblast'))
        with open(os.path.join(drive, 'OSMapsData', 'b_oblast', 'region.txt'), 'w') as f:
            f.write('date=d1\n')
        patch = unittest.mock.patch.object
        with patch(maps_app, 'load_catalog', lambda: cat), patch(maps_app, 'load_outlines', lambda: None), \
                patch(maps_app, 'removable_drives', lambda: [drive]):
            app = maps_app.App(tkinter, tkinter.ttk)
        try:
            app.root.geometry('+-3000+-3000')   # (shown - its rows laid out - but off the screen)
            app.on_catalog(cat)
            app.root.update()
            self.assertEqual(app.tree.get_children(), ('trunk', 'a_oblast', 'b_oblast'))   # the trunk roads first
            self.assertEqual(app.tree.item('b_oblast', 'tags'), ('update',))       # newer in the catalog
            self.assertTrue(app.tree['yscrollcommand'])                             # the list scrolls (2026-10-29)
            self.assertEqual(app.scroll.get(), (0.0, 1.0))
            y = app.tree.bbox('a_oblast')[1] + 5
            app.click(type('E', (), {'y': y})())
            self.assertEqual(app.chosen, {'a_oblast'})
            self.assertEqual(app.tree.item('a_oblast', 'tags'), ('chosen',))
            self.assertIn('выбрано 1 (2 МБ', app.status.get())
            app.click(type('E', (), {'y': y})())
            self.assertEqual(app.chosen, set())
            app.click(type('E', (), {'y': 10000})())                                # no row there
            self.assertEqual(app.chosen, set())
            app.pick_all()
            self.assertEqual(app.chosen, {'a_oblast', 'b_oblast'})
            app.pick_none()
            app.pick_old()
            self.assertEqual(app.chosen, {'b_oblast'})
            app.find.set('аль')
            self.assertEqual(app.tree.get_children(), ('a_oblast',))
            app.find.set('магистр')
            self.assertEqual(app.tree.get_children(), ('trunk',))
            app.find.set('')
            app.pick_none()
            with unittest.mock.patch.object(maps_app.threading, 'Thread') as thread:
                app.write_chosen()
                self.assertFalse(thread.called)                                     # nothing chosen: nothing
                app.chosen = {'a_oblast'}
                app.write_chosen()
                self.assertEqual(thread.call_args[1]['args'][1:], ([cat['regions'][0]], drive, True))   # no trunk
                                                                                   # roads there: they too
                self.assertEqual(app.go['text'], 'Идёт запись…')                     # the button tells it is busy
                self.assertEqual(str(app.go['style']), 'Busy.TButton')
                self.assertTrue(app.go.instate(['disabled']))
                app.write_chosen()
                self.assertEqual(thread.call_count, 1)                              # not twice at once
                app.on_done(None)
                app.chosen = {'trunk'}                                              # the trunk roads alone
                app.write_chosen()
                self.assertEqual(thread.call_args[1]['args'][1:], ([], drive, True))
                app.on_done(None)
                self.assertEqual(app.go['text'], 'Записать выбранные')
                self.assertEqual(str(app.go['style']), 'Go.TButton')
                self.assertFalse(app.go.instate(['disabled']))
        finally:
            app.root.destroy()

    def test_app_icon(self):
        """The icon: every Windows size, a dark floppy disk with the green tag on its label."""
        import make_icon
        from PIL import Image
        out = make_icon.main(os.path.join(self.tmp, 'app.ico'))
        with Image.open(out) as ico:
            self.assertEqual(sorted(ico.info['sizes']), [(n, n) for n in make_icon.SIZES])
        im = make_icon.draw(256)
        self.assertEqual(im.getpixel((100, 170))[:3], make_icon.TAG)      # the tag on the label
        self.assertEqual(im.getpixel((30, 100))[:3], make_icon.DISK)       # the disk
        self.assertEqual(im.getpixel((250, 5))[3], 0)                      # the cut corner: see-through

    def test_download_with_progress(self):
        """A release's file read in pieces, its progress told by its length (the user 2026-10-29: the graph's zip and
        the trunk roads came minutes long with the bar full and nothing told)."""
        body = bytes(range(256)) * 3000

        class Resp(io.BytesIO):
            headers = {'Content-Length': str(len(body))}

            def __enter__(self):
                return self

            def __exit__(self, *a):
                self.close()
        told = []
        got = maps_app.download('http://x/y.zip', lambda part, text: told.append((part, text)),
                                lambda req, timeout: Resp(body), piece=100000)
        self.assertEqual(got, body)
        self.assertEqual([p for p, _ in told], [min(1.0, k * 100000 / len(body)) for k in range(1, 9)])   # 8 pieces
        self.assertEqual(told[-1], (1.0, '0.8 из 0.8 МБ'))
        self.assertEqual([p for p, _ in told], sorted(p for p, _ in told))

    def test_app_writes_all_with_one_progress(self):
        """The bar for the whole job (the user 2026-10-29: full before the end): each region a part of it, the trunk
        roads the last part; never full before they are written."""
        cat = {'regions': [{'name': 'a', 'title': 'A'}, {'name': 'b', 'title': 'B'}]}
        app = type('A', (), {})()
        app.news, app.cat = __import__('queue').Queue(), cat
        app.tell = lambda head, part, text: maps_app.App.tell(app, head, part, text)

        def install(e, drive, get_file, get_bytes, maxzoom, progress, day):
            progress(0.5, 'half')
            progress(1.0, 'done')

        def install_trunk(drive, get_file, tell):
            tell(0.5, 'trunk half')
            tell(1.0, 'trunk done')
            return 'written'
        patch = unittest.mock.patch.object
        with patch(maps_app, 'install', install), patch(maps_app, 'install_trunk', install_trunk), \
                patch(maps_app.pmextract, 'latest_build', lambda: 'https://b/20261029.pmtiles'), \
                patch(maps_app.pmextract, 'http_bytes', lambda url: None):
            maps_app.App.write_all(app, cat['regions'], self.tmp, True)
        news = []
        while not app.news.empty():
            news.append(app.news.get())
        bar = [v for k, v in news if k == 'progress']
        self.assertEqual(bar, [1 / 6, 1 / 3, 1 / 2, 2 / 3, 5 / 6, 1.0])
        self.assertIn(('status', 'Магистрали России и СНГ: trunk half'), news)

    def test_outlines(self):
        """The regions' outlines for the map of the app's (the user 2026-10-29: a visual choice): their sources'
        .poly (openstreetmap.fr, Geofabrik), the outer rings only, simplified, rounded, a region of two extracts both,
        east of 180 degrees (Chukotka) on the east."""
        import make_outlines
        self.assertEqual(make_outlines.poly_url(
            'http://download.openstreetmap.fr/extracts/russia/central_federal_district/moscow_oblast-latest.osm.pbf'),
            'http://download.openstreetmap.fr/polygons/russia/central_federal_district/moscow_oblast.poly')
        self.assertEqual(make_outlines.poly_url('https://download.geofabrik.de/europe/belarus-latest.osm.pbf'),
                         'https://download.geofabrik.de/europe/belarus.poly')
        poly = ('x\n1\n   3.0E+01  5.0E+01\n   30.5 50.0001\n   31 50\n   31 51\n   30 51\n   30 50\nEND\n'
                '!2\n 30.2 50.2\n 30.3 50.2\n 30.3 50.3\nEND\n3\n -179.5 65\n -179 65\n -179 66\n -179.5 65\nEND\nEND\n')
        self.assertEqual(make_outlines.parse_poly(poly),
                         [[(30.0, 50.0), (30.5, 50.0001), (31.0, 50.0), (31.0, 51.0), (30.0, 51.0), (30.0, 50.0)],
                          [(-179.5, 65.0), (-179.0, 65.0), (-179.0, 66.0), (-179.5, 65.0)]])
        polys = {'u/a.poly': poly, 'u/b.poly': 'b\n1\n 40 40\n 41 40\n 41 41\n 40 40\nEND\nEND\n'}
        got = make_outlines.outlines({'a': 'u/a-latest.osm.pbf', 'ab': ['u/a-latest.osm.pbf', 'u/b-latest.osm.pbf']},
                                     polys.__getitem__)
        self.assertEqual(got['a'], [[[30.0, 50.0], [31.0, 50.0], [31.0, 51.0], [30.0, 51.0], [30.0, 50.0]],
                                    [[180.5, 65.0], [181.0, 65.0], [181.0, 66.0], [180.5, 65.0]]])
        self.assertEqual(len(got['ab']), 3)                                 # both extracts' rings

    def test_app_map(self):
        """The map of the regions: their outlines, a click picks one (lit as in the list), again lets it go; a drag
        moves the map, picks nothing; no outlines - the list alone."""
        import tkinter
        import tkinter.ttk
        cat = {'regions': [{'name': 'a_oblast', 'title': 'Альфа', 'size': 2e6, 'date': 'd2'},
                           {'name': 'b_oblast', 'title': 'Бета', 'size': 3e6, 'date': 'd2'}]}
        shapes = {'a_oblast': [[[40, 50], [44, 50], [44, 54], [40, 54], [40, 50]]],
                  'b_oblast': [[[50, 50], [54, 50], [54, 54], [50, 54], [50, 50]]]}
        drive = os.path.join(self.tmp, 'E')
        os.makedirs(drive)
        patch = unittest.mock.patch.object
        with patch(maps_app, 'load_catalog', lambda: cat), patch(maps_app, 'load_outlines', lambda: shapes), \
                patch(maps_app, 'removable_drives', lambda: [drive]):
            app = maps_app.App(tkinter, tkinter.ttk)
        try:
            app.root.geometry('+-3000+-3000')
            app.on_catalog(cat)
            app.on_outlines(shapes)
            app.root.update()
            items = app.map.find_withtag('r:a_oblast')
            self.assertEqual(len(items), 1)
            x0, y0, x1, y1 = app.map.bbox(items[0])
            cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
            ev = lambda x, y: type('E', (), {'x': x, 'y': y})()             # noqa: E731
            app.map_press(ev(cx, cy))
            app.map_release(ev(cx, cy))
            self.assertEqual(app.chosen, {'a_oblast'})
            self.assertEqual(app.map.itemcget(items[0], 'fill'), maps_app.ACCENT)
            self.assertEqual(app.tree.item('a_oblast', 'tags'), ('chosen',))   # the list too
            app.map_press(ev(cx, cy))
            app.map_drag(ev(cx + 40, cy))
            app.map_release(ev(cx + 40, cy))
            self.assertEqual(app.chosen, {'a_oblast'})                          # a drag: nothing picked
            nx0 = app.map.bbox(items[0])[0]
            self.assertEqual(nx0, x0 + 40)                                      # the map moved with it
            app.map_press(ev(cx + 40, cy))
            app.map_release(ev(cx + 40, cy))
            self.assertEqual(app.chosen, set())                                 # again: let go
            self.assertNotEqual(app.map.itemcget(items[0], 'fill'), maps_app.ACCENT)
            app.map_press(ev(2, 2))
            app.map_release(ev(2, 2))
            self.assertEqual(app.chosen, set())                                 # no region there
            app.map_wheel(type('E', (), {'x': cx, 'y': cy, 'delta': 120})())
            self.assertGreater(app.map.bbox(items[0])[2] - app.map.bbox(items[0])[0], x1 - x0)   # nearer
            app.on_outlines(None)
            self.assertEqual(app.map.find_withtag('region'), ())                # no outlines: none drawn
        finally:
            app.root.destroy()

    def test_app_finds_the_removable_drives(self):
        kinds = {'C:\\': 3, 'E:\\': 2, 'F:\\': 2}                          # 2: removable (a card, a flash drive)
        mask = 1 << 2 | 1 << 4 | 1 << 5                                    # C:, E:, F:
        self.assertEqual(maps_app.removable_drives(lambda: mask, kinds.get), ['E:\\', 'F:\\'])


if __name__ == '__main__':
    unittest.main()
