// VTileTest - can the head unit draw OSM vector tiles fast enough for OSMapsCE? (the user's question 2026-10-26)
//
// Reads OpenMapTiles vector tiles (OpenFreeMap, Mapbox Vector Tile = protobuf) from tiles\<z>\<x>_<y>.pbf next to
// the exe, decodes them into compact feature lists, then draws the 800 x 480 screen from them the way a map with
// the heading up needs it: land use, water, parks, buildings filled by our own scanline filler (fixed point - the
// unit's ARM has no FPU), roads as GDI lines with a casing, place names as text. Each zoom is drawn north-up and
// turned 12 times by 30 degrees; the times (read, decode, frame avg / max) are shown and written to
// VestaLogs\VTileTest.txt on the SD card / USB disk (nothing with \SDMMC\VestaLogs.off). A tap on the right half -
// the next zoom, on the left half - the previous one, the top right corner - exit.
// The data: OpenFreeMap tiles go up to z14; z15 and z16 are the z14 tiles drawn larger, as a real map would.
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "vlogdir.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RGB565(r, g, b) ((unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static const int W = 800, H = 480;
static const int kExtent = 4096;

// What is drawn, in drawing order: areas, then lines, then points.
enum { K_RESID, K_INDUSTRIAL, K_GRASS, K_WOOD, K_PARK, K_WATER, K_BUILDING,
       K_WATERWAY, K_RAIL, K_PATH, K_MINOR, K_SECONDARY, K_PRIMARY, K_MAJOR, K_PLACE, K_COUNT };
static const int kFirstLine = K_WATERWAY;

struct Style { unsigned char r, g, b; int width; unsigned char cr, cg, cb; };   // width 0: area; c*: casing
static const Style kStyle[K_COUNT] = {
    { 0xe0, 0xdf, 0xdf, 0 }, { 0xeb, 0xdb, 0xe8, 0 }, { 0xcd, 0xeb, 0xb0, 0 }, { 0xad, 0xd1, 0x9e, 0 },
    { 0xc8, 0xfa, 0xcc, 0 }, { 0xaa, 0xd3, 0xdf, 0 }, { 0xd9, 0xd0, 0xc9, 0 },
    { 0xaa, 0xd3, 0xdf, 2 }, { 0x99, 0x99, 0x99, 2 }, { 0xfa, 0x80, 0x72, 1 },
    { 0xff, 0xff, 0xff, 5, 0xbb, 0xbb, 0xbb }, { 0xf7, 0xfa, 0xbf, 7, 0xb0, 0xb0, 0x90 },
    { 0xfc, 0xd6, 0xa4, 8, 0xa0, 0x80, 0x50 }, { 0xe8, 0x92, 0xa2, 9, 0xa0, 0x50, 0x60 },
    { 0x30, 0x30, 0x30, 0 },
};

struct Feat {
    unsigned char kind;
    unsigned short nparts;
    int part0;                  // into Tile::parts
    short x0, y0, x1, y1;       // bounding box, tile units
    int name;                   // K_PLACE: into Tile::names, -1 none
};

struct Tile {
    int z, x, y;
    Feat* f;     int nf;
    int* parts;  int nparts;    // pairs: first point, point count
    short* pts;  int npts;      // x, y pairs
    wchar_t* names; int nnames;
};

static Tile*  s_tiles;
static int    s_ntiles;
static wchar_t s_dir[MAX_PATH];
static double s_lon = 50.11, s_lat = 53.1955;

// ---------------------------------------------------------------- log / results

static wchar_t s_log[MAX_PATH];

static void Out(const char* fmt, ...)
{
    char b[512];
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf(b, sizeof(b) - 3, fmt, a);
    va_end(a);
    if (n < 0)
        n = (int)strlen(b);
    b[n++] = '\r';
    b[n++] = '\n';
    if (!s_log[0])
        return;
    HANDLE h = CreateFile(s_log, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w;
    WriteFile(h, b, n, &w, NULL);
    CloseHandle(h);
}

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

// Growing arrays for one tile
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

static int s_fcap, s_pcap, s_ptcap, s_ncap;

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
        if (StrEq(cls, "minor") || StrEq(cls, "service"))
            return K_MINOR;
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

static void DecodeLayer(Pb b, Tile& t)
{
    // first pass: the name, keys, values (only strings matter here)
    Pb name = { 0, 0 };
    static Pb keys[512], vals[2048];   // (not on the stack: CE threads have little)
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
                        Push(t.parts, t.nparts, s_pcap, start);
                        Push(t.parts, t.nparts, s_pcap, t.npts / 2 - start);
                        ft.nparts++;
                    }
                    start = t.npts / 2;
                }
                short sx = (short)x, sy = (short)y;
                Push(t.pts, t.npts, s_ptcap, sx);
                Push(t.pts, t.npts, s_ptcap, sy);
                if (sx < ft.x0) ft.x0 = sx;
                if (sx > ft.x1) ft.x1 = sx;
                if (sy < ft.y0) ft.y0 = sy;
                if (sy > ft.y1) ft.y1 = sy;
            }
        }
        if (start >= 0 && t.npts / 2 - start > 0) {
            Push(t.parts, t.nparts, s_pcap, start);
            Push(t.parts, t.nparts, s_pcap, t.npts / 2 - start);
            ft.nparts++;
        }
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
                    Push(t.names, t.nnames, s_ncap, w[i]);
            }
        }
        Push(t.f, t.nf, s_fcap, ft);
    }
}

static void DecodeTile(const unsigned char* data, int len, Tile& t)
{
    s_fcap = s_pcap = s_ptcap = s_ncap = 0;
    Pb b = { data, data + len };
    while (b.p < b.end) {
        unsigned int tag = Varint(b);
        if ((tag >> 3) == 3 && (tag & 7) == 2)
            DecodeLayer(Sub(b), t);
        else
            Skip(b, tag & 7);
    }
}

// ---------------------------------------------------------------- the frame

static HBITMAP         s_bmp;
static unsigned short* s_bits;   // bottom-up rows
static int             s_stride;
static HFONT           s_font, s_fontBig;

static void MakeFrame()
{
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = W;
    bi.h.biHeight = H;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.masks[0] = 0xF800;
    bi.masks[1] = 0x07E0;
    bi.masks[2] = 0x001F;
    void* bits = NULL;
    HDC screen = GetDC(NULL);
    s_bmp = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    s_bits = (unsigned short*)bits;
    s_stride = ((W * 2 + 3) & ~3) / 2;
}

// The view: a tile point (px, py) -> screen in 1/16 px, fixed point (m in 16.16 of 1/16 px per tile unit).
struct Xf { int m11, m12, m21, m22, bx, by; };

static void MakeXf(const Tile& t, double zoom, double cx, double cy, double angle, Xf* x)
{
    // world pixels at the view's zoom: tile unit -> world px
    double s = pow(2.0, zoom - t.z) * 256.0 / kExtent;
    double a = angle * M_PI / 180, c = cos(a), sn = sin(a);
    double ox = t.x * 256.0 * pow(2.0, zoom - t.z) - cx, oy = t.y * 256.0 * pow(2.0, zoom - t.z) - cy;
    // screen = center + R(-angle) * world offset
    x->m11 = (int)(c * s * 16 * 65536);
    x->m12 = (int)(sn * s * 16 * 65536);
    x->m21 = (int)(-sn * s * 16 * 65536);
    x->m22 = (int)(c * s * 16 * 65536);
    x->bx = (int)((W / 2 + ox * c + oy * sn) * 16);
    x->by = (int)((H / 2 - ox * sn + oy * c) * 16);
}

static inline void Apply(const Xf& x, int px, int py, int* sx, int* sy)
{
    *sx = (int)(((__int64)x.m11 * px + (__int64)x.m12 * py) >> 16) + x.bx;
    *sy = (int)(((__int64)x.m21 * px + (__int64)x.m22 * py) >> 16) + x.by;
}

// ---- scanline filler, even-odd, pixel centres
struct Edge { int r0, r1; int x, dx; };   // rows r0..r1-1, x at r0's centre and per row, 16.16 px

static Edge* s_edges;
static int   s_edgeCap;
static int*  s_xs;

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
    if (r1 > H)
        r1 = H;
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
    e.dx = (int)(slope * 16 / 16);                              // per row (16 units of y), px: /16 * 16
    e.x = (int)((((__int64)x0 << 16) + slope * (r0 * 16 + 8 - y0)) / 16);
}

static int CmpEdge(const void* a, const void* b)
{
    return ((const Edge*)a)->r0 - ((const Edge*)b)->r0;
}

static void FillEdges(int ne, unsigned short color)
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
            int j = nx++;   // insertion sort: few crossings per row
            while (j > 0 && s_xs[j - 1] > x) {
                s_xs[j] = s_xs[j - 1];
                j--;
            }
            s_xs[j] = x;
            i++;
        }
        unsigned short* row = s_bits + (H - 1 - r) * s_stride;
        for (int i = 0; i + 1 < nx; i += 2) {
            int a = (s_xs[i] + 0x7FFF) >> 16, b = (s_xs[i + 1] + 0x7FFF) >> 16;   // centres inside [xa, xb)
            if (a < 0)
                a = 0;
            if (b > W)
                b = W;
            for (int x = a; x < b; x++)
                row[x] = color;
        }
    }
}

// ---- one frame from all tiles of a data zoom
struct Stats { int feats, polys, lines, labels; };

static bool BoxVisible(const Xf& x, const Feat& f, int margin)
{
    int xs[4], ys[4];
    Apply(x, f.x0, f.y0, &xs[0], &ys[0]);
    Apply(x, f.x1, f.y0, &xs[1], &ys[1]);
    Apply(x, f.x0, f.y1, &xs[2], &ys[2]);
    Apply(x, f.x1, f.y1, &xs[3], &ys[3]);
    int l = xs[0], r = xs[0], t = ys[0], b = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < l) l = xs[i];
        if (xs[i] > r) r = xs[i];
        if (ys[i] < t) t = ys[i];
        if (ys[i] > b) b = ys[i];
    }
    return r >= -margin * 16 && l <= (W + margin) * 16 && b >= -margin * 16 && t <= (H + margin) * 16;
}

static POINT* s_line;
static int    s_lineCap;

static void DrawFrame(HDC dc, int dataZ, double zoom, double angle, Stats* st)
{
    memset(st, 0, sizeof(*st));
#ifndef UNDER_CE
    GdiFlush();   // the desktop's GDI may still be drawing the last frame's lines into the pixels
#endif
    unsigned short bg = RGB565(0xf2, 0xef, 0xe9);
    for (int y = 0; y < H; y++) {
        unsigned short* row = s_bits + y * s_stride;
        for (int x = 0; x < W; x++)
            row[x] = bg;
    }
    double cx, cy;   // the view center in world px at zoom
    double n = pow(2.0, zoom) * 256;
    cx = (s_lon + 180) / 360 * n;
    double la = s_lat * M_PI / 180;
    cy = (1 - log(tan(la) + 1 / cos(la)) / M_PI) / 2 * n;
    Xf xf[64];
    int idx[64], nt = 0;
    for (int i = 0; i < s_ntiles && nt < 64; i++)
        if (s_tiles[i].z == dataZ) {
            MakeXf(s_tiles[i], zoom, cx, cy, angle, &xf[nt]);
            idx[nt++] = i;
        }
    // areas, kind by kind (all tiles), then the lines with their casings, then the labels
    for (int k = 0; k < kFirstLine; k++) {
        unsigned short c = RGB565(kStyle[k].r, kStyle[k].g, kStyle[k].b);
        for (int ti = 0; ti < nt; ti++) {
            const Tile& t = s_tiles[idx[ti]];
            for (int fi = 0; fi < t.nf; fi++) {
                const Feat& f = t.f[fi];
                if (f.kind != k || !BoxVisible(xf[ti], f, 0))
                    continue;
                st->feats++;
                st->polys++;
                int ne = 0;
                for (int pi = 0; pi < f.nparts; pi++) {
                    int p0 = t.parts[(f.part0 + pi) * 2], pn = t.parts[(f.part0 + pi) * 2 + 1];
                    int fx, fy, px, py;
                    Apply(xf[ti], t.pts[p0 * 2], t.pts[p0 * 2 + 1], &fx, &fy);
                    px = fx;
                    py = fy;
                    for (int q = 1; q < pn; q++) {
                        int x, y;
                        Apply(xf[ti], t.pts[(p0 + q) * 2], t.pts[(p0 + q) * 2 + 1], &x, &y);
                        AddEdge(ne, px, py, x, y);
                        px = x;
                        py = y;
                    }
                    AddEdge(ne, px, py, fx, fy);
                }
                FillEdges(ne, c);
            }
        }
    }
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (int pass = 0; pass < 2; pass++)
        for (int k = kFirstLine; k < K_PLACE; k++) {
            const Style& sty = kStyle[k];
            if (pass == 0 && !sty.cr)
                continue;
            int w = sty.width * (zoom >= 15 ? 2 : 1) + (pass == 0 ? 2 : 0);
            if (zoom < 14 && k <= K_MINOR)
                w = w > 2 ? w / 2 : w;
            HPEN pen = CreatePen(PS_SOLID, w, pass == 0 ? RGB(sty.cr, sty.cg, sty.cb) : RGB(sty.r, sty.g, sty.b));
            HGDIOBJ op = SelectObject(dc, pen);
            for (int ti = 0; ti < nt; ti++) {
                const Tile& t = s_tiles[idx[ti]];
                for (int fi = 0; fi < t.nf; fi++) {
                    const Feat& f = t.f[fi];
                    if (f.kind != k || !BoxVisible(xf[ti], f, w))
                        continue;
                    if (pass == 1) {
                        st->feats++;
                        st->lines++;
                    }
                    for (int pi = 0; pi < f.nparts; pi++) {
                        int p0 = t.parts[(f.part0 + pi) * 2], pn = t.parts[(f.part0 + pi) * 2 + 1];
                        if (pn > s_lineCap) {
                            free(s_line);
                            s_lineCap = pn * 2;
                            s_line = (POINT*)malloc(s_lineCap * sizeof(POINT));
                        }
                        int m = 0;
                        for (int q = 0; q < pn; q++) {
                            int x, y;
                            Apply(xf[ti], t.pts[(p0 + q) * 2], t.pts[(p0 + q) * 2 + 1], &x, &y);
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
            }
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    SelectObject(dc, oldBrush);
    SelectObject(dc, s_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x30, 0x30, 0x30));
    for (int ti = 0; ti < nt; ti++) {
        const Tile& t = s_tiles[idx[ti]];
        for (int fi = 0; fi < t.nf; fi++) {
            const Feat& f = t.f[fi];
            if (f.kind != K_PLACE || f.name < 0 || !BoxVisible(xf[ti], f, 0))
                continue;
            int x, y;
            Apply(xf[ti], f.x0, f.y0, &x, &y);
            const wchar_t* s = t.names + f.name;
            ExtTextOut(dc, x >> 4, y >> 4, 0, NULL, s, (UINT)wcslen(s), NULL);
            st->labels++;
        }
    }
}

// ---------------------------------------------------------------- the benchmark

struct Level { int dataZ; double zoom; };
static const Level kLevels[] = { { 12, 12 }, { 13, 13 }, { 14, 14 }, { 14, 15 }, { 14, 16 } };
static const int kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);
static int s_level = 2;
static wchar_t s_result[6][96];
static HWND s_wnd;
static DWORD s_readMs[20], s_decodeMs[20];
static int s_tileCount[20], s_tileBytes[20], s_featCount[20], s_ptCount[20];

static void LoadTiles()
{
    for (int z = 0; z < 20; z++) {
        wchar_t pat[MAX_PATH];
        _snwprintf(pat, MAX_PATH, L"%stiles\\%d\\*.pbf", s_dir, z);
        WIN32_FIND_DATA fd;
        HANDLE fh = FindFirstFile(pat, &fd);
        if (fh == INVALID_HANDLE_VALUE)
            continue;
        do {
            const wchar_t* us = wcschr(fd.cFileName, L'_');
            if (!us)
                continue;
            int x = _wtoi(fd.cFileName), y = _wtoi(us + 1);
            wchar_t path[MAX_PATH];
            _snwprintf(path, MAX_PATH, L"%stiles\\%d\\%s", s_dir, z, fd.cFileName);
            DWORD t0 = GetTickCount();
            HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            DWORD len = GetFileSize(h, NULL), got = 0;
            unsigned char* data = (unsigned char*)malloc(len ? len : 1);
            if (data)
                ReadFile(h, data, len, &got, NULL);
            CloseHandle(h);
            DWORD t1 = GetTickCount();
            if (!data)
                continue;
            Tile* nt = (Tile*)realloc(s_tiles, (s_ntiles + 1) * sizeof(Tile));
            if (!nt) {
                free(data);
                continue;
            }
            s_tiles = nt;
            Tile& t = s_tiles[s_ntiles++];
            memset(&t, 0, sizeof(t));
            t.z = z;
            t.x = x;
            t.y = y;
            DecodeTile(data, (int)got, t);
            free(data);
            s_readMs[z] += t1 - t0;
            s_decodeMs[z] += GetTickCount() - t1;
            s_tileCount[z]++;
            s_tileBytes[z] += got;
            s_featCount[z] += t.nf;
            s_ptCount[z] += t.npts / 2;
        } while (FindNextFile(fh, &fd));
        FindClose(fh);
        if (s_tileCount[z])
            Out("z%d: %d tiles, %d KB: read %lu ms, decode %lu ms; %d features kept, %d points", z,
                s_tileCount[z], s_tileBytes[z] / 1024, s_readMs[z], s_decodeMs[z], s_featCount[z], s_ptCount[z]);
    }
}

static void Bench()
{
    const Level& lv = kLevels[s_level];
    HDC wdc = GetDC(s_wnd);
    HDC dc = CreateCompatibleDC(wdc);
    HGDIOBJ ob = SelectObject(dc, s_bmp);
    Stats st;
    DWORD north = 0, sum = 0, mx = 0;
    const int kTurns = 12;
    for (int i = 0; i <= kTurns; i++) {
        DWORD t0 = GetTickCount();
        DrawFrame(dc, lv.dataZ, lv.zoom, i == 0 ? 0 : i * 30 - 15, &st);
        DWORD ms = GetTickCount() - t0;
        BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);   // shown, not timed
        if (i == 0)
            north = ms;
        else {
            sum += ms;
            if (ms > mx)
                mx = ms;
        }
    }
    DrawFrame(dc, lv.dataZ, lv.zoom, 0, &st);
    SelectObject(dc, ob);
    DeleteDC(dc);
    ReleaseDC(s_wnd, wdc);
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    int z = lv.dataZ;
    _snwprintf(s_result[0], 96, L"Масштаб %d (данные z%d, %d тайлов)", (int)lv.zoom, z, s_tileCount[z]);
    _snwprintf(s_result[1], 96, L"Чтение %lu мс, разбор %lu мс (%d КБ)", s_readMs[z], s_decodeMs[z],
               s_tileBytes[z] / 1024);
    _snwprintf(s_result[2], 96, L"Кадр: север %lu мс, поворот ср. %lu / макс. %lu мс", north, sum / kTurns, mx);
    _snwprintf(s_result[3], 96, L"В кадре: %d площадей, %d линий, %d подписей", st.polys, st.lines, st.labels);
    _snwprintf(s_result[4], 96, L"ОЗУ свободно %lu КБ, процессу %lu КБ", ms.dwAvailPhys / 1024,
               ms.dwAvailVirtual / 1024);
    wcscpy(s_result[5], L"справа - дальше, слева - назад, угол - выход");
    Out("zoom %d (z%d data): frame north %lu ms, turned avg %lu max %lu ms; %d areas, %d lines, %d labels; "
        "ram free %lu KB, proc %lu KB", (int)lv.zoom, z, north, sum / kTurns, mx, st.polys, st.lines, st.labels,
        ms.dwAvailPhys / 1024, ms.dwAvailVirtual / 1024);
}

static void Paint(HDC hdc)
{
    HDC dc = CreateCompatibleDC(hdc);
    HGDIOBJ ob = SelectObject(dc, s_bmp);
    RECT box = { 8, 8, 560, 8 + 6 * 22 + 12 };
    HBRUSH b = CreateSolidBrush(RGB(20, 22, 26));
    FillRect(dc, &box, b);
    DeleteObject(b);
    SelectObject(dc, s_fontBig);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    for (int i = 0; i < 6; i++)
        ExtTextOut(dc, 18, 14 + i * 22, 0, NULL, s_result[i], (UINT)wcslen(s_result[i]), NULL);
    RECT ex = { W - 64, 0, W, 56 };
    b = CreateSolidBrush(RGB(180, 40, 40));
    FillRect(dc, &ex, b);
    DeleteObject(b);
    ExtTextOut(dc, W - 40, 16, 0, NULL, L"X", 1, NULL);
    BitBlt(hdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteDC(dc);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x >= W - 64 && y < 56) {
            DestroyWindow(hwnd);
            return 0;
        }
        s_level = (s_level + (x >= W / 2 ? 1 : kLevelCount - 1)) % kLevelCount;
        Bench();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static HFONT Font(int h, int weight)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -h;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = NONANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, L"Tahoma");
    return CreateFontIndirect(&lf);
}

#ifdef UNDER_CE
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
#else
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
#endif
{
    GetModuleFileName(NULL, s_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(s_dir, L'\\');
    if (slash)
        slash[1] = 0;
    wchar_t ld[MAX_PATH];
#ifdef UNDER_CE
    bool logs = GetFileAttributes(L"\\SDMMC\\VestaLogs.off") == 0xFFFFFFFF;   // the one switch of our logs
#else
    bool logs = true;
#endif
    if (logs && LogDir(ld, s_dir))
        _snwprintf(s_log, MAX_PATH, L"%sVTileTest.txt", ld);
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    Out("---- VTileTest %04d-%02d-%02d %02d:%02d", tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute);
    s_xs = (int*)malloc(4096 * sizeof(int));

    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"VTileTest_Wnd";
    RegisterClass(&wc);
#ifdef UNDER_CE
    DWORD style = WS_POPUP | WS_VISIBLE;
    int x = 0, y = 0, w = W, h = H;
#else
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    RECT rc = { 0, 0, W, H };
    AdjustWindowRect(&rc, style, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = rc.right - rc.left, h = rc.bottom - rc.top;
#endif
    s_wnd = CreateWindowEx(0, wc.lpszClassName, L"VTileTest", style, x, y, w, h, NULL, NULL, inst, NULL);
    if (!s_wnd)
        return 1;
    s_font = Font(14, FW_BOLD);
    s_fontBig = Font(17, FW_NORMAL);
    MakeFrame();
    if (!s_bits) {
        Out("no frame memory");
        return 1;
    }
    HDC hdc = GetDC(s_wnd);
    RECT all = { 0, 0, W, H };
    FillRect(hdc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetTextColor(hdc, RGB(240, 240, 240));
    SetBkMode(hdc, TRANSPARENT);
    const wchar_t* wait = L"Чтение и разбор тайлов...";
    ExtTextOut(hdc, 20, 20, 0, NULL, wait, (UINT)wcslen(wait), NULL);
    ReleaseDC(s_wnd, hdc);
    LoadTiles();
    if (!s_ntiles) {
        wcscpy(s_result[0], L"Нет тайлов: tiles\\<z>\\<x>_<y>.pbf рядом с программой");
        Out("no tiles");
    } else {
        Bench();
    }
    InvalidateRect(s_wnd, NULL, FALSE);
    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}
