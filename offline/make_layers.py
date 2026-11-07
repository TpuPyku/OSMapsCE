"""make_layers.py - a region's map layers from its OSM extract (the user 2026-11-06): traffic lights, speed bumps and car
parks, with whether they are paid - OSMapsCE's layers.lay.

    python make_layers.py <region>.osm.pbf <out>/layers.lay

The file (little endian): the header, the cells sorted by key (key, its first point), then 12 bytes a point. A cell is
0.01 degree: the unit reads only the cells on the screen.
"""
import struct
import sys

import osmium

VERSION = 1
HEADER = '<4sIII'     # 'OSML', version, cells, points
kCell = 10000         # 1e-6 degrees
SIGNAL, BUMP, PARKING = 1, 2, 3
FEE_UNKNOWN, FEE_FREE, FEE_PAID = 0, 1, 2
kBumps = ('bump', 'hump', 'table', 'cushion')   # (what a car feels: a choker or an island only narrows the road)


def kind_of(tags):
    if tags.get('highway') == 'traffic_signals':
        return SIGNAL
    if tags.get('traffic_calming') in kBumps:
        return BUMP
    if tags.get('amenity') == 'parking' and tags.get('access') not in ('private', 'no'):
        return PARKING
    return None


def fee_of(tags):
    """Unknown stays unknown: most car parks do not say, and calling them free could cost a fine."""
    if tags.get('fee:conditional'):
        return FEE_PAID
    fee = tags.get('fee') or ''
    if fee == 'no':
        return FEE_FREE
    return FEE_PAID if fee == 'yes' or any(c.isdigit() for c in fee) else FEE_UNKNOWN


def places_of(tags):
    capacity = tags.get('capacity') or ''
    return min(int(capacity), 65535) if capacity.isdigit() else 0


def cell_of(lat, lon):
    return (lat + 90000000) // kCell << 16 | (lon + 180000000) // kCell


class Reader(osmium.SimpleHandler):
    def __init__(self):
        super().__init__()
        self.points = []   # (lat, lon, kind, fee, places), 1e-6 degrees

    def add(self, tags, lat, lon):
        kind = kind_of(tags)
        if kind:
            paid = kind == PARKING
            self.points.append((round(lat * 1e6), round(lon * 1e6), kind, fee_of(tags) if paid else 0,
                                places_of(tags) if paid else 0))

    def node(self, n):
        if n.tags:
            self.add(n.tags, n.location.lat, n.location.lon)

    def way(self, w):
        if not kind_of(w.tags):
            return
        pts = {n.ref: (n.location.lat, n.location.lon) for n in w.nodes if n.location.valid()}   # (a closed one's first once)
        if pts:
            self.add(w.tags, sum(p[0] for p in pts.values()) / len(pts), sum(p[1] for p in pts.values()) / len(pts))


def write_cells(out, magic, version, items, key_of, pack):
    """The header, the sorted cells, the items (make_parklines.py's too)."""
    items = sorted(items, key=lambda p: (key_of(p), p))
    cells = []
    for i, p in enumerate(items):
        key = key_of(p)
        if not cells or cells[-1][0] != key:
            cells.append((key, i))
    with open(out, 'wb') as f:
        f.write(struct.pack(HEADER, magic, version, len(cells), len(items)))
        for c in cells:
            f.write(struct.pack('<II', *c))
        for p in items:
            f.write(pack(p))


def write(points, out):
    write_cells(out, b'OSML', VERSION, points, lambda p: cell_of(p[0], p[1]), lambda p: struct.pack('<iiBBH', *p))


def build(src, out):
    r = Reader()
    r.apply_file(src, locations=True)
    write(r.points, out)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2])
