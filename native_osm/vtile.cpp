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
       K_WATERWAY, K_RAIL, K_PATH, K_SERVICE, K_MINOR, K_SECONDARY, K_PRIMARY, K_MAJOR, K_PLACE, K_ROADNAME, K_HOUSENUM, K_FUEL, K_PARKING, K_COUNT };
static const int kFirstLine = K_WATERWAY;

// The two palettes of design v3 (VestaHome/skin/make_osm_v3_mockup.py, 2026-10-26): the day warm and light, the night
// its own dark one (not the raster map's inverted lightness), roads without casings. K_PLACE, K_ROADNAME, K_HOUSENUM:
// the text, c* its halo; K_FUEL, K_PARKING: the sign's colour.
struct Style { unsigned char r, g, b; int width; bool cased; unsigned char cr, cg, cb; };   // width 0: area
static const Style kStyle[2][K_COUNT] = { {
    { 236, 232, 226, 0 }, { 234, 228, 232, 0 }, { 216, 234, 202, 0 }, { 198, 222, 186, 0 },
    { 206, 232, 196, 0 }, { 168, 204, 226, 0 }, { 224, 217, 208, 0 },
    { 168, 204, 226, 2 }, { 184, 180, 174, 2 }, { 206, 196, 184, 1 },
    { 255, 255, 255, 2, true, 214, 208, 200 }, { 255, 255, 255, 5, true, 214, 208, 200 },
    { 255, 247, 214, 7, true, 222, 204, 160 }, { 255, 228, 176, 8, true, 224, 182, 120 },
    { 255, 206, 150, 9, true, 214, 150, 90 },
    { 40, 44, 50, 0, true, 255, 255, 255 }, { 66, 70, 76, 0, true, 255, 255, 255 },
    { 120, 112, 104, 0, true, 243, 240, 235 },
    { 46, 110, 190, 0, true, 255, 255, 255 }, { 46, 110, 190, 0, true, 255, 255, 255 },
}, {
    { 22, 25, 30, 0 }, { 25, 25, 31, 0 }, { 20, 31, 25, 0 }, { 18, 33, 25, 0 },
    { 21, 37, 29, 0 }, { 13, 34, 50, 0 }, { 31, 35, 42, 0 },
    { 13, 34, 50, 2 }, { 48, 52, 58, 2 }, { 32, 36, 42, 1 },
    { 38, 43, 50, 2 }, { 44, 50, 58, 5 }, { 62, 70, 80, 7 }, { 84, 80, 68, 8 }, { 112, 88, 58, 9 },
    { 205, 208, 214, 0, true, 11, 12, 14 }, { 150, 156, 166, 0, true, 11, 12, 14 },
    { 110, 116, 126, 0, true, 17, 19, 23 },
    { 58, 118, 196, 0, true, 11, 12, 14 }, { 58, 118, 196, 0, true, 11, 12, 14 },
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
    int name;                   // K_PLACE, K_ROADNAME, K_HOUSENUM: into VTile::names, -1 none
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

// What a feature is drawn as, by its layer, class (NULL: any) and geometry type (1 point, 2 line, 3 area); the first
// row that matches wins, none: not drawn.
struct KindRow { const char* layer; const char* cls; unsigned char type, kind; };
static const KindRow kKinds[] = {
    { "water", NULL, 3, K_WATER }, { "building", NULL, 3, K_BUILDING }, { "park", NULL, 3, K_PARK },
    { "landcover", "wood", 3, K_WOOD }, { "landcover", NULL, 3, K_GRASS },
    { "landuse", "residential", 3, K_RESID }, { "landuse", "suburb", 3, K_RESID },
    { "landuse", "neighbourhood", 3, K_RESID },
    { "landuse", "industrial", 3, K_INDUSTRIAL }, { "landuse", "commercial", 3, K_INDUSTRIAL },
    { "landuse", "retail", 3, K_INDUSTRIAL }, { "landuse", "railway", 3, K_INDUSTRIAL },
    { "waterway", NULL, 2, K_WATERWAY },
    { "transportation", "motorway", 2, K_MAJOR }, { "transportation", "trunk", 2, K_MAJOR },
    { "transportation", "primary", 2, K_PRIMARY },
    { "transportation", "secondary", 2, K_SECONDARY }, { "transportation", "tertiary", 2, K_SECONDARY },
    { "transportation", "minor", 2, K_MINOR }, { "transportation", "service", 2, K_SERVICE },
    { "transportation", "track", 2, K_PATH }, { "transportation", "path", 2, K_PATH },
    { "transportation", "rail", 2, K_RAIL }, { "transportation", "transit", 2, K_RAIL },
    { "place", NULL, 1, K_PLACE }, { "transportation_name", NULL, 2, K_ROADNAME },
    { "housenumber", NULL, 1, K_HOUSENUM },
    { "poi", "fuel", 1, K_FUEL }, { "poi", "parking", 1, K_PARKING },   // for the driver: filling stations, car parks
};

static int Kind(const Pb& layer, const Pb& cls, int geomType)
{
    for (int i = 0; i < (int)(sizeof(kKinds) / sizeof(kKinds[0])); i++) {
        const KindRow& k = kKinds[i];
        if (k.type == geomType && StrEq(layer, k.layer) && (!k.cls || StrEq(cls, k.cls)))
            return k.kind;
    }
    return -1;
}

// The names' rank by kind and class (NULL: any; first match wins, none: 2); -1: a street not named (service roads,
// paths).
struct RankRow { unsigned char kind; const char* cls; signed char rank; };
static const RankRow kRanks[] = {
    { K_PLACE, "city", 0 }, { K_PLACE, "town", 0 },
    { K_PLACE, "suburb", 1 }, { K_PLACE, "village", 1 }, { K_PLACE, "quarter", 1 },
    { K_FUEL, NULL, 0 }, { K_PARKING, NULL, 1 },
    { K_ROADNAME, "motorway", 0 }, { K_ROADNAME, "trunk", 0 }, { K_ROADNAME, "primary", 0 },
    { K_ROADNAME, "secondary", 1 }, { K_ROADNAME, "tertiary", 1 }, { K_ROADNAME, "minor", 2 },
    { K_ROADNAME, NULL, -1 },
};

static int Rank(int kind, const Pb& cls)
{
    for (int i = 0; i < (int)(sizeof(kRanks) / sizeof(kRanks[0])); i++) {
        const RankRow& r = kRanks[i];
        if (r.kind == kind && (!r.cls || StrEq(cls, r.cls)))
            return r.rank;
    }
    return 2;
}

// A layer's name and the indexes of its "class" and name keys; its values in s_vals (only strings matter here)
struct LayerHead { Pb name; int nv, kClass, kName; };
static Pb s_keys[512], s_vals[2048];   // (not on the stack: CE threads have little; s_cs held)

static void Keep(Pb* a, int& n, int cap, const Pb& v)
{
    if (n < cap)
        a[n++] = v;
}

static Pb StringValue(Pb v)
{
    Pb sv = { 0, 0 };
    while (v.p < v.end) {
        unsigned int vt = Varint(v);
        if ((vt >> 3) == 1 && (vt & 7) == 2)
            sv = Sub(v);
        else
            Skip(v, vt & 7);
    }
    return sv;
}

static void ReadLayerHead(Pb scan, LayerHead& h)
{
    Pb none = { 0, 0 };
    int nk = 0;
    h.name = none;
    h.nv = 0;
    while (scan.p < scan.end) {
        unsigned int tag = Varint(scan);
        if ((tag & 7) != 2) {
            Skip(scan, tag & 7);
            continue;
        }
        Pb s = Sub(scan);
        int field = tag >> 3;
        if (field == 1)
            h.name = s;
        else if (field == 3)
            Keep(s_keys, nk, 512, s);
        else if (field == 4)
            Keep(s_vals, h.nv, 2048, StringValue(s));
    }
    const char* nameKey = StrEq(h.name, "housenumber") ? "housenumber" : "name";
    h.kClass = h.kName = -1;
    for (int i = 0; i < nk; i++) {
        if (StrEq(s_keys[i], "class"))
            h.kClass = i;
        else if (StrEq(s_keys[i], nameKey))
            h.kName = i;
    }
}

// A feature's fields: its tags, geometry type, geometry
static void ReadFeature(Pb f, Pb& tags, int& type, Pb& geom)
{
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
}

static void ReadTags(Pb tags, const LayerHead& h, Pb& cls, Pb& nm)
{
    while (tags.p < tags.end) {
        unsigned int k = Varint(tags), v = Varint(tags);
        if ((int)k == h.kClass && v < (unsigned)h.nv)
            cls = s_vals[v];
        else if ((int)k == h.kName && v < (unsigned)h.nv)
            nm = s_vals[v];
    }
}

// ---- geometry: MoveTo starts a part, LineTo adds points; false = out of memory

static void Grow(Feat& ft, short x, short y)   // the bounding box
{
    if (x < ft.x0) ft.x0 = x;
    if (x > ft.x1) ft.x1 = x;
    if (y < ft.y0) ft.y0 = y;
    if (y > ft.y1) ft.y1 = y;
}

static bool EndPart(VTile& t, Feat& ft, int start)
{
    bool ok = true;
    if (start >= 0 && t.npts / 2 - start > 0) {
        ok &= Push(t.parts, t.nparts, t.pcap, start);
        ok &= Push(t.parts, t.nparts, t.pcap, t.npts / 2 - start);
        ft.nparts++;
    }
    return ok;
}

static int Zigzag(unsigned int v)
{
    return (int)((v >> 1) ^ (0 - (v & 1)));
}

static bool DecodeGeometry(Pb geom, VTile& t, Feat& ft)
{
    int x = 0, y = 0, start = -1;
    bool ok = true;
    while (geom.p < geom.end) {
        unsigned int ci = Varint(geom);
        int cmd = ci & 7, count = ci >> 3;
        if (cmd == 7)
            continue;   // ClosePath: the filler closes rings itself
        for (int i = 0; i < count && geom.p < geom.end; i++) {
            x += Zigzag(Varint(geom));
            y += Zigzag(Varint(geom));
            if (cmd == 1) {   // MoveTo: a new part
                ok &= EndPart(t, ft, start);
                start = t.npts / 2;
            }
            short sx = (short)x, sy = (short)y;
            ok &= Push(t.pts, t.npts, t.ptcap, sx);
            ok &= Push(t.pts, t.npts, t.ptcap, sy);
            Grow(ft, sx, sy);
        }
    }
    ok &= EndPart(t, ft, start);
    return ok;
}

// ---- names

static void AddName(VTile& t, Feat& ft, const Pb& nm)   // UTF-8 -> the tile's names, with its terminating 0
{
    wchar_t w[64];
    char u[128];
    int n = (int)(nm.end - nm.p);
    n = n > 127 ? 127 : n;
    memcpy(u, nm.p, n);
    u[n] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, u, -1, w, 64)) {
        w[63] = 0;
        ft.name = t.nnames;
        for (int i = 0; i == 0 || w[i - 1]; i++)
            Push(t.names, t.nnames, t.ncap, w[i]);
    }
}

// The feature's name (a sign without one: an empty one); false = a street or house number without a name: dropped.
static bool Named(VTile& t, Feat& ft, int kind, const Pb& nm)
{
    if (kind < K_PLACE)
        return true;
    if (nm.p)
        AddName(t, ft, nm);
    else if (kind >= K_FUEL) {
        ft.name = t.nnames;
        Push(t.names, t.nnames, t.ncap, (wchar_t)0);
    }
    return ft.name >= 0 || (kind != K_ROADNAME && kind != K_HOUSENUM);
}

// One feature into the tile (or skipped); false = out of memory
static bool DecodeFeature(Pb f, const LayerHead& h, VTile& t)
{
    Pb tags = { 0, 0 }, geom = { 0, 0 }, cls = { 0, 0 }, nm = { 0, 0 };
    int type = 0;
    ReadFeature(f, tags, type, geom);
    ReadTags(tags, h, cls, nm);
    int kind = Kind(h.name, cls, type);
    int rank = kind < 0 ? -1 : Rank(kind, cls);
    if (rank < 0)
        return true;
    Feat ft;
    ft.kind = (unsigned char)kind;
    ft.nparts = 0;
    ft.part0 = t.nparts / 2;
    ft.x0 = ft.y0 = 32767;
    ft.x1 = ft.y1 = -32768;
    ft.name = -1;
    ft.rank = (unsigned char)rank;
    if (!DecodeGeometry(geom, t, ft))
        return false;
    if (!ft.nparts || !Named(t, ft, kind, nm))
        return true;
    return Push(t.f, t.nf, t.fcap, ft);
}

static bool DecodeLayer(Pb b, VTile& t)
{
    LayerHead h;
    ReadLayerHead(b, h);
    bool ok = true;
    while (ok && b.p < b.end) {
        unsigned int tag = Varint(b);
        if ((tag >> 3) == 2 && (tag & 7) == 2)
            ok = DecodeFeature(Sub(b), h, t);
        else
            Skip(b, tag & 7);
    }
    return ok;
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

// A recent entry (s_parsed, s_labels) of this key, its use time renewed; NULL: none, oldest = the one to reuse.
template <class T> static T* Recent(T* a, int n, const MapRequest& key, T*& oldest)
{
    oldest = &a[0];
    for (int i = 0; i < n; i++) {
        if (a[i].useTick && SameKey(a[i].key, key)) {
            a[i].useTick = GetTickCount();
            return &a[i];
        }
        if (a[i].useTick < oldest->useTick)
            oldest = &a[i];
    }
    return NULL;
}

static void ClearSet(VLabelSet* set)
{
    free(set->l);
    free(set->names);
    memset(set, 0, sizeof(*set));
}

// Room for the tile's named features and a copy of its names; false = out of memory (the set left empty)
static bool AllocSet(VLabelSet* set, const VTile& t)
{
    int n = 0;
    for (int i = 0; i < t.nf; i++)
        if (t.f[i].name >= 0)
            n++;
    set->l = (VLabel*)malloc((n ? n : 1) * sizeof(VLabel));
    set->names = (wchar_t*)malloc((t.nnames ? t.nnames : 1) * sizeof(wchar_t));
    if (!set->l || !set->names) {
        ClearSet(set);
        return false;
    }
    memcpy(set->names, t.names, t.nnames * sizeof(wchar_t));
    set->nn = t.nnames;
    return true;
}

// A street's label at the middle of its longest segment; false = no segment
static bool LongestSegment(const VTile& t, const Feat& f, VLabel& l)
{
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
    return best > 0;
}

static bool InTile(const VLabel& l)
{
    return l.x >= 0 && l.y >= 0 && l.x < kExtent && l.y < kExtent;
}

// A named feature's label: a place or a sign at its point, a street along its longest segment; false = none here
static bool LabelOf(const VTile& t, const Feat& f, VLabel& l)
{
    l.kind = f.kind;
    l.rank = f.rank;
    l.name = f.name;
    l.dx = l.dy = 0;
    l.x = f.x0;
    l.y = f.y0;
    if (f.kind == K_ROADNAME && !LongestSegment(t, f, l))
        return false;
    return InTile(l);
}

static void LabelsFrom(const VTile& t)
{
    VLabelSet* set;
    if (Recent(s_labels, kLabelSets, t.key, set))
        return;
    ClearSet(set);
    if (!AllocSet(set, t))
        return;
    for (int i = 0; i < t.nf; i++) {
        VLabel l;
        if (t.f[i].name >= 0 && LabelOf(t, t.f[i], l))
            set->l[set->n++] = l;
    }
    set->key = t.key;
    set->useTick = GetTickCount();
}

static bool DecodeTile(VTile& t, const unsigned char* data, int len)   // false = out of memory
{
    Pb b = { data, data + len };
    bool ok = true;
    while (ok && b.p < b.end) {
        unsigned int tag = Varint(b);
        if ((tag >> 3) == 3 && (tag & 7) == 2)
            ok = DecodeLayer(Sub(b), t);
        else
            Skip(b, tag & 7);
    }
    return ok;
}

// The data tile parsed (from the recent ones, or now); NULL = bad data / out of memory.
static VTile* Parsed(const MapRequest& key, const unsigned char* data, int len)
{
    VTile* slot;
    VTile* t = Recent(s_parsed, kParsed, key, slot);
    if (t || !data)
        return t;
    FreeParsed(*slot);
    if (!DecodeTile(*slot, data, len)) {
        Log("vtile: out of memory parsing %d/%d/%d", key.z - VEC_ZBASE, key.x, key.y);
        FreeParsed(*slot);
        return NULL;
    }
    slot->key = key;
    slot->useTick = GetTickCount();
    LabelsFrom(*slot);
    return slot;
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

static int s_act[4096];   // the edges crossing the row
static int s_nact;

static int LastRow(int ne)   // past the last row of the edges
{
    int rmax = 0;
    for (int i = 0; i < ne; i++)
        if (s_edges[i].r1 > rmax)
            rmax = s_edges[i].r1;
    return rmax;
}

static int Activate(int next, int ne, int r)   // the edges starting at row r (sorted by r0); the next one after
{
    while (next < ne && s_edges[next].r0 == r) {
        if (s_nact < 4096)
            s_act[s_nact++] = next;
        next++;
    }
    return next;
}

static void InsertSorted(int& nx, int x)   // insertion sort: few crossings per row
{
    if (nx >= 1024)
        return;
    int j = nx++;
    while (j > 0 && s_xs[j - 1] > x) {
        s_xs[j] = s_xs[j - 1];
        j--;
    }
    s_xs[j] = x;
}

static int Crossings(int r)   // row r's crossings into s_xs, sorted; the edges past it dropped; their count
{
    int nx = 0;
    for (int i = 0; i < s_nact;) {
        Edge& e = s_edges[s_act[i]];
        if (e.r1 <= r) {
            s_act[i] = s_act[--s_nact];
            continue;
        }
        InsertSorted(nx, e.x);
        e.x += e.dx;
        i++;
    }
    return nx;
}

static void FillRow(unsigned short* row, int nx, unsigned short color)   // even-odd between the crossings
{
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

static void FillEdges(unsigned short* bits, int ne, unsigned short color)
{
    if (!ne)
        return;
    qsort(s_edges, ne, sizeof(Edge), CmpEdge);
    int rmax = LastRow(ne), next = 0;
    s_nact = 0;
    for (int r = s_edges[0].r0; r < rmax; r++) {
        next = Activate(next, ne, r);
        FillRow(bits + (TILE - 1 - r) * TILE, Crossings(r), color);   // bottom-up rows, 512 bytes each
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

// One part of an area as edges: its ring, closed
static void AddRing(int& ne, const VTile& t, const Xf& xf, int p0, int pn)
{
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

// The areas, kind by kind (the later over the earlier)
static void FillAreas(const VTile& t, const Xf& xf, int z, const Style* pal, unsigned short* bits)
{
    for (int k = 0; k < kFirstLine; k++) {
        if (z < MinZoom(k))
            continue;
        unsigned short c = RGB565(pal[k].r, pal[k].g, pal[k].b);
        for (int fi = 0; fi < t.nf; fi++) {
            const Feat& f = t.f[fi];
            if (f.kind != k || !BoxVisible(xf, f, 0))
                continue;
            int ne = 0;
            for (int pi = 0; pi < f.nparts; pi++)
                AddRing(ne, t, xf, t.parts[(f.part0 + pi) * 2], t.parts[(f.part0 + pi) * 2 + 1]);
            FillEdges(bits, ne, c);
        }
    }
}

static bool LineRoom(int pn)   // s_line holds pn points
{
    if (pn <= s_lineCap)
        return true;
    POINT* nl = (POINT*)realloc(s_line, pn * 2 * sizeof(POINT));
    if (!nl)
        return false;
    s_line = nl;
    s_lineCap = pn * 2;
    return true;
}

// One part of a line in whole pixels, points closer than 2 px to the last one left out (but the end)
static void StrokePart(HDC dc, const VTile& t, const Xf& xf, int p0, int pn)
{
    if (!LineRoom(pn))
        return;
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

// The lines of kind k with a pen w px wide
static void StrokeKind(HDC dc, const VTile& t, const Xf& xf, int k, int w, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, w, color);
    HGDIOBJ op = SelectObject(dc, pen);
    for (int fi = 0; fi < t.nf; fi++) {
        const Feat& f = t.f[fi];
        if (f.kind != k || !BoxVisible(xf, f, w))
            continue;
        for (int pi = 0; pi < f.nparts; pi++)
            StrokePart(dc, t, xf, t.parts[(f.part0 + pi) * 2], t.parts[(f.part0 + pi) * 2 + 1]);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

// All the lines, kind by kind: the casings (2 px wider, of the cased kinds), or the lines themselves
static void StrokeLines(HDC dc, const VTile& t, const Xf& xf, int z, const Style* pal, bool casing)
{
    for (int k = kFirstLine; k < K_PLACE; k++) {
        const Style& sty = pal[k];
        if ((casing && !sty.cased) || z < MinZoom(k))
            continue;
        if (casing)
            StrokeKind(dc, t, xf, k, LineWidth(sty.width, z) + 2, RGB(sty.cr, sty.cg, sty.cb));
        else
            StrokeKind(dc, t, xf, k, LineWidth(sty.width, z), RGB(sty.r, sty.g, sty.b));
    }
}

static void Draw(const VTile& t, const Xf& xf, int z, bool night, HDC dc, unsigned short* bits)
{
    const Style* pal = kStyle[night ? 1 : 0];
    const unsigned char* bc = kBg[night ? 1 : 0];
    unsigned short bg = RGB565(bc[0], bc[1], bc[2]);
    for (int i = 0; i < TILE * TILE; i++)
        bits[i] = bg;
    // areas, kind by kind, then the lines with their casings (the labels: over the frame, VLabelsDraw)
    FillAreas(t, xf, z, pal, bits);
#ifndef UNDER_CE
    GdiFlush();   // the desktop's GDI may batch: our pixels before its lines (CE draws at once)
#endif
    StrokeLines(dc, t, xf, z, pal, true);
    StrokeLines(dc, t, xf, z, pal, false);
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
static HFONT s_houseFont;
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

// One VLabelsDraw: the view (world point (vx, vy) at the screen's (cx, cy), turned by rc, rs = cos, sin), what is
// placed so far (s_placed[0..np), at most cap)
struct LabelView {
    HDC dc;
    const RECT* mr;             // a name only whole on the map
    double cx, cy, vx, vy, rc, rs, unit;
    int z;
    const Style* pal;
    HGDIOBJ font0;              // the dc's own font, put back at the end
    int np, cap;
};
static Placed s_placed[16 + 400];

static bool OnMap(const RECT& mr, const Placed& box)
{
    return box.l >= mr.left && box.t >= mr.top && box.r <= mr.right && box.b <= mr.bottom;
}

static bool Overlap(const Placed& a, const Placed& b)
{
    return b.l < a.r && b.r > a.l && b.t < a.b && b.b > a.t;
}

static bool Free(const LabelView& v, const Placed& box)
{
    if (!OnMap(*v.mr, box))
        return false;
    for (int i = 0; i < v.np; i++)
        if (Overlap(s_placed[i], box))
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

// A filling station (a pump in a disc) or a car park ("P" in a rounded square), 20 px, centred on (x, y).
static void PoiSign(HDC dc, int x, int y, int kind, const Style& st)
{
    HBRUSH br = CreateSolidBrush(RGB(st.r, st.g, st.b));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    if (kind == K_FUEL)
        Ellipse(dc, x - 10, y - 10, x + 10, y + 10);
    else
        RoundRect(dc, x - 9, y - 9, x + 10, y + 10, 6, 6);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    if (kind == K_FUEL) {   // the pump: a body with its window, the hose to the right
        RECT body = { x - 5, y - 6, x + 2, y + 7 };
        FillRect(dc, &body, white);
        HBRUSH bb = CreateSolidBrush(RGB(st.r, st.g, st.b));
        RECT win = { x - 4, y - 5, x + 1, y - 1 };
        FillRect(dc, &win, bb);
        DeleteObject(bb);
        RECT hose = { x + 3, y - 3, x + 5, y + 4 };
        FillRect(dc, &hose, white);
        RECT top = { x + 2, y - 4, x + 4, y - 2 };
        FillRect(dc, &top, white);
    } else {   // "P" of 2 px strokes
        RECT stem = { x - 4, y - 6, x - 1, y + 7 };
        FillRect(dc, &stem, white);
        RECT t = { x - 1, y - 6, x + 3, y - 4 };
        FillRect(dc, &t, white);
        RECT r = { x + 3, y - 5, x + 5, y };
        FillRect(dc, &r, white);
        RECT b = { x - 1, y, x + 3, y + 2 };
        FillRect(dc, &b, white);
    }
    DeleteObject(white);
}

// The label passes in drawing order: places (the biggest first), signs, streets, house numbers; each from its zoom
// (car parks from z17: PC 2026-10-27, many at z16; neighbourhoods and the small streets close up only)
struct LabelPass { unsigned char kind, rank, minZoom; };
static const LabelPass kPasses[] = {
    { K_PLACE, 0, 0 }, { K_PLACE, 1, 0 }, { K_PLACE, 2, 14 },
    { K_FUEL, 0, 14 }, { K_PARKING, 1, 17 },
    { K_ROADNAME, 0, 14 }, { K_ROADNAME, 1, 14 }, { K_ROADNAME, 2, 15 },
    { K_HOUSENUM, 2, 17 },
};

static HFONT Cached(HFONT& f, int height, int weight, int tenths)   // created when first needed
{
    if (!f)
        f = LabelFont(height, weight, tenths);
    return f;
}

static HFONT PassFont(int kind, int rank)
{
    static const int kPlaceHeight[3] = { 17, 15, 13 };
    if (kind == K_PLACE)
        return Cached(s_placeFont[rank], kPlaceHeight[rank], rank < 2 ? FW_BOLD : FW_NORMAL, 0);
    if (kind == K_ROADNAME)
        return Cached(s_streetFont[18], 13, FW_NORMAL, 0);
    return Cached(s_houseFont, 11, FW_NORMAL, 0);   // house numbers, the stations' names
}

static HFONT StreetFont(int bucket)
{
    return Cached(s_streetFont[bucket + 18], 13, FW_NORMAL, bucket * 50);
}

static void AddAvoid(LabelView& v, const RECT* avoid, int navoid)   // taken by the controls
{
    for (int i = 0; i < navoid && i < 16; i++) {
        Placed& p = s_placed[v.np++];
        p.l = avoid[i].left;
        p.t = avoid[i].top;
        p.r = avoid[i].right;
        p.b = avoid[i].bottom;
        p.name = NULL;   // (no name: never "the same street")
        p.cx = p.cy = 0;
    }
}

// The label's screen point (x, y); false = too far off the map
static bool OnScreen(const LabelView& v, const VLabelSet& set, const VLabel& lb, double& x, double& y)
{
    double ox = (double)set.key.x * kExtent * v.unit, oy = (double)set.key.y * kExtent * v.unit;
    double dx = ox + lb.x * v.unit - v.vx, dy = oy + lb.y * v.unit - v.vy;
    x = v.cx + dx * v.rc + dy * v.rs;
    y = v.cy - dx * v.rs + dy * v.rc;
    const RECT& mr = *v.mr;
    return x >= mr.left - 60 && x <= mr.right + 60 && y >= mr.top - 20 && y <= mr.bottom + 20;
}

static bool SignNamed(const LabelView& v, int kind, int n)   // a station's name under its sign from z16
{
    return n && kind == K_FUEL && v.z >= 16;
}

static bool SignLabel(LabelView& v, int kind, const wchar_t* s, int n, const SIZE& sz, double x, double y,
                      Placed& box)
{
    bool named = SignNamed(v, kind, n);
    int w = 20, below = 0;
    if (named) {
        below = sz.cy;
        if (sz.cx > 20)
            w = sz.cx;
    }
    box.l = (int)x - w / 2 - 1;
    box.t = (int)y - 11;
    box.r = box.l + w + 2;
    box.b = (int)y + 11 + below;
    box.name = NULL;
    if (!Free(v, box))
        return false;
    PoiSign(v.dc, (int)x, (int)y, kind, v.pal[kind]);
    if (named)
        Halo(v.dc, (int)x - sz.cx / 2, (int)y + 11, s, n, v.pal[K_ROADNAME]);
    return true;
}

// A place's name (with its halo) or a house number (many, small - no halo: 9 draws each would be slow on the unit)
static bool TextLabel(LabelView& v, int kind, const wchar_t* s, int n, const SIZE& sz, double x, double y,
                      Placed& box)
{
    box.l = (int)x - sz.cx / 2 - 2;
    box.t = (int)y - sz.cy / 2 - 1;
    box.r = box.l + sz.cx + 4;
    box.b = box.t + sz.cy + 2;
    if (!Free(v, box))
        return false;
    if (kind == K_PLACE)
        Halo(v.dc, box.l + 2, box.t + 1, s, n, v.pal[K_PLACE]);
    else {
        SetTextColor(v.dc, RGB(v.pal[K_HOUSENUM].r, v.pal[K_HOUSENUM].g, v.pal[K_HOUSENUM].b));
        ExtTextOut(v.dc, box.l + 2, box.t + 1, 0, NULL, s, (UINT)n, NULL);
    }
    return true;
}

// The road's screen direction (sx, sy) upright (never upside down), in 5 degree steps counterclockwise: -18..18
static int UprightBucket(double sx, double sy)
{
    if (sx < 0 || (sx == 0 && sy > 0)) {
        sx = -sx;
        sy = -sy;
    }
    double th = atan2(-sy, sx) * 180.0 / M_PI;   // counterclockwise, -90..90
    int bucket = (int)floor(th / 5.0 + 0.5);
    return bucket < -18 ? -18 : bucket > 18 ? 18 : bucket;
}

static void Span(const double* v, double& lo, double& hi)   // of 4 values
{
    lo = hi = v[0];
    for (int k = 1; k < 4; k++) {
        if (v[k] < lo) lo = v[k];
        if (v[k] > hi) hi = v[k];
    }
}

// The text of size sz centred on (x, y) turned by the bucket's angle: its origin (tx, ty), its screen box
static void TurnedBox(double x, double y, const SIZE& sz, int bucket, Placed& box, double& tx, double& ty)
{
    double a = bucket * 5.0 * M_PI / 180.0, ca = cos(a), sa = sin(a);
    // the text's direction (ca, -sa), its "down" (sa, ca); the origin is its top left corner
    double w = sz.cx, h = sz.cy;
    tx = x - ca * w / 2 - sa * h / 2;
    ty = y + sa * w / 2 - ca * h / 2;
    double xs[4] = { tx, tx + ca * w, tx + sa * h, tx + ca * w + sa * h };
    double ys[4] = { ty, ty - sa * w, ty + ca * h, ty - sa * w + ca * h };
    double l, r, t, b;
    Span(xs, l, r);
    Span(ys, t, b);
    box.l = (int)l - 1;
    box.t = (int)t - 1;
    box.r = (int)r + 1;
    box.b = (int)b + 1;
}

static bool SameStreetNear(const LabelView& v, const Placed& box)   // its name already placed within 300 px
{
    bool again = false;
    for (int k = 0; k < v.np && !again; k++)
        again = s_placed[k].name && !wcscmp(s_placed[k].name, box.name) &&
                abs(s_placed[k].cx - box.cx) + abs(s_placed[k].cy - box.cy) < 300;
    return again;
}

// A street's name along its road, upright
static bool StreetLabel(LabelView& v, const VLabel& lb, const wchar_t* s, int n, const SIZE& sz, double x, double y,
                        Placed& box)
{
    double sx = lb.dx * v.rc + lb.dy * v.rs, sy = -lb.dx * v.rs + lb.dy * v.rc;   // the road on the screen
    double len = sqrt(sx * sx + sy * sy);
    if (len * v.unit < sz.cx + 12)
        return false;   // the segment is shorter than the name
    int bucket = UprightBucket(sx, sy);
    double tx, ty;
    TurnedBox(x, y, sz, bucket, box, tx, ty);
    if (SameStreetNear(v, box) || !Free(v, box))
        return false;
    SelectObject(v.dc, StreetFont(bucket));
    Halo(v.dc, (int)floor(tx + 0.5), (int)floor(ty + 0.5), s, n, v.pal[K_ROADNAME]);
    return true;
}

// One label at its screen point (x, y), unless it would cover another
static void PlaceLabel(LabelView& v, int kind, HFONT pf, const VLabelSet& set, const VLabel& lb, double x, double y)
{
    const wchar_t* s = set.names + lb.name;
    int n = (int)wcslen(s);
    HGDIOBJ prev = SelectObject(v.dc, pf);
    if (!v.font0)
        v.font0 = prev;
    SIZE sz;
    GetTextExtentPoint32(v.dc, s, n, &sz);
    Placed box;
    box.name = s;
    box.cx = (int)x;
    box.cy = (int)y;
    bool shown;
    if (kind == K_FUEL || kind == K_PARKING)
        shown = SignLabel(v, kind, s, n, sz, x, y, box);
    else if (kind == K_ROADNAME)
        shown = StreetLabel(v, lb, s, n, sz, x, y, box);
    else
        shown = TextLabel(v, kind, s, n, sz, x, y, box);
    if (shown)
        s_placed[v.np++] = box;
}

static void DrawSet(LabelView& v, const LabelPass& p, HFONT pf, const VLabelSet& set)   // one tile's labels of a pass
{
    for (int li = 0; li < set.n && v.np < v.cap; li++) {
        const VLabel& lb = set.l[li];
        double x, y;
        if (lb.rank == p.rank && lb.kind == p.kind && OnScreen(v, set, lb, x, y))
            PlaceLabel(v, p.kind, pf, set, lb, x, y);
    }
}

static void DrawPass(LabelView& v, const LabelPass& p, int zd)   // the labels of a pass, of all tiles of zoom zd
{
    HFONT pf = PassFont(p.kind, p.rank);
    for (int si = 0; si < kLabelSets && v.np < v.cap; si++) {
        const VLabelSet& set = s_labels[si];
        if (set.useTick && set.key.z == VEC_ZBASE + zd)
            DrawSet(v, p, pf, set);
    }
}

static void LogLabelTime(DWORD ms, int shown)   // for the unit's log: turned text may be slow there (every 30 frames)
{
    static DWORD s_ms, s_max;
    static int s_frames, s_shown;
    s_ms += ms;
    s_max = ms > s_max ? ms : s_max;
    s_shown += shown;
    if (++s_frames == 30) {
        Log("vtile: labels %d per frame, %lu ms avg, %lu max", s_shown / 30, s_ms / 30, s_max);
        s_ms = s_max = 0;
        s_frames = s_shown = 0;
    }
}

// The names of the view's data tiles over the frame: (cx, cy) the screen point of the view's world point (vx, vy) at
// zoom z, the map turned by angle (degrees, as DrawRoute). Places first (the biggest first), then streets along their
// road, upright; none over another or over avoid (the car), a street's name once per 300 px.
void VLabelsDraw(HDC dc, const RECT& mr, double cx, double cy, double vx, double vy, double angle, int z,
                 const RECT* avoid, int navoid)
{
    int zd = z > VEC_MAXZ ? VEC_MAXZ : z;
    if (zd < 10)
        return;
    DWORD t0 = GetTickCount();
    double ra = angle * M_PI / 180.0;
    LabelView v;
    v.dc = dc;
    v.mr = &mr;
    v.cx = cx;
    v.cy = cy;
    v.vx = vx;
    v.vy = vy;
    v.rc = cos(ra);
    v.rs = sin(ra);
    v.unit = (double)(1 << (z - zd)) * TILE / kExtent;   // world px per tile unit
    v.z = z;
    v.pal = kStyle[g_cfg.night ? 1 : 0];
    v.font0 = NULL;
    v.np = 0;
    AddAvoid(v, avoid, navoid);
    v.cap = v.np + (z >= 17 ? 400 : 160);   // (close up: the house numbers too)
    SetBkMode(dc, TRANSPARENT);
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < (int)(sizeof(kPasses) / sizeof(kPasses[0])); i++)
        if (z >= kPasses[i].minZoom)
            DrawPass(v, kPasses[i], zd);
    LeaveCriticalSection(&s_cs);
    if (v.font0)
        SelectObject(dc, v.font0);
    LogLabelTime(GetTickCount() - t0, v.np - navoid);
}
