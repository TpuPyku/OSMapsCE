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
    HANDLE  file;      // the geometry is read from it
};

static Graph s_g;      // the region loaded (one at a time)

// ---------------------------------------------------------------- the data

static const wchar_t* kDataDirs[] = { L"\\SDMemory\\OSMapsData", L"\\USBDevice\\OSMapsData" };

static void Unload()
{
    free(s_g.nodes);
    free(s_g.first);
    free(s_g.edges);
    free(s_g.names);
    free(s_g.cstart);
    free(s_g.items);
    if (s_g.file && s_g.file != INVALID_HANDLE_VALUE)
        CloseHandle(s_g.file);
    memset(&s_g, 0, sizeof(s_g));
}

static void* ReadAt(HANDLE f, DWORD off, DWORD bytes)
{
    void* p = malloc(bytes ? bytes : 1);
    DWORD got = 0;
    if (!p || SetFilePointer(f, off, NULL, FILE_BEGIN) == 0xFFFFFFFF || !ReadFile(f, p, bytes, &got, NULL) ||
        got != bytes) {
        free(p);
        return NULL;
    }
    return p;
}

static bool ReadHead(const wchar_t* path, GHead* h)
{
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD got = 0;
    bool ok = ReadFile(f, h, sizeof(*h), &got, NULL) && got == sizeof(*h) && !memcmp(h->magic, "OSMG", 4) &&
              h->version == 1;
    CloseHandle(f);
    return ok;
}

static bool Load(const wchar_t* path)
{
    if (s_g.nodes && !wcscmp(s_g.path, path))
        return true;
    Unload();
    DWORD t0 = GetTickCount();
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    s_g.file = f;
    GHead& h = s_g.h;
    DWORD got = 0;
    if (!ReadFile(f, &h, sizeof(h), &got, NULL) || got != sizeof(h)) {
        Unload();
        return false;
    }
    s_g.nodes = (int*)ReadAt(f, h.off[0], h.nodes * 8);
    s_g.first = (DWORD*)ReadAt(f, h.off[1], (h.nodes + 1) * 4);
    s_g.edges = (GEdge*)ReadAt(f, h.off[2], h.edges * sizeof(GEdge));
    s_g.names = (char*)ReadAt(f, h.off[4], h.nameBytes);
    s_g.cstart = (DWORD*)ReadAt(f, h.off[5], (h.gw * h.gh + 1) * 4);
    s_g.items = (DWORD*)ReadAt(f, h.off[5] + (h.gw * h.gh + 1) * 4, h.items * 4);
    if (!s_g.nodes || !s_g.first || !s_g.edges || !s_g.names || !s_g.cstart || !s_g.items) {
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

// The folder of the region whose graph holds all the points (lat, lon pairs, 1e-6 degrees): dir = its folder, with
// route.grf (the graph), search.idx (offsearch.cpp); false = none.
bool OfflineDir(const int* pts, int n, wchar_t* dir)
{
    wchar_t dirs[4][MAX_PATH];
    int nd = 0;
    for (int i = 0; i < 2; i++)
        wcscpy(dirs[nd++], kDataDirs[i]);
    PathInDir(dirs[nd++], L"OSMapsData");
#ifndef UNDER_CE
    PathInDir(dirs[nd++], L"..\\OSMapsData");   // the PC: the tool's output beside the program's folder
#endif
    for (int d = 0; d < nd; d++) {
        wchar_t mask[MAX_PATH];
        _snwprintf(mask, MAX_PATH, L"%s\\*", dirs[d]);
        WIN32_FIND_DATA fd;
        HANDLE ff = FindFirstFile(mask, &fd);
        if (ff == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            wchar_t path[MAX_PATH];
            _snwprintf(path, MAX_PATH, L"%s\\%s\\route.grf", dirs[d], fd.cFileName);
            path[MAX_PATH - 1] = 0;
            GHead h;
            if (!ReadHead(path, &h))
                continue;
            int i = 0;
            while (i < n && Inside(h, pts[i * 2], pts[i * 2 + 1]))
                i++;
            if (i == n) {
                FindClose(ff);
                _snwprintf(dir, MAX_PATH, L"%s\\%s", dirs[d], fd.cFileName);
                dir[MAX_PATH - 1] = 0;
                return true;
            }
        } while (FindNextFile(ff, &fd));
        FindClose(ff);
    }
    return false;
}

// The region whose graph holds all the points: loaded; false = none.
static bool Region(const int* pts, int n)
{
    if (s_g.nodes) {
        int i = 0;
        while (i < n && Inside(s_g.h, pts[i * 2], pts[i * 2 + 1]))
            i++;
        if (i == n)
            return true;
    }
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
    if (inner > 0) {
        DWORD got = 0;
        if (SetFilePointer(s_g.file, s_g.h.off[3] + g.geom * 8, NULL, FILE_BEGIN) != 0xFFFFFFFF &&
            ReadFile(s_g.file, out + 2, inner * 8, &got, NULL) && got == (DWORD)inner * 8) {
            if (g.flags & 1)   // backwards
                for (int i = 0; i < inner / 2; i++)
                    for (int k = 0; k < 2; k++) {
                        int t = out[2 + i * 2 + k];
                        out[2 + i * 2 + k] = out[2 + (inner - 1 - i) * 2 + k];
                        out[2 + (inner - 1 - i) * 2 + k] = t;
                    }
            n += inner;
        }
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
        DWORD got = 0, at = g.geom + ((g.flags & 1) ? g.ngeom - 1 : 0);
        if (SetFilePointer(s_g.file, s_g.h.off[3] + at * 8, NULL, FILE_BEGIN) == 0xFFFFFFFF ||
            !ReadFile(s_g.file, p, 8, &got, NULL) || got != 8) {
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

static bool SnapTo(int lat, int lon, Snap* best)
{
    const GHead& h = s_g.h;
    best->distM = 1e18;
    int cy = (lat - h.minLat) / h.cell, cx = (lon - h.minLon) / h.cell;
    for (int r = 1; r <= 4 && best->distM > 1e17; r += 3)   // 3 x 3 cells, else 9 x 9
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (x < 0 || y < 0 || x >= (int)h.gw || y >= (int)h.gh)
                    continue;
                DWORD c = y * h.gw + x;
                for (DWORD i = s_g.cstart[c]; i < s_g.cstart[c + 1]; i++) {
                    DWORD e = s_g.items[i], from = FromNode(e);
                    int n = EdgePts(from, e, s_pts, kPtsCap);
                    double len = 0, total = 0;
                    for (int k = 0; k + 1 < n; k++) {
                        double ax = s_pts[k * 2 + 1] * s_kx, ay = s_pts[k * 2] * 0.111195;
                        double bx = s_pts[k * 2 + 3] * s_kx, by = s_pts[k * 2 + 2] * 0.111195;
                        total += sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
                    }
                    for (int k = 0; k + 1 < n; k++) {
                        double ax = s_pts[k * 2 + 1] * s_kx, ay = s_pts[k * 2] * 0.111195;
                        double bx = s_pts[k * 2 + 3] * s_kx, by = s_pts[k * 2 + 2] * 0.111195;
                        double px = lon * s_kx, py = lat * 0.111195;
                        double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
                        double u = l2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0;
                        u = u < 0 ? 0 : u > 1 ? 1 : u;
                        double qx = ax + vx * u, qy = ay + vy * u;
                        double d = sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy)), sl = sqrt(l2);
                        if (d < best->distM) {
                            best->distM = d;
                            best->e = e;
                            best->from = from;
                            best->seg = k;
                            best->lat = (int)(s_pts[k * 2] + (s_pts[k * 2 + 2] - s_pts[k * 2]) * u);
                            best->lon = (int)(s_pts[k * 2 + 1] + (s_pts[k * 2 + 3] - s_pts[k * 2 + 1]) * u);
                            best->t = total > 0 ? (len + sl * u) / total : 0;
                        }
                        len += sl;
                    }
                }
            }
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

// The fastest way between two snaps; starts: the start's directions allowed (1 its own, 2 the other road side).
static bool Search(const Snap& sa, const Snap& sb, bool anyStart, Way* w)
{
    Snap starts[2], ends[2];
    int ns = 0, ne = 0;
    starts[ns++] = sa;
    if (anyStart && Flip(sa, &starts[ns]))
        ns++;
    ends[ne++] = sb;
    if (Flip(sb, &ends[ne]))
        ne++;
    DWORD N = s_g.h.nodes;
    if (!s_dist) {
        s_dist = (DWORD*)malloc(N * 4);
        s_prevE = (DWORD*)malloc(N * 4);
    }
    if (!s_dist || !s_prevE)
        return false;
    memset(s_dist, 0xFF, N * 4);
    s_heapN0 = 0;
    s_tLat = sb.lat;
    s_tLon = sb.lon;
    s_kxInt = (int)(s_kx * 1000);
    DWORD best = 0xFFFFFFFF;
    int bestEnd = -1;
    memset(w, 0, sizeof(*w));
    for (int i = 0; i < ns; i++) {
        const GEdge& g = s_g.edges[starts[i].e];
        for (int j = 0; j < ne; j++)   // the same edge, the end ahead: no search
            if (ends[j].e == starts[i].e && ends[j].t >= starts[i].t) {
                DWORD c = Part(g, ends[j].t - starts[i].t);
                if (c < best) {
                    best = c;
                    w->a = starts[i];
                    w->b = ends[j];
                    w->same = true;
                    bestEnd = -2;
                }
            }
        DWORD c = Part(g, 1 - starts[i].t);
        if (c < s_dist[g.to]) {
            s_dist[g.to] = c;
            s_prevE[g.to] = 0x80000000 | i;   // reached from start i
            Push(c + Est(g.to), g.to);
        }
    }
    DWORD pops = 0;
    while (s_heapN0) {
        DWORD k, n;
        Pop(&k, &n);
        if (k >= best)
            break;
        DWORD d = s_dist[n];
        if (k != d + Est(n))
            continue;   // a stale entry
        pops++;
        for (int j = 0; j < ne; j++)
            if (ends[j].from == n) {
                DWORD c = d + Part(s_g.edges[ends[j].e], ends[j].t);
                if (c < best) {
                    best = c;
                    bestEnd = j;
                    w->same = false;
                }
            }
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
    }
    Log("offline: search: %lu nodes settled, %lu.%lu s", pops, best / 10, best % 10);
    if (bestEnd == -1)
        return false;
    w->timeDs = best;
    if (bestEnd == -2)
        return true;
    w->b = ends[bestEnd];
    // back from the end's node to a start
    int cnt = 0;
    DWORD n = w->b.from;
    while (!(s_prevE[n] & 0x80000000)) {
        n = FromNode(s_prevE[n]);
        cnt++;
    }
    w->a = starts[s_prevE[n] & 0x7FFFFFFF];
    w->e = (DWORD*)malloc((cnt ? cnt : 1) * 4);
    w->from = (DWORD*)malloc((cnt ? cnt : 1) * 4);
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

// ---------------------------------------------------------------- the answer (OSRM's JSON)

struct Buf { char* p; int n, cap; };

static void Put(Buf& b, const char* s, int n = -1)
{
    if (n < 0)
        n = (int)strlen(s);
    if (b.n + n + 1 > b.cap) {
        int c = (b.cap ? b.cap * 2 : 65536);
        while (c < b.n + n + 1)
            c *= 2;
        char* p = (char*)realloc(b.p, c);
        if (!p)
            return;
        b.p = p;
        b.cap = c;
    }
    memcpy(b.p + b.n, s, n);
    b.n += n;
    b.p[b.n] = 0;
}

static void Putf(Buf& b, const char* fmt, ...)
{
    char t[256];
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf(t, sizeof(t) - 1, fmt, a);
    va_end(a);
    t[sizeof(t) - 1] = 0;
    Put(b, t, n < 0 ? (int)strlen(t) : n);
}

static void PutStr(Buf& b, const char* s, int n)   // a JSON string
{
    Put(b, "\"");
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', c };
            Put(b, e, 2);
        } else if ((unsigned char)c >= 0x20)
            Put(b, &c, 1);
    }
    Put(b, "\"");
}

static void PutName(Buf& b, DWORD name)   // "name":..,"ref":..
{
    const char* s = s_g.names + name;
    const char* tab = strchr(s, '\t');
    int nl = tab ? (int)(tab - s) : (int)strlen(s);
    Put(b, "\"name\":");
    PutStr(b, s, nl);
    Put(b, ",\"ref\":");
    const char* r = tab ? tab + 1 : "";
    PutStr(b, r, (int)strlen(r));
}

static void PolyNum(Buf& b, int v)   // Google's polyline, one value
{
    unsigned int u = v < 0 ? ~((unsigned int)v << 1) : (unsigned int)v << 1;
    char t[8];
    int n = 0;
    while (u >= 0x20) {
        t[n++] = (char)((0x20 | (u & 0x1f)) + 63);
        u >>= 5;
    }
    t[n++] = (char)(u + 63);
    Put(b, t, n);
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

static void Step(Buf& b, bool& firstStep, const char* type, const char* mod, double before, double after, int lat,
                 int lon, DWORD name, int exit)
{
    if (!firstStep)
        Put(b, ",");
    firstStep = false;
    Put(b, "{");
    PutName(b, name);
    Putf(b, ",\"maneuver\":{\"type\":\"%s\",\"modifier\":\"%s\",\"bearing_before\":%d,\"bearing_after\":%d,"
            "\"location\":[%.6f,%.6f]", type, mod, (int)before, (int)after, lon / 1e6, lat / 1e6);
    if (exit > 0)
        Putf(b, ",\"exit\":%d", exit);
    Put(b, "}}");
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

// One leg's steps: depart, the maneuvers at the nodes between its edges, arrive. The line grows.
static void Steps(Buf& b, Line& line, const Way& w)
{
    bool first = true;
    // the way as a list of (from, edge, start snap?, end snap?)
    int cnt = w.same ? 1 : w.n + 2;
    double prevAfter = 0, b0, b1;
    DWORD prevE = 0, prevFrom = 0;
    int i;
    for (i = 0; i < cnt; i++) {
        DWORD e, from;
        const Snap* a = NULL;
        const Snap* z = NULL;
        if (w.same) {
            e = w.a.e; from = w.a.from; a = &w.a; z = &w.b;
        } else if (i == 0) {
            e = w.a.e; from = w.a.from; a = &w.a;
        } else if (i == cnt - 1) {
            e = w.b.e; from = w.b.from; z = &w.b;
        } else {
            e = w.e[i - 1]; from = w.from[i - 1];
        }
        int before = line.n;
        AddEdge(line, from, e, a, z, &b0, &b1);
        const GEdge& g = s_g.edges[e];
        if (i == 0) {
            int la = line.p[before * 2], lo = line.p[before * 2 + 1];
            Step(b, first, "depart", "straight", 0, b0, la, lo, g.name, 0);
        } else {
            const GEdge& p = s_g.edges[prevE];
            int la = s_g.nodes[from * 2], lo = s_g.nodes[from * 2 + 1];
            double turn = TurnOf(prevAfter, b0);
            // the other ways out of this node (not back the way we came); "rivals": not of a lower class than ours
            int alts = 0, rivals = 0;
            double rivalMin = 1e9, rivalTurn = 0;   // the straightest rival's turn
            for (DWORD k = s_g.first[from]; k < s_g.first[from + 1]; k++) {
                const GEdge& o = s_g.edges[k];
                if (k == e || (o.to == prevFrom && o.geom == p.geom && o.ngeom == p.ngeom))
                    continue;
                alts++;
                if (Level(o.cls) > Level(g.cls))
                    continue;
                double ot = TurnOf(prevAfter, FirstBearing(from, k));
                rivals++;
                if (fabs(ot) < rivalMin) {
                    rivalMin = fabs(ot);
                    rivalTurn = ot;
                }
            }
            // the obvious way on: the straightest of the rivals and ours (10 degrees' slack)
            bool obvious = fabs(turn) <= rivalMin + 10;
            bool sameRoad = g.name == p.name && strcmp(s_g.names + g.name, "\t");
            bool rbIn = (g.flags & 2) && !(p.flags & 2), rbOn = (g.flags & 2) && (p.flags & 2);
            if (rbIn) {   // onto a roundabout: the exit's number (its nodes with a way off it, up to ours)
                int exitNo = 0;
                DWORD outName = g.name;
                for (int j = i; j < cnt; j++) {
                    DWORD ej = EdgeAt(w, j);
                    if (!(s_g.edges[ej].flags & 2)) {   // off it: this road names the step
                        outName = s_g.edges[ej].name;
                        break;
                    }
                    if (j == cnt - 1)
                        break;   // the way ends on the roundabout
                    DWORD node = s_g.edges[ej].to;
                    for (DWORD k = s_g.first[node]; k < s_g.first[node + 1]; k++)
                        if (!(s_g.edges[k].flags & 2)) {
                            exitNo++;
                            break;
                        }
                }
                Step(b, first, "roundabout", "right", prevAfter, b0, la, lo, outName, exitNo);
            } else if (rbOn || (p.flags & 2)) {
                // on it, or off it: told at the entry
            } else if ((g.flags & 4) && !(p.flags & 4)) {
                Step(b, first, p.cls <= C_TRUNK ? "off ramp" : "on ramp", Modifier(turn), prevAfter, b0, la, lo,
                     g.name, 0);
            } else if (!(g.flags & 4) && (p.flags & 4) && g.cls <= C_TRUNK && fabs(turn) < 45) {
                Step(b, first, "merge", turn > 0 ? "slight right" : "slight left", prevAfter, b0, la, lo, g.name, 0);
            } else if (!alts || (obvious && (sameRoad || fabs(turn) < 35))) {
                // the road just goes on: told only when its name changes
                if (alts && !sameRoad && g.name != p.name && strcmp(s_g.names + g.name, "\t"))
                    Step(b, first, "new name", "straight", prevAfter, b0, la, lo, g.name, 0);
            } else if (fabs(turn) < 35 && rivals) {   // two ways ahead: keep to our side of the other
                Step(b, first, "fork", turn >= rivalTurn ? "slight right" : "slight left", prevAfter, b0, la, lo,
                     g.name, 0);
            } else {
                Step(b, first, "turn", Modifier(turn), prevAfter, b0, la, lo, g.name, 0);
            }
        }
        prevAfter = b1;
        prevE = e;
        prevFrom = from;
    }
    Step(b, first, "arrive", "straight", prevAfter, 0, line.p[(line.n - 1) * 2], line.p[(line.n - 1) * 2 + 1],
         s_g.edges[prevE].name, 0);
}

static void FreeWay(Way& w)
{
    free(w.e);
    free(w.from);
    w.e = w.from = NULL;
}

// "/routed-car/route/v1/driving/lon,lat;lon,lat...?...&bearings=deg,60;..." -> an OSRM answer, or false (no region
// holds the points, no road near, no way): then OSRM is asked.
bool OfflineRoute(const char* path, char** body, int* len)
{
    const char* p = strstr(path, "/driving/");
    if (!p)
        return false;
    p += 9;
    int pts[2 * (kMaxVia + 2)], n = 0;
    while (n < kMaxVia + 2 && *p && *p != '?') {
        double lon = atof(p);
        const char* c = strchr(p, ',');
        if (!c)
            return false;
        double lat = atof(c + 1);
        pts[n * 2] = (int)(lat * 1e6);
        pts[n * 2 + 1] = (int)(lon * 1e6);
        n++;
        p = c + 1;
        while (*p && *p != ';' && *p != '?')
            p++;
        if (*p == ';')
            p++;
    }
    if (n < 2)
        return false;
    double course = -1;
    const char* br = strstr(path, "bearings=");
    if (br && br[9] != ',' && br[9] != ';')
        course = atof(br + 9);
    DWORD t0 = GetTickCount();
    if (!Region(pts, n))
        return false;
    s_kx = 0.111195 * cos(pts[0] / 1e6 * M_PI / 180);
    Buf b = { NULL, 0, 0 };
    Line line = { NULL, 0, 0 };
    Put(b, "{\"code\":\"Ok\",\"routes\":[{\"legs\":[");
    DWORD totalDs = 0;
    double totalM = 0;
    bool ok = true;
    for (int leg = 0; leg + 1 < n && ok; leg++) {
        Snap sa, sb, fa;
        if (!SnapTo(pts[leg * 2], pts[leg * 2 + 1], &sa) || !SnapTo(pts[leg * 2 + 2], pts[leg * 2 + 3], &sb) ||
            sa.distM > 2000 || sb.distM > 2000) {
            Log("offline: no road near point %d", sa.distM > 2000 ? leg : leg + 1);
            ok = false;
            break;
        }
        // the start: the way the car goes (within 90 degrees of its course), else either side
        bool any = true;
        if (leg == 0 && course >= 0) {
            int np = EdgePts(sa.from, sa.e, s_pts, kPtsCap);
            int k = sa.seg < np - 1 ? sa.seg : np - 2;
            double eb = Bearing(s_pts[k * 2], s_pts[k * 2 + 1], s_pts[k * 2 + 2], s_pts[k * 2 + 3]);
            if (fabs(TurnOf(course, eb)) > 90 && Flip(sa, &fa))
                sa = fa;
            any = false;
        }
        Way w;
        memset(&w, 0, sizeof(w));
        if (!Search(sa, sb, any, &w) && !(any == false && Search(sa, sb, true, &w))) {
            ok = false;
            FreeWay(w);
            break;
        }
        if (leg)
            Put(b, ",");
        Put(b, "{\"steps\":[");
        int lineFrom = line.n;
        Steps(b, line, w);
        Put(b, "]}");
        double m = 0;
        for (int i = lineFrom + 1; i < line.n; i++) {
            double dx = (line.p[i * 2 + 1] - line.p[i * 2 - 1]) * s_kx, dy = (line.p[i * 2] - line.p[i * 2 - 2]) * 0.111195;
            m += sqrt(dx * dx + dy * dy);
        }
        totalM += m;
        totalDs += w.timeDs;
        FreeWay(w);
    }
    if (ok && line.n >= 2) {
        Putf(b, "],\"duration\":%.1f,\"distance\":%.1f,\"geometry\":\"", totalDs / 10.0, totalM);
        int plat = 0, plon = 0;
        for (int i = 0; i < line.n; i++) {
            PolyNum(b, line.p[i * 2] - plat);
            PolyNum(b, line.p[i * 2 + 1] - plon);
            plat = line.p[i * 2];
            plon = line.p[i * 2 + 1];
        }
        Put(b, "\"}]}");
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
