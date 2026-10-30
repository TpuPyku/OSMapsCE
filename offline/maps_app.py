"""OSMapsCE's maps for the unit, from a PC (the user 2026-10-27): the regions' list from the release's catalog
(release_regions.py) and their map (make_outlines.py's outlines.json), the chosen ones written to a flash drive or the
SD card as \\OSMapsData\\<region>\\ - the release's zip (the road graph, the search) and the tiles cut out of
Protomaps' newest build (z0-15; nothing of the tiles stored by us); the trunk roads (routes between the regions) as
\\OSMapsData\\trunk\\.

    python maps_app.py          (or the exe PyInstaller makes of it)
"""
import ctypes
import io
import json
import math
import os
import queue
import shutil
import string
import tempfile
import threading
import time
import urllib.request
import zipfile

import pmextract
import release_regions

MAXZOOM = 15   # Protomaps' deepest: the house numbers, the car parks
STATE = {'new': 'нет', 'update': 'есть новее', 'ok': 'свежая'}
kTilesPart = 0.75   # a region's progress: its tiles up to here, then the release's zip (the graph, the search)
STAGE = 'OSMapsCE-Maps'   # the PC's temp folder the maps go to first (copy_to: onto the card after)
STAGE_DAYS = 7            # left longer (a job given up): removed, not gone on with


http_get = pmextract.http_get


def download(url, tell, open_url=urllib.request.urlopen, piece=256 * 1024):
    """A release's file read in pieces, tell(part, 'x из y МБ') after each (the user 2026-10-29: the graph's zip and
    the trunk roads came for minutes, the bar full, nothing told)."""
    with open_url(urllib.request.Request(url, headers=pmextract.UA), timeout=120) as r:
        total, data = int(r.headers.get('Content-Length') or 0), bytearray()
        while True:
            chunk = r.read(piece)
            if not chunk:
                return bytes(data)
            data += chunk
            if total:
                tell(min(1.0, len(data) / total), '%.1f из %.1f МБ' % (len(data) / 1e6, total / 1e6))


def load_catalog(get=http_get):
    return json.loads(get(release_regions.ASSETS + 'catalog.json'))


def load_outlines(get=http_get):
    return json.loads(get(release_regions.ASSETS + 'outlines.json'))


def installed(drive):
    """{name: date} of the regions on the drive (their region.txt)."""
    root = os.path.join(drive, 'OSMapsData')
    names = os.listdir(root) if os.path.isdir(root) else []
    return {n: release_regions.region_info(os.path.join(root, n, 'region.txt')).get('date') for n in names
            if n != TRUNK and os.path.exists(os.path.join(root, n, 'region.txt'))}


TRUNK = 'trunk'   # the trunk roads of Russia and the CIS (4b): routes from one region to another
TRUNK_TITLE = 'Магистрали России и СНГ'   # (the routes between the regions)


def trunk_date(drive):
    """The trunk roads' build on the drive, None: not there."""
    path = os.path.join(drive, 'OSMapsData', TRUNK, 'region.txt')
    return release_regions.region_info(path).get('date') if os.path.exists(path) else None


def install_trunk(drive, get_file, tell=lambda part, text: None, have=None):
    """The trunk roads' graph on the drive (\\OSMapsData\\trunk\\, region.txt last): 'written', 'same' (that build
    there already - on have, the card, when written elsewhere first) or 'none' (no graph in the release)."""
    try:
        data = get_file(release_regions.ASSETS + TRUNK + '.zip', tell)
    except Exception:
        return 'none'
    dst = os.path.join(drive, 'OSMapsData', TRUNK)
    there = os.path.join(have or drive, 'OSMapsData', TRUNK, 'region.txt')
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        info = z.read('region.txt')
        if os.path.exists(there) and read_bytes(there) == info:
            return 'same'
        os.makedirs(dst, exist_ok=True)
        for f in ('trunk.grf', 'region.txt'):
            with open(os.path.join(dst, f), 'wb') as out:
                out.write(z.read(f))
    return 'written'


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def states(cat, have):
    """{name: 'new' | 'update' | 'ok'} of the catalog's regions against the drive's."""
    return {r['name']: 'ok' if have.get(r['name']) == r['date'] else 'update' if r['name'] in have else 'new'
            for r in cat['regions']}


def install(entry, drive, get_file, get_bytes, maxzoom, progress=None, tiles_date=''):
    """The region on the drive: the tiles first (the longest part), then the release's files - region.txt last, so a
    region half written never looks whole; it also gets the tiles' build (tiles=), for the unit: OpenFreeMap's newer
    tiles may then take their place. progress(part, text): the tiles up to kTilesPart, the zip's download after."""
    tell = progress or (lambda part, text: None)
    dst = os.path.join(drive, 'OSMapsData', entry['name'])
    os.makedirs(dst, exist_ok=True)
    tiles = os.path.join(dst, 'tiles.vtp')
    pmextract.extract(get_bytes, entry['bbox'], maxzoom, tiles + '.part',
                      lambda part, text: tell(part * kTilesPart, text), pmextract.slim)
    os.replace(tiles + '.part', tiles)
    data = get_file(release_regions.ASSETS + entry['zip'],
                    lambda part, text: tell(kTilesPart + (1 - kTilesPart) * part, 'граф и поиск: ' + text))
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for f in release_regions.FILES:   # (only these: the zip's own names never chose a path)
            with open(os.path.join(dst, f), 'wb') as out:
                out.write(z.read(f) + (b'tiles=%s\n' % tiles_date.encode() if f == 'region.txt' else b''))


def copy_file(src, dst, tell, piece=1 << 20):
    """A file copied in pieces, tell(bytes) after each."""
    with open(src, 'rb') as f, open(dst, 'wb') as out:
        while True:
            chunk = f.read(piece)
            if not chunk:
                return
            out.write(chunk)
            tell(len(chunk))


def copy_to(stage, drive, tell):
    """The folders written on the PC (stage\\OSMapsData\\*) onto the card at once (the user 2026-10-30: written there
    straight, the card's sectors went astray; a slow card takes it faster too): its room checked first, a folder's
    region.txt last (a region half copied never looks whole); tell(part, text)."""
    src_root = os.path.join(stage, 'OSMapsData')
    files = []
    for name in sorted(os.listdir(src_root)) if os.path.isdir(src_root) else []:
        folder = os.listdir(os.path.join(src_root, name))
        files += [(name, f) for f in sorted(folder, key=lambda f: f == 'region.txt')]
    total = sum(os.path.getsize(os.path.join(src_root, n, f)) for n, f in files)
    free = shutil.disk_usage(drive).free
    if free < total:
        raise OSError('Мало места на флешке: свободно %.0f МБ, нужно %.0f МБ' % (free / 1e6, total / 1e6))
    done = [0]

    def step(n):
        done[0] += n
        tell(done[0] / total, '%.1f из %.1f МБ' % (done[0] / 1e6, total / 1e6))
    for name, f in files:
        os.makedirs(os.path.join(drive, 'OSMapsData', name), exist_ok=True)
        copy_file(os.path.join(src_root, name, f), os.path.join(drive, 'OSMapsData', name, f), step)
    tell(1.0, 'готово')


def stage_dir(now=None):
    """The temp folder (the user 2026-10-30: kept after a break - the next time goes on there); one older than
    STAGE_DAYS removed first."""
    path = os.path.join(tempfile.gettempdir(), STAGE)
    now = now or time.time()
    if os.path.isdir(path) and now - os.path.getmtime(path) > STAGE_DAYS * 86400:
        shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path, exist_ok=True)
    return path


def staged(stage, entry, day):
    """The region in the temp folder whole: its region.txt (written last) of this catalog's date and these tiles."""
    path = os.path.join(stage, 'OSMapsData', entry['name'], 'region.txt')
    if not os.path.exists(path):
        return False
    info = release_regions.region_info(path)
    return info.get('date') == entry.get('date') and info.get('tiles') == day


def toggled(chosen, name):
    """The regions chosen after a click on one (the user 2026-10-28: a click picks, no Ctrl)."""
    return chosen ^ {name}


def chosen_size(cat, chosen):
    return sum(r['size'] for r in cat['regions'] if r['name'] in chosen)


def matching(cat, text):
    """The regions whose Russian or latin name has the text, by the Russian name."""
    t = text.strip().casefold()
    hit = [r for r in cat['regions'] if t in r.get('title', r['name']).casefold() or t in r['name'].casefold()]
    return sorted(hit, key=lambda r: r.get('title', r['name']))


def outdated(cat, have):
    return {n for n, s in states(cat, have).items() if s == 'update'}


def removable_drives(mask=None, kind=None):
    """The removable drives (a flash drive, an SD card), as 'E:\\'."""
    mask = mask or ctypes.windll.kernel32.GetLogicalDrives
    kind = kind or ctypes.windll.kernel32.GetDriveTypeW
    m = mask()
    return [d + ':\\' for i, d in enumerate(string.ascii_uppercase) if m >> i & 1 and kind(d + ':\\') == 2]


def fit(shapes, w, h, pad=12):
    """The map's projection of the outlines into w x h: (lon, lat) -> (x, y), the longitude shrunk by the middle
    latitude's cosine (the CIS's shapes as on a globe, not stretched)."""
    lons = [p[0] for rings in shapes.values() for r in rings for p in r]
    lats = [p[1] for rings in shapes.values() for r in rings for p in r]
    lon0, lat0, lat1 = min(lons), min(lats), max(lats)
    kx = math.cos(math.radians((lat0 + lat1) / 2))
    s = min((w - 2 * pad) / max(1e-9, (max(lons) - lon0) * kx), (h - 2 * pad) / max(1e-9, lat1 - lat0))
    return lambda lon, lat: (pad + (lon - lon0) * kx * s, pad + (lat1 - lat) * s)


def box_area(rings):
    """The rings' bounding box's area, degrees squared (0: no rings)."""
    pts = [p for r in rings for p in r]
    return (max(p[0] for p in pts) - min(p[0] for p in pts)) * (max(p[1] for p in pts) - min(p[1] for p in pts)) \
        if pts else 0


# ---- the window (tkinter): the work in a thread, its news through a queue

# OSMapsCE's colours (VestaHome's tokens): the background, the panel, the rows, the text, the dim text, the accent
INK, PANEL, ROW, TEXT, DIM, ACCENT = '#0E1013', '#1A1D22', '#23272E', '#F5F3EE', '#AAB0B8', '#FF6B1A'
FRESH, EDGE = '#2E5D45', '#4A505A'   # the map: a region on the drive and fresh; the regions' borders
GO, BUSY = 'Записать выбранные', 'Идёт запись…'   # the button, and while it writes (the user 2026-10-29)
MAP_W, MAP_H = 560, 520


class App:
    def __init__(self, tk, ttk):
        self.tk, self.ttk = tk, ttk
        self.news = queue.Queue()
        self.cat = {'regions': []}
        self.shapes = None
        self.chosen = set()
        self.press = None
        self.root = tk.Tk()
        self.root.title('Карты OSMapsCE')
        self.root.configure(bg=INK)
        self.drive = tk.StringVar(value=(removable_drives() or [''])[0])
        self.find = tk.StringVar()
        self.status = tk.StringVar(value='Загрузка списка областей…')
        self.style()
        self.layout()
        self.find.trace_add('write', lambda *a: self.refresh())
        threading.Thread(target=self.work, args=(self.fetch_catalog,), daemon=True).start()
        self.root.after(200, self.poll)

    def style(self):
        st = self.ttk.Style(self.root)
        st.theme_use('clam')
        font = ('Segoe UI', 11)
        st.configure('.', background=INK, foreground=TEXT, fieldbackground=ROW, font=font, bordercolor=PANEL)
        st.configure('TButton', background=ROW, foreground=TEXT, padding=(12, 6), borderwidth=0)
        st.map('TButton', background=[('active', PANEL)])
        st.configure('Go.TButton', background=ACCENT, foreground=INK, font=(font[0], 11, 'bold'))
        st.map('Go.TButton', background=[('active', '#FF8A47')])
        st.configure('Busy.TButton', background=ROW, foreground=ACCENT, font=(font[0], 11, 'bold'))   # writing: dim
        st.map('Busy.TButton', background=[('disabled', ROW)], foreground=[('disabled', ACCENT)])
        st.configure('Treeview', background=PANEL, fieldbackground=PANEL, foreground=TEXT, rowheight=30, borderwidth=0)
        st.configure('Treeview.Heading', background=INK, foreground=DIM, borderwidth=0)
        st.map('Treeview', background=[('selected', PANEL)], foreground=[('selected', TEXT)])
        for w in ('TEntry', 'TCombobox'):   # the fields dark, no light frames
            st.configure(w, foreground=TEXT, fieldbackground=ROW, background=ROW, insertcolor=TEXT, arrowcolor=TEXT,
                         bordercolor=ROW, lightcolor=ROW, darkcolor=ROW)
            st.map(w, fieldbackground=[('readonly', ROW)], foreground=[('readonly', TEXT)])
        st.configure('Treeview', bordercolor=INK, lightcolor=INK, darkcolor=INK)
        st.configure('Vertical.TScrollbar', background=ROW, troughcolor=PANEL, arrowcolor=TEXT, bordercolor=PANEL,
                     lightcolor=ROW, darkcolor=ROW)
        st.configure('Horizontal.TProgressbar', background=ACCENT, troughcolor=PANEL, borderwidth=0)
        st.configure('Dim.TLabel', foreground=DIM)

    def layout(self):
        ttk, top = self.ttk, self.ttk.Frame(self.root)
        top.pack(fill='x', padx=12, pady=10)
        ttk.Label(top, text='Флешка / карта:').pack(side='left')
        ttk.Combobox(top, textvariable=self.drive, values=removable_drives(), width=8).pack(side='left', padx=6)
        ttk.Button(top, text='Папка…', command=self.choose).pack(side='left')
        ttk.Button(top, text='Обновить', command=self.refresh).pack(side='left', padx=6)
        ttk.Entry(top, textvariable=self.find, width=24).pack(side='right')
        ttk.Label(top, text='Поиск:', style='Dim.TLabel').pack(side='right', padx=6)
        picks = ttk.Frame(self.root)
        picks.pack(fill='x', padx=12)
        for text, cmd in (('Все', self.pick_all), ('Ничего', self.pick_none), ('Устаревшие', self.pick_old)):
            ttk.Button(picks, text=text, command=cmd).pack(side='left', padx=(0, 6))
        ttk.Label(picks, text='щелчок по области на карте или по строке — выбрать или снять; колесо — ближе, '
                              'перетаскивание — сдвинуть карту', style='Dim.TLabel').pack(side='left', padx=6)
        middle = ttk.Frame(self.root)
        middle.pack(fill='both', expand=True, padx=12, pady=8)
        self.map = self.tk.Canvas(middle, width=MAP_W, height=MAP_H, bg=PANEL, highlightthickness=0)
        self.map.pack(side='left', fill='both')
        self.map.create_text(MAP_W // 2, MAP_H // 2, text='Карта областей загружается…', fill=DIM, tags='note')
        self.hover = self.map.create_text(12, 12, anchor='nw', text='', fill=TEXT, font=('Segoe UI', 11, 'bold'))
        for event, handler in (('<ButtonPress-1>', self.map_press), ('<B1-Motion>', self.map_drag),
                               ('<ButtonRelease-1>', self.map_release), ('<MouseWheel>', self.map_wheel),
                               ('<Motion>', self.map_motion)):
            self.map.bind(event, handler)
        self.tree = ttk.Treeview(middle, columns=('size', 'have', 'state'), height=16, selectmode='none')
        for col, text, w in (('#0', 'Область', 300), ('size', 'Граф и поиск', 110), ('have', 'На флешке', 110),
                             ('state', 'Состояние', 110)):
            self.tree.heading(col, text=text, anchor='w')
            self.tree.column(col, width=w)
        self.tree.tag_configure('chosen', background=ACCENT, foreground=INK)   # the chosen ones: lit
        self.tree.tag_configure('update', foreground=ACCENT)
        self.tree.bind('<Button-1>', self.click)
        self.scroll = ttk.Scrollbar(middle, orient='vertical', command=self.tree.yview)   # (the user 2026-10-29)
        self.tree.configure(yscrollcommand=self.scroll.set)
        self.scroll.pack(side='right', fill='y')
        self.tree.pack(side='right', fill='both', expand=True, padx=(8, 0))
        self.bar = ttk.Progressbar(self.root, maximum=1.0)
        self.bar.pack(fill='x', padx=12)
        ttk.Label(self.root, textvariable=self.status, style='Dim.TLabel').pack(fill='x', padx=12, pady=4)
        bottom = ttk.Frame(self.root)
        bottom.pack(fill='x', padx=12, pady=(0, 12))
        self.go = ttk.Button(bottom, text=GO, style='Go.TButton', command=self.write_chosen)
        self.go.pack(side='left')

    def work(self, job, *args):   # (in the thread: an error shown, not lost; then the button back)
        try:
            job(*args)
        except Exception as e:    # noqa: BLE001 - the user sees it
            self.news.put(('status', 'Ошибка: %s' % e))
        self.news.put(('done', None))

    def fetch_catalog(self):
        self.news.put(('catalog', load_catalog()))
        try:
            shapes = load_outlines()
        except Exception:         # noqa: BLE001 - no map: the list alone
            shapes = None
        self.news.put(('outlines', shapes))

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

    def have(self):
        return installed(self.drive.get()) if os.path.isdir(self.drive.get()) else {}

    def refresh(self):
        have = self.have()
        st = states(self.cat, have)
        self.tree.delete(*self.tree.get_children())
        find = self.find.get().strip().casefold()
        if find in TRUNK_TITLE.casefold():   # the trunk roads first (the user 2026-10-29: "where are they?")
            trunk = trunk_date(self.drive.get())
            self.tree.insert('', 'end', iid=TRUNK, text=TRUNK_TITLE, tags=('chosen',) if TRUNK in self.chosen else (),
                             values=('≈10 МБ', trunk or '—', 'есть' if trunk else 'нет'))
        for r in matching(self.cat, self.find.get()):
            tags = ('chosen',) if r['name'] in self.chosen else ('update',) if st[r['name']] == 'update' else ()
            self.tree.insert('', 'end', iid=r['name'], text=r.get('title', r['name']), tags=tags,
                             values=('%.1f МБ' % (r['size'] / 1e6), have.get(r['name'], '—'), STATE[st[r['name']]]))
        self.paint(st)
        self.status.set('Областей: %d, выбрано %d (%.0f МБ графа и поиска; плитки ещё в 5–15 раз больше). '
                        'Свободно: %s' % (len(self.cat['regions']), len(self.chosen - {TRUNK}),
                                          chosen_size(self.cat, self.chosen) / 1e6, self.free()))

    def click(self, event):   # a click picks a region or lets it go (no Ctrl)
        name = self.tree.identify_row(event.y)
        if name:
            self.chosen = toggled(self.chosen, name)
            self.refresh()
        return 'break'

    # ---- the map: the regions' outlines, lit as their rows

    def on_outlines(self, shapes):
        self.shapes = shapes
        self.map.delete('region')
        self.map.itemconfigure('note', text='' if shapes else 'Карты областей нет — выбор по списку')
        if not shapes:
            return
        xy = fit(shapes, MAP_W, MAP_H)
        for name in sorted(shapes, key=lambda n: -box_area(shapes[n])):   # the big ones first: the small on top
            for ring in shapes[name]:
                self.map.create_polygon([c for p in ring for c in xy(*p)], fill=ROW, outline=EDGE,
                                        tags=('region', 'r:' + name))
        self.map.tag_raise(self.hover)
        self.refresh()

    def paint(self, st):
        for name in self.shapes or ():
            fill = ACCENT if name in self.chosen else FRESH if st.get(name) == 'ok' else ROW
            self.map.itemconfigure('r:' + name, fill=fill, outline=ACCENT if st.get(name) == 'update' else EDGE)

    def region_at(self, x, y):
        for item in reversed(self.map.find_overlapping(x, y, x, y)):
            for tag in self.map.gettags(item):
                if tag.startswith('r:'):
                    return tag[2:]
        return None

    def map_press(self, e):
        self.press, self.last, self.moved = (e.x, e.y), (e.x, e.y), False

    def map_drag(self, e):
        if self.press and (abs(e.x - self.press[0]) > 4 or abs(e.y - self.press[1]) > 4):
            self.moved = True
        if self.press and self.moved:
            self.map.move('region', e.x - self.last[0], e.y - self.last[1])
            self.last = (e.x, e.y)

    def map_release(self, e):
        name = self.region_at(e.x, e.y) if self.press and not self.moved else None
        self.press = None
        if name:
            self.chosen = toggled(self.chosen, name)
            self.refresh()

    def map_wheel(self, e):
        f = 1.25 if e.delta > 0 else 0.8
        self.map.scale('region', e.x, e.y, f, f)

    def map_motion(self, e):
        name = self.region_at(e.x, e.y)
        titles = {r['name']: r.get('title', r['name']) for r in self.cat['regions']}
        self.map.itemconfigure(self.hover, text=titles.get(name, name or ''))

    # ----

    def pick_all(self):
        self.chosen = {r['name'] for r in matching(self.cat, self.find.get())}
        self.refresh()

    def pick_none(self):
        self.chosen = set()
        self.refresh()

    def pick_old(self):
        self.chosen = outdated(self.cat, self.have())
        self.refresh()

    def free(self):
        return '%.1f ГБ' % (shutil.disk_usage(self.drive.get()).free / 1e9) if os.path.isdir(self.drive.get()) else '—'

    def choose(self):
        self.drive.set(self.tk.filedialog.askdirectory() or self.drive.get())
        self.refresh()

    def write_chosen(self):
        entries = [r for r in self.cat['regions'] if r['name'] in self.chosen]
        drive = self.drive.get()
        trunk = TRUNK in self.chosen or bool(entries) and trunk_date(drive) is None   # (none there: with any region)
        if (entries or trunk) and not self.go.instate(['disabled']):   # (once at a time)
            self.go.configure(text=BUSY, style='Busy.TButton')
            self.go.state(['disabled'])
            threading.Thread(target=self.work, args=(self.write_all, entries, drive, trunk), daemon=True).start()

    def on_done(self, _):   # the writing over (or failed): the button back
        self.go.configure(text=GO, style='Go.TButton')
        self.go.state(['!disabled'])

    def tell(self, head, part, text):   # (from the writing thread: through the queue)
        self.news.put(('progress', part))
        self.news.put(('status', '%s: %s' % (head, text)))

    def write_all(self, entries, drive, trunk):
        """The regions, then the trunk roads into the PC's temp folder, then all onto the card (copy_to); the bar for
        the whole job (the user 2026-10-29: full before the end) - each a part of it, the copy the last."""
        total = len(entries) + (1 if trunk else 0) + 1
        stage = stage_dir()
        done = self.write_stage(entries, stage, drive, trunk, total)
        copy_to(stage, drive, lambda part, text: self.tell('На флешку', (total - 1 + part) / total, text))
        shutil.rmtree(stage, ignore_errors=True)   # (an error before: kept - the next time goes on there)
        self.news.put(('status', done + '. Флешку — в ГУ (папка OSMapsData в корне).'))
        self.news.put(('catalog', self.cat))

    def write_stage(self, entries, stage, drive, trunk, total):   # the regions and the trunk roads into stage
        if entries:
            build = pmextract.latest_build()
            get_bytes, day = pmextract.http_bytes(build), build.rsplit('/', 1)[1].split('.')[0]
        for i, e in enumerate(entries):
            head = '%s (%d из %d)' % (e.get('title', e['name']), i + 1, len(entries))
            if staged(stage, e, day):   # downloaded before a break
                self.tell(head, (i + 1) / total, 'уже скачано')
                continue
            self.news.put(('status', head + ': начало…'))
            install(e, stage, download, get_bytes, MAXZOOM,
                    lambda part, text, h=head, k=i: self.tell(h, (k + part) / total, text), day)
        done = 'Готово: областей %d' % len(entries)
        if trunk:
            head = 'Магистрали России и СНГ'
            got = install_trunk(stage, download, lambda part, text: self.tell(head, (total - 2 + part) / total, text),
                                drive)
            done += {'written': ', магистрали записаны', 'same': ', магистрали уже свежие',
                     'none': ', магистралей в релизе нет'}[got]
        return done


if __name__ == '__main__':
    import tkinter
    import tkinter.filedialog   # noqa: F401 (App.choose)
    import tkinter.ttk
    App(tkinter, tkinter.ttk).root.mainloop()
