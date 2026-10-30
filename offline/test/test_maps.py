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

    def planet(self):   # Protomaps' build in little: the test town's z14 tile, a far one, z0
        planet = os.path.join(self.tmp, 'planet.pmtiles')
        make_pmtiles(planet, {(14, 10467, 5336): mvt({'roads': ['major_road']}), (14, 100, 100): b'far',
                              (0, 0, 0): mvt({'water': ['ocean']})})
        data = read(planet)
        return lambda off, n: data[off:off + n]

    def test_release_full_region(self):
        """The region whole on Yandex Disk (the user 2026-10-30: GitHub's downloads slow): its tiles cut out of
        Protomaps' build, one zip - the big files stored (the unit unpacks them piece by piece, no memory for them
        whole), region.txt with tiles= - uploaded, its catalog entry told (full: zip, size, tiles), not left in dist."""
        entry = self.release_tiny()
        folder = os.path.join(self.tmp, 'work', 'data', 'tiny')
        up = []
        disk = type('D', (), {'upload': lambda self, path, file: up.append((path, read(file)))})()
        got = release_regions.build_full(entry, folder, self.dist, self.planet(), '20261030', disk)
        self.assertEqual(up[0][0], 'disk:/OSMapsCE/maps/tiny.zip')
        with zipfile.ZipFile(io.BytesIO(up[0][1])) as z:
            self.assertEqual(sorted(z.namelist()), ['region.txt', 'route.grf', 'search.idx', 'tiles.vtp'])
            self.assertEqual({i.compress_type for i in z.infolist()}, {zipfile.ZIP_STORED})
            self.assertIn(b'tiles=20261030', z.read('region.txt'))
            z.extract('tiles.vtp', self.tmp)
        _, _, tiles = pack_tiles(os.path.join(self.tmp, 'tiles.vtp'))
        self.assertEqual(sorted(tiles), [(0, 0, 0), (14, 10467, 5336)])         # the region's tiles
        self.assertEqual(got['full'], {'zip': 'tiny.zip', 'size': len(up[0][1]), 'tiles': '20261030'})
        self.assertEqual(json.loads(read(os.path.join(self.dist, 'tiny.json'))), got)
        self.assertEqual(sorted(os.listdir(self.dist)), ['tiny.json', 'tiny.zip'])   # (the full zip not in dist)

    def test_release_disk_catalog(self):
        """The publish job: the overview whole too (overview.zip), the folder public - the catalog tells its link and
        the overview; a run without the disk keeps the previous catalog's."""
        os.makedirs(self.dist)
        cat = {'regions': [{'name': 'tiny', 'bbox': [49.9, 52.9, 50.1, 53.1]}]}
        with open(os.path.join(self.dist, 'catalog.json'), 'w', encoding='utf-8') as f:
            json.dump(cat, f)
        up = []
        disk = type('D', (), {'upload': lambda self, path, file: up.append((path, read(file))),
                              'publish': lambda self, path: up.append(('publish', path)) or 'https://disk.example/d/x'})()
        got = release_regions.publish_disk(self.dist, disk, self.planet(), '20261030')
        self.assertEqual(up[0][0], 'disk:/OSMapsCE/maps/overview.zip')
        with zipfile.ZipFile(io.BytesIO(up[0][1])) as z:
            self.assertEqual(sorted(z.namelist()), ['overview.vtp', 'region.txt'])
        self.assertEqual(up[1], ('publish', 'disk:/OSMapsCE/maps'))
        self.assertEqual(got['disk'], 'https://disk.example/d/x')
        self.assertEqual(got['overview'], {'zip': 'overview.zip', 'size': len(up[0][1]), 'tiles': '20261030'})
        self.assertEqual(json.loads(read(os.path.join(self.dist, 'catalog.json'))), got)
        self.assertFalse(os.path.exists(os.path.join(self.dist, 'overview.zip')))
        os.makedirs(os.path.join(self.tmp, 'd2'))
        again = release_regions.catalog(os.path.join(self.tmp, 'd2'), got, {'tiny'})
        self.assertEqual((again['disk'], again['overview']), (got['disk'], got['overview']))

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

    def test_app_writes_the_overview(self):
        """The overview pack (the user 2026-10-30: a region's pack holds its region only - its small zooms empty round
        it): z0..OVERVIEW_Z of all the regions' box whole, slim, its region.txt last (tiles= its build); not a region
        of the list."""
        cat = {'regions': [{'name': 'a', 'bbox': [37.0, 55.0, 38.0, 56.0]}, {'name': 'b', 'bbox': [49.0, 53.0, 50.0, 54.0]}]}
        self.assertEqual(maps_app.overview_box(cat), (37.0, 53.0, 50.0, 56.0))
        planet = os.path.join(self.tmp, 'planet.pmtiles')
        inside, outside, deep = (8, 154, 79), (8, 10, 10), (9, 308, 158)
        make_pmtiles(planet, {(0, 0, 0): mvt({'water': ['ocean']}), inside: mvt({'landuse': ['wood', 'park']}),
                              outside: b'far', deep: b'deep'})
        data = read(planet)
        drive = os.path.join(self.tmp, 'E')
        told = []
        maps_app.install_overview(cat, drive, lambda off, n: data[off:off + n], '20261030',
                                  lambda part, text: told.append(part))
        folder = os.path.join(drive, 'OSMapsData', 'overview')
        self.assertEqual(sorted(os.listdir(folder)), ['overview.vtp', 'region.txt'])
        _, _, got = pack_tiles(os.path.join(folder, 'overview.vtp'))
        self.assertEqual(sorted(got), [(0, 0, 0), inside])                     # its box, z8 at most
        self.assertEqual(mvt_kinds(got[inside]), {'landuse': ['park']})         # slim
        self.assertEqual(maps_app.overview_date(drive), '20261030')
        self.assertIsNone(maps_app.overview_date(self.tmp))
        self.assertEqual(maps_app.installed(drive), {})
        self.assertEqual(told[-1], 1.0)

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
            self.assertEqual(app.tree.get_children(), ('trunk', 'overview', 'a_oblast', 'b_oblast'))   # the trunk
                                                                                   # roads, the overview first
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
                self.assertEqual(thread.call_args[1]['args'][1:], ([cat['regions'][0]], drive, True, True))   # no
                                                                                   # trunk roads, no overview there: too
                self.assertEqual(app.go['text'], 'Отменить запись')                 # busy: the button cancels
                self.assertEqual(str(app.go['style']), 'Busy.TButton')
                self.assertFalse(app.go.instate(['disabled']))
                app.write_chosen()
                self.assertEqual(thread.call_count, 1)                              # not twice at once
                self.assertTrue(app.stop.is_set())                                  # stopped
                self.assertEqual(app.go['text'], 'Отмена…')
                self.assertTrue(app.go.instate(['disabled']))
                app.on_done(None)
                self.assertFalse(app.stop.is_set())                                 # the next one not stopped
                app.chosen = {'trunk'}                                              # the trunk roads alone
                app.write_chosen()
                self.assertEqual(thread.call_args[1]['args'][1:], ([], drive, True, False))
                app.on_done(None)
                self.assertEqual(app.go['text'], 'Записать выбранные')
                self.assertEqual(str(app.go['style']), 'Go.TButton')
                self.assertFalse(app.go.instate(['disabled']))
            app.chosen = {'a_oblast', 'b_oblast'}                                    # only b on the card
            asked = []
            with unittest.mock.patch.object(maps_app, 'ask', lambda title, text: asked.append(text) or False):
                app.remove_chosen()
            self.assertIn('Бета', asked[0])
            self.assertNotIn('Альфа', asked[0])
            self.assertIn('b_oblast', maps_app.installed(drive))                     # "no": kept
            with unittest.mock.patch.object(maps_app, 'ask', lambda title, text: True):
                app.remove_chosen()
            self.assertEqual(maps_app.installed(drive), {})
            self.assertIn('Удалено: 1', app.status.get())
            app.chosen = {'a_oblast'}                                                 # none of them there
            with unittest.mock.patch.object(maps_app, 'ask', lambda title, text: asked.append(text)):
                app.remove_chosen()
            self.assertEqual(len(asked), 1)                                          # not asked
            self.assertIn('Нечего удалять', app.status.get())
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

    installs = []

    def staged_job(self, free=None, stop=False, overview=False):
        """write_all with install / install_trunk writing small files where they are told (the PC's temp folder);
        returns the news, the stage the regions went to, the files copied in order."""
        cat = {'regions': [{'name': 'a', 'title': 'A', 'date': 'd'}, {'name': 'b', 'title': 'B', 'date': 'd'}]}
        app = type('A', (), {})()
        app.news, app.cat = __import__('queue').Queue(), cat
        app.stop = __import__('threading').Event()
        if stop:
            app.stop.set()
        app.tell = lambda head, part, text: maps_app.App.tell(app, head, part, text)
        app.write_stage = lambda *a: maps_app.App.write_stage(app, *a)
        app.write_overview = lambda *a: maps_app.App.write_overview(app, *a)
        stages, copied = self.installs, []

        def put(folder, files):
            os.makedirs(folder, exist_ok=True)
            for f, data in files.items():
                with open(os.path.join(folder, f), 'wb') as out:
                    out.write(data)

        def install(e, drive, get_file, get_bytes, maxzoom, progress, day):
            stages.append(drive)
            progress(0.5, 'half')
            put(os.path.join(drive, 'OSMapsData', e['name']), {'tiles.vtp': b'T' * 3000,
                                                             'region.txt': b'date=d\ntiles=%s\n' % day.encode()})
            progress(1.0, 'done')

        def install_overview(cat, drive, get_bytes, day, tell):
            stages.append(drive)
            tell(1.0, 'overview done')
            put(os.path.join(drive, 'OSMapsData', 'overview'), {'overview.vtp': b'O' * 500,
                                                               'region.txt': b'name=overview\ntiles=%s\n' % day.encode()})

        def install_trunk(drive, get_file, tell, have=None):
            tell(0.5, 'trunk half')
            put(os.path.join(drive, 'OSMapsData', 'trunk'), {'trunk.grf': b'G' * 1000, 'region.txt': b'date=t\n'})
            tell(1.0, 'trunk done')
            return 'written'
        keep = maps_app.copy_file

        def copy_file(src, dst, tell):
            copied.append(os.path.relpath(dst, target))
            return keep(src, dst, tell)
        target = os.path.join(self.tmp, 'E')
        os.makedirs(target, exist_ok=True)
        patch = unittest.mock.patch.object
        with patch(maps_app, 'install', install), patch(maps_app, 'install_trunk', install_trunk), \
                patch(maps_app, 'install_overview', install_overview), \
                patch(maps_app, 'copy_file', copy_file), \
                patch(maps_app.pmextract, 'latest_build', lambda: 'https://b/20261029.pmtiles'), \
                patch(maps_app.pmextract, 'http_bytes', lambda url: None), \
                patch(maps_app.tempfile, 'gettempdir', lambda: os.path.join(self.tmp, 'T')), \
                patch(maps_app.shutil, 'disk_usage', (lambda p: type('U', (), {'free': free})()) if free is not None
                      else maps_app.shutil.disk_usage):
            maps_app.App.work(app, maps_app.App.write_all.__get__(app), cat['regions'], target, True, overview)
        news = []
        while not app.news.empty():
            news.append(app.news.get())
        return news, stages, copied, target

    def test_app_cancelled(self):
        """"Отменить" (the user 2026-10-30): the job stopped at its next step, told; what was downloaded kept in the
        temp folder (the next time goes on there), nothing onto the card."""
        self.installs = []
        news, stages, copied, target = self.staged_job(stop=True)
        self.assertTrue(any(k == 'status' and v.startswith('Остановлено') for k, v in news), news)
        self.assertEqual(copied, [])
        self.assertTrue(os.path.isdir(os.path.join(self.tmp, 'T', 'OSMapsCE-Maps')))   # kept

    def test_copy_cancelled_never_looks_whole(self):
        """Stopped in the middle of a region's copy over an older one: its old region.txt gone first - the card's
        folder never looks whole with half its files new."""
        stage, card = os.path.join(self.tmp, 'S'), os.path.join(self.tmp, 'C')
        for root, date in ((stage, b'new'), (card, b'old')):
            os.makedirs(os.path.join(root, 'OSMapsData', 'a'))
            for f, data in (('route.grf', b'G' * 10), ('region.txt', b'date=' + date + b'\n')):
                with open(os.path.join(root, 'OSMapsData', 'a', f), 'wb') as out:
                    out.write(data)

        def tell(part, text):
            raise maps_app.Cancelled()
        with self.assertRaises(maps_app.Cancelled):
            maps_app.copy_to(stage, card, tell)
        self.assertEqual(maps_app.installed(card), {})

    def test_remove_regions(self):
        """"Удалить с флешки" (the user 2026-10-30): the chosen regions' folders gone (region.txt first), the trunk
        roads too when chosen; a name that is no folder there - or no plain name - touches nothing."""
        drive = os.path.join(self.tmp, 'E')
        for name in ('a', 'b', 'trunk'):
            os.makedirs(os.path.join(drive, 'OSMapsData', name))
            with open(os.path.join(drive, 'OSMapsData', name, 'region.txt'), 'w') as f:
                f.write('date=d\n')
        with open(os.path.join(drive, 'keep.txt'), 'w') as f:
            f.write('x')
        got = maps_app.remove_regions(drive, ['a', 'trunk', 'zz', '..', '', os.path.join('b', '..')])
        self.assertEqual(got, ['a', 'trunk'])
        self.assertEqual(maps_app.installed(drive), {'b': 'd'})
        self.assertIsNone(maps_app.trunk_date(drive))
        self.assertTrue(os.path.exists(os.path.join(drive, 'keep.txt')))

    def test_app_writes_all_with_one_progress(self):
        """The whole job (the user 2026-10-29: the bar full before the end; 2026-10-30: written straight to the card,
        its tiles got broken - first all on the PC, then onto the card at once, faster on a slow card too): the regions
        and the trunk roads into the PC's temp folder, each a part of the bar, then the copy - its last part; a
        folder's region.txt copied last (a region half copied never looks whole); the temp folder gone after."""
        self.installs = []
        news, stages, copied, target = self.staged_job()
        bar = [v for k, v in news if k == 'progress']
        self.assertEqual(bar[:6], [1 / 8, 1 / 4, 3 / 8, 1 / 2, 5 / 8, 3 / 4])
        self.assertEqual(bar[-1], 1.0)
        self.assertEqual(bar, sorted(bar))
        self.assertIn(('status', 'Магистрали России и СНГ: trunk half'), news)
        self.assertTrue(any(k == 'status' and v.startswith('На флешку:') for k, v in news), news)
        self.assertEqual(len(set(stages)), 1)
        self.assertNotEqual(os.path.abspath(stages[0]), os.path.abspath(target))   # not straight onto the card
        self.assertFalse(os.path.exists(stages[0]))                                 # the temp folder gone
        for name in ('a', 'b', 'trunk'):
            folder = [c for c in copied if c.split(os.sep)[1] == name]
            self.assertEqual(os.path.basename(folder[-1]), 'region.txt', folder)
        self.assertEqual(read(os.path.join(target, 'OSMapsData', 'a', 'tiles.vtp')), b'T' * 3000)
        self.assertEqual(sorted(maps_app.installed(target)), ['a', 'b'])
        self.assertEqual(maps_app.trunk_date(target), 't')

    def test_app_writes_all_with_the_overview(self):
        """The overview with the regions (none on the card): its own part of the bar, after the regions, copied too."""
        self.installs = []
        news, stages, copied, target = self.staged_job(overview=True)
        bar = [v for k, v in news if k == 'progress']
        self.assertEqual(bar[:7], [1 / 10, 1 / 5, 3 / 10, 2 / 5, 3 / 5, 7 / 10, 4 / 5])
        self.assertEqual(maps_app.overview_date(target), '20261029')
        self.assertTrue(any(k == 'status' and 'обзорная карта записана' in v for k, v in news), news)

    def test_app_copy_needs_room(self):
        """Not room enough on the card (or the link lost): told, nothing half written there; what was downloaded kept
        in the temp folder (the user 2026-10-30: go on there after a break) - the next time those regions are not
        downloaded again, only copied."""
        self.installs = []
        news, stages, copied, target = self.staged_job(free=1000)
        self.assertEqual(copied, [])
        self.assertTrue(any(k == 'status' and 'Мало места' in v for k, v in news), news)
        self.assertTrue(os.path.exists(os.path.join(stages[0], 'OSMapsData', 'b', 'region.txt')))   # kept
        self.assertFalse(os.path.exists(os.path.join(target, 'OSMapsData')))
        self.installs = []
        news, stages, copied, target = self.staged_job()
        self.assertEqual(stages, [])                                         # none downloaded again
        self.assertEqual(sorted(maps_app.installed(target)), ['a', 'b'])     # copied
        self.assertFalse(os.path.exists(os.path.join(self.tmp, 'T', 'OSMapsCE-Maps')))   # gone after

    def test_app_old_temp_folder_removed(self):
        """A temp folder left over a week: not gone on with (its maps old), removed first."""
        old = os.path.join(self.tmp, 'T', 'OSMapsCE-Maps', 'OSMapsData', 'z')
        os.makedirs(old)
        with open(os.path.join(old, 'region.txt'), 'w') as f:
            f.write('date=old\n')
        stamp = __import__('time').time() - 8 * 86400
        os.utime(os.path.join(self.tmp, 'T', 'OSMapsCE-Maps'), (stamp, stamp))
        self.installs = []
        news, stages, copied, target = self.staged_job()
        self.assertNotIn('z', maps_app.installed(target))

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

    def test_region_colours(self):
        """The map's regions (the user 2026-10-30: the outline of one with an update hid under its neighbours'): chosen -
        orange, on the drive and fresh - green, both edged white; with an update - white, edged black; else as the
        panel."""
        A = maps_app
        self.assertEqual(A.region_look(True, 'update'), (A.ACCENT, A.WHITE))
        self.assertEqual(A.region_look(False, 'ok'), (A.FRESH, A.WHITE))
        self.assertEqual(A.region_look(False, 'update'), (A.WHITE, A.BLACK))
        self.assertEqual(A.region_look(False, 'new'), (A.ROW, A.EDGE))
        self.assertEqual(A.region_look(False, None), (A.ROW, A.EDGE))

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
