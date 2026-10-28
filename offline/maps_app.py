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
    pmextract.extract(get_bytes, entry['bbox'], maxzoom, tiles + '.part', progress, pmextract.slim)
    os.replace(tiles + '.part', tiles)
    with zipfile.ZipFile(io.BytesIO(get_file(release_regions.ASSETS + entry['zip']))) as z:
        for f in release_regions.FILES:   # (only these: the zip's own names never chose a path)
            with open(os.path.join(dst, f), 'wb') as out:
                out.write(z.read(f) + (b'tiles=%s\n' % tiles_date.encode() if f == 'region.txt' else b''))


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


# ---- the window (tkinter): the work in a thread, its news through a queue

# OSMapsCE's colours (VestaHome's tokens): the background, the panel, the rows, the text, the dim text, the accent
INK, PANEL, ROW, TEXT, DIM, ACCENT = '#0E1013', '#1A1D22', '#23272E', '#F5F3EE', '#AAB0B8', '#FF6B1A'
GO, BUSY = 'Записать выбранные', 'Идёт запись…'   # the button, and while it writes (the user 2026-10-29)


class App:
    def __init__(self, tk, ttk):
        self.tk, self.ttk = tk, ttk
        self.news = queue.Queue()
        self.cat = {'regions': []}
        self.chosen = set()
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
        ttk.Label(picks, text='щелчок по строке — выбрать или снять', style='Dim.TLabel').pack(side='left', padx=6)
        self.tree = ttk.Treeview(self.root, columns=('size', 'have', 'state'), height=18, selectmode='none')
        for col, text, w in (('#0', 'Область', 300), ('size', 'Граф и поиск', 110), ('have', 'На флешке', 110),
                             ('state', 'Состояние', 110)):
            self.tree.heading(col, text=text, anchor='w')
            self.tree.column(col, width=w)
        self.tree.tag_configure('chosen', background=ACCENT, foreground=INK)   # the chosen ones: lit
        self.tree.tag_configure('update', foreground=ACCENT)
        self.tree.bind('<Button-1>', self.click)
        self.tree.pack(fill='both', expand=True, padx=12, pady=8)
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
        for r in matching(self.cat, self.find.get()):
            tags = ('chosen',) if r['name'] in self.chosen else ('update',) if st[r['name']] == 'update' else ()
            self.tree.insert('', 'end', iid=r['name'], text=r.get('title', r['name']), tags=tags,
                             values=('%.1f МБ' % (r['size'] / 1e6), have.get(r['name'], '—'), STATE[st[r['name']]]))
        self.status.set('Областей: %d, выбрано %d (%.0f МБ графа и поиска; плитки ещё в 5–15 раз больше). '
                        'Свободно: %s' % (len(self.cat['regions']), len(self.chosen),
                                          chosen_size(self.cat, self.chosen) / 1e6, self.free()))

    def click(self, event):   # a click picks a region or lets it go (no Ctrl)
        name = self.tree.identify_row(event.y)
        if name:
            self.chosen = toggled(self.chosen, name)
            self.refresh()
        return 'break'

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
        if entries and not self.go.instate(['disabled']):   # (once at a time)
            self.go.configure(text=BUSY, style='Busy.TButton')
            self.go.state(['disabled'])
            threading.Thread(target=self.work, args=(self.write_all, entries, self.drive.get()), daemon=True).start()

    def on_done(self, _):   # the writing over (or failed): the button back
        self.go.configure(text=GO, style='Go.TButton')
        self.go.state(['!disabled'])

    def tell(self, head, part, text):   # (from the writing thread: through the queue)
        self.news.put(('progress', part))
        self.news.put(('status', '%s: %s' % (head, text)))

    def write_all(self, entries, drive):
        build = pmextract.latest_build()
        get_bytes, day = pmextract.http_bytes(build), build.rsplit('/', 1)[1].split('.')[0]
        for i, e in enumerate(entries):
            head = '%s (%d из %d)' % (e.get('title', e['name']), i + 1, len(entries))
            self.news.put(('status', head + ': начало…'))
            install(e, drive, http_get, get_bytes, MAXZOOM, lambda part, text, h=head: self.tell(h, part, text), day)
            self.news.put(('status', head + ': граф и поиск записаны'))
        self.news.put(('status', 'Готово: %d. Флешку — в ГУ (папка OSMapsData в корне).' % len(entries)))
        self.news.put(('catalog', self.cat))


if __name__ == '__main__':
    import tkinter
    import tkinter.filedialog   # noqa: F401 (App.choose)
    import tkinter.ttk
    App(tkinter, tkinter.ttk).root.mainloop()
