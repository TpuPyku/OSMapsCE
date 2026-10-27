#include "yamaps.h"

// Vector map tiles (2026-10-27, after VTileTest on the unit): OpenMapTiles vector tiles from OpenFreeMap (Mapbox Vector
// Tile = protobuf, no key, z0-14) drawn into the same 256 x 256 RGB565 tiles the raster map uses - the mosaic, the
// rotation, the night filter and the memory cache work on them unchanged, a tile is drawn once (~40 ms on the unit for
// z14) and only turned with the view. z15 and up: a part of the z14 tile drawn larger. The .pbf files go to the disk
// cache as Cache\<100 + z>\... (VEC_ZBASE: the same size limit and pruning as the raster tiles).
// The parser and the filler are VTileTest's (native_vtest/vtest.cpp): areas filled by our own scanline filler in
// integers (the unit's ARM has no FPU), roads as GDI lines with a casing. The names (places, streets) are not in the
// tiles: each parsed tile leaves its labels (VLabelSet), drawn over the turned frame by VLabelsDraw - upright, streets
// along their road, without overlaps.

static const int kExtent = 4096;

// What is drawn, in drawing order: areas, then lines, then points.
enum { K_RESID, K_INDUSTRIAL, K_GRASS, K_WOOD, K_PARK, K_WATER, K_BUILDING,
       K_WATERWAY, K_RAIL, K_PATH, K_SERVICE, K_MINOR, K_SECONDARY, K_PRIMARY, K_MAJOR, K_PLACE, K_ROADNAME, K_COUNT };
static const int kFirstLine = K_WATERWAY;

// The two palettes of design v3 (VestaHome/skin/make_osm_v3_mockup.py, 2026-10-26): the day warm and light, the night
// its own dark one (not the raster map's inverted lightness), roads without casings. K_PLACE, K_ROADNAME: the text,
// c* its halo.
struct Style { unsigned char r, g, b; int width; bool cased; unsigned char cr, cg, cb; };   // width 0: area
static const Style kStyle[2][K_COUNT] = { {
    { 236, 232, 226, 0 }, { 234, 228, 232, 0 }, { 216, 234, 202, 0 }, { 198, 222, 186, 0 },
    { 206, 232, 196, 0 }, { 168, 204, 226, 0 }, { 224, 217, 208, 0 },
    { 168, 204, 226, 2 }, { 184, 180, 174, 2 }, { 206, 196, 184, 1 },
    { 255, 255, 255, 2, true, 214, 208, 200 }, { 255, 255, 255, 5, true, 214, 208, 200 },
    { 255, 247, 214, 7, true, 222, 204, 160 }, { 255, 228, 176, 8, true, 224, 182, 120 },
    { 255, 206, 150, 9, true, 214, 150, 90 },
    { 40, 44, 50, 0, true, 255, 255, 255 }, { 66, 70, 76, 0, true, 255, 255, 255 },
}, {
    { 22, 25, 30, 0 }, { 25, 25, 31, 0 }, { 20, 31, 25, 0 }, { 18, 33, 25, 0 },
    { 21, 37, 29, 0 }, { 13, 34, 50, 0 }, { 31, 35, 42, 0 },
    { 13, 34, 50, 2 }, { 48, 52, 58, 2 }, { 32, 36, 42, 1 },
    { 38, 43, 50, 2 }, { 44, 50, 58, 5 }, { 62, 70, 80, 7 }, { 84, 80, 68, 8 }, { 112, 88, 58, 9 },
    { 205, 208, 214, 0, true, 11, 12, 14 }, { 150, 156, 166, 0, true, 11, 12, 14 },
} };
static const unsigned char kBg[2][3] = { { 243, 240, 235 }, { 17, 19, 23 } };
// The smallest shown zoom of a kind (design v3: buildings from z15, paths from z16)
static int MinZoom(int kind)
{
    return kind == K_BUILDING || kind == K_SERVICE ? 15 : kind == K_PATH ? 16 : 0;
}

struct Feat {
    unsigned char kind;
    unsigned short nparts;
    int part0;                  // into VTile::parts
    short x0, y0, x1, y1;       // bounding box, tile units
    int name;                   // K_PLACE, K_ROADNAME: into VTile::names, -1 none
    unsigned char rank;         // the names: 0 the biggest (a city, a main road) .. 2
};

struct VTile {
    MapRequest key;             // the data tile (VEC_ZBASE + z)
    Feat* f;     int nf, fcap;
    int* parts;  int nparts, pcap;   // pairs: first point, point count
    short* pts;  int npts, ptcap;    // x, y pairs
    wchar_t* names; int nnames, ncap;
    DWORD useTick;
};

// The last data tiles parsed: a z16 view draws 16 tiles from one z14 tile.
enum { kParsed = 3 };
static VTile s_parsed[kParsed];
static CRITICAL_SECTION s_cs;   // the download thread and the UI thread (disk loads) both draw

// ---------------------------------------------------------------- protobuf (MVT)

struct Pb { const unsigned char *p, *end; };

static unsigned int Varint(Pb& b)
{
    unsigned int v = 0;
    int shift = 0;
    while (b.p < b.end) {
        unsigned char c = *b.p++;
        if (shift < 32)
            v |= (unsigned int)(c & 0x7f) << shift;
        shift += 7;
        if (!(c & 0x80))
            break;
    }
    return v;
}

static void Skip(Pb& b, int wire)
{
    if (wire == 0)
        Varint(b);
    else if (wire == 1)
        b.p += 8;
    else if (wire == 2) {
        unsigned int n = Varint(b);
        b.p += n;
    } else if (wire == 5)
        b.p += 4;
    if (b.p > b.end)
        b.p = b.end;
}

static Pb Sub(Pb& b)   // a length-delimited field
{
    unsigned int n = Varint(b);
    Pb s;
    s.p = b.p;
    s.end = b.p + n > b.end ? b.end : b.p + n;
    b.p = s.end;
    return s;
}

static bool StrEq(const Pb& s, const char* t)
{
    int n = (int)(s.end - s.p);
    return (int)strlen(t) == n && !memcmp(s.p, t, n);
}

// Growing arrays for one tile; false = out of memory
template <class T> static bool Push(T*& a, int& n, int& cap, const T& v)
{
    if (n == cap) {
        int c = cap ? cap * 2 : 256;
        T* na = (T*)realloc(a, c * sizeof(T));
        if (!na)
            return false;
        a = na;
        cap = c;
    }
    a[n++] = v;
    return true;
}

static int Kind(const Pb& layer, const Pb& cls, int geomType)
{
    if (StrEq(layer, "water"))
        return geomType == 3 ? K_WATER : -1;
    if (StrEq(layer, "building"))
        return geomType == 3 ? K_BUILDING : -1;
    if (StrEq(layer, "park"))
        return geomType == 3 ? K_PARK : -1;
    if (StrEq(layer, "landcover") && geomType == 3)
        return StrEq(cls, "wood") ? K_WOOD : K_GRASS;
    if (StrEq(layer, "landuse") && geomType == 3) {
        if (StrEq(cls, "residential") || StrEq(cls, "suburb") || StrEq(cls, "neighbourhood"))
            return K_RESID;
        if (StrEq(cls, "industrial") || StrEq(cls, "commercial") || StrEq(cls, "retail") || StrEq(cls, "railway"))
            return K_INDUSTRIAL;
        return -1;
    }
    if (StrEq(layer, "waterway"))
        return geomType == 2 ? K_WATERWAY : -1;
    if (StrEq(layer, "transportation") && geomType == 2) {
        if (StrEq(cls, "motorway") || StrEq(cls, "trunk"))
            return K_MAJOR;
        if (StrEq(cls, "primary"))
            return K_PRIMARY;
        if (StrEq(cls, "secondary") || StrEq(cls, "tertiary"))
            return K_SECONDARY;
        if (StrEq(cls, "minor"))
            return K_MINOR;
        if (StrEq(cls, "service"))
            return K_SERVICE;
        if (StrEq(cls, "track") || StrEq(cls, "path"))
            return K_PATH;
        if (StrEq(cls, "rail") || StrEq(cls, "transit"))
            return K_RAIL;
        return -1;
    }
    if (StrEq(layer, "place") && geomType == 1)
        return K_PLACE;
    if (StrEq(layer, "transportation_name") && geomType == 2)
        return K_ROADNAME;
    return -1;
}

static bool DecodeLayer(Pb b, VTile& t)
{
    // first pass: the name, keys, values (only strings matter here)
    Pb name = { 0, 0 };
    static Pb keys[512], vals[2048];   // (not on the stack: CE threads have little; s_cs held)
    int nk = 0, nv = 0;
    Pb scan = b;
    while (scan.p < scan.end) {
        unsigned int tag = Varint(scan);
        int field = tag >> 3, wire = tag & 7;
        if (field == 1 && wire == 2)
            name = Sub(scan);
        else if (field == 3 && wire == 2) {
            Pb k = Sub(scan);
            if (nk < 512)
                keys[nk++] = k;
        } else if (field == 4 && wire == 2) {
            Pb v = Sub(scan), sv = { 0, 0 };
            while (v.p < v.end) {
                unsigned int vt = Varint(v);
                if ((vt >> 3) == 1 && (vt & 7) == 2)
                    sv = Sub(v);
                else
                    Skip(v, vt & 7);
            }
            if (nv < 2048)
                vals[nv++] = sv;
        } else
            Skip(scan, wire);
    }
    int kClass = -1, kName = -1;
    for (int i = 0; i < nk; i++) {
        if (StrEq(keys[i], "class"))
            kClass = i;
        else if (StrEq(keys[i], "name"))
            kName = i;
    }
    // features
    while (b.p < b.end) {
        unsigned int tag = Varint(b);
        if ((tag >> 3) != 2 || (tag & 7) != 2) {
            Skip(b, tag & 7);
            continue;
        }
        Pb f = Sub(b), tags = { 0, 0 }, geom = { 0, 0 };
        int type = 0;
        while (f.p < f.end) {
            unsigned int ft = Varint(f);
            int field = ft >> 3;
            if (field == 2 && (ft & 7) == 2)
                tags = Sub(f);
            else if (field == 3)
                type = (int)Varint(f);
            else if (field == 4 && (ft & 7) == 2)
                geom = Sub(f);
            else
                Skip(f, ft & 7);
        }
        Pb cls = { 0, 0 }, nm = { 0, 0 };
        while (tags.p < tags.end) {
            unsigned int k = Varint(tags), v = Varint(tags);
            if ((int)k == kClass && v < (unsigned)nv)
                cls = vals[v];
            else if ((int)k == kName && v < (unsigned)nv)
                nm = vals[v];
        }
        int kind = Kind(name, cls, type);
        if (kind < 0)
            continue;
        Feat ft;
        ft.kind = (unsigned char)kind;
        ft.nparts = 0;
        ft.part0 = t.nparts / 2;
        ft.x0 = ft.y0 = 32767;
        ft.x1 = ft.y1 = -32768;
        ft.name = -1;
        ft.rank = 2;
        if (kind == K_PLACE)
            ft.rank = StrEq(cls, "city") || StrEq(cls, "town") ? 0 :
                      StrEq(cls, "suburb") || StrEq(cls, "village") || StrEq(cls, "quarter") ? 1 : 2;
        else if (kind == K_ROADNAME) {
            if (StrEq(cls, "motorway") || StrEq(cls, "trunk") || StrEq(cls, "primary"))
                ft.rank = 0;
            else if (StrEq(cls, "secondary") || StrEq(cls, "tertiary"))
                ft.rank = 1;
            else if (!StrEq(cls, "minor"))
                continue;   // (service roads, paths: no names)
        }
        int x = 0, y = 0, start = -1;
        bool ok = true;
        while (geom.p < geom.end) {
            unsigned int ci = Varint(geom);
            int cmd = ci & 7, count = ci >> 3;
            if (cmd == 7)
                continue;   // ClosePath: the filler closes rings itself
            for (int i = 0; i < count && geom.p < geom.end; i++) {
                unsigned int dx = Varint(geom), dy = Varint(geom);
                x += (int)((dx >> 1) ^ (0 - (dx & 1)));
                y += (int)((dy >> 1) ^ (0 - (dy & 1)));
                if (cmd == 1) {   // MoveTo: a new part
                    if (start >= 0 && t.npts / 2 - start > 0) {
                        ok &= Push(t.parts, t.nparts, t.pcap, start);
                        ok &= Push(t.parts, t.nparts, t.pcap, t.npts / 2 - start);
                        ft.nparts++;
                    }
                    start = t.npts / 2;
                }
                short sx = (short)x, sy = (short)y;
                ok &= Push(t.pts, t.npts, t.ptcap, sx);
                ok &= Push(t.pts, t.npts, t.ptcap, sy);
                if (sx < ft.x0) ft.x0 = sx;
                if (sx > ft.x1) ft.x1 = sx;
                if (sy < ft.y0) ft.y0 = sy;
                if (sy > ft.y1) ft.y1 = sy;
            }
        }
        if (start >= 0 && t.npts / 2 - start > 0) {
            ok &= Push(t.parts, t.nparts, t.pcap, start);
            ok &= Push(t.parts, t.nparts, t.pcap, t.npts / 2 - start);
            ft.nparts++;
        }
        if (!ok)
            return false;
        if (!ft.nparts)
            continue;
        if ((kind == K_PLACE || kind == K_ROADNAME) && nm.p) {
            wchar_t w[64];
            char u[128];
            int n = (int)(nm.end - nm.p);
            n = n > 127 ? 127 : n;
            memcpy(u, nm.p, n);
            u[n] = 0;
            if (MultiByteToWideChar(CP_UTF8, 0, u, -1, w, 64)) {
                w[63] = 0;
                ft.name = t.nnames;
                for (int i = 0; i == 0 || w[i - 1]; i++)   // with its terminating 0
                    Push(t.names, t.nnames, t.ncap, w[i]);
            }
        }
        if (kind == K_ROADNAME && ft.name < 0)
            continue;
        if (!Push(t.f, t.nf, t.fcap, ft))
            return false;
    }
    return true;
}

static void FreeParsed(VTile& t)
{
    free(t.f);
    free(t.parts);
    free(t.pts);
    free(t.names);
    memset(&t, 0, sizeof(t));
}

// ---------------------------------------------------------------- labels

// A tile's names: places at their point, streets at the middle of their longest segment (dx, dy: the segment).
// Only the ones whose point is inside the tile: the tiles' buffers repeat their neighbours' names.
struct VLabel { unsigned char kind, rank; short x, y, dx, dy; int name; };
struct VLabelSet { MapRequest key; VLabel* l; int n; wchar_t* names; int nn; DWORD useTick; };
enum { kLabelSets = 48 };       // a z13 view needs ~24 data tiles
static VLabelSet s_labels[kLabelSets];

static void LabelsFrom(const VTile& t)
{
    VLabelSet* set = &s_labels[0];
    for (int i = 0; i < kLabelSets; i++) {
        if (s_labels[i].useTick && SameKey(s_labels[i].key, t.key)) {
            s_labels[i].useTick = GetTickCount();
            return;
        }
        if (s_labels[i].useTick < set->useTick)
            set = &s_labels[i];
    }
    free(set->l);
    free(set->names);
    memset(set, 0, sizeof(*set));
    int n = 0;
    for (int i = 0; i < t.nf; i++)
        if (t.f[i].name >= 0)
            n++;
    set->l = (VLabel*)malloc((n ? n : 1) * sizeof(VLabel));
    set->names = (wchar_t*)malloc((t.nnames ? t.nnames : 1) * sizeof(wchar_t));
    if (!set->l || !set->names) {
        free(set->l);
        free(set->names);
        memset(set, 0, sizeof(*set));
        return;
    }
    memcpy(set->names, t.names, t.nnames * sizeof(wchar_t));
    set->nn = t.nnames;
    for (int i = 0; i < t.nf; i++) {
        const Feat& f = t.f[i];
        if (f.name < 0)
            continue;
        VLabel l;
        l.kind = f.kind;
        l.rank = f.rank;
        l.name = f.name;
        l.dx = l.dy = 0;
        if (f.kind == K_PLACE) {
            l.x = f.x0;
            l.y = f.y0;
        } else {
            int best = -1;
            for (int pi = 0; pi < f.nparts; pi++) {
                int p0 = t.parts[(f.part0 + pi) * 2], pn = t.parts[(f.part0 + pi) * 2 + 1];
                for (int q = 1; q < pn; q++) {
                    int ax = t.pts[(p0 + q - 1) * 2], ay = t.pts[(p0 + q - 1) * 2 + 1];
                    int bx = t.pts[(p0 + q) * 2], by = t.pts[(p0 + q) * 2 + 1];
                    int d2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
                    if (d2 > best) {
                        best = d2;
                        l.x = (short)((ax + bx) / 2);
                        l.y = (short)((ay + by) / 2);
                        l.dx = (short)(bx - ax);
                        l.dy = (short)(by - ay);
                    }
                }
            }
            if (best <= 0)
                continue;
        }
        if (l.x < 0 || l.y < 0 || l.x >= kExtent || l.y >= kExtent)
            continue;
        set->l[set->n++] = l;
    }
    set->key = t.key;
    set->useTick = GetTickCount();
}

// The data tile parsed (from the recent ones, or now); NULL = bad data / out of memory.
static VTile* Parsed(const MapRequest& key, const unsigned char* data, int len)
{
    VTile* slot = &s_parsed[0];
    for (int i = 0; i < kParsed; i++) {
        if (s_parsed[i].useTick && SameKey(s_parsed[i].key, key)) {
            s_parsed[i].useTick = GetTickCount();
            return &s_parsed[i];
        }
        if (s_parsed[i].useTick < slot->useTick)
            slot = &s_parsed[i];
    }
    if (!data)
        return NULL;
    FreeParsed(*slot);
    VTile& t = *slot;
    Pb b = { data, data + len };
    bool ok = true;
    while (ok && b.p < b.end) {
        unsigned int tag = Varint(b);
        if ((tag >> 3) == 3 && (tag & 7) == 2)
            ok = DecodeLayer(Sub(b), t);
        else
            Skip(b, tag & 7);
    }
    if (!ok) {
        Log("vtile: out of memory parsing %d/%d/%d", key.z - VEC_ZBASE, key.x, key.y);
        FreeParsed(t);
        return NULL;
    }
    t.key = key;
    t.useTick = GetTickCount();
    LabelsFrom(t);
    return &t;
}

// ---------------------------------------------------------------- drawing one tile

// A data tile point -> the drawn tile in 1/16 px: X = (u << dz) - ox, Y = (v << dz) - oy (256 px = 4096 units at dz 0).
struct Xf { int dz, ox, oy; };

static inline void Apply(const Xf& x, int u, int v, int* sx, int* sy)
{
    *sx = (u << x.dz) - x.ox;
    *sy = (v << x.dz) - x.oy;
}

// ---- scanline filler, even-odd, pixel centres
struct Edge { int r0, r1; int x, dx; };   // rows r0..r1-1, x at r0's centre and per row, 16.16 px

static Edge* s_edges;
static int   s_edgeCap;
static int   s_xs[1024];

static void AddEdge(int& ne, int x0, int y0, int x1, int y1)   // 1/16 px
{
    if (y0 == y1)
        return;
    if (y0 > y1) {
        int t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    // rows whose centre r*16+8 is in [y0, y1)
    int r0 = (y0 - 8 + 15 + 16 * 65536) / 16 - 65536, r1 = (y1 - 8 + 15 + 16 * 65536) / 16 - 65536;   // ceil
    if (r0 < 0)
        r0 = 0;
    if (r1 > TILE)
        r1 = TILE;
    if (r0 >= r1)
        return;
    if (ne == s_edgeCap) {
        int c = s_edgeCap ? s_edgeCap * 2 : 1024;
        Edge* e = (Edge*)realloc(s_edges, c * sizeof(Edge));
        if (!e)
            return;
        s_edges = e;
        s_edgeCap = c;
    }
    Edge& e = s_edges[ne++];
    __int64 slope = ((__int64)(x1 - x0) << 16) / (y1 - y0);   // 1/16 px of x per 1/16 px of y, 16.16
    e.r0 = r0;
    e.r1 = r1;
    e.dx = (int)slope;                                          // per row (16 units of y), px
    e.x = (int)((((__int64)x0 << 16) + slope * (r0 * 16 + 8 - y0)) / 16);
}

static int CmpEdge(const void* a, const void* b)
{
    return ((const Edge*)a)->r0 - ((const Edge*)b)->r0;
}

static void FillEdges(unsigned short* bits, int ne, unsigned short color)
{
    if (!ne)
        return;
    qsort(s_edges, ne, sizeof(Edge), CmpEdge);
    int rmin = s_edges[0].r0, rmax = 0;
    for (int i = 0; i < ne; i++)
        if (s_edges[i].r1 > rmax)
            rmax = s_edges[i].r1;
    int next = 0, nact = 0;
    static int act[4096];
    for (int r = rmin; r < rmax; r++) {
        while (next < ne && s_edges[next].r0 == r) {
            if (nact < 4096)
                act[nact++] = next;
            next++;
        }
        int nx = 0;
        for (int i = 0; i < nact;) {
            Edge& e = s_edges[act[i]];
            if (e.r1 <= r) {
                act[i] = act[--nact];
                continue;
            }
            int x = e.x;
            e.x += e.dx;
            if (nx < 1024) {
                int j = nx++;   // insertion sort: few crossings per row
                while (j > 0 && s_xs[j - 1] > x) {
                    s_xs[j] = s_xs[j - 1];
                    j--;
                }
                s_xs[j] = x;
            }
            i++;
        }
        unsigned short* row = bits + (TILE - 1 - r) * TILE;   // bottom-up rows, 512 bytes each
        for (int i = 0; i + 1 < nx; i += 2) {
            int a = (s_xs[i] + 0x7FFF) >> 16, b = (s_xs[i + 1] + 0x7FFF) >> 16;   // centres inside [xa, xb)
            if (a < 0)
                a = 0;
            if (b > TILE)
                b = TILE;
            for (int x = a; x < b; x++)
                row[x] = color;
        }
    }
}

// The feature's box near the drawn tile (margin in px)?
static bool BoxVisible(const Xf& x, const Feat& f, int margin)
{
    int l, t, r, b;
    Apply(x, f.x0, f.y0, &l, &t);
    Apply(x, f.x1, f.y1, &r, &b);
    return r >= -margin * 16 && l <= (TILE + margin) * 16 && b >= -margin * 16 && t <= (TILE + margin) * 16;
}

static POINT* s_line;
static int    s_lineCap;

// Line widths by the shown zoom: the styles' are for z14.
static int LineWidth(int width, int z)
{
    static const int kNum[8] = { 1, 1, 2, 4, 5, 6, 8, 10 };   // z11..z18, quarters (PC check 2026-10-27: 2x at z16 too wide)
    int i = z < 11 ? 0 : z > 18 ? 7 : z - 11;
    int w = (width * kNum[i] + 2) / 4;
    return w < 1 ? 1 : w;
}

static void Draw(const VTile& t, const Xf& xf, int z, bool night, HDC dc, unsigned short* bits)
{
    const Style* pal = kStyle[night ? 1 : 0];
    const unsigned char* bc = kBg[night ? 1 : 0];
    unsigned short bg = RGB565(bc[0], bc[1], bc[2]);
    for (int i = 0; i < TILE * TILE; i++)
        bits[i] = bg;
    // areas, kind by kind, then the lines with their casings, then the labels
    for (int k = 0; k < kFirstLine; k++) {
        if (z < MinZoom(k))
            continue;
        unsigned short c = RGB565(pal[k].r, pal[k].g, pal[k].b);
        for (int fi = 0; fi < t.nf; fi++) {
            const Feat& f = t.f[fi];
            if (f.kind != k || !BoxVisible(xf, f, 0))
                continue;
            int ne = 0;
            for (int pi = 0; pi < f.nparts; pi++) {
                int p0 = t.parts[(f.part0 + pi) * 2], pn = t.parts[(f.part0 + pi) * 2 + 1];
                int fx, fy, px, py;
                Apply(xf, t.pts[p0 * 2], t.pts[p0 * 2 + 1], &fx, &fy);
                px = fx;
                py = fy;
                for (int q = 1; q < pn; q++) {
                    int x, y;
                    Apply(xf, t.pts[(p0 + q) * 2], t.pts[(p0 + q) * 2 + 1], &x, &y);
                    AddEdge(ne, px, py, x, y);
                    px = x;
                    py = y;
                }
                AddEdge(ne, px, py, fx, fy);
            }
            FillEdges(bits, ne, c);
        }
    }
#ifndef UNDER_CE
    GdiFlush();   // the desktop's GDI may batch: our pixels before its lines (CE draws at once)
#endif
    for (int pass = 0; pass < 2; pass++)
        for (int k = kFirstLine; k < K_PLACE; k++) {
            const Style& sty = pal[k];
            if ((pass == 0 && !sty.cased) || z < MinZoom(k))
                continue;
            int w = LineWidth(sty.width, z) + (pass == 0 ? 2 : 0);
            HPEN pen = CreatePen(PS_SOLID, w, pass == 0 ? RGB(sty.cr, sty.cg, sty.cb) : RGB(sty.r, sty.g, sty.b));
            HGDIOBJ op = SelectObject(dc, pen);
            for (int fi = 0; fi < t.nf; fi++) {
                const Feat& f = t.f[fi];
                if (f.kind != k || !BoxVisible(xf, f, w))
                    continue;
                for (int pi = 0; pi < f.nparts; pi++) {
                    int p0 = t.parts[(f.part0 + pi) * 2], pn = t.parts[(f.part0 + pi) * 2 + 1];
                    if (pn > s_lineCap) {
                        POINT* nl = (POINT*)realloc(s_line, pn * 2 * sizeof(POINT));
                        if (!nl)
                            continue;
                        s_line = nl;
                        s_lineCap = pn * 2;
                    }
                    int m = 0;
                    for (int q = 0; q < pn; q++) {
                        int x, y;
                        Apply(xf, t.pts[(p0 + q) * 2], t.pts[(p0 + q) * 2 + 1], &x, &y);
                        x = (x + 8) >> 4;
                        y = (y + 8) >> 4;
                        if (m && abs(x - s_line[m - 1].x) + abs(y - s_line[m - 1].y) < 2 && q < pn - 1)
                            continue;
                        s_line[m].x = x;
                        s_line[m].y = y;
                        m++;
                    }
                    if (m > 1)
                        Polyline(dc, s_line, m);
                }
            }
            SelectObject(dc, op);
            DeleteObject(pen);
        }
#ifndef UNDER_CE
    GdiFlush();
#endif
}

// The data tile a shown tile is drawn from: its own up to z14, else its z14 ancestor.
MapRequest VTileData(const MapRequest& r)
{
    int dz = r.z > VEC_MAXZ ? r.z - VEC_MAXZ : 0;
    MapRequest k = { SRC_OSM, VEC_ZBASE + r.z - dz, r.x >> dz, r.y >> dz };
    return k;
}

bool VTileRender(const MapRequest& r, const unsigned char* data, int len, Image* out)
{
    MapRequest key = VTileData(r);
    DWORD t0 = GetTickCount();
    EnterCriticalSection(&s_cs);
    bool ok = false;
    VTile* t = Parsed(key, data, len);
    unsigned short* bits = t ? ImageCreate(TILE, TILE, out) : NULL;
    if (bits) {
        Xf xf;
        xf.dz = r.z - (key.z - VEC_ZBASE);
        xf.ox = (r.x - (key.x << xf.dz)) * kExtent;
        xf.oy = (r.y - (key.y << xf.dz)) * kExtent;
        HDC dc = CreateCompatibleDC(NULL);
        HGDIOBJ ob = SelectObject(dc, out->bmp);
        Draw(*t, xf, r.z, g_cfg.night != 0, dc, bits);
        SelectObject(dc, ob);
        DeleteDC(dc);
        ok = true;
    }
    LeaveCriticalSection(&s_cs);
    if (ok)
        Log("vtile: %d/%d/%d from %d/%d/%d: %lu ms", r.z, r.x, r.y, key.z - VEC_ZBASE, key.x, key.y, GetTickCount() - t0);
    return ok;
}

void VTileInit()   // before the download thread starts
{
    InitializeCriticalSection(&s_cs);
}

// ---------------------------------------------------------------- the labels over the frame

static HFONT s_placeFont[3];    // by rank
static HFONT s_streetFont[37];  // by angle: -90..90 degrees in 5 degree steps (lazily)

static HFONT LabelFont(int height, int weight, int tenths)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    lf.lfEscapement = lf.lfOrientation = tenths;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = NONANTIALIASED_QUALITY;   // (smoothed comes out as ClearType's fringes on the unit)
    wcscpy(lf.lfFaceName, MapFace());
    return CreateFontIndirect(&lf);
}

struct Placed { int l, t, r, b; const wchar_t* name; int cx, cy; };

static bool Free(const Placed* p, int np, const Placed& box)
{
    for (int i = 0; i < np; i++)
        if (box.l < p[i].r && box.r > p[i].l && box.t < p[i].b && box.b > p[i].t)
            return false;
    return true;
}

// Text with the halo of its palette: 8 copies 1 px around in the halo's colour, then the text.
static void Halo(HDC dc, int x, int y, const wchar_t* s, int n, const Style& st)
{
    static const int kHalo[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
    SetTextColor(dc, RGB(st.cr, st.cg, st.cb));
    for (int h = 0; h < 8; h++)
        ExtTextOut(dc, x + kHalo[h][0], y + kHalo[h][1], 0, NULL, s, (UINT)n, NULL);
    SetTextColor(dc, RGB(st.r, st.g, st.b));
    ExtTextOut(dc, x, y, 0, NULL, s, (UINT)n, NULL);
}

// The names of the view's data tiles over the frame: (cx, cy) the screen point of the view's world point (vx, vy) at
// zoom z, the map turned by angle (degrees, as DrawRoute). Places first (the biggest first), then streets along their
// road, upright; none over another, a street's name once per 300 px.
void VLabelsDraw(HDC dc, const RECT& mr, double cx, double cy, double vx, double vy, double angle, int z)
{
    int zd = z > VEC_MAXZ ? VEC_MAXZ : z;
    if (zd < 10)
        return;
    DWORD t0 = GetTickCount();
    const Style* pal = kStyle[g_cfg.night ? 1 : 0];
    double ra = angle * M_PI / 180.0, rc = cos(ra), rs = sin(ra);
    double unit = (double)(1 << (z - zd)) * TILE / kExtent;   // world px per tile unit
    static Placed placed[160];
    int np = 0;
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = NULL;
    EnterCriticalSection(&s_cs);
    for (int kind = 0; kind < 2; kind++)
        for (int rank = 0; rank < 3; rank++) {
            if (kind == 0 && rank == 2 && z < 14)
                continue;   // neighbourhoods: close up only
            if (kind == 1 && (z < 14 || (rank == 2 && z < 15)))
                continue;   // streets: from z14, the small ones from z15
            HFONT pf;
            if (kind == 0) {
                if (!s_placeFont[rank])
                    s_placeFont[rank] = LabelFont(rank == 0 ? 17 : rank == 1 ? 15 : 13, rank < 2 ? FW_BOLD : FW_NORMAL, 0);
                pf = s_placeFont[rank];
            } else {
                if (!s_streetFont[18])
                    s_streetFont[18] = LabelFont(13, FW_NORMAL, 0);
                pf = s_streetFont[18];
            }
            for (int si = 0; si < kLabelSets && np < 160; si++) {
                const VLabelSet& set = s_labels[si];
                if (!set.useTick || set.key.z != VEC_ZBASE + zd)
                    continue;
                double ox = (double)set.key.x * kExtent * unit, oy = (double)set.key.y * kExtent * unit;
                for (int li = 0; li < set.n && np < 160; li++) {
                    const VLabel& lb = set.l[li];
                    if (lb.rank != rank || (lb.kind == K_PLACE) != (kind == 0))
                        continue;
                    double dx = ox + lb.x * unit - vx, dy = oy + lb.y * unit - vy;
                    double x = cx + dx * rc + dy * rs, y = cy - dx * rs + dy * rc;
                    if (x < mr.left - 60 || x > mr.right + 60 || y < mr.top - 20 || y > mr.bottom + 20)
                        continue;
                    const wchar_t* s = set.names + lb.name;
                    int n = (int)wcslen(s);
                    HGDIOBJ prev = SelectObject(dc, pf);
                    if (!of)
                        of = prev;
                    SIZE sz;
                    GetTextExtentPoint32(dc, s, n, &sz);
                    Placed box;
                    box.name = s;
                    box.cx = (int)x;
                    box.cy = (int)y;
                    if (kind == 0) {
                        box.l = (int)x - sz.cx / 2 - 2;
                        box.t = (int)y - sz.cy / 2 - 1;
                        box.r = box.l + sz.cx + 4;
                        box.b = box.t + sz.cy + 2;
                        if (!Free(placed, np, box))
                            continue;
                        Halo(dc, box.l + 2, box.t + 1, s, n, pal[K_PLACE]);
                    } else {
                        // the road's direction on the screen, upright (never upside down)
                        double sx = lb.dx * rc + lb.dy * rs, sy = -lb.dx * rs + lb.dy * rc;
                        double len = sqrt(sx * sx + sy * sy);
                        if (len * unit < sz.cx + 12)
                            continue;   // the segment is shorter than the name
                        if (sx < 0 || (sx == 0 && sy > 0)) {
                            sx = -sx;
                            sy = -sy;
                        }
                        double th = atan2(-sy, sx) * 180.0 / M_PI;   // counterclockwise, -90..90
                        int bucket = (int)floor(th / 5.0 + 0.5);
                        bucket = bucket < -18 ? -18 : bucket > 18 ? 18 : bucket;
                        double a = bucket * 5.0 * M_PI / 180.0, ca = cos(a), sa = sin(a);
                        // the text's direction (ca, -sa), its "down" (sa, ca); the origin is its top left corner
                        double w = sz.cx, h = sz.cy;
                        double tx = x - ca * w / 2 - sa * h / 2, ty = y + sa * w / 2 - ca * h / 2;
                        double xs[4] = { tx, tx + ca * w, tx + sa * h, tx + ca * w + sa * h };
                        double ys[4] = { ty, ty - sa * w, ty + ca * h, ty - sa * w + ca * h };
                        double l = xs[0], r = xs[0], t = ys[0], b = ys[0];
                        for (int k = 1; k < 4; k++) {
                            if (xs[k] < l) l = xs[k];
                            if (xs[k] > r) r = xs[k];
                            if (ys[k] < t) t = ys[k];
                            if (ys[k] > b) b = ys[k];
                        }
                        box.l = (int)l - 1;
                        box.t = (int)t - 1;
                        box.r = (int)r + 1;
                        box.b = (int)b + 1;
                        bool again = false;   // the same street close by: once
                        for (int k = 0; k < np && !again; k++)
                            again = placed[k].name && !wcscmp(placed[k].name, s) &&
                                    abs(placed[k].cx - box.cx) + abs(placed[k].cy - box.cy) < 300;
                        if (again || !Free(placed, np, box))
                            continue;
                        int fi = bucket + 18;
                        if (!s_streetFont[fi])
                            s_streetFont[fi] = LabelFont(13, FW_NORMAL, bucket * 50);
                        SelectObject(dc, s_streetFont[fi]);
                        Halo(dc, (int)floor(tx + 0.5), (int)floor(ty + 0.5), s, n, pal[K_ROADNAME]);
                    }
                    placed[np++] = box;
                }
            }
        }
    LeaveCriticalSection(&s_cs);
    if (of)
        SelectObject(dc, of);
    // the time, for the unit's log: turned text may be slow there (every 30 frames)
    static DWORD s_ms, s_max;
    static int s_frames, s_shown;
    DWORD ms = GetTickCount() - t0;
    s_ms += ms;
    s_max = ms > s_max ? ms : s_max;
    s_shown += np;
    if (++s_frames == 30) {
        Log("vtile: labels %d per frame, %lu ms avg, %lu max", s_shown / 30, s_ms / 30, s_max);
        s_ms = s_max = 0;
        s_frames = s_shown = 0;
    }
}
