"""OSMapsCE's maps for the unit, from a PC (the user 2026-10-27): the regions' list from the release's catalog
(release_regions.py), the chosen ones written to a flash drive or the SD card as \\OSMapsData\\<region>\\ - the
release's zip (the road graph, the search) and the tiles cut out of Protomaps' newest build (z0-15; nothing of the
tiles stored by us).

    python maps_app.py          (or the exe PyInstaller makes of it)
"""
import ctypes
import io
import json
import os
import queue
import shutil
import string
import threading
import zipfile

import pmextract
import release_regions

MAXZOOM = 15   # Protomaps' deepest: the house numbers, the car parks
STATE = {'new': 'нет', 'update': 'есть новее', 'ok': 'свежая'}


http_get = pmextract.http_get


def load_catalog(get=http_get):
    return json.loads(get(release_regions.ASSETS + 'catalog.json'))


def installed(drive):
    """{name: date} of the regions on the drive (their region.txt)."""
    root = os.path.join(drive, 'OSMapsData')
    names = os.listdir(root) if os.path.isdir(root) else []
    return {n: release_regions.region_info(os.path.join(root, n, 'region.txt')).get('date') for n in names
            if os.path.exists(os.path.join(root, n, 'region.txt'))}


def states(cat, have):
    """{name: 'new' | 'update' | 'ok'} of the catalog's regions against the drive's."""
    return {r['name']: 'ok' if have.get(r['name']) == r['date'] else 'update' if r['name'] in have else 'new'
            for r in cat['regions']}


def install(entry, drive, get_file, get_bytes, maxzoom, progress=None, tiles_date=''):
    """The region on the drive: the tiles first (the longest part), then the release's files - region.txt last, so a
    region half written never looks whole; it also gets the tiles' build (tiles=), for the unit: OpenFreeMap's newer
    tiles may then take their place."""
    dst = os.path.join(drive, 'OSMapsData', entry['name'])
    os.makedirs(dst, exist_ok=True)
    tiles = os.path.join(dst, 'tiles.vtp')
    pmextract.extract(get_bytes, entry['bbox'], maxzoom, tiles + '.part', progress)
    os.replace(tiles + '.part', tiles)
    with zipfile.ZipFile(io.BytesIO(get_file(release_regions.ASSETS + entry['zip']))) as z:
        for f in release_regions.FILES:   # (only these: the zip's own names never chose a path)
            with open(os.path.join(dst, f), 'wb') as out:
                out.write(z.read(f) + (b'tiles=%s\n' % tiles_date.encode() if f == 'region.txt' else b''))


def removable_drives(mask=None, kind=None):
    """The removable drives (a flash drive, an SD card), as 'E:\\'."""
    mask = mask or ctypes.windll.kernel32.GetLogicalDrives
    kind = kind or ctypes.windll.kernel32.GetDriveTypeW
    m = mask()
    return [d + ':\\' for i, d in enumerate(string.ascii_uppercase) if m >> i & 1 and kind(d + ':\\') == 2]


# ---- the window (tkinter): the work in a thread, its news through a queue

class App:
    def __init__(self, tk, ttk):
        self.tk, self.ttk = tk, ttk
        self.news = queue.Queue()
        self.cat = {'regions': []}
        self.root = tk.Tk()
        self.root.title('Карты OSMapsCE')
        self.drive = tk.StringVar(value=(removable_drives() or [''])[0])
        self.status = tk.StringVar(value='Загрузка списка областей…')
        self.layout()
        threading.Thread(target=self.work, args=(self.fetch_catalog,), daemon=True).start()
        self.root.after(200, self.poll)

    def layout(self):
        tk, ttk, top = self.tk, self.ttk, self.ttk.Frame(self.root)
        top.pack(fill='x', padx=8, pady=6)
        ttk.Label(top, text='Флешка / карта:').pack(side='left')
        ttk.Combobox(top, textvariable=self.drive, values=removable_drives(), width=8).pack(side='left', padx=4)
        ttk.Button(top, text='Папка…', command=self.choose).pack(side='left')
        ttk.Button(top, text='Обновить список', command=self.refresh).pack(side='left', padx=4)
        self.tree = ttk.Treeview(self.root, columns=('size', 'have', 'state'), height=20, selectmode='extended')
        for col, text, w in (('#0', 'Область', 260), ('size', 'Граф и поиск', 100), ('have', 'На флешке', 100),
                             ('state', 'Состояние', 100)):
            self.tree.heading(col, text=text)
            self.tree.column(col, width=w)
        self.tree.pack(fill='both', expand=True, padx=8)
        self.bar = ttk.Progressbar(self.root, maximum=1.0)
        self.bar.pack(fill='x', padx=8, pady=4)
        ttk.Label(self.root, textvariable=self.status).pack(fill='x', padx=8)
        bottom = ttk.Frame(self.root)
        bottom.pack(fill='x', padx=8, pady=6)
        ttk.Button(bottom, text='Записать выбранные', command=self.write_chosen).pack(side='left')
        ttk.Button(bottom, text='Обновить устаревшие', command=self.write_old).pack(side='left', padx=4)

    def work(self, job, *args):   # (in the thread: an error shown, not lost)
        try:
            job(*args)
        except Exception as e:    # noqa: BLE001 - the user sees it
            self.news.put(('status', 'Ошибка: %s' % e))

    def fetch_catalog(self):
        self.news.put(('catalog', load_catalog()))

    def poll(self):
        while not self.news.empty():
            kind, value = self.news.get()
            getattr(self, 'on_' + kind)(value)
        self.root.after(200, self.poll)

    def on_status(self, text):
        self.status.set(text)

    def on_progress(self, part):
        self.bar['value'] = part

    def on_catalog(self, cat):
        self.cat = cat
        self.refresh()

    def refresh(self):
        have = installed(self.drive.get()) if self.drive.get() else {}
        st = states(self.cat, have)
        self.tree.delete(*self.tree.get_children())
        for r in sorted(self.cat['regions'], key=lambda r: r.get('title', r['name'])):   # (by the Russian name)
            self.tree.insert('', 'end', iid=r['name'], text=r.get('title', r['name']),
                             values=('%.1f МБ' % (r['size'] / 1e6), have.get(r['name'], '—'), STATE[st[r['name']]]))
        self.status.set('Областей: %d. Свободно: %s' % (len(self.cat['regions']), self.free()))

    def free(self):
        return '%.1f ГБ' % (shutil.disk_usage(self.drive.get()).free / 1e9) if os.path.isdir(self.drive.get()) else '—'

    def choose(self):
        self.drive.set(self.tk.filedialog.askdirectory() or self.drive.get())
        self.refresh()

    def write_chosen(self):
        self.start([r for r in self.cat['regions'] if r['name'] in self.tree.selection()])

    def write_old(self):
        st = states(self.cat, installed(self.drive.get()))
        self.start([r for r in self.cat['regions'] if st[r['name']] == 'update'])

    def start(self, entries):
        threading.Thread(target=self.work, args=(self.write_all, entries, self.drive.get()), daemon=True).start()

    def write_all(self, entries, drive):
        build = pmextract.latest_build()
        get_bytes, day = pmextract.http_bytes(build), build.rsplit('/', 1)[1].split('.')[0]
        for i, e in enumerate(entries):
            self.news.put(('status', '%s (%d из %d): плитки и данные…' % (e['name'], i + 1, len(entries))))
            install(e, drive, http_get, get_bytes, MAXZOOM, lambda part: self.news.put(('progress', part)), day)
        self.news.put(('status', 'Готово: %d. Флешку — в ГУ (папка OSMapsData в корне).' % len(entries)))
        self.news.put(('catalog', self.cat))


if __name__ == '__main__':
    import tkinter
    import tkinter.filedialog   # noqa: F401 (App.choose)
    import tkinter.ttk
    App(tkinter, tkinter.ttk).root.mainloop()
