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
        self.streets = {}   # name -> [(lat, lon, length, wayid)]
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
        nm = tags.get('name')
        if nm:
            for (k, v), what in kPoi.items():
                if tags.get(k) == v:
                    self.pois.append((lat, lon, nm, what, st or '', hn or ''))
                    break

    def node(self, n):
        if not n.tags:
            return
        lat, lon = n.location.lat, n.location.lon
        self.place(n.tags, lat, lon)
        self.common(n.tags, lat, lon)

    def way(self, w):
        t = w.tags
        nm = t.get('name')
        if t.get('highway') and nm and t.get('area') != 'yes':
            c = center(w)
            if c:
                (la, lo), pts = c
                length = sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]) for i in range(len(pts) - 1))
                mid = pts[len(pts) // 2]
                self.streets.setdefault(nm, []).append((mid[0], mid[1], length))
            return
        if 'addr:housenumber' in t or (nm and any(t.get(k) == v for k, v in kPoi)) or 'place' in t:
            c = center(w)
            if c:
                (la, lo), _ = c
                self.place(t, la, lo)
                self.common(t, la, lo)


def dist_m(a, b, c, d):
    return math.hypot((c - a) * 111195, (d - b) * 111195 * math.cos(math.radians(a)))


def main():
    src, out = sys.argv[1], sys.argv[2]
    r = Reader()
    r.apply_file(src, locations=True)
    print('places', len(r.places), 'street names', len(r.streets), 'addresses', len(r.addrs), 'pois', len(r.pois))
    settle = [p for p in r.places if p[3] <= 3]
    # a grid of the settlements for the nearest one (0.1 degree cells)
    sgrid = {}
    for i, p in enumerate(settle):
        sgrid.setdefault((int(p[0] * 10), int(p[1] * 10)), []).append(i)

    def city_of(lat, lon):
        best, bd = '', 1e18
        cx, cy = int(lat * 10), int(lon * 10)
        for dx in range(-3, 4):
            for dy in range(-5, 6):
                for i in sgrid.get((cx + dx, cy + dy), ()):
                    p = settle[i]
                    d = dist_m(lat, lon, p[0], p[1])
                    reach = kReachM[p[3]]
                    if d <= reach and d / reach < bd:   # the one whose reach holds it best
                        bd, best = d / reach, p[2]
        return best

    S = Strings()
    # streets: one per (name, settlement): its parts' longest one's middle
    streets = {}
    for nm, parts in r.streets.items():
        for la, lo, length in parts:
            key = (nm, city_of(la, lo))
            cur = streets.get(key)
            if cur is None or length > cur[2]:
                streets[key] = (la, lo, length)
    # the houses: onto their street (by name and settlement; a street of an address only - made)
    houses = {}
    by_name = {}
    for k in streets:
        by_name.setdefault(k[0], []).append(k)
    for la, lo, st, hn, city in r.addrs:
        c = city or city_of(la, lo)
        key = (st, c)
        if key not in streets:
            # the same name in the nearest settlement's spelling may differ (addr:city): the street of that name nearest
            cands = by_name.get(st)
            if cands:
                key = min(cands, key=lambda k: dist_m(la, lo, streets[k][0], streets[k][1]))
            else:
                streets[key] = (la, lo, 0)
                by_name[st] = [key]
        houses.setdefault(key, []).append((la, lo, hn))
    skeys = sorted(streets, key=lambda k: (norm(k[0]), k[1]))
    sidx = {k: i for i, k in enumerate(skeys)}
    street_rows, house_rows = [], []
    for k in skeys:
        la, lo, _ = streets[k]
        hs = sorted(houses.get(k, []), key=lambda h: (len(re.match(r'\d*', h[2]).group()), h[2]))
        street_rows.append((int(la * 1e6), int(lo * 1e6), S.add(k[0]), S.add(norm(k[0])), S.add(k[1]),
                            len(house_rows), len(hs)))
        for hla, hlo, hn in hs:
            house_rows.append((int(hla * 1e6), int(hlo * 1e6), sidx[k], S.add(hn)))
    place_rows = [(int(p[0] * 1e6), int(p[1] * 1e6), S.add(p[2]), S.add(norm(p[2])), p[3]) for p in r.places]
    poi_rows = []
    for la, lo, nm, what, st, hn in r.pois:
        c = city_of(la, lo)
        s = sidx.get((st, c), 0xFFFFFFFF) if st else 0xFFFFFFFF
        poi_rows.append((int(la * 1e6), int(lo * 1e6), S.add(nm), S.add(norm(nm)), S.add(c), S.add(what), s,
                         S.add(hn)))
    # the houses' grid (0.005 degrees)
    cell = 5000
    lats = [h[0] for h in house_rows] or [0]
    lons = [h[1] for h in house_rows] or [0]
    minLat, minLon = min(lats), min(lons)
    gw = (max(lons) - minLon) // cell + 1
    gh = (max(lats) - minLat) // cell + 1
    cells = {}
    for i, h in enumerate(house_rows):
        cells.setdefault(((h[0] - minLat) // cell) * gw + (h[1] - minLon) // cell, []).append(i)
    cstart, items = [], []
    for c in range(gw * gh):
        cstart.append(len(items))
        items += cells.get(c, [])
    cstart.append(len(items))

    hdr = '<4s8I3iI6I'
    hl = struct.calcsize(hdr)
    sd = bytes(S.data) + b'\x00' * ((4 - len(S.data) % 4) % 4)
    o_str = hl
    o_pl = o_str + len(sd)
    o_st = o_pl + len(place_rows) * 20
    o_ho = o_st + len(street_rows) * 28
    o_po = o_ho + len(house_rows) * 16
    o_gr = o_po + len(poi_rows) * 32
    with open(out, 'wb') as f:
        f.write(struct.pack(hdr, b'OSMS', VERSION, len(place_rows), len(street_rows), len(house_rows), len(poi_rows),
                            len(S.data), gw, gh, cell, minLat, minLon, len(items), o_str, o_pl, o_st, o_ho, o_po, o_gr))
        f.write(sd)
        f.write(b''.join(struct.pack('<iiIII', *p) for p in place_rows))
        f.write(b''.join(struct.pack('<iiIIIII', *s) for s in street_rows))
        f.write(b''.join(struct.pack('<iiII', *h) for h in house_rows))
        f.write(b''.join(struct.pack('<iiIIIIII', *p) for p in poi_rows))
        f.write(struct.pack('<%dI' % len(cstart), *cstart))
        f.write(struct.pack('<%dI' % len(items), *items))
    size = o_gr + (len(cstart) + len(items)) * 4
    print('places %d, streets %d, houses %d, pois %d, strings %.1f MB; file %.1f MB' % (
        len(place_rows), len(street_rows), len(house_rows), len(poi_rows), len(S.data) / 1e6, size / 1e6))


if __name__ == '__main__':
    main()
