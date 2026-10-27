"""The search index of a region for OSMapsCE's offline search (offsearch.cpp on the unit).

    python make_search.py <region>.osm.pbf <out>/search.idx

Places (towns, villages, suburbs), streets (by name and settlement: the nearest place whose reach holds the street),
their houses (addr:street + addr:housenumber, nodes and buildings), named places to go (fuel, shops, food, health...).
The names are matched by their normalized form (see norm(): offsearch.cpp's Norm() must do the same). The file
(little endian, offsets in bytes from the start):

    header  'OSMS' u32 version, places, streets, houses, pois, strBytes, gridW, gridH, i32 cellE6, minLat, minLon,
            u32 gridItems, u32 off[6]: strings, places, streets, houses, pois, grid (cellStart[W*H+1] then items)
    strings UTF-8, 0-terminated; offset 0 = ""
    places  i32 lat, lon; u32 name, norm; u32 rank (0 city .. 3 hamlet, 4 suburb / quarter)          - 20 bytes
    streets i32 lat, lon; u32 name, norm, city; u32 firstHouse, houseCount                            - 28 bytes
    houses  i32 lat, lon; u32 street, number                                                          - 16 bytes
    pois    i32 lat, lon; u32 name, norm, city, what (its kind, Russian), u32 street (~0: none), number - 32 bytes
    grid    the houses near each cell (reverse lookups)
"""
import math
import re
import struct
import sys
import osmium

VERSION = 1
HEADER = '<4s8I3iI6I'
kCell = 5000   # the houses' grid cell, 1e-6 degrees
kPlaceRank = {'city': 0, 'town': 1, 'village': 2, 'hamlet': 3, 'isolated_dwelling': 3, 'suburb': 4, 'quarter': 4,
              'neighbourhood': 4, 'locality': 3}
kReachM = {0: 25000, 1: 8000, 2: 3000, 3: 1500}   # how far a settlement's streets go
kPoi = {('amenity', 'fuel'): 'АЗС', ('amenity', 'charging_station'): 'Зарядка', ('amenity', 'hospital'): 'Больница',
        ('amenity', 'clinic'): 'Поликлиника', ('amenity', 'pharmacy'): 'Аптека', ('amenity', 'cafe'): 'Кафе',
        ('amenity', 'restaurant'): 'Ресторан', ('amenity', 'fast_food'): 'Еда', ('amenity', 'bank'): 'Банк',
        ('amenity', 'police'): 'Полиция', ('amenity', 'post_office'): 'Почта', ('amenity', 'school'): 'Школа',
        ('amenity', 'kindergarten'): 'Детский сад', ('amenity', 'university'): 'Университет',
        ('amenity', 'theatre'): 'Театр', ('amenity', 'cinema'): 'Кино', ('amenity', 'parking'): 'Парковка',
        ('amenity', 'car_wash'): 'Мойка', ('shop', 'car_repair'): 'Автосервис', ('amenity', 'car_repair'): 'Автосервис',
        ('shop', 'car_parts'): 'Автозапчасти', ('shop', 'tyres'): 'Шины', ('shop', 'supermarket'): 'Супермаркет',
        ('shop', 'mall'): 'Торговый центр', ('tourism', 'hotel'): 'Гостиница', ('tourism', 'museum'): 'Музей',
        ('railway', 'station'): 'Вокзал', ('aeroway', 'aerodrome'): 'Аэропорт'}


def norm(s):
    """The searched form: lower case, ё -> е, everything but letters and digits -> one space."""
    s = s.lower().replace('ё', 'е')
    return ' '.join(re.findall(r'[0-9a-zа-я]+', s))


class Strings:
    def __init__(self):
        self.data = bytearray(b'\x00')
        self.at = {'': 0}

    def add(self, s):
        o = self.at.get(s)
        if o is None:
            o = self.at[s] = len(self.data)
            self.data += s.encode('utf-8') + b'\x00'
        return o


def center(w):
    pts = [(n.location.lat, n.location.lon) for n in w.nodes if n.location.valid()]
    if not pts:
        return None
    return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)), pts


class Reader(osmium.SimpleHandler):
    def __init__(self):
        super().__init__()
        self.places = []    # (lat, lon, name, rank)
        self.streets = {}   # name -> [(lat, lon, length)] of its ways' middles
        self.addrs = []     # (lat, lon, street, number, city)
        self.pois = []      # (lat, lon, name, what, street, number)

    def place(self, tags, lat, lon):
        p = tags.get('place')
        nm = tags.get('name')
        if p in kPlaceRank and nm:
            self.places.append((lat, lon, nm, kPlaceRank[p]))

    def common(self, tags, lat, lon):
        hn = tags.get('addr:housenumber')
        st = tags.get('addr:street') or tags.get('addr:place')
        if hn and st:
            self.addrs.append((lat, lon, st, hn, tags.get('addr:city', '')))
        what = poi_kind(tags)
        if what:
            self.pois.append((lat, lon, tags['name'], what, st or '', hn or ''))

    def node(self, n):
        if not n.tags:
            return
        lat, lon = n.location.lat, n.location.lon
        self.place(n.tags, lat, lon)
        self.common(n.tags, lat, lon)

    def street(self, w, name):
        c = center(w)
        if c:
            pts = c[1]
            length = sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]) for i in range(len(pts) - 1))
            mid = pts[len(pts) // 2]
            self.streets.setdefault(name, []).append((mid[0], mid[1], length))

    def outline(self, w):
        """A building or an area (not a street): its address, place to go or settlement at its nodes' centre."""
        c = center(w)
        if c:
            (la, lo), _ = c
            self.place(w.tags, la, lo)
            self.common(w.tags, la, lo)

    def way(self, w):
        t = w.tags
        if is_street(t):
            self.street(w, t['name'])
        elif 'addr:housenumber' in t or poi_kind(t) or 'place' in t:
            self.outline(w)


def is_street(tags):
    """A named road (not a square drawn as an area)."""
    return tags.get('highway') and tags.get('name') and tags.get('area') != 'yes'


def poi_kind(tags):
    """A named place to go: its kind (Russian), else None."""
    if not tags.get('name'):
        return None
    return next((what for (k, v), what in kPoi.items() if tags.get(k) == v), None)


def dist_m(a, b, c, d):
    return math.hypot((c - a) * 111195, (d - b) * 111195 * math.cos(math.radians(a)))


class Settlements:
    """The settlement a point belongs to: of those whose reach holds it, the one holding it best (0.1 degree grid)."""
    def __init__(self, places):
        self.settle = [p for p in places if p[3] <= 3]
        self.grid = {}
        for i, p in enumerate(self.settle):
            self.grid.setdefault((int(p[0] * 10), int(p[1] * 10)), []).append(i)

    def near(self, lat, lon):
        cx, cy = int(lat * 10), int(lon * 10)
        for dx in range(-3, 4):
            for dy in range(-5, 6):
                for i in self.grid.get((cx + dx, cy + dy), ()):
                    yield self.settle[i]

    def city_of(self, lat, lon):
        best, bd = '', 1e18
        for p in self.near(lat, lon):
            share = dist_m(lat, lon, p[0], p[1]) / kReachM[p[3]]
            if share <= 1 and share < bd:
                bd, best = share, p[2]
        return best


def street_table(parts_by_name, city_of):
    """(name, settlement) -> (lat, lon, length) of its longest part's middle."""
    streets = {}
    for nm, parts in parts_by_name.items():
        for la, lo, length in parts:
            key = (nm, city_of(la, lo))
            cur = streets.get(key)
            if cur is None or length > cur[2]:
                streets[key] = (la, lo, length)
    return streets


def attach_houses(addrs, streets, city_of):
    """The houses onto their street (by name and settlement; else the nearest of that name; else a street made)."""
    houses, by_name = {}, {}
    for k in streets:
        by_name.setdefault(k[0], []).append(k)
    for la, lo, st, hn, city in addrs:
        key = (st, city or city_of(la, lo))
        if key not in streets:
            cands = by_name.get(st)
            if cands:
                key = min(cands, key=lambda k: dist_m(la, lo, streets[k][0], streets[k][1]))
            else:
                streets[key] = (la, lo, 0)
                by_name[st] = [key]
        houses.setdefault(key, []).append((la, lo, hn))
    return houses


def street_rows(streets, houses, S):
    """The streets sorted by their searched name, each with its houses' run (numbers in order)."""
    skeys = sorted(streets, key=lambda k: (norm(k[0]), k[1]))
    sidx = {k: i for i, k in enumerate(skeys)}
    srows, hrows = [], []
    for k in skeys:
        la, lo, _ = streets[k]
        hs = sorted(houses.get(k, []), key=lambda h: (len(re.match(r'\d*', h[2]).group()), h[2]))
        srows.append((int(la * 1e6), int(lo * 1e6), S.add(k[0]), S.add(norm(k[0])), S.add(k[1]), len(hrows), len(hs)))
        hrows += [(int(hla * 1e6), int(hlo * 1e6), sidx[k], S.add(hn)) for hla, hlo, hn in hs]
    return srows, hrows, sidx


def poi_rows(pois, sidx, city_of, S):
    rows = []
    for la, lo, nm, what, st, hn in pois:
        c = city_of(la, lo)
        s = sidx.get((st, c), 0xFFFFFFFF) if st else 0xFFFFFFFF
        rows.append((int(la * 1e6), int(lo * 1e6), S.add(nm), S.add(norm(nm)), S.add(c), S.add(what), s, S.add(hn)))
    return rows


def house_grid(hrows):
    """(minLat, minLon, gw, gh, cellStart, items) of the houses (0.005 degree cells)."""
    lats = [h[0] for h in hrows] or [0]
    lons = [h[1] for h in hrows] or [0]
    min_lat, min_lon = min(lats), min(lons)
    gw, gh = (max(lons) - min_lon) // kCell + 1, (max(lats) - min_lat) // kCell + 1
    cells = {}
    for i, h in enumerate(hrows):
        cells.setdefault(((h[0] - min_lat) // kCell) * gw + (h[1] - min_lon) // kCell, []).append(i)
    cstart, items = [], []
    for c in range(gw * gh):
        cstart.append(len(items))
        items += cells.get(c, [])
    cstart.append(len(items))
    return min_lat, min_lon, gw, gh, cstart, items


def write(out, S, prows, srows, hrows, orows, grid):
    min_lat, min_lon, gw, gh, cstart, items = grid
    sd = bytes(S.data) + b'\x00' * ((4 - len(S.data) % 4) % 4)
    off = [struct.calcsize(HEADER)]
    for size in (len(sd), len(prows) * 20, len(srows) * 28, len(hrows) * 16, len(orows) * 32):
        off.append(off[-1] + size)
    with open(out, 'wb') as f:
        f.write(struct.pack(HEADER, b'OSMS', VERSION, len(prows), len(srows), len(hrows), len(orows), len(S.data),
                            gw, gh, kCell, min_lat, min_lon, len(items), *off))
        f.write(sd)
        for fmt, rows in (('<iiIII', prows), ('<iiIIIII', srows), ('<iiII', hrows), ('<iiIIIIII', orows)):
            f.write(b''.join(struct.pack(fmt, *r) for r in rows))
        f.write(struct.pack('<%dI' % len(cstart), *cstart))
        f.write(struct.pack('<%dI' % len(items), *items))
    return off[-1] + (len(cstart) + len(items)) * 4


def build(src, out):
    r = Reader()
    r.apply_file(src, locations=True)
    city_of = Settlements(r.places).city_of
    streets = street_table(r.streets, city_of)
    houses = attach_houses(r.addrs, streets, city_of)
    S = Strings()
    srows, hrows, sidx = street_rows(streets, houses, S)
    prows = [(int(p[0] * 1e6), int(p[1] * 1e6), S.add(p[2]), S.add(norm(p[2])), p[3]) for p in r.places]
    orows = poi_rows(r.pois, sidx, city_of, S)
    size = write(out, S, prows, srows, hrows, orows, house_grid(hrows))
    print('places %d, streets %d, houses %d, pois %d, strings %.1f MB; file %.1f MB' % (
        len(prows), len(srows), len(hrows), len(orows), len(S.data) / 1e6, size / 1e6))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2])
