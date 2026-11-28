"""Speed cameras and road dangers from openspeedcam.net's Navitel export (the user 2026-11-22): the PC loader
(maps_app.py) cuts them per region into its OSMapsData\\<name>\\ - speedcam.txt (the cameras) and danger.lay (the
bumps and dangers, OSMapsCE's layers file).

    python -m unittest discover -s offline/test
"""
import os
import queue
import shutil
import struct
import sys
import tempfile
import threading
import unittest
import unittest.mock

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ['OSMAPS_LANG'] = 'ru'
sys.path.insert(0, os.path.dirname(HERE))
import maps_app    # noqa: E402
import speedcams   # noqa: E402

CSV = ('IDX,X,Y,TYPE,SPEED,DirType,Direction\r\n'
       '1,37.6175,55.7558,1,60,1,90\r\n'          # a static camera in Moscow
       '2,37.5,55.7,101,40,0,0\r\n'                # a "speed limit": dropped
       '\r\n'
       'мусор,строка\r\n'
       '3;37.61;55.75;102;0;0;0\r\n'               # a speed bump, ';' separated
       '4,37.62,55.76,105,0,0,0\r\n'               # a dangerous crossroads
       '5,30.3,59.9,3,40,2,180\r\n'                # a red-light camera in St Petersburg
       '6,37.63,55.77,5,0,0,0\r\n'                 # a mobile ambush
       '7,37.64,55.78,999,0,0,0\r\n').encode('cp1251')   # unknown: dropped
MOSCOW = [36.0, 54.0, 40.0, 57.0]   # minLon, minLat, maxLon, maxLat


def read_lay(path):
    """OSMapsCE's points file: the header, the cells, the 12-byte points."""
    with open(path, 'rb') as f:
        data = f.read()
    magic, version, cells, n = struct.unpack_from('<4sIII', data)
    at = 16 + 8 * cells
    return magic, version, [struct.unpack_from('<iiBBH', data, at + 12 * i) for i in range(n)]


class Speedcams(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_parse(self):
        rows = speedcams.parse(CSV)
        self.assertEqual([r[0] for r in rows], [1, 2, 3, 4, 5, 6, 7])
        self.assertEqual(rows[0], (1, 37.6175, 55.7558, 1, 60, 1, 90))
        self.assertEqual(rows[2], (3, 37.61, 55.75, 102, 0, 0, 0))
        self.assertEqual(speedcams.parse(b''), [])
        self.assertEqual(speedcams.parse(b'1,2,3\n'), [])   # (too short)

    def test_cut(self):
        rows = speedcams.parse(CSV)
        self.assertEqual([r[0] for r in speedcams.cut(rows, MOSCOW)], [1, 2, 3, 4, 6, 7])
        self.assertEqual(speedcams.cut(rows, [37.6175, 55.7558, 37.6175, 55.7558])[0][0], 1)   # the edge: inside

    def test_speedcam_txt(self):
        """The cameras alone, as VestaHome's CamLine reads them (the header's columns, ',', CRLF)."""
        path = os.path.join(self.tmp, 'speedcam.txt')
        n = speedcams.write_speedcam(path, speedcams.cut(speedcams.parse(CSV), MOSCOW))
        self.assertEqual(n, 2)
        with open(path, 'rb') as f:
            self.assertEqual(f.read(), b'IDX,X,Y,TYPE,SPEED,DirType,Direction\r\n'
                                       b'1,37.6175,55.7558,1,60,1,90\r\n6,37.63,55.77,5,0,0,0\r\n')
        self.assertEqual(speedcams.write_speedcam(path, []), 0)   # none: an old file off
        self.assertFalse(os.path.exists(path))
        self.assertEqual(speedcams.write_speedcam(path, []), 0)   # (and none there)

    def test_speedcam_fillers(self):
        """openspeedcam.net's fillers are no limits (HU 2026-11-24: red-light cameras "5 km/h", beeping all the
        way): its red-light cameras' 5, any speed under 10 or of 160 and above - written as 0, unknown; a red-light
        camera with a real limit keeps it."""
        path = os.path.join(self.tmp, 'speedcam.txt')
        rows = [(1, 37.6, 55.7, 3, 5, 1, 90), (2, 37.6, 55.7, 1, 160, 1, 90), (3, 37.6, 55.7, 1, 9, 0, 0),
                (4, 37.6, 55.7, 1, 10, 0, 0), (5, 37.6, 55.7, 4, 159, 0, 0), (6, 37.6, 55.7, 3, 60, 0, 0)]
        speedcams.write_speedcam(path, rows)
        with open(path, 'rb') as f:
            speeds = [line.split(b',')[4] for line in f.read().split(b'\r\n')[1:] if line]
        self.assertEqual(speeds, [b'0', b'0', b'0', b'10', b'159', b'60'])

    def test_new_navitel_codes(self):
        """The export is New Navitel's (newnavitel.blogspot.com, "Различие между speedcam Navitel и New Navitel" - the
        user 2026-11-27): a kind's subtype in SPEED. 101 with 155 (a radar complex) and 25 (a lane camera) are
        cameras - written as static ones, no limit; 101's others (a limit sign, a police post 5, a town 15, railway
        crossings 125/135, video control 160) dropped. An ambush (5) with x5: control at x5 - 5; its 125..155 (a STOP
        sign, a crossing, overtaking, the oncoming lane) no limit."""
        path = os.path.join(self.tmp, 'speedcam.txt')
        rows = [(1, 37.6, 55.7, 101, 155, 1, 90), (2, 37.6, 55.7, 101, 25, 1, 0), (3, 37.6, 55.7, 101, 40, 0, 0),
                (4, 37.6, 55.7, 101, 5, 0, 0), (5, 37.6, 55.7, 101, 15, 0, 0), (6, 37.6, 55.7, 101, 125, 0, 0),
                (7, 37.6, 55.7, 101, 160, 0, 0), (8, 37.6, 55.7, 5, 65, 0, 0), (9, 37.6, 55.7, 5, 15, 0, 0),
                (10, 37.6, 55.7, 5, 115, 0, 0), (11, 37.6, 55.7, 5, 145, 0, 0), (12, 37.6, 55.7, 5, 125, 0, 0),
                (13, 37.6, 55.7, 5, 60, 0, 0), (14, 37.6, 55.7, 1, 60, 0, 0)]
        self.assertEqual(speedcams.write_speedcam(path, rows), 9)
        with open(path, 'rb') as f:
            got = [tuple(line.split(b',')[i] for i in (0, 3, 4)) for line in f.read().split(b'\r\n')[1:] if line]
        self.assertEqual(got, [(b'1', b'1', b'0'), (b'2', b'1', b'0'), (b'8', b'5', b'60'), (b'9', b'5', b'10'),
                               (b'10', b'5', b'110'), (b'11', b'5', b'0'), (b'12', b'5', b'0'), (b'13', b'5', b'60'),
                               (b'14', b'1', b'60')])

    def test_danger_lay(self):
        """The bumps (kind 2) and dangers (kind 4, fee = the danger's kind) as layers.lay's points."""
        path = os.path.join(self.tmp, 'danger.lay')
        n = speedcams.write_danger(path, speedcams.cut(speedcams.parse(CSV), MOSCOW))
        self.assertEqual(n, 2)
        magic, version, pts = read_lay(path)
        self.assertEqual((magic, version), (b'OSML', 1))
        self.assertEqual(sorted(pts), [(55750000, 37610000, 2, 0, 0), (55760000, 37620000, 4, 5, 0)])
        self.assertEqual(speedcams.write_danger(path, []), 0)
        self.assertFalse(os.path.exists(path))

    def test_cams_lay(self):
        """The cameras for OSMapsCE beside speedcam.txt (the user 2026-11-28: a layer of them): cams.lay's points -
        kind 5, fee the camera's type, places its limit (0 not known); none - neither file."""
        path = os.path.join(self.tmp, 'speedcam.txt')
        n = speedcams.write_speedcam(path, speedcams.cut(speedcams.parse(CSV), MOSCOW))
        self.assertEqual(n, 2)
        magic, version, pts = read_lay(os.path.join(self.tmp, 'cams.lay'))
        self.assertEqual((magic, version), (b'OSML', 1))
        self.assertEqual(sorted(pts), [(55755800, 37617500, 5, 1, 60), (55770000, 37630000, 5, 5, 0)])
        self.assertEqual(speedcams.write_speedcam(path, []), 0)
        self.assertFalse(os.path.exists(path))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, 'cams.lay')))

    def test_download(self):
        """Once a run: the file kept in the cache, read from there the next time; not the file - an error."""
        cache = os.path.join(self.tmp, 'cache')
        got = []

        def fetch(url):
            got.append(url)
            return CSV
        self.assertEqual(speedcams.download(cache, fetch, 'RU'), CSV)
        self.assertEqual(speedcams.download(cache, fetch, 'RU'), CSV)
        self.assertEqual(got, [speedcams.url('RU')])
        with unittest.mock.patch.object(speedcams, 'RUN', speedcams.RUN + 1e6):   # (a later run: fetched anew)
            speedcams.download(cache, fetch, 'RU')
        self.assertEqual(len(got), 2)
        speedcams.download(cache, fetch, 'BY')   # (another country's file: its own, kept apart)
        self.assertEqual(got[2:], [speedcams.url('BY')])
        self.assertEqual(len(set(map(speedcams.url, speedcams.IDS))), 4)
        with self.assertRaises(ValueError):
            speedcams.download(os.path.join(self.tmp, 'other'), lambda url: b'<html>quota</html>', 'RU')

    def test_countries(self):
        """The countries' files a set of regions needs (the user 2026-11-27: only for the regions chosen)."""
        self.assertEqual(speedcams.countries([]), [])
        self.assertEqual(speedcams.countries(['moscow_oblast', 'germany']), ['RU'])   # (any other: Russia's)
        self.assertEqual(speedcams.countries(['belarus', 'kiev_oblast', 'lviv_oblast']), ['BY', 'UA'])
        self.assertEqual(speedcams.countries(['kazakhstan']), ['KZ'])
        self.assertEqual(speedcams.countries(['donetsk']), ['RU', 'UA'])   # (the disputed lands: in both files)
        self.assertEqual([n for n in speedcams.UKRAINE if n not in maps_app.release_regions.TITLES], [])


class Loader(unittest.TestCase):
    """maps_app: the cameras into each written region's folder on the card."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.card = os.path.join(self.tmp, 'E')
        for name in ('moscow', 'spb'):
            os.makedirs(os.path.join(self.card, 'OSMapsData', name))
        self.cat = {'regions': [{'name': 'moscow', 'title': 'М', 'bbox': MOSCOW, 'date': 'd'},
                                {'name': 'spb', 'title': 'П', 'bbox': [29.0, 59.0, 31.0, 61.0], 'date': 'd'}]}
        app = type('A', (), {})()
        app.news, app.cat, app.stop = queue.Queue(), self.cat, threading.Event()
        app.tell = lambda head, part, text: maps_app.App.tell(app, head, part, text)
        self.app = app

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def file(self, name, f):
        return os.path.join(self.card, 'OSMapsData', name, f)

    def add(self, names, download):
        with unittest.mock.patch.object(maps_app.speedcams, 'download', download), \
                unittest.mock.patch.object(maps_app, 'CAMS_CACHE', os.path.join(self.tmp, 'cache')):
            return maps_app.App.add_cams(self.app, self.card, names)

    def test_written(self):
        said = self.add(['moscow', 'spb', 'gone'], lambda cache, fetch, country: CSV)
        self.assertEqual(said, ', камер и опасностей: 5 (данные камер: openspeedcam.net)')
        self.assertTrue(os.path.exists(self.file('moscow', 'speedcam.txt')))
        self.assertTrue(os.path.exists(self.file('moscow', 'danger.lay')))
        self.assertTrue(os.path.exists(self.file('spb', 'speedcam.txt')))
        self.assertTrue(os.path.exists(self.file('moscow', 'cams.lay')) and os.path.exists(self.file('spb', 'cams.lay')))
        self.assertFalse(os.path.exists(self.file('spb', 'danger.lay')))   # (no dangers there)

    def test_failed(self):
        """No cameras: told, the maps written all the same."""
        def fail(cache, fetch, country):
            raise OSError('нет сети')
        self.assertEqual(self.add(['moscow'], fail), ', камеры не скачаны (нет сети)')

    def test_countries_files(self):
        """Only the chosen regions' countries downloaded (not for the overview); a region of its own countries' files
        only, whatever else chosen (Russia's cameras in Belarus's bbox not there)."""
        os.makedirs(os.path.join(self.card, 'OSMapsData', 'belarus'))
        self.cat['regions'].append({'name': 'belarus', 'title': 'Б', 'bbox': [23.0, 51.0, 33.0, 56.5], 'date': 'd'})
        minsk = b'IDX,X,Y,TYPE,SPEED,DirType,Direction\r\n9,27.56,53.9,1,60,0,0\r\n'
        asked = []

        def download(cache, fetch, country):
            asked.append(country)
            return minsk if country == 'BY' else CSV + b'8,27.57,53.91,1,60,0,0\r\n'
        said = self.add(['belarus', 'overview'], download)
        self.assertEqual(said, ', камер и опасностей: 1 (данные камер: openspeedcam.net)')
        self.assertEqual(asked, ['BY'])
        self.add(['moscow', 'belarus'], download)
        self.assertEqual(asked[1:], ['BY', 'RU'])
        with open(self.file('belarus', 'speedcam.txt')) as f:
            self.assertEqual(f.read().splitlines()[1:], ['9,27.56,53.9,1,60,0,0'])

    def test_fetch_through_download(self):
        """The file read with the loader's own download, its progress shown."""
        def download(cache, fetch, country):
            return fetch(speedcams.url(country))
        with unittest.mock.patch.object(maps_app, 'download', lambda url, tell: tell(1.0, 'x') or CSV):
            self.add(['moscow'], download)
        self.assertEqual(self.app.news.get(), ('progress', 1.0))

    def test_write_all_and_ready(self):
        """write_all and fetch_ready add them when asked (the checkbox)."""
        calls = []
        app = self.app
        app.add_cams = lambda drive, names: calls.append(names) or ', cams'
        app.write_stage = lambda *a: 'Готово'
        patch = unittest.mock.patch.object
        with patch(maps_app, 'copy_to', lambda *a: None), patch(maps_app, 'stage_dir', lambda: self.tmp + '/S'):
            maps_app.App.write_all(app, self.cat['regions'], self.card, False, False, True)
            maps_app.App.write_all(app, self.cat['regions'], self.card, False, False, False)
            with patch(maps_app, 'whole_url', lambda *a: 'http://f/x.zip'), \
                    patch(maps_app, 'download_any', lambda urls, path, tell: open(path, 'wb').close()), \
                    patch(maps_app, 'unpack_whole', lambda *a: None):
                maps_app.App.fetch_ready(app, [('spb', self.cat['regions'][1]), ('overview', {})], 'files',
                                         self.card, True)
        self.assertEqual(calls, [['moscow', 'spb'], ['spb', 'overview']])   # (write_cams: regions of the catalog only)
        news = []
        while not app.news.empty():
            k, v = app.news.get()
            if k == 'status':
                news.append(v)
        self.assertTrue(any(v.startswith('Готово, cams.') for v in news), news)
        self.assertTrue(any(v.startswith('Готово с сервера: П, Обзорная карта и магистрали, cams.') for v in news), news)

    def test_cams_only(self):
        """"Обновить камеры": the regions on the card, no maps downloaded."""
        for name in ('moscow', 'spb'):
            with open(self.file(name, 'region.txt'), 'w') as f:
                f.write('date=d\n')
        calls = []
        self.app.add_cams = lambda drive, names: calls.append(sorted(names)) or ', cams'
        maps_app.App.cams_only(self.app, self.card)
        self.assertEqual(calls, [['moscow', 'spb']])
        self.assertEqual(self.app.news.get(), ('status', 'Камеры обновлены, cams.'))

    def test_window(self):
        """The checkbox (on) goes with the job; "Обновить камеры": not while writing, not with no regions there."""
        import tkinter
        import tkinter.ttk
        patch = unittest.mock.patch.object
        with patch(maps_app, 'load_catalog', lambda: self.cat), patch(maps_app, 'load_outlines', lambda: {}), \
                patch(maps_app, 'removable_drives', lambda: [self.tmp]):
            app = maps_app.App(tkinter, tkinter.ttk)
        started = []
        try:
            app.root.geometry('+-3000+-3000')
            self.assertTrue(app.cams_on.get())
            app.update_cams()
            self.assertEqual(app.status.get(), 'Областей на флешке нет')
            with patch(maps_app.threading, 'Thread', lambda target, args, daemon: type(
                    'T', (), {'start': lambda s: started.append(args)})()):
                app.go.configure(text=maps_app.BUSY)
                app.update_cams()
                self.assertEqual(app.status.get(), 'Идёт запись')
                app.go.configure(text=maps_app.GO)
                app.drive.set(self.card)
                with open(self.file('moscow', 'region.txt'), 'w') as f:
                    f.write('date=d\n')
                app.update_cams()
                app.go.configure(text=maps_app.GO)
                app.cat, app.chosen = self.cat, {'moscow'}
                app.write_chosen()
        finally:
            app.root.destroy()
        self.assertEqual(started, [(app.cams_only, self.card),
                                   (app.write_all, [self.cat['regions'][0]], self.card, True, True, True)])


if __name__ == '__main__':
    unittest.main()
