#include "yamaps.h"

// Offline routing (2026-10-27, the user: a full offline mode; the data on the SD card, regions to choose): a region's
// road graph (OSMapsData\<region>\route.grf, made by offline/make_graph.py from OSM) and A* over it on the download
// thread. The answer is OSRM's JSON (geometry polyline6, legs, steps, maneuvers): route.cpp reads it as it reads
// OSRM's - the guidance, the line, the stops work unchanged. A route request (OSRM's URL) is answered here when a
// region holds all its points; else it goes to OSRM. Integer A* (the unit's ARM has no FPU): times in tenths of a
// second, the estimate a straight line at 110 km/h. Turn restrictions: not yet in the data.

#pragma pack(push, 1)
struct GHead {
    char  magic[4];
    DWORD version, nodes, edges, geomPts, nameBytes;
    int   minLat, minLon, maxLat, maxLon;   // 1e-6 degrees
    DWORD gw, gh;
    int   cell;
    DWORD items;
    DWORD off[6];   // nodes, first, edges, geom, names, grid
};
struct GEdge {
    DWORD to, lenDm, timeDs, geom;
    WORD  ngeom;
    BYTE  cls, flags;   // flags: 1 geometry read backwards, 2 roundabout, 4 link
    DWORD name;
};
#pragma pack(pop)

enum { C_MOTORWAY, C_TRUNK, C_PRIMARY };   // make_graph.py's kClasses (the first ones)
static const int kMaxV = 110;               // km/h: the estimate's speed (no road is faster)

struct Graph {
    wchar_t path[MAX_PATH];
    GHead   h;
    int*    nodes;     // lat, lon pairs
    DWORD*  first;
    GEdge*  edges;
    char*   names;
    DWORD*  cstart;    // gw * gh + 1
    DWORD*  items;
    OffFile d;         // the parts above; the geometry is read from its file
};

static Graph s_g;      // the region loaded (one at a time)

// ---------------------------------------------------------------- the data

static const wchar_t* kDataDirs[] = { L"\\SDMemory\\OSMapsData", L"\\USBDevice\\OSMapsData" };

static void Unload()
{
    OffClose(&s_g.d);
    memset(&s_g, 0, sizeof(s_g));
}

static bool ReadHead(const wchar_t* path, GHead* h)
{
    OffFile f;
    bool ok = OffOpen(&f, path, h, sizeof(*h)) && !memcmp(h->magic, "OSMG", 4) && h->version == 1;
    OffClose(&f);
    return ok;
}

static bool Load(const wchar_t* path)
{
    if (s_g.nodes && !wcscmp(s_g.path, path))
        return true;
    Unload();
    DWORD t0 = GetTickCount();
    GHead& h = s_g.h;
    if (!OffOpen(&s_g.d, path, &h, sizeof(h)))
        return false;
    OffFile* f = &s_g.d;
    s_g.nodes = (int*)OffPart(f, h.off[0], h.nodes * 8);
    s_g.first = (DWORD*)OffPart(f, h.off[1], (h.nodes + 1) * 4);
    s_g.edges = (GEdge*)OffPart(f, h.off[2], h.edges * sizeof(GEdge));
    s_g.names = (char*)OffPart(f, h.off[4], h.nameBytes);
    s_g.cstart = (DWORD*)OffPart(f, h.off[5], (h.gw * h.gh + 1) * 4);
    s_g.items = (DWORD*)OffPart(f, h.off[5] + (h.gw * h.gh + 1) * 4, h.items * 4);
    if (f->bad) {
        Log("offline: cannot load %S (memory?)", path);
        Unload();
        return false;
    }
    wcsncpy(s_g.path, path, MAX_PATH - 1);
    Log("offline: %S: %lu nodes, %lu edges, %lu ms", path, h.nodes, h.edges, GetTickCount() - t0);
    return true;
}

static bool Inside(const GHead& h, int lat, int lon)
{
    return lat >= h.minLat && lat <= h.maxLat && lon >= h.minLon && lon <= h.maxLon;
}

static bool HoldsAll(const GHead& h, const int* pts, int n)
{
    int i = 0;
    while (i < n && Inside(h, pts[i * 2], pts[i * 2 + 1]))
        i++;
    return i == n;
}

// The folders the regions' folders may be in; returns their count.
static int DataDirs(wchar_t dirs[][MAX_PATH])
{
    int nd = 0;
    for (int i = 0; i < 2; i++)
        wcscpy(dirs[nd++], kDataDirs[i]);
    PathInDir(dirs[nd++], L"OSMapsData");
#ifndef UNDER_CE
    PathInDir(dirs[nd++], L"..\\OSMapsData");   // the PC: the tool's output beside the program's folder
#endif
    return nd;
}

// A found entry of a data folder: a region's folder whose graph holds the points (dir = it)?
static bool Holds(const wchar_t* data, const WIN32_FIND_DATA& fd, const int* pts, int n, wchar_t* dir)
{
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
        return false;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\%s\\route.grf", data, fd.cFileName);
    path[MAX_PATH - 1] = 0;
    GHead h;
    if (!ReadHead(path, &h) || !HoldsAll(h, pts, n))
        return false;
    _snwprintf(dir, MAX_PATH, L"%s\\%s", data, fd.cFileName);
    dir[MAX_PATH - 1] = 0;
    return true;
}

// The region in one data folder whose graph holds the points: dir = its folder; false = none there.
static bool RegionIn(const wchar_t* data, const int* pts, int n, wchar_t* dir)
{
    wchar_t mask[MAX_PATH];
    _snwprintf(mask, MAX_PATH, L"%s\\*", data);
    WIN32_FIND_DATA fd;
    HANDLE ff = FindFirstFile(mask, &fd);
    if (ff == INVALID_HANDLE_VALUE)
        return false;
    bool found = Holds(data, fd, pts, n, dir);
    while (!found && FindNextFile(ff, &fd))
        found = Holds(data, fd, pts, n, dir);
    FindClose(ff);
    return found;
}

// The folder of the region whose graph holds all the points (lat, lon pairs, 1e-6 degrees): dir = its folder, with
// route.grf (the graph), search.idx (offsearch.cpp); false = none.
bool OfflineDir(const int* pts, int n, wchar_t* dir)
{
    wchar_t dirs[4][MAX_PATH];
    int nd = DataDirs(dirs);
    for (int d = 0; d < nd; d++)
        if (RegionIn(dirs[d], pts, n, dir))
            return true;
    return false;
}

// The region whose graph holds all the points: loaded; false = none.
static bool Region(const int* pts, int n)
{
    if (s_g.nodes && HoldsAll(s_g.h, pts, n))
        return true;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!OfflineDir(pts, n, dir))
        return false;
    _snwprintf(path, MAX_PATH, L"%s\\route.grf", dir);
    path[MAX_PATH - 1] = 0;
    return Load(path);
}

// ---------------------------------------------------------------- geometry

static double Bearing(int lat0, int lon0, int lat1, int lon1);

static DWORD FromNode(DWORD e)   // the node an edge leaves (binary search in first[])
{
    DWORD lo = 0, hi = s_g.h.nodes;
    while (hi - lo > 1) {
        DWORD m = (lo + hi) / 2;
        if (s_g.first[m] <= e)
            lo = m;
        else
            hi = m;
    }
    return lo;
}

// An edge's points (lat, lon pairs) from its start node to its end node; returns the count (<= cap).
static int EdgePts(DWORD from, DWORD e, int* out, int cap)
{
    const GEdge& g = s_g.edges[e];
    int n = 0;
    out[n * 2] = s_g.nodes[from * 2];
    out[n * 2 + 1] = s_g.nodes[from * 2 + 1];
    n++;
    int inner = g.ngeom;
    if (inner > cap - 2)
        inner = cap - 2;
    if (inner > 0 && OffRead(s_g.d.file, s_g.h.off[3] + g.geom * 8, out + 2, inner * 8)) {
        if (g.flags & 1)   // backwards
            for (int i = 0; i < inner / 2; i++)
                for (int k = 0; k < 2; k++) {
                    int t = out[2 + i * 2 + k];
                    out[2 + i * 2 + k] = out[2 + (inner - 1 - i) * 2 + k];
                    out[2 + (inner - 1 - i) * 2 + k] = t;
                }
        n += inner;
    }
    out[n * 2] = s_g.nodes[g.to * 2];
    out[n * 2 + 1] = s_g.nodes[g.to * 2 + 1];
    return n + 1;
}

// The bearing an edge leaves its start node with (its first inner point, or its end).
static double FirstBearing(DWORD from, DWORD e)
{
    const GEdge& g = s_g.edges[e];
    int p[2] = { s_g.nodes[g.to * 2], s_g.nodes[g.to * 2 + 1] };
    if (g.ngeom) {
        DWORD at = g.geom + ((g.flags & 1) ? g.ngeom - 1 : 0);
        if (!OffRead(s_g.d.file, s_g.h.off[3] + at * 8, p, 8)) {
            p[0] = s_g.nodes[g.to * 2];
            p[1] = s_g.nodes[g.to * 2 + 1];
        }
    }
    return Bearing(s_g.nodes[from * 2], s_g.nodes[from * 2 + 1], p[0], p[1]);
}

// The same road the other way (to -> from, the same geometry); -1 = one-way.
static int Twin(DWORD from, DWORD e)
{
    const GEdge& g = s_g.edges[e];
    for (DWORD k = s_g.first[g.to]; k < s_g.first[g.to + 1]; k++) {
        const GEdge& t = s_g.edges[k];
        if (t.to == from && t.geom == g.geom && t.ngeom == g.ngeom && ((t.flags ^ g.flags) & 1))
            return (int)k;
    }
    return -1;
}

static double s_kx;   // metres per 1e-6 degree of longitude here (latitude: 0.111195)

static double Bearing(int lat0, int lon0, int lat1, int lon1)   // degrees, 0 = north, clockwise
{
    double dx = (lon1 - lon0) * s_kx, dy = (lat1 - lat0) * 0.111195;
    double b = atan2(dx, dy) * 180 / M_PI;
    return b < 0 ? b + 360 : b;
}

// ---------------------------------------------------------------- snapping a point to a road

struct Snap {
    DWORD e, from;      // the edge (of the road, any direction) and its start node
    int   seg;          // the projection lies on its segment seg (points seg .. seg + 1)
    int   lat, lon;     // the projection
    double t;           // its share of the edge's length from the start
    double distM;
};

static const int kPtsCap = 4096;
static int s_pts[kPtsCap * 2];

static double SegM(const int* p)   // a segment's metres (p: its two points' lat, lon)
{
    double ax = p[1] * s_kx, ay = p[0] * 0.111195;
    double bx = p[3] * s_kx, by = p[2] * 0.111195;
    return sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
}

// The point's projection onto a segment (p: its two points): u = its share of the segment; returns the distance, m.
static double Project(int lat, int lon, const int* p, double* u)
{
    double ax = p[1] * s_kx, ay = p[0] * 0.111195;
    double bx = p[3] * s_kx, by = p[2] * 0.111195;
    double px = lon * s_kx, py = lat * 0.111195;
    double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    double t = l2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double qx = ax + vx * t, qy = ay + vy * t;
    *u = t;
    return sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

// The point onto an edge's nearest segment: best replaced when nearer.
static void SnapEdge(int lat, int lon, DWORD e, Snap* best)
{
    DWORD from = FromNode(e);
    int n = EdgePts(from, e, s_pts, kPtsCap);
    double len = 0, total = 0;
    for (int k = 0; k + 1 < n; k++)
        total += SegM(s_pts + k * 2);
    for (int k = 0; k + 1 < n; k++) {
        const int* p = s_pts + k * 2;
        double u, d = Project(lat, lon, p, &u), sl = SegM(p);
        if (d < best->distM) {
            best->distM = d;
            best->e = e;
            best->from = from;
            best->seg = k;
            best->lat = (int)(p[0] + (p[2] - p[0]) * u);
            best->lon = (int)(p[1] + (p[3] - p[1]) * u);
            best->t = total > 0 ? (len + sl * u) / total : 0;
        }
        len += sl;
    }
}

static void SnapCell(int lat, int lon, int x, int y, Snap* best)   // the grid cell's edges (outside the grid: none)
{
    const GHead& h = s_g.h;
    if (x < 0 || y < 0 || x >= (int)h.gw || y >= (int)h.gh)
        return;
    DWORD c = y * h.gw + x;
    for (DWORD i = s_g.cstart[c]; i < s_g.cstart[c + 1]; i++)
        SnapEdge(lat, lon, s_g.items[i], best);
}

static bool SnapTo(int lat, int lon, Snap* best)
{
    const GHead& h = s_g.h;
    best->distM = 1e18;
    int cy = (lat - h.minLat) / h.cell, cx = (lon - h.minLon) / h.cell;
    for (int r = 1; r <= 4 && best->distM > 1e17; r += 3)   // 3 x 3 cells, else 9 x 9
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++)
                SnapCell(lat, lon, x, y, best);
    return best->distM < 1e17;
}

// The snap seen from the other direction of its road (twin edge): false = one-way.
static bool Flip(const Snap& s, Snap* o)
{
    int tw = Twin(s.from, s.e);
    if (tw < 0)
        return false;
    *o = s;
    o->e = (DWORD)tw;
    o->from = s_g.edges[s.e].to;
    o->seg = s_g.edges[s.e].ngeom - s.seg;   // (points: ngeom + 2, segments: ngeom + 1)
    o->t = 1 - s.t;
    return true;
}

// ---------------------------------------------------------------- A*

static DWORD* s_dist;
static DWORD* s_prevE;    // the edge that reached the node; 0xFFFFFFFF = from the start
static DWORD* s_heapK;
static DWORD* s_heapN;
static DWORD  s_heapCap, s_heapN0;
static int    s_tLat, s_tLon, s_kxInt;   // the target, metres per 1000 microdegrees of longitude

static DWORD Est(DWORD n)   // tenths of a second to the target at kMaxV, never more than the truth
{
    int dy = abs(s_g.nodes[n * 2] - s_tLat) / 9, dx = (int)((__int64)abs(s_g.nodes[n * 2 + 1] - s_tLon) * s_kxInt / 1000);
    int a = dx > dy ? dx : dy, b = dx > dy ? dy : dx;
    int m = a + b * 41 / 100;      // the octagon's metres: at most 8 % over
    return (DWORD)((__int64)m * 92 / 100 * 36 / kMaxV);
}

static bool Push(DWORD key, DWORD n)
{
    if (s_heapN0 == s_heapCap) {
        DWORD c = s_heapCap ? s_heapCap * 2 : 65536;
        DWORD* k = (DWORD*)realloc(s_heapK, c * 4);
        if (!k)
            return false;
        s_heapK = k;
        DWORD* nn = (DWORD*)realloc(s_heapN, c * 4);
        if (!nn)
            return false;
        s_heapN = nn;
        s_heapCap = c;
    }
    DWORD i = s_heapN0++;
    while (i > 0) {
        DWORD p = (i - 1) / 2;
        if (s_heapK[p] <= key)
            break;
        s_heapK[i] = s_heapK[p];
        s_heapN[i] = s_heapN[p];
        i = p;
    }
    s_heapK[i] = key;
    s_heapN[i] = n;
    return true;
}

static void Pop(DWORD* key, DWORD* n)
{
    *key = s_heapK[0];
    *n = s_heapN[0];
    DWORD k = s_heapK[--s_heapN0], v = s_heapN[s_heapN0], i = 0;
    for (;;) {
        DWORD c = i * 2 + 1;
        if (c >= s_heapN0)
            break;
        if (c + 1 < s_heapN0 && s_heapK[c + 1] < s_heapK[c])
            c++;
        if (s_heapK[c] >= k)
            break;
        s_heapK[i] = s_heapK[c];
        s_heapN[i] = s_heapN[c];
        i = c;
    }
    s_heapK[i] = k;
    s_heapN[i] = v;
}

struct Way {             // one leg's way: the start's edge (part), the whole edges, the end's edge (part)
    Snap   a, b;         // a: on its edge from a.t to the end; b: from the start to b.t
    DWORD* e;            // the whole edges between
    DWORD* from;
    int    n;
    bool   same;         // a and b on the same edge, a before b: only that part
    DWORD  timeDs;
};

static DWORD Part(const GEdge& g, double share)
{
    return (DWORD)(g.timeDs * share + 0.5);
}

struct Fastest {          // one search: its ends (each road side allowed), the best so far
    Snap  starts[2], ends[2];
    int   ns, ne;
    DWORD best;          // tenths of a second
    int   bestEnd;       // ends[] reached through the graph; -2 the start's own edge, -1 none yet
};

static int Sides(const Snap& s, bool both, Snap* out)   // the snap, and its other road side when allowed
{
    out[0] = s;
    return both && Flip(s, &out[1]) ? 2 : 1;
}

static bool Prepare(const Snap& target)   // the search's arrays (made once: one region at a time), the estimate's target
{
    DWORD N = s_g.h.nodes;
    if (!s_dist) {
        s_dist = (DWORD*)malloc(N * 4);
        s_prevE = (DWORD*)malloc(N * 4);
    }
    if (!s_dist || !s_prevE)
        return false;
    memset(s_dist, 0xFF, N * 4);
    s_heapN0 = 0;
    s_tLat = target.lat;
    s_tLon = target.lon;
    s_kxInt = (int)(s_kx * 1000);
    return true;
}

// Start i: an end ahead on its own edge (no search needed), and the edge's end node into the heap.
static void Seed(Fastest& f, int i, Way* w)
{
    const Snap& s = f.starts[i];
    const GEdge& g = s_g.edges[s.e];
    for (int j = 0; j < f.ne; j++) {
        if (f.ends[j].e != s.e || f.ends[j].t < s.t)
            continue;
        DWORD c = Part(g, f.ends[j].t - s.t);
        if (c < f.best) {
            f.best = c;
            w->a = s;
            w->b = f.ends[j];
            w->same = true;
            f.bestEnd = -2;
        }
    }
    DWORD c = Part(g, 1 - s.t);
    if (c < s_dist[g.to]) {
        s_dist[g.to] = c;
        s_prevE[g.to] = 0x80000000 | i;   // reached from start i
        Push(c + Est(g.to), g.to);
    }
}

static void ReachEnds(Fastest& f, DWORD n, DWORD d, Way* w)   // node n settled at d: an end's edge leaves it?
{
    for (int j = 0; j < f.ne; j++) {
        if (f.ends[j].from != n)
            continue;
        DWORD c = d + Part(s_g.edges[f.ends[j].e], f.ends[j].t);
        if (c < f.best) {
            f.best = c;
            f.bestEnd = j;
            w->same = false;
        }
    }
}

static bool Relax(DWORD n, DWORD d)   // node n's edges; false = no memory
{
    for (DWORD e = s_g.first[n]; e < s_g.first[n + 1]; e++) {
        const GEdge& g = s_g.edges[e];
        DWORD c = d + g.timeDs;
        if (c < s_dist[g.to]) {
            s_dist[g.to] = c;
            s_prevE[g.to] = e;
            if (!Push(c + Est(g.to), g.to))
                return false;
        }
    }
    return true;
}

static bool Settle(Fastest& f, Way* w)   // A* until nothing in the heap can beat the best; false = no memory
{
    DWORD pops = 0;
    while (s_heapN0) {
        DWORD k, n;
        Pop(&k, &n);
        if (k >= f.best)
            break;
        DWORD d = s_dist[n];
        if (k != d + Est(n))
            continue;   // a stale entry
        pops++;
        ReachEnds(f, n, d, w);
        if (!Relax(n, d))
            return false;
    }
    Log("offline: search: %lu nodes settled, %lu.%lu s", pops, f.best / 10, f.best % 10);
    return true;
}

static bool Reconstruct(const Fastest& f, Way* w)   // back from the end's node to a start: the whole edges between
{
    w->b = f.ends[f.bestEnd];
    int cnt = 0;
    DWORD n = w->b.from;
    while (!(s_prevE[n] & 0x80000000)) {
        n = FromNode(s_prevE[n]);
        cnt++;
    }
    w->a = f.starts[s_prevE[n] & 0x7FFFFFFF];
    int cap = cnt ? cnt : 1;
    w->e = (DWORD*)malloc(cap * 4);
    w->from = (DWORD*)malloc(cap * 4);
    if (!w->e || !w->from)
        return false;
    w->n = cnt;
    n = w->b.from;
    for (int i = cnt - 1; i >= 0; i--) {
        DWORD e = s_prevE[n];
        w->e[i] = e;
        w->from[i] = FromNode(e);
        n = w->from[i];
    }
    return true;
}

// The fastest way between two snaps; anyStart: the start's other road side allowed too.
static bool Search(const Snap& sa, const Snap& sb, bool anyStart, Way* w)
{
    Fastest f;
    f.ns = Sides(sa, anyStart, f.starts);
    f.ne = Sides(sb, true, f.ends);
    if (!Prepare(sb))
        return false;
    f.best = 0xFFFFFFFF;
    f.bestEnd = -1;
    memset(w, 0, sizeof(*w));
    for (int i = 0; i < f.ns; i++)
        Seed(f, i, w);
    if (!Settle(f, w) || f.bestEnd == -1)
        return false;
    w->timeDs = f.best;
    return f.bestEnd == -2 || Reconstruct(f, w);
}

// ---------------------------------------------------------------- the answer (OSRM's JSON)

static void PutName(JBuf& b, DWORD name)   // "name":..,"ref":..
{
    const char* s = s_g.names + name;
    const char* tab = strchr(s, '\t');
    int nl = tab ? (int)(tab - s) : (int)strlen(s);
    JPut(b, "\"name\":");
    JPutStr(b, s, nl);
    JPut(b, ",\"ref\":");
    const char* r = tab ? tab + 1 : "";
    JPutStr(b, r, (int)strlen(r));
}

// Google's polyline, one value, inside a JSON string: its characters (63..126) include the backslash, written "\\"
// as OSRM writes it (route.cpp's JSON reader takes a lone one for an escape)
static void PolyNum(JBuf& b, int v)
{
    unsigned int u = v < 0 ? ~((unsigned int)v << 1) : (unsigned int)v << 1;
    char t[16];
    int n = 0;
    for (;;) {
        char c = (char)((u >= 0x20 ? 0x20 | (u & 0x1f) : u) + 63);
        if (c == '\\')
            t[n++] = '\\';
        t[n++] = c;
        if (u < 0x20)
            break;
        u >>= 5;
    }
    JPut(b, t, n);
}

// The way's line (lat, lon pairs): growing array
struct Line { int* p; int n, cap; };

static void LineAdd(Line& l, int lat, int lon)
{
    if (l.n && l.p[l.n * 2 - 2] == lat && l.p[l.n * 2 - 1] == lon)
        return;
    if (l.n == l.cap) {
        int c = l.cap ? l.cap * 2 : 1024;
        int* p = (int*)realloc(l.p, c * 8);
        if (!p)
            return;
        l.p = p;
        l.cap = c;
    }
    l.p[l.n * 2] = lat;
    l.p[l.n * 2 + 1] = lon;
    l.n++;
}

// A whole or partial edge into the line; its first and last segment's bearings.
static void AddEdge(Line& l, DWORD from, DWORD e, const Snap* a, const Snap* b, double* b0, double* b1)
{
    int n = EdgePts(from, e, s_pts, kPtsCap);
    int k0 = 0, k1 = n - 1;   // the points used
    if (a) {
        LineAdd(l, a->lat, a->lon);
        k0 = a->seg + 1;
    }
    if (b)
        k1 = b->seg;
    for (int k = k0; k <= k1; k++)
        LineAdd(l, s_pts[k * 2], s_pts[k * 2 + 1]);
    if (b)
        LineAdd(l, b->lat, b->lon);
    *b0 = Bearing(s_pts[0], s_pts[1], s_pts[2], s_pts[3]);
    *b1 = Bearing(s_pts[(n - 2) * 2], s_pts[(n - 2) * 2 + 1], s_pts[(n - 1) * 2], s_pts[(n - 1) * 2 + 1]);
}

static const char* Modifier(double turn)   // right = positive
{
    double a = fabs(turn);
    if (a < 25)  return "straight";
    if (a > 165) return "uturn";
    if (turn > 0)
        return a < 60 ? "slight right" : a < 135 ? "right" : "sharp right";
    return a < 60 ? "slight left" : a < 135 ? "left" : "sharp left";
}

static double TurnOf(double before, double after)
{
    double t = after - before;
    while (t > 180) t -= 360;
    while (t < -180) t += 360;
    return t;
}

static void Step(JBuf& b, bool& firstStep, const char* type, const char* mod, double before, double after, int lat,
                 int lon, DWORD name, int exit)
{
    if (!firstStep)
        JPut(b, ",");
    firstStep = false;
    JPut(b, "{");
    PutName(b, name);
    JPutf(b, ",\"maneuver\":{\"type\":\"%s\",\"modifier\":\"%s\",\"bearing_before\":%d,\"bearing_after\":%d,"
            "\"location\":[%.6f,%.6f]", type, mod, (int)before, (int)after, lon / 1e6, lat / 1e6);
    if (exit > 0)
        JPutf(b, ",\"exit\":%d", exit);
    JPut(b, "}}");
}

// A road class's rank for the instructions (make_graph.py's kClasses): 0 motorway, trunk .. 5 service, track; the
// links as their roads.
static int Level(int cls)
{
    static const BYTE kLevel[16] = { 0, 0, 1, 2, 3, 4, 4, 5, 5, 0, 0, 1, 2, 3, 4, 5 };
    return cls < 16 ? kLevel[cls] : 5;
}

static DWORD EdgeAt(const Way& w, int i)   // the way's i-th edge (0: the start's, then the whole ones, the end's)
{
    if (w.same || i == 0)
        return w.a.e;
    return i - 1 < w.n ? w.e[i - 1] : w.b.e;
}

struct Piece {           // the way's edge with its start node, cut at the start's snap (a) / the end's (z) if it holds them
    DWORD       e, from;
    const Snap* a;
    const Snap* z;
};

static Piece PieceAt(const Way& w, int i, int cnt)
{
    Piece pc = { EdgeAt(w, i), 0, i == 0 ? &w.a : NULL, i == cnt - 1 ? &w.b : NULL };
    pc.from = pc.a ? w.a.from : pc.z ? w.b.from : w.from[i - 1];
    return pc;
}

struct Prev { DWORD e, from; double after; };   // the edge the way came by: its start node, its last bearing

static bool Named(const GEdge& g)
{
    return strcmp(s_g.names + g.name, "\t") != 0;
}

static bool BackWay(const GEdge& o, const Prev& prev)   // o: the edge we came by, the other way
{
    const GEdge& p = s_g.edges[prev.e];
    return o.to == prev.from && o.geom == p.geom && o.ngeom == p.ngeom;
}

struct Turn {            // a node between two of the way's edges
    const GEdge* p;      // the edge before
    const GEdge* g;      // the edge after
    double turn;         // degrees, right = positive
    int    alts;         // the other ways out (not back the way we came)
    int    rivals;       // of them: not of a lower class than ours
    double rivalMin;     // the straightest rival's turn (its size)
    double rivalTurn;
};

// The other ways out of the node (from, the way going on by e) and the rivals among them.
static void Rivals(DWORD from, DWORD e, const Prev& prev, Turn* t)
{
    t->alts = t->rivals = 0;
    t->rivalMin = 1e9;
    t->rivalTurn = 0;
    for (DWORD k = s_g.first[from]; k < s_g.first[from + 1]; k++) {
        const GEdge& o = s_g.edges[k];
        if (k == e || BackWay(o, prev))
            continue;
        t->alts++;
        if (Level(o.cls) > Level(t->g->cls))
            continue;
        double ot = TurnOf(prev.after, FirstBearing(from, k));
        t->rivals++;
        if (fabs(ot) < t->rivalMin) {
            t->rivalMin = fabs(ot);
            t->rivalTurn = ot;
        }
    }
}

static bool HasWayOff(DWORD node)   // a roundabout's node with a road off it
{
    for (DWORD k = s_g.first[node]; k < s_g.first[node + 1]; k++)
        if (!(s_g.edges[k].flags & 2))
            return true;
    return false;
}

// Onto a roundabout by the way's edge i: the exit's number (its nodes with a way off it, up to ours); name = the road
// off it (kept when the way ends on the roundabout).
static int RoundaboutExit(const Way& w, int i, int cnt, DWORD* name)
{
    int exitNo = 0;
    for (int j = i; j < cnt; j++) {
        const GEdge& g = s_g.edges[EdgeAt(w, j)];
        if (!(g.flags & 2)) {   // off it: this road names the step
            *name = g.name;
            break;
        }
        if (j == cnt - 1)
            break;   // the way ends on the roundabout
        exitNo += HasWayOff(g.to);
    }
    return exitNo;
}

static const char* RampKind(const Turn& t, const char** mod)   // onto a link: off a big road, else onto one
{
    if (!(t.g->flags & 4) || (t.p->flags & 4))
        return NULL;
    *mod = Modifier(t.turn);
    return t.p->cls <= C_TRUNK ? "off ramp" : "on ramp";
}

static const char* MergeKind(const Turn& t, const char** mod)   // off a link onto a big road, nearly straight
{
    if ((t.g->flags & 4) || !(t.p->flags & 4) || t.g->cls > C_TRUNK || fabs(t.turn) >= 45)
        return NULL;
    *mod = t.turn > 0 ? "slight right" : "slight left";
    return "merge";
}

// The road just goes on: no other way, or ours the obvious one (the straightest of the rivals and ours, 10 degrees'
// slack) and the same road or nearly straight.
static bool GoesOn(const Turn& t)
{
    bool sameRoad = t.g->name == t.p->name && Named(*t.g);
    bool obvious = fabs(t.turn) <= t.rivalMin + 10;
    return !t.alts || (obvious && (sameRoad || fabs(t.turn) < 35));
}

static const char* PlainKind(const Turn& t, const char** mod)   // not at a link's end
{
    if (GoesOn(t)) {   // told only when its name changes
        *mod = "straight";
        return t.alts && t.g->name != t.p->name && Named(*t.g) ? "new name" : NULL;
    }
    if (fabs(t.turn) < 35 && t.rivals) {   // two ways ahead: keep to our side of the other
        *mod = t.turn >= t.rivalTurn ? "slight right" : "slight left";
        return "fork";
    }
    *mod = Modifier(t.turn);
    return "turn";
}

// The maneuver at a node (not onto a roundabout): its type and modifier; NULL = nothing to tell.
static const char* Kind(const Turn& t, const char** mod)
{
    if (t.p->flags & 2)
        return NULL;   // on the roundabout, or off it: told at the entry
    const char* k = RampKind(t, mod);
    if (!k)
        k = MergeKind(t, mod);
    return k ? k : PlainKind(t, mod);
}

// The step at the node where the way's edge i (pc) begins, coming by prev; b0: the edge's first bearing.
static void Maneuver(JBuf& b, bool& first, const Way& w, int i, int cnt, const Piece& pc, const Prev& prev, double b0)
{
    Turn t;
    t.p = &s_g.edges[prev.e];
    t.g = &s_g.edges[pc.e];
    t.turn = TurnOf(prev.after, b0);
    Rivals(pc.from, pc.e, prev, &t);
    int la = s_g.nodes[pc.from * 2], lo = s_g.nodes[pc.from * 2 + 1];
    if ((t.g->flags & 2) && !(t.p->flags & 2)) {   // onto a roundabout
        DWORD name = t.g->name;
        int exitNo = RoundaboutExit(w, i, cnt, &name);
        Step(b, first, "roundabout", "right", prev.after, b0, la, lo, name, exitNo);
        return;
    }
    const char* mod;
    const char* type = Kind(t, &mod);
    if (type)
        Step(b, first, type, mod, prev.after, b0, la, lo, t.g->name, 0);
}

// One leg's steps: depart, the maneuvers at the nodes between its edges, arrive. The line grows.
static void Steps(JBuf& b, Line& line, const Way& w)
{
    bool first = true;
    int cnt = w.same ? 1 : w.n + 2;
    double b0, b1;
    Prev prev = { 0, 0, 0 };
    for (int i = 0; i < cnt; i++) {
        Piece pc = PieceAt(w, i, cnt);
        int before = line.n;
        AddEdge(line, pc.from, pc.e, pc.a, pc.z, &b0, &b1);
        if (i == 0)
            Step(b, first, "depart", "straight", 0, b0, line.p[before * 2], line.p[before * 2 + 1],
                 s_g.edges[pc.e].name, 0);
        else
            Maneuver(b, first, w, i, cnt, pc, prev, b0);
        prev.e = pc.e;
        prev.from = pc.from;
        prev.after = b1;
    }
    Step(b, first, "arrive", "straight", prev.after, 0, line.p[(line.n - 1) * 2], line.p[(line.n - 1) * 2 + 1],
         s_g.edges[prev.e].name, 0);
}

static void FreeWay(Way& w)
{
    free(w.e);
    free(w.from);
    w.e = w.from = NULL;
}

// The name of the road nearest to a point within maxM (the region's graph, as a route's snapping): false = no
// region, no road that near, or a road without a name.
bool OfflineRoadAt(int lat, int lon, double maxM, char* name, int cap)
{
    int pt[2] = { lat, lon };
    if (!Region(pt, 1))
        return false;
    s_kx = 0.111195 * cos(lat / 1e6 * M_PI / 180);
    Snap s;
    if (!SnapTo(lat, lon, &s) || s.distM > maxM)
        return false;
    const char* n = s_g.names + s_g.edges[s.e].name;   // "name\tref"
    int len = (int)(strchr(n, '\t') ? strchr(n, '\t') - n : strlen(n));
    if (!len || len >= cap)
        return false;
    memcpy(name, n, len);
    name[len] = 0;
    return true;
}

// ---------------------------------------------------------------- the request

struct Request {         // OSRM's route request, checked once
    int    pts[2 * (kMaxVia + 2)];   // the start, the stops, the end: lat, lon pairs (1e-6 degrees)
    int    n;
    double course;       // the car's course at the start, degrees; -1 none
};

// "lon,lat" at p into pt (lat, lon); returns the next point's text (past its ';'); NULL = no comma.
static const char* ParsePoint(const char* p, int* pt)
{
    double lon = atof(p);
    const char* c = strchr(p, ',');
    if (!c)
        return NULL;
    double lat = atof(c + 1);
    pt[0] = (int)(lat * 1e6);
    pt[1] = (int)(lon * 1e6);
    p = c + 1;
    while (*p && *p != ';' && *p != '?')
        p++;
    return *p == ';' ? p + 1 : p;
}

static double Course(const char* path)   // "bearings=deg,60;..": the first one; -1 none ("bearings=,;")
{
    const char* br = strstr(path, "bearings=");
    return br && br[9] != ',' && br[9] != ';' ? atof(br + 9) : -1;
}

// "/routed-car/route/v1/driving/lon,lat;lon,lat...?...&bearings=deg,60;..." into r; false = not a route of 2+ points.
static bool ParseRequest(const char* path, Request* r)
{
    const char* p = strstr(path, "/driving/");
    if (!p)
        return false;
    p += 9;
    r->n = 0;
    while (r->n < kMaxVia + 2 && *p && *p != '?') {
        p = ParsePoint(p, r->pts + r->n * 2);
        if (!p)
            return false;
        r->n++;
    }
    r->course = Course(path);
    return r->n >= 2;
}

// ---------------------------------------------------------------- a route

static bool SnapLeg(const Request& r, int leg, Snap* sa, Snap* sb)   // the leg's ends onto roads within 2 km
{
    const int* p = r.pts + leg * 2;
    if (SnapTo(p[0], p[1], sa) && SnapTo(p[2], p[3], sb) && sa->distM <= 2000 && sb->distM <= 2000)
        return true;
    Log("offline: no road near point %d", sa->distM > 2000 ? leg : leg + 1);
    return false;
}

// The start the way the car goes (within 90 degrees of its course): returns false = only that side; true = no
// course, either side.
static bool AnyStart(Snap* s, double course)
{
    if (course < 0)
        return true;
    int np = EdgePts(s->from, s->e, s_pts, kPtsCap);
    int k = s->seg < np - 1 ? s->seg : np - 2;
    double eb = Bearing(s_pts[k * 2], s_pts[k * 2 + 1], s_pts[k * 2 + 2], s_pts[k * 2 + 3]);
    Snap f;
    if (fabs(TurnOf(course, eb)) > 90 && Flip(*s, &f))
        *s = f;
    return false;
}

static double LineM(const Line& line, int from)   // the line's metres from its point from on
{
    double m = 0;
    for (int i = from + 1; i < line.n; i++) {
        double dx = (line.p[i * 2 + 1] - line.p[i * 2 - 1]) * s_kx, dy = (line.p[i * 2] - line.p[i * 2 - 2]) * 0.111195;
        m += sqrt(dx * dx + dy * dy);
    }
    return m;
}

// One leg: found, its steps written, the line grown, its time and length added; false = no road near, no way.
static bool RouteLeg(const Request& r, int leg, JBuf& b, Line& line, DWORD* totalDs, double* totalM)
{
    Snap sa, sb;
    if (!SnapLeg(r, leg, &sa, &sb))
        return false;
    bool any = leg ? true : AnyStart(&sa, r.course);
    Way w;
    memset(&w, 0, sizeof(w));
    if (!Search(sa, sb, any, &w) && (any || !Search(sa, sb, true, &w))) {   // against the course: either side
        FreeWay(w);
        return false;
    }
    if (leg)
        JPut(b, ",");
    JPut(b, "{\"steps\":[");
    int lineFrom = line.n;
    Steps(b, line, w);
    JPut(b, "]}");
    *totalM += LineM(line, lineFrom);
    *totalDs += w.timeDs;
    FreeWay(w);
    return true;
}

static bool RouteLegs(const Request& r, JBuf& b, Line& line, DWORD* totalDs, double* totalM)
{
    bool ok = true;
    for (int leg = 0; leg + 1 < r.n && ok; leg++)
        ok = RouteLeg(r, leg, b, line, totalDs, totalM);
    return ok;
}

static void PutGeometry(JBuf& b, const Line& line)   // the line as polyline6
{
    int plat = 0, plon = 0;
    for (int i = 0; i < line.n; i++) {
        PolyNum(b, line.p[i * 2] - plat);
        PolyNum(b, line.p[i * 2 + 1] - plon);
        plat = line.p[i * 2];
        plon = line.p[i * 2 + 1];
    }
}

// "/routed-car/route/v1/driving/lon,lat;lon,lat...?...&bearings=deg,60;..." -> an OSRM answer, or false (no region
// holds the points, no road near, no way): then OSRM is asked.
bool OfflineRoute(const char* path, char** body, int* len)
{
    Request r;
    if (!ParseRequest(path, &r))
        return false;
    DWORD t0 = GetTickCount();
    if (!Region(r.pts, r.n))
        return false;
    s_kx = 0.111195 * cos(r.pts[0] / 1e6 * M_PI / 180);
    JBuf b = { NULL, 0, 0 };
    Line line = { NULL, 0, 0 };
    JPut(b, "{\"code\":\"Ok\",\"routes\":[{\"legs\":[");
    DWORD totalDs = 0;
    double totalM = 0;
    bool ok = RouteLegs(r, b, line, &totalDs, &totalM);
    if (ok && line.n >= 2) {
        JPutf(b, "],\"duration\":%.1f,\"distance\":%.1f,\"geometry\":\"", totalDs / 10.0, totalM);
        PutGeometry(b, line);
        JPut(b, "\"}]}");
    }
    free(line.p);
    if (!ok || !b.p) {
        free(b.p);
        Log("offline: no route (%lu ms)", GetTickCount() - t0);
        return false;
    }
    Log("offline: route %d points, %.1f km, %lu min, %lu ms", line.n, totalM / 1000, totalDs / 600, GetTickCount() - t0);
    *body = b.p;
    *len = b.n;
    return true;
}
