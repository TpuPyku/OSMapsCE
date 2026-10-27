"""The road graph of a region for OSMapsCE's offline routing (offline.cpp on the unit).

    python make_graph.py <region>.osm.pbf <out>/route.grf

Car roads only (highway classes below, access / motor_vehicle not "no" / "private"), split at the junctions; each
piece is an edge in both directions unless one-way. Times from the class's speed (or maxspeed, capped) plus the
traffic lights' wait. Turn restrictions: not yet. The file (little endian, all offsets in bytes from the file's start):

    header   'OSMG' u32 version, u32 nodes, u32 edges, u32 geomPts, u32 nameBytes,
             i32 minLat, minLon, maxLat, maxLon (1e-6 degrees), u32 gridW, gridH, i32 cellE6, u32 gridItems,
             u32 off[6]: nodes, first, edges, geom, names, grid (cellStart[W*H+1] then items)
    nodes    i32 lat, lon (1e-6 degrees)                                  - nodes x 8
    first    u32 the node's first edge; first[nodes] = edges             - (nodes + 1) x 4
    edges    u32 to, u32 lenDm, u32 timeDs, u32 geom, u16 ngeom, u8 cls, u8 flags, u32 name   - edges x 24
             geom: the first inner point (geomPts index), ngeom inner points; flags 1 = its geometry is read backwards,
             2 = a roundabout, 4 = a link (ramp); cls: kClasses' index
    geom     i32 lat, lon of the inner points                             - geomPts x 8
    names    "name\\tref\\0" UTF-8, an edge's name = its offset (0 = none: the table starts with "\\t\\0")
    grid     the edges (forward ones: each piece once) near each cell, cellE6 square: snapping a point to a road
"""
import math
import struct
import sys
from collections import namedtuple

import osmium

# class, km/h (a town's typical; maxspeed wins when lower or set)
# (town values a little under the limits: the crossings, the traffic; checked against OSRM's times 2026-10-27)
kClasses = [('motorway', 110), ('trunk', 90), ('primary', 60), ('secondary', 50), ('tertiary', 40),
             ('unclassified', 35), ('residential', 25), ('living_street', 10), ('service', 15),
             ('motorway_link', 60), ('trunk_link', 50), ('primary_link', 40), ('secondary_link', 40),
             ('tertiary_link', 30), ('road', 30), ('track', 15)]
kCls = {c: i for i, (c, _) in enumerate(kClasses)}
kSignalS = 8   # a traffic light's wait, seconds (on average: half the time it is red)
kNoAccess = {'no', 'private', 'agricultural', 'forestry', 'delivery', 'destination_no'}
kNoService = {'parking_aisle', 'drive-through', 'emergency_access'}
kCell = 5000   # the grid's cell, 1e-6 degrees: 0.005 degrees, ~550 x 330 m at 53 N
VERSION = 1
HEADER = '<4s5I4i2IiI6I'

# a car road as read: its node ids, class, one-way (1 forward, -1 backward, 0 both), roundabout, km/h, name, ref
Way = namedtuple('Way', 'ids cls oneway rb speed name ref')
# a junction-to-junction part of a way: node indices a -> b, inner points, metres, its way's data, lights on it
Piece = namedtuple('Piece', 'a b inner length cls oneway rb speed name signals')


def maxspeed_kmh(text):
    """OSM's maxspeed ("60", "40 mph") in km/h; None when not a number (or below 5)."""
    try:
        m = int(text.split()[0])
    except (ValueError, IndexError):
        return None
    m = int(m * 1.609) if 'mph' in text else m
    return m if m >= 5 else None


def speed_of(tags, cls):
    v = kClasses[cls][1]
    m = maxspeed_kmh(tags.get('maxspeed', ''))
    v = min(v, m) if m else v
    if tags.get('highway') == 'track' and tags.get('tracktype', 'grade1') not in ('grade1', 'grade2'):
        v = 8
    return max(v, 5)


def allowed(tags):
    """A road cars may take (not an area, no access ban, not a parking aisle and the like)."""
    if tags.get('area') == 'yes' or any(tags.get(k) in kNoAccess for k in ('access', 'motor_vehicle', 'motorcar', 'vehicle')):
        return False
    return not (tags.get('highway') == 'service' and tags.get('service') in kNoService)


def oneway_of(tags, rb):
    ow = tags.get('oneway', '')
    if ow == 'no':
        return 0
    if ow in ('yes', 'true', '1') or rb or tags.get('highway') in ('motorway', 'motorway_link'):
        return 1
    return -1 if ow == '-1' else 0


def car_way(tags, ids):
    """The Way of a car road, None for anything else."""
    hw = tags.get('highway')
    if hw not in kCls or not allowed(tags) or len(ids) < 2:
        return None
    rb = tags.get('junction') in ('roundabout', 'circular')
    cls = kCls[hw]
    return Way(ids, cls, oneway_of(tags, rb), rb, speed_of(tags, cls), tags.get('name', ''), tags.get('ref', ''))


class Ways(osmium.SimpleHandler):
    def __init__(self):
        super().__init__()
        self.ways = []
        self.uses = {}       # node id -> how many way ends (2 each) / passes (1)

    def way(self, w):
        way = car_way(w.tags, [n.ref for n in w.nodes])
        if not way:
            return
        self.ways.append(way)
        last = len(way.ids) - 1
        for i, n in enumerate(way.ids):
            self.uses[n] = self.uses.get(n, 0) + (2 if i in (0, last) else 1)


class Locs(osmium.SimpleHandler):
    def __init__(self, want):
        super().__init__()
        self.want = want
        self.loc = {}
        self.signals = set()

    def node(self, n):
        if n.id in self.want:
            self.loc[n.id] = (int(round(n.location.lat * 1e6)), int(round(n.location.lon * 1e6)))
            if n.tags.get('highway') == 'traffic_signals':
                self.signals.add(n.id)


def dist_m(a, b):
    la = math.radians((a[0] + b[0]) / 2e6)
    dy = (b[0] - a[0]) * 0.111195
    dx = (b[1] - a[1]) * 0.111195 * math.cos(la)
    return math.hypot(dx, dy)


class Names:
    """The "name\\tref" table: one offset per pair, 0 = none."""
    def __init__(self):
        self.data = bytearray(b'\t\x00')
        self.at = {('', ''): 0}

    def add(self, name, ref):
        key = (name, ref)
        if key not in self.at:
            self.at[key] = len(self.data)
            self.data += (name + '\t' + ref).encode('utf-8') + b'\x00'
        return self.at[key]


class Nodes:
    """The graph's nodes: OSM node id -> index, in the order first met."""
    def __init__(self, loc):
        self.loc = loc
        self.idx = {}
        self.pts = []

    def index(self, n):
        if n not in self.idx:
            self.idx[n] = len(self.pts)
            self.pts.append(self.loc[n])
        return self.idx[n]


def segments(ids, junction):
    """The way's node runs from junction to junction (a closed 2-node run left out)."""
    start = 0
    for i in range(1, len(ids)):
        if i == len(ids) - 1 or ids[i] in junction:
            seg = ids[start:i + 1]
            if seg[0] != seg[-1] or len(seg) > 2:
                yield seg
            start = i


def make_pieces(ways, loc, signals, junction):
    nodes, names, pieces = Nodes(loc), Names(), []
    for w in ways:
        ids = [n for n in w.ids if n in loc]
        if len(ids) < 2:
            continue
        nm = names.add(w.name, w.ref)
        for seg in segments(ids, junction):
            pts = [loc[n] for n in seg]
            length = sum(dist_m(pts[k], pts[k + 1]) for k in range(len(pts) - 1))
            # a light: its wait on the piece that ends at it (the start node's: the previous piece's)
            sig = sum(1 for n in seg[1:] if n in signals)
            pieces.append(Piece(nodes.index(seg[0]), nodes.index(seg[-1]), pts[1:-1], length, w.cls, w.oneway, w.rb,
                                w.speed, nm, sig))
    return nodes.pts, names.data, pieces


def directed(pieces, nnodes):
    """Each node's edges (to, dm, ds, geom, ngeom, cls, flags, name, piece) and the inner points."""
    geom, edges = [], [[] for _ in range(nnodes)]
    for pi, p in enumerate(pieces):
        g0 = len(geom)
        geom += p.inner
        dm = int(round(p.length * 10))
        ds = int(round((p.length / (p.speed / 3.6) + p.signals * kSignalS) * 10))
        flags = (2 if p.rb else 0) | (4 if kClasses[p.cls][0].endswith('_link') else 0)
        if p.oneway >= 0:
            edges[p.a].append((p.b, dm, ds, g0, len(p.inner), p.cls, flags, p.name, pi))
        if p.oneway <= 0:
            edges[p.b].append((p.a, dm, ds, g0, len(p.inner), p.cls, flags | 1, p.name, pi))
    return edges, geom


def flatten(edges):
    """first[] (CSR), the edges in a row, each piece's first edge (the grid's)."""
    first, flat, piece_edge = [], [], {}
    for lst in edges:
        first.append(len(flat))
        for e in lst:
            piece_edge.setdefault(e[8], len(flat))
            flat.append(e)
    first.append(len(flat))
    return first, flat, piece_edge


def seg_cells(p, q, box):
    """The cells along a segment (sampled at half a cell: fine against the short segments)."""
    min_lat, min_lon, gw = box
    steps = int(max(abs(q[0] - p[0]), abs(q[1] - p[1])) // (kCell // 2)) + 1
    for s in range(steps + 1):
        la = p[0] + (q[0] - p[0]) * s // steps
        lo = p[1] + (q[1] - p[1]) * s // steps
        yield ((la - min_lat) // kCell) * gw + (lo - min_lon) // kCell


def make_grid(pieces, nodes, piece_edge, box, ncells):
    cells = {}
    for pi, p in enumerate(pieces):
        e = piece_edge.get(pi)
        if e is None:
            continue
        pts = [nodes[p.a]] + p.inner + [nodes[p.b]]
        seen = set()
        for k in range(len(pts)):
            for c in seg_cells(pts[k], pts[k + 1] if k + 1 < len(pts) else pts[k], box):
                if c not in seen:
                    seen.add(c)
                    cells.setdefault(c, []).append(e)
    cstart, items = [], []
    for c in range(ncells):
        cstart.append(len(items))
        items += cells.get(c, [])
    cstart.append(len(items))
    return cstart, items


def write(out, nodes, first, flat, geom, names, bbox, grid):
    (min_lat, min_lon, max_lat, max_lon), (gw, gh, cstart, items) = bbox, grid
    off = [struct.calcsize(HEADER)]
    for size in (len(nodes) * 8, (len(nodes) + 1) * 4, len(flat) * 24, len(geom) * 8, (len(names) + 3) & ~3):
        off.append(off[-1] + size)
    with open(out, 'wb') as f:
        f.write(struct.pack(HEADER, b'OSMG', VERSION, len(nodes), len(flat), len(geom), len(names),
                            min_lat, min_lon, max_lat, max_lon, gw, gh, kCell, len(items), *off))
        f.write(b''.join(struct.pack('<ii', *n) for n in nodes))
        f.write(struct.pack('<%dI' % len(first), *first))
        f.write(b''.join(struct.pack('<IIIIHBBI', *e[:8]) for e in flat))
        f.write(b''.join(struct.pack('<ii', *g) for g in geom))
        f.write(bytes(names) + b'\x00' * (((len(names) + 3) & ~3) - len(names)))
        f.write(struct.pack('<%dI' % len(cstart), *cstart))
        f.write(struct.pack('<%dI' % len(items), *items))
    return off[-1] + (len(cstart) + len(items)) * 4


def build(src, out):
    w = Ways()
    w.apply_file(src)
    locs = Locs(set(w.uses))
    locs.apply_file(src)
    junction = {n for n, c in w.uses.items() if c > 1}   # shared by ways, or a way's end
    nodes, names, pieces = make_pieces(w.ways, locs.loc, locs.signals, junction)
    edges, geom = directed(pieces, len(nodes))
    first, flat, piece_edge = flatten(edges)
    lats, lons = [n[0] for n in nodes], [n[1] for n in nodes]
    bbox = (min(lats), min(lons), max(lats), max(lons))
    gw, gh = (bbox[3] - bbox[1]) // kCell + 1, (bbox[2] - bbox[0]) // kCell + 1
    cstart, items = make_grid(pieces, nodes, piece_edge, (bbox[0], bbox[1], gw), gw * gh)
    size = write(out, nodes, first, flat, geom, names, bbox, (gw, gh, cstart, items))
    print('nodes %d, edges %d, geom %d, names %d B, grid %dx%d (%d items); file %.1f MB' % (
        len(nodes), len(flat), len(geom), len(names), gw, gh, len(items), size / 1e6))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2])
