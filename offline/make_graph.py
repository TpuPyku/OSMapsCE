"""The road graph of a region for OSMapsCE's offline routing (offline.cpp on the unit).

    python make_graph.py <region>.osm.pbf <out>/route.grf
    python make_graph.py <country>.osm.pbf <out>/trunk.grf trunk      (4b: the trunk roads, see TRUNK)

Car roads only (highway classes below, access / motor_vehicle not "no" / "private"), split at the junctions; each
piece is an edge in both directions unless one-way. Times from the class's speed (or maxspeed, capped) plus the
traffic lights' wait. Turn restrictions (from way, via node, to way; no_* / only_*): the restricted way in reaches a
copy of its via node with only the turns allowed (turn_rules) - the file and the router know nothing of them. The file (little endian, all offsets in bytes from the file's start):

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
    bearings 'OSMB', then u8 out, u8 in for each edge: its way off its start and into its end, kBearM along
             (256 = a full circle, 0 = north, clockwise) - the turns' costs (the user 2026-11-09); after the grid,
             so the header (and the readers before it) stay as they were
    limits   0-2 zero bytes to a multiple of 4 (the bearings' 2 an edge), 'OSMS', then u8 for each edge: its posted
             speed limit, km/h (limit_of; 0 = not known) - the speed limit sign (design v3, 2026-11-11); after the
             bearings, for the same reason
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
kSignalJoinM = 60   # lights this close along a way (each to the one before): one crossing's - counted once
# The barriers on roads (the user 2026-10-29: a route through a hospital's grounds, closed by gates): a gate with no
# access tag - kGateS more (a way through avoided, a place inside still reached), the passed ones - nothing, any
# other (a block, a bollard) - the road cut there. A gate closed by its access tag (private, no...) - kLockedGates gates'
# worth (the user 2026-11-08: a place behind one - the route ended at the gate): a way into it, never one through it
kGateS = 600
kLockedGates = 3
kGates = {'gate', 'swing_gate', 'sliding_gate', 'hampshire_gate', 'wicket_gate',
          'lift_gate'}   # (2026-10-31: grounds behind lift gates - into them, not across)
kPassed = {'toll_booth', 'cattle_grid', 'border_control', 'entrance', 'no', 'height_restrictor', 'kerb',
           'sally_port', 'checkpoint'}
kAccessKeys = ('access', 'motor_vehicle', 'motorcar', 'vehicle')
kNoAccess = {'no', 'private', 'agricultural', 'forestry', 'delivery', 'destination_no'}
kNoService = {'parking_aisle', 'drive-through', 'emergency_access'}
kCell = 5000   # the grid's cell, 1e-6 degrees: 0.005 degrees, ~550 x 330 m at 53 N
kMaxCells = 1 << 20   # a bigger grid: its cell doubled (a whole country: Kazakhstan's 4.7 M cells were 19 MB)
VERSION = 1
HEADER = '<4s5I4i2IiI6I'

# a car road as read: its node ids, class, one-way (1 forward, -1 backward, 0 both), roundabout, km/h, name, ref
Way = namedtuple('Way', 'ids cls oneway rb speed name ref toll wid limit', defaults=(0, 0))   # wid: its OSM id; limit: km/h
# a junction-to-junction part of a way: node indices a -> b, inner points, metres, its way's data, lights on it
Piece = namedtuple('Piece', 'a b inner length cls oneway rb speed name signals gates toll wid limit', defaults=(0, 0))
# the countries' limits by the kind of road (maxspeed's "RU:urban" and the like): Russia's and its neighbours' rules
kKindLimits = {'urban': 60, 'rural': 90, 'motorway': 110, 'living_street': 20, 'trunk': 90}


def maxspeed_kmh(text):
    """OSM's maxspeed ("60", "40 mph") in km/h; None when not a number (or below 5)."""
    try:
        m = int(text.split()[0])
    except (ValueError, IndexError):
        return None
    m = int(m * 1.609) if 'mph' in text else m
    return m if m >= 5 else None


def limit_of(tags):
    """The posted speed limit, km/h: maxspeed's number (or mph), else a country's code for the kind of road in maxspeed,
    maxspeed:type, source:maxspeed ("RU:urban") or zone:maxspeed ("RU:20"); 0 = not known (or not a real one)."""
    m = maxspeed_kmh(tags.get('maxspeed', ''))
    if m:
        return m if m <= 250 else 0
    for key in ('maxspeed', 'maxspeed:type', 'source:maxspeed', 'zone:maxspeed'):
        code = tags.get(key, '')
        if ':' not in code:
            continue
        kind = code.split(':', 1)[1]
        if kind in kKindLimits:
            return kKindLimits[kind]
        if kind.isdigit() and 5 <= int(kind) <= 250:
            return int(kind)
    return 0


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


def barrier_of(tags):
    """A node's barrier for cars: None (none, or passed), 'gate' (passed, kGateS more), 'locked' (a gate closed by
    its access tag: kLockedGates gates' worth), 'block' (no way past)."""
    b = tags.get('barrier')
    if not b:
        return None
    access = [tags.get(k) for k in kAccessKeys if tags.get(k)]
    if any(a in kNoAccess for a in access):
        return 'locked' if b in kGates else 'block'
    if access or b in kPassed:
        return None
    return 'gate' if b in kGates else 'block'


def toll_of(tags):   # a toll road (the user 2026-10-29: an option to keep off them; the edges' flag 8)
    return tags.get('toll') == 'yes' or tags.get('toll:motorcar') == 'yes'


def car_way(tags, ids, wid=0):
    """The Way of a car road, None for anything else."""
    hw = tags.get('highway')
    if hw not in kCls or not allowed(tags) or len(ids) < 2:
        return None
    rb = tags.get('junction') in ('roundabout', 'circular')
    cls = kCls[hw]
    return Way(ids, cls, oneway_of(tags, rb), rb, speed_of(tags, cls), tags.get('name', ''), tags.get('ref', ''),
               toll_of(tags), wid, limit_of(tags))


kTurnKinds = {'no_left_turn', 'no_right_turn', 'no_straight_on', 'no_u_turn', 'only_left_turn', 'only_right_turn',
              'only_straight_on', 'only_u_turn'}


def turn_rule(tags):
    """A relation's turn restriction for cars: 'no' / 'only', None for anything else (another kind, cars excepted)."""
    if tags.get('type') != 'restriction' or 'motorcar' in tags.get('except', '').split(';'):
        return None
    kind = tags.get('restriction:motorcar') or tags.get('restriction')
    return kind.split('_')[0] if kind in kTurnKinds else None


# The trunk graph (4b, the user 2026-10-27/29: from one region to another, Vladivostok - Moscow, without the internet):
# the main roads only, their shapes simplified, a coarse grid - for the whole country
TRUNK = dict(classes={'motorway', 'trunk', 'primary', 'motorway_link', 'trunk_link', 'primary_link'},
             simplify_m=50, cell=200000, west=0)   # (west: Chukotka's east beyond 180 degrees left out)


class Ways(osmium.SimpleHandler):
    def __init__(self, classes=None):
        super().__init__()
        self.ways = []
        self.rules = []
        self.uses = {}       # node id -> how many way ends (2 each) / passes (1)
        self.keep = {kCls[c] for c in classes} if classes else None

    def relation(self, r):   # a turn restriction: (no / only, from ways, its via node, to ways)
        rule = turn_rule(r.tags)
        roles = {}
        for m in r.members:
            roles.setdefault((m.role, m.type), []).append(m.ref)
        via = roles.get(('via', 'n'), [])
        if rule and len(via) == 1 and roles.get(('from', 'w')) and roles.get(('to', 'w')):
            self.rules.append((rule, set(roles[('from', 'w')]), via[0], set(roles[('to', 'w')])))

    def way(self, w):
        way = car_way(w.tags, [n.ref for n in w.nodes], w.id)
        if not way or (self.keep is not None and way.cls not in self.keep):
            return
        self.ways.append(way)
        last = len(way.ids) - 1
        for i, n in enumerate(way.ids):
            self.uses[n] = self.uses.get(n, 0) + (2 if i in (0, last) else 1)


class Locs(osmium.SimpleHandler):
    def __init__(self, want, west=None):
        super().__init__()
        self.want = want
        self.west = west   # 1e-6 degrees: the nodes west of it left out (None: none)
        self.loc = {}
        self.signals = set()
        self.gates = set()     # barriers passed for kGateS more
        self.blocks = set()    # barriers no car passes: the road cut there
        self.locked = set()    # gates closed by their access tags: kLockedGates gates' worth

    def node(self, n):
        if n.id in self.want and (self.west is None or n.location.lon * 1e6 >= self.west):
            self.loc[n.id] = (int(round(n.location.lat * 1e6)), int(round(n.location.lon * 1e6)))
            if n.tags.get('highway') == 'traffic_signals':
                self.signals.add(n.id)
            kind = barrier_of(n.tags)
            if kind:
                {'gate': self.gates, 'locked': self.locked}.get(kind, self.blocks).add(n.id)


def dist_m(a, b):
    la = math.radians((a[0] + b[0]) / 2e6)
    dy = (b[0] - a[0]) * 0.111195
    dx = (b[1] - a[1]) * 0.111195 * math.cos(la)
    return math.hypot(dx, dy)


def simplify(pts, tol_m):
    """Douglas-Peucker: the points of the line within tol_m metres of it dropped (the ends kept)."""
    if tol_m <= 0 or len(pts) < 3:
        return list(pts)
    a, b = pts[0], pts[-1]
    k = math.cos(math.radians(a[0] / 1e6))
    ax, ay, bx, by = a[1] * k, a[0], b[1] * k, b[0]
    dx, dy = bx - ax, by - ay
    span = math.hypot(dx, dy)
    worst, at = -1.0, 0
    for i in range(1, len(pts) - 1):
        px, py = pts[i][1] * k - ax, pts[i][0] - ay
        d = abs(px * dy - py * dx) / span if span else math.hypot(px, py)
        if d > worst:
            worst, at = d, i
    if worst * 0.111195 <= tol_m:
        return [a, b]
    return simplify(pts[:at + 1], tol_m)[:-1] + simplify(pts[at:], tol_m)


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

    def alone(self, n):   # a node of its own at n's place (a block: each side's end there, joined to nothing)
        self.pts.append(self.loc[n])
        return len(self.pts) - 1


def segments(ids, junction):
    """The way's node runs from junction to junction (a closed 2-node run left out)."""
    start = 0
    for i in range(1, len(ids)):
        if i == len(ids) - 1 or ids[i] in junction:
            seg = ids[start:i + 1]
            if seg[0] != seg[-1] or len(seg) > 2:
                yield seg
            start = i


Barriers = namedtuple('Barriers', 'gates blocks locked')
NONE = Barriers(frozenset(), frozenset(), frozenset())


def make_piece(w, seg, nm, loc, signals, nodes, tol_m=0, bars=NONE):
    """The way's run seg (junction to junction) as a piece of road; its shape within tol_m (its length the true one);
    its gates counted, an end at a block a node of its own."""
    pts = [loc[n] for n in seg]
    length = sum(dist_m(pts[k], pts[k + 1]) for k in range(len(pts) - 1))
    # a light: its wait on the piece that ends at it (the start node's: the previous piece's)
    sig = sum(1 for n in seg[1:] if n in signals)
    end = lambda n: nodes.alone(n) if n in bars.blocks else nodes.index(n)   # noqa: E731
    return Piece(end(seg[0]), end(seg[-1]), simplify(pts, tol_m)[1:-1], length, w.cls, w.oneway, w.rb,
                 w.speed, nm, sig, sum(1 if n in bars.gates else kLockedGates if n in bars.locked else 0 for n in seg), w.toll,
                 w.wid, w.limit)


def lone_signals(ids, loc, signals):
    """The way's lights that count: one a crossing (the user 2026-11-14: OSM marks each stop line - Щёлковское шоссе
    at Хабаровская has four on 92 m, 32 s, its дублёр two: the route took the дублёр). A light within kSignalJoinM
    along the way of the light before it is that one's crossing's."""
    out, at, last = set(), 0.0, None
    for k, n in enumerate(ids):
        at += dist_m(loc[ids[k - 1]], loc[n]) if k else 0
        if n in signals:
            if last is None or at - last > kSignalJoinM:
                out.add(n)
            last = at
    return out


def make_pieces(ways, loc, signals, junction, tol_m=0, bars=NONE):
    nodes, names, pieces = Nodes(loc), Names(), []
    for w in ways:
        ids = [n for n in w.ids if n in loc]
        if len(ids) < 2:
            continue
        nm = names.add(w.name, w.ref)
        lone = lone_signals(ids, loc, signals)
        pieces += [make_piece(w, seg, nm, loc, lone, nodes, tol_m, bars) for seg in segments(ids, junction)]
    return nodes.pts, names.data, pieces


def main_network(nodes, pieces):
    """The nodes and pieces of the largest strongly connected part (the user's log 2026-10-29: a point snapped onto
    the Kremlin's inner roads, joined to no other - no way, or the whole region searched for 67 s; OSRM keeps only
    its main part too): a road one can drive to from anywhere and away again."""
    comp = scc(adjacency(pieces, len(nodes)))
    sizes = {}
    for c in comp:
        sizes[c] = sizes.get(c, 0) + 1
    main = max(sizes, key=sizes.get) if sizes else -1
    return renumbered(nodes, [p for p in pieces if comp[p.a] == main and comp[p.b] == main])


def adjacency(pieces, n):   # each node's next nodes by the pieces' directions
    adj = [[] for _ in range(n)]
    for p in pieces:
        if p.oneway >= 0:
            adj[p.a].append(p.b)
        if p.oneway <= 0:
            adj[p.b].append(p.a)
    return adj


def hilbert(x, y, order):
    """(x, y) of a 2^order square's grid -> its place along the Hilbert curve (from (0, 0) up first)."""
    d, s = 0, 1 << (order - 1)
    while s:
        rx, ry = int(x & s > 0), int(y & s > 0)
        d += s * s * ((3 * rx) ^ ry)
        if not ry:
            if rx:
                x, y = s - 1 - x, s - 1 - y
            x, y = y, x
        s >>= 1
    return d


def curve_keys(nodes, used):   # each used node's place along the Hilbert curve over their box (65536 a side)
    if not used:
        return {}
    lats, lons = [nodes[n][0] for n in used], [nodes[n][1] for n in used]
    la0, lo0 = min(lats), min(lons)
    span = max(max(lats) - la0, max(lons) - lo0, 1)
    return {n: hilbert((nodes[n][1] - lo0) * 65535 // span, (nodes[n][0] - la0) * 65535 // span, 16) for n in used}


def renumbered(nodes, keep):
    """The kept pieces' nodes only, numbered along a Hilbert curve over their box (the user's Shturmann 2026-10-31: in
    the order met a 115 km way read most of the 96 MB graph into memory - a place's neighbours were all over the
    file); the pieces by their first node, their inner points and edges near too."""
    used = sorted({p.a for p in keep} | {p.b for p in keep})
    key = curve_keys(nodes, used)
    order = sorted(used, key=lambda n: (key[n], n))
    new = {n: i for i, n in enumerate(order)}
    pieces = [p._replace(a=new[p.a], b=new[p.b]) for p in keep]
    return [nodes[n] for n in order], sorted(pieces, key=lambda p: min(p.a, p.b))


class Tarjan:
    """Each node's strongly connected part's number (Tarjan's, without recursion: a million nodes)."""
    def __init__(self, adj):
        n = len(adj)
        self.adj, self.index, self.low, self.comp = adj, [-1] * n, [0] * n, [-1] * n
        self.stack, self.on, self.counter, self.parts = [], [False] * n, 0, 0

    def run(self, root):
        work = [(root, 0)]
        while work:
            v, i = work.pop()
            if i == 0:
                self.enter(v)
            if i < len(self.adj[v]):
                self.step(v, i, work)
            else:
                self.leave(v)

    def enter(self, v):
        self.index[v] = self.low[v] = self.counter
        self.counter += 1
        self.stack.append(v)
        self.on[v] = True

    def step(self, v, i, work):   # v's edge i: its node to visit, or a node on the stack lowering v
        work.append((v, i + 1))
        w = self.adj[v][i]
        if self.index[w] < 0:
            work.append((w, 0))
        elif self.on[w]:
            self.low[v] = min(self.low[v], self.index[w])

    def leave(self, v):   # v done: its children's lows; a root - its part off the stack
        for w in self.adj[v]:
            if self.on[w] and self.comp[w] < 0:
                self.low[v] = min(self.low[v], self.low[w])
        if self.low[v] != self.index[v]:
            return
        while True:
            w = self.stack.pop()
            self.on[w] = False
            self.comp[w] = self.parts
            if w == v:
                break
        self.parts += 1


def scc(adj):
    t = Tarjan(adj)
    for root in range(len(adj)):
        if t.index[root] < 0:
            t.run(root)
    return t.comp


def directed(pieces, nnodes):
    """Each node's edges (to, dm, ds, geom, ngeom, cls, flags, name, piece) and the inner points."""
    geom, edges = [], [[] for _ in range(nnodes)]
    for pi, p in enumerate(pieces):
        g0 = len(geom)
        geom += p.inner
        dm = int(round(p.length * 10))
        ds = int(round((p.length / (p.speed / 3.6) + p.signals * kSignalS + p.gates * kGateS) * 10))
        flags = (2 if p.rb else 0) | (4 if kClasses[p.cls][0].endswith('_link') else 0) | (8 if p.toll else 0)
        if p.oneway >= 0:
            edges[p.a].append((p.b, dm, ds, g0, len(p.inner), p.cls, flags, p.name, pi))
        if p.oneway <= 0:
            edges[p.b].append((p.a, dm, ds, g0, len(p.inner), p.cls, flags | 1, p.name, pi))
    return edges, geom


def turn_rules(nodes, edges, pieces, rules, loc):
    """The restrictions into the graph (edges: each node's list, as directed() makes them): each restricted way in
    (an edge into the via node by a "from" way) goes to a copy of the via node at its place, with the via node's edges
    out but the ones forbidden (no_*: onto the "to" ways; only_*: onto any other). Returns the copies made."""
    at, into = {}, {}
    for i, p in enumerate(nodes):
        at.setdefault(tuple(p), i)
    for u, lst in enumerate(edges):
        for k, e in enumerate(lst):
            into.setdefault(e[0], []).append((u, k))
    banned = {}   # (u, k) -> (via, the forbidden places in its list)
    for kind, froms, via, tos in rules:
        v = at.get(tuple(loc[via])) if via in loc else None
        if v is None:
            continue
        outs = {j for j, e in enumerate(edges[v]) if pieces[e[8]].wid in tos}
        if not outs:
            continue
        forbid = outs if kind == 'no' else set(range(len(edges[v]))) - outs
        for u, k in into.get(v, []):
            if pieces[edges[u][k][8]].wid in froms:
                banned.setdefault((u, k), (v, set()))[1].update(forbid)
    for (u, k), (v, forbid) in banned.items():
        nodes.append(nodes[v])
        edges.append([e for j, e in enumerate(edges[v]) if j not in forbid])
        edges[u][k] = (len(nodes) - 1,) + tuple(edges[u][k][1:])
    return len(banned)


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
    min_lat, min_lon, gw, cell = box
    steps = int(max(abs(q[0] - p[0]), abs(q[1] - p[1])) // (cell // 2)) + 1
    for s in range(steps + 1):
        la = p[0] + (q[0] - p[0]) * s // steps
        lo = p[1] + (q[1] - p[1]) * s // steps
        yield ((la - min_lat) // cell) * gw + (lo - min_lon) // cell


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


kBearM = 20   # metres along an edge for its bearings: past the first few metres (a fork's short first segments)


def bearing_byte(p, q):
    """From point p to q (lat, lon 1e-6 degrees): 0..255, 0 = north, clockwise."""
    dx = (q[1] - p[1]) * math.cos(math.radians(p[0] / 1e6))
    return int(round(math.degrees(math.atan2(dx, q[0] - p[0])) * 256 / 360)) & 255


def point_along(pts):
    """The point kBearM along a line from its first point (its last one, if shorter)."""
    m, kx = 0.0, math.cos(math.radians(pts[0][0] / 1e6))
    for k in range(1, len(pts)):
        seg = math.hypot((pts[k][1] - pts[k - 1][1]) * kx, pts[k][0] - pts[k - 1][0]) * 0.111195
        if m + seg >= kBearM:
            u = (kBearM - m) / seg
            return tuple(round(pts[k - 1][i] + (pts[k][i] - pts[k - 1][i]) * u) for i in (0, 1))
        m += seg
    return pts[-1]


def bearings(nodes, first, flat, geom):
    """Each edge's (out, in) bearing bytes, in the edges' order."""
    out = bytearray()
    for a in range(len(nodes)):
        for e in flat[first[a]:first[a + 1]]:
            inner = geom[e[3]:e[3] + e[4]]
            pts = [nodes[a]] + (inner[::-1] if e[6] & 1 else inner) + [nodes[e[0]]]
            out.append(bearing_byte(pts[0], point_along(pts)))
            out.append(bearing_byte(point_along(pts[::-1]), pts[-1]))
    return bytes(out)


def write(out, nodes, first, flat, geom, names, bbox, grid, cell=kCell, limits=b''):
    (min_lat, min_lon, max_lat, max_lon), (gw, gh, cstart, items) = bbox, grid
    off = [struct.calcsize(HEADER)]
    for size in (len(nodes) * 8, (len(nodes) + 1) * 4, len(flat) * 24, len(geom) * 8, (len(names) + 3) & ~3):
        off.append(off[-1] + size)
    with open(out, 'wb') as f:
        f.write(struct.pack(HEADER, b'OSMG', VERSION, len(nodes), len(flat), len(geom), len(names),
                            min_lat, min_lon, max_lat, max_lon, gw, gh, cell, len(items), *off))
        f.write(b''.join(struct.pack('<ii', *n) for n in nodes))
        f.write(struct.pack('<%dI' % len(first), *first))
        f.write(b''.join(struct.pack('<IIIIHBBI', *e[:8]) for e in flat))
        f.write(b''.join(struct.pack('<ii', *g) for g in geom))
        f.write(bytes(names) + b'\x00' * (((len(names) + 3) & ~3) - len(names)))
        f.write(struct.pack('<%dI' % len(cstart), *cstart))
        f.write(struct.pack('<%dI' % len(items), *items))
        f.write(b'OSMB' + bearings(nodes, first, flat, geom))
        pad = (len(flat) * 2) % 4
        if limits:
            f.write(b'\0' * pad + b'OSMS' + limits)
    return off[-1] + (len(cstart) + len(items)) * 4 + 4 + len(flat) * 2 + (pad + 4 + len(limits) if limits else 0)


def build(src, out, classes=None, simplify_m=0, cell=kCell, west=None, max_cells=kMaxCells):
    w = Ways(classes)
    w.apply_file(src)
    locs = Locs(set(w.uses), west)
    locs.apply_file(src)
    junction = {n for n, c in w.uses.items() if c > 1} | locs.blocks   # shared by ways, a way's end, a block
    nodes, names, pieces = make_pieces(w.ways, locs.loc, locs.signals, junction, simplify_m,
                                       Barriers(locs.gates, locs.blocks, locs.locked))
    nodes, pieces = main_network(nodes, pieces)
    edges, geom = directed(pieces, len(nodes))
    turn_rules(nodes, edges, pieces, w.rules, locs.loc)
    first, flat, piece_edge = flatten(edges)
    lats, lons = [n[0] for n in nodes], [n[1] for n in nodes]
    bbox = (min(lats), min(lons), max(lats), max(lons))
    gw, gh = (bbox[3] - bbox[1]) // cell + 1, (bbox[2] - bbox[0]) // cell + 1
    while gw * gh > max_cells:
        cell *= 2
        gw, gh = (bbox[3] - bbox[1]) // cell + 1, (bbox[2] - bbox[0]) // cell + 1
    cstart, items = make_grid(pieces, nodes, piece_edge, (bbox[0], bbox[1], gw, cell), gw * gh)
    limits = bytes(pieces[e[8]].limit for e in flat)
    size = write(out, nodes, first, flat, geom, names, bbox, (gw, gh, cstart, items), cell, limits)
    print('nodes %d, edges %d, geom %d, names %d B, grid %dx%d (%d items); file %.1f MB' % (
        len(nodes), len(flat), len(geom), len(names), gw, gh, len(items), size / 1e6))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], **(TRUNK if sys.argv[3:] == ['trunk'] else {}))
