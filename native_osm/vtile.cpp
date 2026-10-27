#include "yamaps.h"

// Vector map tiles (2026-10-27, after VTileTest on the unit): OpenMapTiles vector tiles from OpenFreeMap (Mapbox Vector
// Tile = protobuf, no key, z0-14) drawn into the same 256 x 256 RGB565 tiles the raster map uses - the mosaic, the
// rotation, the night filter and the memory cache work on them unchanged, a tile is drawn once (~40 ms on the unit for
// z14) and only turned with the view. z15 and up: a part of the z14 tile drawn larger. The .pbf files go to the disk
// cache as Cache\<100 + z>\... (VEC_ZBASE: the same size limit and pruning as the raster tiles).
// The parser and the filler are VTileTest's (native_vtest/vtest.cpp): areas filled by our own scanline filler in
// integers (the unit's ARM has no FPU), roads as GDI lines with a casing, place names as text.

static const int kExtent = 4096;

// What is drawn, in drawing order: areas, then lines, then points.
enum { K_RESID, K_INDUSTRIAL, K_GRASS, K_WOOD, K_PARK, K_WATER, K_BUILDING,
       K_WATERWAY, K_RAIL, K_PATH, K_SERVICE, K_MINOR, K_SECONDARY, K_PRIMARY, K_MAJOR, K_PLACE, K_COUNT };
static const int kFirstLine = K_WATERWAY;

struct Style { unsigned char r, g, b; int width; unsigned char cr, cg, cb; };   // width 0: area; c*: casing
static const Style kStyle[K_COUNT] = {
    { 0xe0, 0xdf, 0xdf, 0 }, { 0xeb, 0xdb, 0xe8, 0 }, { 0xcd, 0xeb, 0xb0, 0 }, { 0xad, 0xd1, 0x9e, 0 },
    { 0xc8, 0xfa, 0xcc, 0 }, { 0xaa, 0xd3, 0xdf, 0 }, { 0xd9, 0xd0, 0xc9, 0 },
    { 0xaa, 0xd3, 0xdf, 2 }, { 0x99, 0x99, 0x99, 2 }, { 0xfa, 0x80, 0x72, 1 },
    { 0xff, 0xff, 0xff, 2, 0xcc, 0xcc, 0xcc }, { 0xff, 0xff, 0xff, 5, 0xbb, 0xbb, 0xbb }, { 0xf7, 0xfa, 0xbf, 7, 0xb0, 0xb0, 0x90 },
    { 0xfc, 0xd6, 0xa4, 8, 0xa0, 0x80, 0x50 }, { 0xe8, 0x92, 0xa2, 9, 0xa0, 0x50, 0x60 },
    { 0x30, 0x30, 0x30, 0 },
};

struct Feat {
    unsigned char kind;
    unsigned short nparts;
    int part0;                  // into VTile::parts
    short x0, y0, x1, y1;       // bounding box, tile units
    int name;                   // K_PLACE: into VTile::names, -1 none
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
        if (kind == K_PLACE && nm.p) {
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

static void Draw(const VTile& t, const Xf& xf, int z, HDC dc, unsigned short* bits)
{
    unsigned short bg = RGB565(0xf2, 0xef, 0xe9);
    for (int i = 0; i < TILE * TILE; i++)
        bits[i] = bg;
    // areas, kind by kind, then the lines with their casings, then the labels
    for (int k = 0; k < kFirstLine; k++) {
        unsigned short c = RGB565(kStyle[k].r, kStyle[k].g, kStyle[k].b);
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
            const Style& sty = kStyle[k];
            if (pass == 0 && !sty.cr)
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
    // place names, centred on their point; a name across the tile's edge is drawn by both tiles (each its part)
    static HFONT s_font;
    if (!s_font) {
        LOGFONT lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -15;
        lf.lfWeight = FW_BOLD;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = NONANTIALIASED_QUALITY;   // (smoothed comes out as ClearType's fringes on the unit, see MakeFont)
        wcscpy(lf.lfFaceName, MapFace());
        s_font = CreateFontIndirect(&lf);
    }
    HGDIOBJ of = SelectObject(dc, s_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x30, 0x30, 0x30));
    for (int fi = 0; fi < t.nf; fi++) {
        const Feat& f = t.f[fi];
        if (f.kind != K_PLACE || f.name < 0 || !BoxVisible(xf, f, 160))
            continue;
        int x, y;
        Apply(xf, f.x0, f.y0, &x, &y);
        const wchar_t* s = t.names + f.name;
        int n = (int)wcslen(s);
        SIZE sz;
        GetTextExtentPoint32(dc, s, n, &sz);
        ExtTextOut(dc, (x >> 4) - sz.cx / 2, (y >> 4) - sz.cy / 2, 0, NULL, s, (UINT)n, NULL);
    }
    SelectObject(dc, of);
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
        Draw(*t, xf, r.z, dc, bits);
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
