"""OSMapsCE's maps for the unit, from a PC (the user 2026-10-27): the regions' list from the release's catalog
(release_regions.py) and their map (make_outlines.py's outlines.json), the chosen ones written to a flash drive or the
SD card as \\OSMapsData\\<region>\\ - the release's zip (the road graph, the search) and the tiles cut out of
Protomaps' newest build (z0-15; nothing of the tiles stored by us); the trunk roads (routes between the regions) as
\\OSMapsData\\overview\\.

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
import webbrowser
import zipfile

import pmextract
import release_regions
import yadisk

MAXZOOM = 15   # Protomaps' deepest: the house numbers, the car parks
STATE = {'new': 'нет', 'update': 'есть новее', 'ok': 'свежая'}
kTilesPart = 0.75   # a region's progress: its tiles up to here, then the release's zip (the graph, the search)
RELEASE = '1.0'   # the loader's own number, raised by hand (it changes less often than OSMapsCE); the date: build_app.bat's
try:
    from app_build import BUILD
except ImportError:   # (run from the sources)
    BUILD = None


def app_version(build=BUILD):
    """The number shown: "1.0.261106", from the sources "1.0"."""
    return '%s.%s' % (RELEASE, build) if build else RELEASE


STAGE = 'OSMapsCE-Maps'   # the PC's temp folder the maps go to first (copy_to: onto the card after)
STAGE_DAYS = 7            # left longer (a job given up): removed, not gone on with


http_get = pmextract.http_get


class Cancelled(Exception):
    """"Отменить запись" pressed (the user 2026-10-30): raised from the job's progress, where it is."""


def stream(url, put, tell, open_url, piece):
    """A file read in pieces, put(chunk) and tell(part, 'x из y МБ') after each (the user 2026-10-29: the graph's zip
    and the trunk roads came for minutes, the bar full, nothing told)."""
    with open_url(urllib.request.Request(url, headers=pmextract.UA), timeout=120) as r:
        total, got = int(r.headers.get('Content-Length') or 0), 0
        while True:
            chunk = r.read(piece)
            if not chunk:
                return
            put(chunk)
            got += len(chunk)
            if total:
                tell(min(1.0, got / total), '%.1f из %.1f МБ' % (got / 1e6, total / 1e6))


def download(url, tell, open_url=urllib.request.urlopen, piece=256 * 1024):
    """A release's file in memory, read in pieces (stream)."""
    data = bytearray()
    stream(url, data.extend, tell, open_url, piece)
    return bytes(data)


def download_to(url, path, tell, open_url=urllib.request.urlopen, piece=1 << 20):
    """A whole archive into a file (hundreds of MB: not in memory), path.part until it is all there."""
    with open(path + '.part', 'wb') as f:
        stream(url, f.write, tell, open_url, piece)
    os.replace(path + '.part', path)


def secure_first(url):
    """The links to try for an archive: the own server's (plain http://) over HTTPS first - its certificate for its
    address (the user 2026-11-12: the browser warned); others as they are."""
    return ['https://' + url[7:], url] if url.startswith('http://') else [url]


def download_any(urls, path, tell, open_url=None):
    """download_to from the first link that answers; the last one's error when none does."""
    for i, url in enumerate(urls):
        try:
            return download_to(url, path, tell) if open_url is None else download_to(url, path, tell, open_url)
        except (OSError, ValueError):   # (urllib's errors: OSError; a refused certificate too)
            if i == len(urls) - 1:
                raise


def load_catalog(get=http_get):
    return json.loads(get(release_regions.ASSETS + 'catalog.json'))


def load_outlines(get=http_get):
    return json.loads(get(release_regions.ASSETS + 'outlines.json'))


def installed(drive):
    """{name: date} of the regions on the drive (their region.txt)."""
    root = os.path.join(drive, 'OSMapsData')
    names = os.listdir(root) if os.path.isdir(root) else []
    return {n: release_regions.region_info(os.path.join(root, n, 'region.txt')).get('date') for n in names
            if n not in (TRUNK, OVERVIEW) and os.path.exists(os.path.join(root, n, 'region.txt'))}


TRUNK = 'trunk'   # the old folder of the trunk roads (cards from before 2026-11-06), and their archive in the release
OVERVIEW = 'overview'   # one pack as a region's (the user 2026-11-06): the map's small zooms inside the regions' borders
OVERVIEW_TITLE = 'Обзорная карта и магистрали'   # (a region's pack holds its region only - empty round it; OSMapsCE takes
OVERVIEW_Z = 8          # them when no cached tile) and the trunk roads' graph (routes from one region to another)


def bounds(pts):
    """(minX, minY, maxX, maxY) of the points."""
    return min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)


def overview_box(cat):
    """All the regions' bounds: (minLon, minLat, maxLon, maxLat)."""
    return bounds([c for r in cat['regions'] for c in (r['bbox'][:2], r['bbox'][2:])])


def overview_rings(shapes):
    """The regions' outlines as one list of rings, None: not known (the box then)."""
    return [r for rings in shapes.values() for r in rings] if shapes else None


def pack_info(dst, have, **new):
    """The pack's region.txt in dst: what is known of it (on have, the card, when written elsewhere first; in dst) with
    the new keys. One file tells both builds: tiles= the map's, date= the graph's."""
    info = {}
    for root in (have, os.path.dirname(os.path.dirname(dst))):
        path = os.path.join(root, 'OSMapsData', OVERVIEW, 'region.txt') if root else ''
        if path and os.path.exists(path):
            info.update(release_regions.region_info(path))
    info.update(new, name=OVERVIEW)
    with open(os.path.join(dst, 'region.txt'), 'w') as f:
        f.write(''.join('%s=%s\n' % kv for kv in info.items()))


def folder_info(drive, name, key):
    """A key of the drive's OSMapsData\\<name>\\region.txt, None: not there."""
    path = os.path.join(drive, 'OSMapsData', name, 'region.txt')
    return release_regions.region_info(path).get(key) if os.path.exists(path) else None


def overview_date(drive):
    """The overview's tiles' build on the drive, None: not there."""
    return folder_info(drive, OVERVIEW, 'tiles')


def install_overview(cat, drive, get_bytes, day, tell=lambda part, text: None, rings=None, have=None):
    """The overview's map on the drive (\\OSMapsData\\overview\\overview.vtp - not tiles.vtp: no region's), cut out of
    Protomaps' build: the tiles the regions' outlines touch, no margin (rings; none known: their box); region.txt last
    (tiles= its build)."""
    dst = os.path.join(drive, 'OSMapsData', OVERVIEW)
    os.makedirs(dst, exist_ok=True)
    out = os.path.join(dst, 'overview.vtp')
    pmextract.extract(get_bytes, overview_box(cat), OVERVIEW_Z, out + '.part', tell, pmextract.slim, rings, 0)
    os.replace(out + '.part', out)
    pack_info(dst, have, tiles=day)


def trunk_date(drive):
    """The trunk roads' build in the drive's pack, None: not there."""
    return folder_info(drive, OVERVIEW, 'date')


def install_trunk(drive, get_file, tell=lambda part, text: None, have=None):
    """The trunk roads' graph into the drive's pack (region.txt last): 'written', 'same' (that build there already -
    on have, the card, when written elsewhere first) or 'none' (no graph in the release)."""
    try:
        data = get_file(release_regions.ASSETS + TRUNK + '.zip', tell)
    except Exception:
        return 'none'
    dst = os.path.join(drive, 'OSMapsData', OVERVIEW)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        date = dict(line.split('=', 1) for line in z.read('region.txt').decode().split('\n') if '=' in line).get('date')
        if date and trunk_date(have or drive) == date:
            return 'same'
        os.makedirs(dst, exist_ok=True)
        with open(os.path.join(dst, 'trunk.grf'), 'wb') as out:
            out.write(z.read('trunk.grf'))
    pack_info(dst, have, date=date)
    return 'written'


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
    # (z15 whole for now, as release_regions: deepest=pmextract.numbers with the OSMapsCE release drawing z15+ from z14)
    os.replace(tiles + '.part', tiles)
    data = get_file(release_regions.ASSETS + entry['zip'],
                    lambda part, text: tell(kTilesPart + (1 - kTilesPart) * part, 'граф и поиск: ' + text))
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for f in release_regions.FILES:   # (only these: the zip's own names never chose a path)
            if f not in z.namelist():   # (a region not rebuilt since: its old zip has no layers)
                continue
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


def staged_files(src_root):
    """(folder, file) of the temp folder's regions, each folder's region.txt last."""
    files = []
    for name in sorted(os.listdir(src_root)) if os.path.isdir(src_root) else []:
        folder = os.listdir(os.path.join(src_root, name))
        files += [(name, f) for f in sorted(folder, key=lambda f: f == 'region.txt')]
    return files


def unmark(folder):
    """The region's region.txt off (written last, removed first: a folder never looks whole half done)."""
    if os.path.exists(os.path.join(folder, 'region.txt')):
        os.remove(os.path.join(folder, 'region.txt'))


def plain_name(name):
    return bool(name) and name == os.path.basename(name) and name not in ('.', '..')


def copy_to(stage, drive, tell):
    """The folders written on the PC (stage\\OSMapsData\\*) onto the card at once (the user 2026-10-30: written there
    straight, the card's sectors went astray; a slow card takes it faster too): its room checked first, a folder's
    region.txt last (a region half copied never looks whole); tell(part, text)."""
    src_root = os.path.join(stage, 'OSMapsData')
    files = staged_files(src_root)
    total = sum(os.path.getsize(os.path.join(src_root, n, f)) for n, f in files)
    free = shutil.disk_usage(drive).free
    if free < total:
        raise OSError('Мало места на флешке: свободно %.0f МБ, нужно %.0f МБ' % (free / 1e6, total / 1e6))
    done = [0]

    def step(n):
        done[0] += n
        tell(done[0] / total, '%.1f из %.1f МБ' % (done[0] / 1e6, total / 1e6))
    for name in sorted({n for n, _ in files}):   # an older one's region.txt off first: stopped midway, never whole
        unmark(os.path.join(drive, 'OSMapsData', name))
    for name, f in files:
        os.makedirs(os.path.join(drive, 'OSMapsData', name), exist_ok=True)
        copy_file(os.path.join(src_root, name, f), os.path.join(drive, 'OSMapsData', name, f), step)
    tell(1.0, 'готово')


def remove_regions(drive, names):
    """The regions' folders off the drive (the user 2026-10-30: "Удалить с флешки"), each region.txt first (a folder
    half removed never looks whole); only plain names of folders there. The names removed."""
    gone = []
    for name in names:
        path = os.path.join(drive, 'OSMapsData', name)
        if not plain_name(name) or not os.path.isdir(path):
            continue
        unmark(path)
        shutil.rmtree(path)
        gone.append(name)
        if name == OVERVIEW:
            drop_old_trunk(drive)
    return gone


def drop_old_trunk(drive):
    """The trunk roads' own folder of a card from before: OSMapsCE takes the first trunk.grf it finds."""
    shutil.rmtree(os.path.join(drive, 'OSMapsData', TRUNK), ignore_errors=True)


def ready_links(cat, chosen, href=yadisk.public_href):
    """The ready archives on the catalog's Yandex Disk (the user 2026-10-30: the app for one's own build; the ready
    ones by their links): the chosen regions whole there (and the overview) - their download links; none chosen - the
    disk's folder; no disk - none. Those on the maps' own server (their entry's "files": the catalog's "files" folder,
    the user 2026-11-09: the disk's upload limit ran out) by their links straight."""
    files, disk = cat.get('files'), cat.get('disk')
    if not chosen:
        return [disk] if disk else []
    links = []
    for whole in ready_wholes(cat, chosen):
        if whole.get('files') and files:
            links.append(files + whole['zip'])
        elif disk:
            links.append(href(disk, whole['zip']))
    return links


def ready_wholes(cat, chosen):
    """The chosen regions' whole archives' entries (and the overview's) the catalog tells."""
    wholes = [r['full'] for r in cat['regions'] if r['name'] in chosen and 'full' in r]
    if OVERVIEW in chosen and 'overview' in cat:
        wholes.append(cat['overview'])
    return wholes


FILES, DISK = 'files', 'disk'   # a row's two buttons (the user 2026-11-12): the own server's whole zip, the disk's copy
OLD_MARK = '⚠ старая'          # the disk's copy older than the region's build (the disk: uploaded twice a month)
FAR_MARK = '✈ '                # a region outside the former USSR: last in the list (BMP symbols: Windows 7's Tk)


def source_ok(cat, entry, source):
    """The entry's whole zip (a region's, or the overview's) there: on the own server (its "files"), on the disk (its
    "disk" copy, the catalog's listing)."""
    if source == FILES:
        return bool(cat.get('files') and entry.get('full', entry).get('files'))
    return bool(cat.get('disk') and entry.get('disk'))


def whole_url(cat, entry, source, href=yadisk.public_href):
    """The entry's whole zip's download link from that source (the disk's: asked of it - a network call)."""
    name = entry.get('full', entry)['zip']
    return cat['files'] + name if source == FILES else href(cat['disk'], name)


def disk_old(entry):
    """The disk's copy older than the entry's build (both YYYY-MM-DD)."""
    copy, date = entry.get('disk', {}).get('date'), entry.get('date')
    return bool(copy and date and copy < date)


def source_cell(cat, entry, source):
    """A row's button: '—' none there; the disk's with its copy's date (dd.mm), an older one marked."""
    if not source_ok(cat, entry, source):
        return '—'
    if source == FILES:
        return '⬇ скачать'
    day = entry['disk'].get('date', '')
    text = '⬇ ' + ('%s.%s' % (day[8:10], day[5:7]) if len(day) == 10 else 'скачать')
    return text + ' ' + OLD_MARK if disk_old(entry) else text


def row_title(r):
    return (FAR_MARK if r.get('far') else '') + r.get('title', r['name'])


def ready_job(cat, name, source, have_tiles, have_trunk):
    """[(folder, entry)] a row's button gets from that source: the region whole, and the overview's pack when the
    drive needs it (none there, or an older trunk graph) - or that row itself the overview's."""
    jobs = [(r['name'], r) for r in cat['regions'] if r['name'] == name]
    ov = cat.get('overview')
    if ov and (name == OVERVIEW or pack_needed(ov, have_tiles, have_trunk)):
        jobs.append((OVERVIEW, ov))
    return [(n, e) for n, e in jobs if source_ok(cat, e, source)]


def pack_needed(ov, have_tiles, have_trunk):
    """The overview's pack wanted on the drive: none there, or its trunk graph older than the pack's."""
    return have_tiles is None or bool(ov.get('date')) and ov['date'] != have_trunk


def unpack_whole(zpath, dst):
    """A whole zip into its folder: its old region.txt off first, the new one last (half done never looks whole);
    only the zip's plain names (no paths of its own)."""
    os.makedirs(dst, exist_ok=True)
    unmark(dst)
    with zipfile.ZipFile(zpath) as z:
        names = [n for n in z.namelist() if plain_name(n)]
        for n in sorted(names, key=lambda n: n == 'region.txt'):
            with z.open(n) as src, open(os.path.join(dst, n), 'wb') as out:
                shutil.copyfileobj(src, out, 1 << 20)


def inform(title, text):   # a message in its window (tests: replaced)
    import tkinter.messagebox
    tkinter.messagebox.showinfo(title, text)


def ask(title, text):   # a yes / no question (tests: replaced)
    import tkinter.messagebox
    return tkinter.messagebox.askyesno(title, text)


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
    """The regions whose Russian or latin name has the text, by the Russian name - those outside the former USSR
    ("far") last (the user 2026-11-12)."""
    t = text.strip().casefold()
    hit = [r for r in cat['regions'] if t in r.get('title', r['name']).casefold() or t in r['name'].casefold()]
    return sorted(hit, key=lambda r: (bool(r.get('far')), r.get('title', r['name'])))


def trunk_state(cat, drive):
    """The trunk roads on the drive against the catalog's build: 'new' none, 'update' another, 'ok'."""
    there, want = trunk_date(drive), cat.get('trunk', {}).get('date')
    return 'new' if there is None else 'update' if want and there != want else 'ok'


def outdated(cat, have, drive=None):
    """The regions with a newer build in the catalog (and the trunk roads, on the drive)."""
    old = {n for n, s in states(cat, have).items() if s == 'update'}
    return old | {OVERVIEW} if drive and trunk_state(cat, drive) == 'update' else old


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


def region_look(chosen, state):
    """A region's fill and edge on the map (the user 2026-10-30: an update's orange edge hid under its neighbours'):
    chosen orange, fresh on the drive green - edged white; with an update white, edged black; else the panel's."""
    if chosen or state == 'ok':
        return (ACCENT if chosen else FRESH), WHITE
    return (WHITE, BLACK) if state == 'update' else (ROW, EDGE)


def box_area(rings):
    """The rings' bounding box's area, degrees squared (0: no rings)."""
    pts = [p for r in rings for p in r]
    if not pts:
        return 0
    x0, y0, x1, y1 = bounds(pts)
    return (x1 - x0) * (y1 - y0)


# ---- the window (tkinter): the work in a thread, its news through a queue

# OSMapsCE's colours (VestaHome's tokens): the background, the panel, the rows, the text, the dim text, the accent
INK, PANEL, ROW, TEXT, DIM, ACCENT = '#0E1013', '#1A1D22', '#23272E', '#F5F3EE', '#AAB0B8', '#FF6B1A'
FRESH, EDGE = '#2E5D45', '#4A505A'   # the map: a region on the drive and fresh; the regions' borders
WHITE, BLACK = '#FFFFFF', '#000000'   # ... one with an update on the drive (the user 2026-10-30), its edge
GO, BUSY, STOPPING = 'Собрать выбранные карты', 'Отменить запись', 'Отмена…'   # the button (the user 2026-11-12:
                                                    # renamed); while it writes it cancels (the user 2026-10-30)
MAP_W, MAP_H = 560, 520


class App:
    def __init__(self, tk, ttk):
        self.tk, self.ttk = tk, ttk
        self.news = queue.Queue()
        self.stop = threading.Event()   # "Отменить запись": the job's next progress raises Cancelled
        self.cat = {'regions': []}
        self.shapes = None
        self.chosen = set()
        self.press = None
        self.root = tk.Tk()
        self.root.title('Карты OSMapsCE ' + app_version())
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
        self.tree = ttk.Treeview(middle, columns=('size', 'have', 'state', FILES, DISK), height=16, selectmode='none')
        for col, text, w in (('#0', 'Область', 280), ('size', 'Граф и поиск', 100), ('have', 'На флешке', 100),
                             ('state', 'Состояние', 100), (FILES, 'С сервера', 100), (DISK, 'С Я.Диска', 140)):
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
        ttk.Button(bottom, text='Удалить с флешки', command=self.remove_chosen).pack(side='right')
        ttk.Button(bottom, text='Готовые архивы', command=self.open_ready).pack(side='right', padx=6)

    def work(self, job, *args):   # (in the thread: an error shown, not lost; then the button back)
        try:
            job(*args)
        except Cancelled:
            self.news.put(('status', 'Остановлено. Скачанное осталось на ПК — следующая запись продолжит с него.'))
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
        self.special_rows(self.find.get().strip().casefold())
        for r in matching(self.cat, self.find.get()):
            tags = ('chosen',) if r['name'] in self.chosen else ('update',) if st[r['name']] == 'update' else ()
            self.tree.insert('', 'end', iid=r['name'], text=row_title(r), tags=tags,
                             values=('%.1f МБ' % (r['size'] / 1e6), have.get(r['name'], '—'), STATE[st[r['name']]],
                                     source_cell(self.cat, r, FILES), source_cell(self.cat, r, DISK)))
        self.paint(st)
        self.status.set('Областей: %d, выбрано %d (%.0f МБ графа и поиска; плитки ещё в 5–15 раз больше). '
                        'Свободно: %s' % (len(self.cat['regions']), len(self.chosen - {TRUNK, OVERVIEW}),
                                          chosen_size(self.cat, self.chosen) / 1e6, self.free()))

    def special_rows(self, find):
        """The overview and the trunk roads, one row first (the user 2026-11-06), if the search lets it."""
        drive = self.drive.get()
        state = trunk_state(self.cat, drive)
        state = 'new' if not overview_date(drive) else state
        for name, title, size, date in ((OVERVIEW, OVERVIEW_TITLE, '≈70 МБ', overview_date(drive)),):
            if find in title.casefold():
                tags = ('chosen',) if name in self.chosen else ('update',) if state == 'update' else ()
                ov = self.cat.get('overview', {})
                self.tree.insert('', 'end', iid=name, text=title, tags=tags,
                                 values=(size, date or '—', {'new': 'нет', 'update': 'есть новее', 'ok': 'есть'}[state],
                                         source_cell(self.cat, ov, FILES), source_cell(self.cat, ov, DISK)))

    def click(self, event):   # a click picks a region or lets it go (no Ctrl); on its button: that source's zip
        name = self.tree.identify_row(event.y)
        source = {'#4': FILES, '#5': DISK}.get(self.tree.identify_column(event.x))
        if name and source:
            self.get_ready(name, source)
        elif name:
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
            fill, edge = region_look(name in self.chosen, st.get(name))
            self.map.itemconfigure('r:' + name, fill=fill, outline=edge)

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
        titles = {r['name']: row_title(r) for r in self.cat['regions']}
        self.map.itemconfigure(self.hover, text=titles.get(name, name or ''))

    # ----

    def pick_all(self):
        self.chosen = {r['name'] for r in matching(self.cat, self.find.get())}
        self.refresh()

    def pick_none(self):
        self.chosen = set()
        self.refresh()

    def pick_old(self):
        self.chosen = outdated(self.cat, self.have(), self.drive.get())
        self.refresh()

    def free(self):
        return '%.1f ГБ' % (shutil.disk_usage(self.drive.get()).free / 1e9) if os.path.isdir(self.drive.get()) else '—'

    def choose(self):
        self.drive.set(self.tk.filedialog.askdirectory() or self.drive.get())
        self.refresh()

    def job(self, drive):
        """What to write: the chosen regions, the trunk roads and the overview (chosen, or none there: with any region)."""
        entries = [r for r in self.cat['regions'] if r['name'] in self.chosen]
        trunk = OVERVIEW in self.chosen or bool(entries) and trunk_state(self.cat, drive) != 'ok'   # (none, older)
        overview = OVERVIEW in self.chosen or bool(entries) and overview_date(drive) is None
        return entries, trunk, overview

    def write_chosen(self):
        drive = self.drive.get()
        entries, trunk, overview = self.job(drive)
        if self.go['text'] == BUSY:   # writing: this press stops it (once at a time)
            self.stop.set()
            self.go.configure(text=STOPPING)
            self.go.state(['disabled'])
        elif entries or trunk or overview:
            self.go.configure(text=BUSY, style='Busy.TButton')
            threading.Thread(target=self.work, args=(self.write_all, entries, drive, trunk, overview),
                             daemon=True).start()

    def on_done(self, _):   # the writing over (or failed, or stopped): the button back
        self.stop.clear()
        self.go.configure(text=GO, style='Go.TButton')
        self.go.state(['!disabled'])

    def chosen_there(self, drive):
        """The chosen ones on the drive, and their titles."""
        have = self.have()
        there = sorted(n for n in self.chosen if n in have or n == OVERVIEW and (overview_date(drive) or trunk_date(drive)))
        titles = {r['name']: r.get('title', r['name']) for r in self.cat['regions']}
        titles[OVERVIEW] = OVERVIEW_TITLE
        return there, [titles.get(n, n) for n in there]

    def open_ready(self):
        """The ready archives in the browser: the chosen ones' downloads, else the disk's folder."""
        try:
            links = ready_links(self.cat, self.chosen)
        except Exception as e:   # noqa: BLE001 - the user sees it
            inform('Готовые архивы', 'Ссылки не получены: %s' % e)
            return
        if not links:   # (the status line alone went unseen - the user 2026-10-30)
            inform('Готовые архивы', 'Готовых архивов пока нет или не отмечены области: архивы появятся '
                                     'после сборки карт. Пока — запись выбранных областей этой программой.')
            return
        for url in links:
            webbrowser.open(url)
        self.status.set('Открыто в браузере: %d (готовые области — граф, поиск и плитки; распаковать в OSMapsData '
                        'на флешке)' % len(links))

    def remove_chosen(self):
        """The chosen regions that are on the drive off it, asked first; not while writing."""
        drive = self.drive.get()
        there, titles = self.chosen_there(drive)
        if not there or self.go['text'] != GO:
            self.status.set('Нечего удалять: выбранных областей на флешке нет' if not there else 'Идёт запись')
            return
        if ask('Удалить с флешки', 'Удалить с %s:\n%s?' % (drive, ', '.join(titles))):
            gone = remove_regions(drive, there)
            self.chosen -= set(gone)
            self.refresh()
            self.status.set('Удалено: %d. ' % len(gone) + self.status.get())

    def get_ready(self, name, source):
        """A row's button (the user 2026-11-12): the region's whole zip from that source onto the drive (the overview's
        too when needed); the bottom button cancels it as a writing; not while writing."""
        drive = self.drive.get()
        if self.go['text'] != GO:
            self.status.set('Идёт запись')
            return
        jobs = ready_job(self.cat, name, source, overview_date(drive), trunk_date(drive))
        if name not in [n for n, _ in jobs]:
            self.status.set('Готового архива %s нет' % ('на сервере' if source == FILES else 'на Я.Диске'))
            return
        self.go.configure(text=BUSY, style='Busy.TButton')
        threading.Thread(target=self.work, args=(self.fetch_ready, jobs, source, drive), daemon=True).start()

    def fetch_ready(self, jobs, source, drive):
        """The whole zips into the PC's temp folder, unpacked there, then onto the card at once (copy_to); the bar for
        the whole job - each zip a part of it, the copy the last."""
        stage = os.path.join(stage_dir(), 'ready')
        shutil.rmtree(stage, ignore_errors=True)   # (a job stopped before: no going on with a half zip)
        os.makedirs(stage)
        total = len(jobs) + 1
        titles = {r['name']: r.get('title', r['name']) for r in self.cat['regions']}
        titles[OVERVIEW] = OVERVIEW_TITLE
        for k, (name, entry) in enumerate(jobs):
            tell = lambda part, text, h=titles.get(name, name), k=k: self.tell(h, (k + part) / total, text)   # noqa: E731
            tell(0, 'ссылка…')
            zpath = os.path.join(stage, name + '.zip')
            download_any(secure_first(whole_url(self.cat, entry, source)), zpath, tell)
            unpack_whole(zpath, os.path.join(stage, 'OSMapsData', name))
            os.remove(zpath)
        copy_to(stage, drive, lambda part, text: self.tell('На флешку', (total - 1 + part) / total, text))
        if OVERVIEW in [n for n, _ in jobs]:
            drop_old_trunk(drive)
        shutil.rmtree(stage, ignore_errors=True)
        self.news.put(('status', 'Готово %s: %s. Флешку — в ГУ (папка OSMapsData в корне).' % (
            'с сервера' if source == FILES else 'с Я.Диска', ', '.join(titles.get(n, n) for n, _ in jobs))))
        self.news.put(('catalog', self.cat))

    def write_overview(self, stage, get_bytes, day, k, total, drive=None):   # the overview into stage: the bar's part k
        if overview_date(stage) == day:   # (downloaded before a break)
            return
        install_overview(self.cat, stage, get_bytes, day,
                         lambda part, text: self.tell(OVERVIEW_TITLE, (k + part) / total, text),
                         overview_rings(getattr(self, 'shapes', None)), drive)

    def tell(self, head, part, text):   # (from the writing thread: through the queue); stopped - Cancelled
        if self.stop.is_set():
            raise Cancelled()
        self.news.put(('progress', part))
        self.news.put(('status', '%s: %s' % (head, text)))

    def write_all(self, entries, drive, trunk, overview=False):
        """The regions, the overview, the trunk roads into the PC's temp folder, then all onto the card (copy_to); the
        bar for the whole job (the user 2026-10-29: full before the end) - each a part of it, the copy the last."""
        total = len(entries) + (1 if trunk else 0) + (1 if overview else 0) + 1
        stage = stage_dir()
        done = self.write_stage(entries, stage, drive, trunk, total, overview)
        copy_to(stage, drive, lambda part, text: self.tell('На флешку', (total - 1 + part) / total, text))
        if trunk_date(drive):   # the pack holds the graph now
            drop_old_trunk(drive)
        shutil.rmtree(stage, ignore_errors=True)   # (an error before: kept - the next time goes on there)
        self.news.put(('status', done + '. Флешку — в ГУ (папка OSMapsData в корне).'))
        self.news.put(('catalog', self.cat))

    def write_stage(self, entries, stage, drive, trunk, total, overview=False):   # the regions, the overview, the
        if entries or overview:                                                  # trunk roads into stage
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
        if overview:
            self.write_overview(stage, get_bytes, day, len(entries), total, drive)
            done += ', обзорная карта записана'
        if trunk:
            got = install_trunk(stage, download,
                                lambda part, text: self.tell(OVERVIEW_TITLE, (total - 2 + part) / total, text), drive)
            done += {'written': ', магистрали записаны', 'same': ', магистрали уже свежие',
                     'none': ', магистралей в релизе нет'}[got]
        return done


if __name__ == '__main__':
    import tkinter
    import tkinter.filedialog   # noqa: F401 (App.choose)
    import tkinter.ttk
    App(tkinter, tkinter.ttk).root.mainloop()
