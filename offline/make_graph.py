"""The road graph of a region for OSMapsCE's offline routing (offline.cpp on the unit).

    python make_graph.py <region>.osm.pbf <out>/route.grf

Car roads only (highway classes below, access / motor_vehicle not "no" / "private"), split at the junctions; each
piece is an edge in both directions unless one-way. Times from the class's speed (or maxspeed, capped). Turn
restrictions: not yet. The file (little endian, all offsets in bytes from the file's start):

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
VERSION = 1


def speed_of(tags, cls):
    v = kClasses[cls][1]
    ms = tags.get('maxspeed', '')
    try:
        m = int(ms.split()[0])
        if 'mph' in ms:
            m = int(m * 1.609)
        v = min(v, m) if m >= 5 else v
    except (ValueError, IndexError):
        pass
    if tags.get('highway') == 'track' and tags.get('tracktype', 'grade1') not in ('grade1', 'grade2'):
        v = 8
    return max(v, 5)


class Ways(osmium.SimpleHandler):
    def __init__(self):
        super().__init__()
        self.ways = []       # (node ids, cls, oneway, roundabout, speed, name, ref)
        self.uses = {}       # node id -> how many way ends / passes

    def way(self, w):
        t = w.tags
        hw = t.get('highway')
        if hw not in kCls:
            return
        if t.get('area') == 'yes':
            return
        for k in ('access', 'motor_vehicle', 'motorcar', 'vehicle'):
            if t.get(k) in kNoAccess:
                return
        if hw == 'service' and t.get('service') in ('parking_aisle', 'drive-through', 'emergency_access'):
            return
        ids = [n.ref for n in w.nodes]
        if len(ids) < 2:
            return
        cls = kCls[hw]
        rb = t.get('junction') in ('roundabout', 'circular')
        ow = t.get('oneway', '')
        oneway = 1 if ow in ('yes', 'true', '1') or rb or hw in ('motorway', 'motorway_link') else -1 if ow == '-1' else 0
        if ow == 'no':
            oneway = 0
        self.ways.append((ids, cls, oneway, rb, speed_of(t, cls), t.get('name', ''), t.get('ref', '')))
        for i, n in enumerate(ids):
            self.uses[n] = self.uses.get(n, 0) + (2 if i in (0, len(ids) - 1) else 1)


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


def main():
    src, out = sys.argv[1], sys.argv[2]
    w = Ways()
    w.apply_file(src)
    print('ways', len(w.ways), 'nodes used', len(w.uses))
    l = Locs(set(w.uses))
    l.apply_file(src)
    loc = l.loc
    # junctions: a node shared by ways or a way's end
    junction = {n for n, c in w.uses.items() if c > 1}
    idx = {}
    nodes = []

    def node_index(n):
        i = idx.get(n)
        if i is None:
            i = idx[n] = len(nodes)
            nodes.append(loc[n])
        return i

    names = bytearray(b'\t\x00')
    name_off = {('', ''): 0}
    pieces = []   # (a, b, inner points, lenM, cls, oneway, rb, speed, nameOff)
    for ids, cls, oneway, rb, speed, name, ref in w.ways:
        ids = [n for n in ids if n in loc]
        if len(ids) < 2:
            continue
        key = (name, ref)
        if key not in name_off:
            name_off[key] = len(names)
            names += (name + '\t' + ref).encode('utf-8') + b'\x00'
        start = 0
        for i in range(1, len(ids)):
            if i == len(ids) - 1 or ids[i] in junction:
                seg = ids[start:i + 1]
                if seg[0] != seg[-1] or len(seg) > 2:
                    pts = [loc[n] for n in seg]
                    length = sum(dist_m(pts[k], pts[k + 1]) for k in range(len(pts) - 1))
                    # a light: its wait on the piece that ends at it (the start node's: the previous piece's)
                    sig = sum(1 for n in seg[1:] if n in l.signals)
                    pieces.append((node_index(seg[0]), node_index(seg[-1]), pts[1:-1], length, cls, oneway, rb,
                                   speed, name_off[key], sig))
                start = i
    print('nodes', len(nodes), 'pieces', len(pieces))
    # directed edges, grouped by their start node
    geom = []
    edges = [[] for _ in nodes]
    for pi, (a, b, inner, length, cls, oneway, rb, speed, nm, sig) in enumerate(pieces):
        g0 = len(geom)
        geom += inner
        dm = int(round(length * 10))
        ds = int(round((length / (speed / 3.6) + sig * kSignalS) * 10))
        flags = (2 if rb else 0) | (4 if kClasses[cls][0].endswith('_link') else 0)
        if oneway >= 0:
            edges[a].append((b, dm, ds, g0, len(inner), cls, flags, nm, pi))
        if oneway <= 0:
            edges[b].append((a, dm, ds, g0, len(inner), cls, flags | 1, nm, pi))
    nedges = sum(len(e) for e in edges)
    lats = [n[0] for n in nodes]
    lons = [n[1] for n in nodes]
    minLat, maxLat, minLon, maxLon = min(lats), max(lats), min(lons), max(lons)
    # the grid: each piece (its forward edge, or the backward one for oneway=-1) in the cells its points touch
    cell = 5000   # 0.005 degrees: ~550 x 330 m here
    gw = (maxLon - minLon) // cell + 1
    gh = (maxLat - minLat) // cell + 1
    cells = {}
    first = []
    flat = []
    eid = 0
    piece_edge = {}
    for n, lst in enumerate(edges):
        first.append(eid)
        for e in lst:
            if e[8] not in piece_edge:
                piece_edge[e[8]] = eid
            flat.append(e)
            eid += 1
    first.append(eid)
    for pi, (a, b, inner, *_rest) in enumerate(pieces):
        e = piece_edge.get(pi)
        if e is None:
            continue
        pts = [nodes[a]] + inner + [nodes[b]]
        seen = set()
        for k in range(len(pts)):
            p = pts[k]
            q = pts[k + 1] if k + 1 < len(pts) else p
            # the segment's cells (fine enough: segments are short against 0.005 degrees; long ones sampled)
            steps = int(max(abs(q[0] - p[0]), abs(q[1] - p[1])) // (cell // 2)) + 1
            for s in range(steps + 1):
                la = p[0] + (q[0] - p[0]) * s // steps
                lo = p[1] + (q[1] - p[1]) * s // steps
                c = ((la - minLat) // cell) * gw + (lo - minLon) // cell
                if c not in seen:
                    seen.add(c)
                    cells.setdefault(c, []).append(e)
    cstart = []
    items = []
    for c in range(gw * gh):
        cstart.append(len(items))
        items += cells.get(c, [])
    cstart.append(len(items))

    hdr_fmt = '<4s5I4i2IiI6I'
    hdr_len = struct.calcsize(hdr_fmt)
    off_nodes = hdr_len
    off_first = off_nodes + len(nodes) * 8
    off_edges = off_first + (len(nodes) + 1) * 4
    off_geom = off_edges + nedges * 24
    off_names = off_geom + len(geom) * 8
    off_grid = off_names + ((len(names) + 3) & ~3)
    with open(out, 'wb') as f:
        f.write(struct.pack(hdr_fmt, b'OSMG', VERSION, len(nodes), nedges, len(geom), len(names),
                            minLat, minLon, maxLat, maxLon, gw, gh, cell, len(items),
                            off_nodes, off_first, off_edges, off_geom, off_names, off_grid))
        f.write(b''.join(struct.pack('<ii', *n) for n in nodes))
        f.write(struct.pack('<%dI' % len(first), *first))
        f.write(b''.join(struct.pack('<IIIIHBBI', e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]) for e in flat))
        f.write(b''.join(struct.pack('<ii', *g) for g in geom))
        f.write(bytes(names) + b'\x00' * (((len(names) + 3) & ~3) - len(names)))
        f.write(struct.pack('<%dI' % len(cstart), *cstart))
        f.write(struct.pack('<%dI' % len(items), *items))
    total = off_grid + (len(cstart) + len(items)) * 4
    print('edges %d, geom %d, names %d B, grid %dx%d (%d items); file %.1f MB (in memory without geom: %.1f MB)' % (
        nedges, len(geom), len(names), gw, gh, len(items), total / 1e6, (total - len(geom) * 8) / 1e6))


if __name__ == '__main__':
    main()
