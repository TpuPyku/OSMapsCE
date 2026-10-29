// The vector tiles (vtile.cpp) on hand-made Mapbox Vector Tiles: water in the top left quarter, a primary road
// across at 3/4 of the height, a town's point; a town's tile with every kind of area, road, street name and sign.
// Drawn as OSMapsCE draws them (VTileRender, VLabelsDraw), checked by the pixels the driver would see. Exit code: the number of failed checks. (test_vtile.bat builds and runs it.)
#include "yamaps.h"
#include <stdio.h>

Config  g_cfg;
wchar_t g_dir[MAX_PATH];

void Log(const char*, ...) {}
const wchar_t* MapFace() { return L"Tahoma"; }
bool SameKey(const MapRequest& a, const MapRequest& b)
{
    return a.src == b.src && a.z == b.z && a.x == b.x && a.y == b.y;
}

// The local data VTileLocal reads: a region's pack's tiles, the cache's file (one), each read counted
enum { kFakePack = 2 };
static MapRequest           s_packKey[kFakePack];
static const unsigned char* s_packData[kFakePack];
static int                  s_packLen[kFakePack], s_nPack, s_reads;
static wchar_t              s_file[32];
static const unsigned char* s_fileData;
static int                  s_fileLen;

static bool Give(const unsigned char* from, int n, unsigned char** data, int* len)
{
    *data = (unsigned char*)malloc(n);
    memcpy(*data, from, n);
    *len = n;
    s_reads++;
    return true;
}

bool VPackTile(const MapRequest& d, unsigned char** data, int* len)
{
    for (int i = 0; i < s_nPack; i++)
        if (SameKey(s_packKey[i], d))
            return Give(s_packData[i], s_packLen[i], data, len);
    return false;
}

void CachePath(wchar_t* out, const MapRequest& r)
{
    _snwprintf(out, MAX_PATH, L"%d/%d/%d", r.z, r.x, r.y);
}

bool ReadWholeFile(const wchar_t* path, unsigned char** data, int* len)
{
    return s_file[0] && !wcscmp(path, s_file) && Give(s_fileData, s_fileLen, data, len);
}

static int s_failed, s_checks;

static void Check(bool ok, const char* what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

// ---- a protobuf writer (only what an MVT needs)
struct Pbw { unsigned char b[4096]; int n; };

static void Var(Pbw& w, unsigned int v)
{
    while (v >= 0x80) {
        w.b[w.n++] = (unsigned char)(v | 0x80);
        v >>= 7;
    }
    w.b[w.n++] = (unsigned char)v;
}

static void Bytes(Pbw& w, int field, const void* p, int len)
{
    Var(w, field << 3 | 2);
    Var(w, len);
    memcpy(w.b + w.n, p, len);
    w.n += len;
}

static void Str(Pbw& w, int field, const char* s)
{
    Bytes(w, field, s, (int)strlen(s));
}

static unsigned int Zz(int v)
{
    return (unsigned int)((v << 1) ^ (v >> 31));
}

// a feature: tags (key, value index pairs), type, the geometry's commands
static void Feature(Pbw& layer, const unsigned int* tags, int ntags, int type, const unsigned int* geom, int ngeom)
{
    Pbw f = { { 0 }, 0 }, t = { { 0 }, 0 }, g = { { 0 }, 0 };
    for (int i = 0; i < ntags; i++)
        Var(t, tags[i]);
    for (int i = 0; i < ngeom; i++)
        Var(g, geom[i]);
    Bytes(f, 2, t.b, t.n);
    Var(f, 3 << 3);
    Var(f, type);
    Bytes(f, 4, g.b, g.n);
    Bytes(layer, 2, f.b, f.n);
}

static void Value(Pbw& layer, const char* s)
{
    Pbw v = { { 0 }, 0 };
    Str(v, 1, s);
    Bytes(layer, 4, v.b, v.n);
}

static int MakeTile(unsigned char* out)
{
    Pbw tile = { { 0 }, 0 };
    Pbw water = { { 0 }, 0 };
    Str(water, 1, "water");
    unsigned int sq[] = { 9, Zz(0), Zz(0), 2 | 3 << 3, Zz(2048), Zz(0), Zz(0), Zz(2048), Zz(-2048), Zz(0), 15 };
    Feature(water, NULL, 0, 3, sq, sizeof(sq) / 4);
    Bytes(tile, 3, water.b, water.n);

    Pbw road = { { 0 }, 0 };
    Str(road, 1, "transportation");
    Str(road, 3, "class");
    Value(road, "primary");
    unsigned int rt[] = { 0, 0 };
    unsigned int ln[] = { 9, Zz(0), Zz(3072), 2 | 1 << 3, Zz(4096), Zz(0) };
    Feature(road, rt, 2, 2, ln, sizeof(ln) / 4);
    Bytes(tile, 3, road.b, road.n);

    Pbw place = { { 0 }, 0 };
    Str(place, 1, "place");
    Str(place, 3, "class");
    Str(place, 3, "name");
    Value(place, "town");
    Value(place, "Town");
    unsigned int pt[] = { 0, 0, 1, 1 };
    unsigned int pg[] = { 9, Zz(1024), Zz(3584) };
    Feature(place, pt, 4, 1, pg, sizeof(pg) / 4);
    Bytes(tile, 3, place.b, place.n);
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

// ---- a layer with the keys "class" and a name key; each feature brings its own values
struct Layer { Pbw w; int nv; };

static void Begin(Layer& l, const char* name, const char* nameKey)
{
    l.w.n = 0;
    l.nv = 0;
    Str(l.w, 1, name);
    Str(l.w, 3, "class");
    Str(l.w, 3, nameKey);
}

static void Add(Layer& l, const char* cls, const char* nm, int type, const unsigned int* geom, int ngeom)
{
    unsigned int tags[4];
    int nt = 0;
    if (cls) {
        Value(l.w, cls);
        tags[nt++] = 0;
        tags[nt++] = l.nv++;
    }
    if (nm) {
        Value(l.w, nm);
        tags[nt++] = 1;
        tags[nt++] = l.nv++;
    }
    Feature(l.w, tags, nt, type, geom, ngeom);
}

static void End(Pbw& tile, Layer& l)
{
    Bytes(tile, 3, l.w.b, l.w.n);
}

// geometries in the z14 tile's pixels (16 units each)
static int Box(unsigned int* g, int x0, int y0, int x1, int y1)
{
    unsigned int b[] = { 9, Zz(x0 * 16), Zz(y0 * 16), 2 | 3 << 3, Zz((x1 - x0) * 16), 0, 0, Zz((y1 - y0) * 16),
                         Zz((x0 - x1) * 16), 0, 15 };
    memcpy(g, b, sizeof(b));
    return sizeof(b) / 4;
}

static int HLine(unsigned int* g, int x0, int x1, int y)
{
    unsigned int b[] = { 9, Zz(x0 * 16), Zz(y * 16), 2 | 1 << 3, Zz((x1 - x0) * 16), 0 };
    memcpy(g, b, sizeof(b));
    return sizeof(b) / 4;
}

static int Point(unsigned int* g, int x, int y)
{
    g[0] = 9;
    g[1] = Zz(x * 16);
    g[2] = Zz(y * 16);
    return 3;
}

// A town's tile: areas in two rows, roads of every class across, then the names (streets, signs, places).
static int MakeCity(unsigned char* out, bool village)
{
    Pbw tile = { { 0 }, 0 };
    unsigned int g[16];
    Layer l;
    Begin(l, "building", "name");
    Add(l, NULL, NULL, 3, g, Box(g, 32, 32, 64, 64));
    End(tile, l);
    Begin(l, "park", "name");
    Add(l, "park", NULL, 3, g, Box(g, 128, 32, 192, 64));
    End(tile, l);
    Begin(l, "landcover", "name");
    Add(l, "wood", NULL, 3, g, Box(g, 192, 32, 256, 64));
    Add(l, "grass", NULL, 3, g, Box(g, 128, 96, 192, 128));
    Add(l, "farmland", NULL, 3, g, Box(g, 64, 32, 128, 64));   // not drawn (the fields of small zooms)
    End(tile, l);
    Begin(l, "landuse", "name");
    Add(l, "residential", NULL, 3, g, Box(g, 0, 96, 64, 128));
    Add(l, "retail", NULL, 3, g, Box(g, 64, 96, 128, 128));
    Add(l, "farmland", NULL, 3, g, Box(g, 192, 96, 256, 128));   // not drawn
    End(tile, l);
    Begin(l, "transportation", "name");
    Add(l, "trunk", NULL, 2, g, HLine(g, 0, 256, 144));
    Add(l, "tertiary", NULL, 2, g, HLine(g, 0, 256, 164));
    Add(l, "minor", NULL, 2, g, HLine(g, 0, 256, 184));
    Add(l, "rail", NULL, 2, g, HLine(g, 0, 256, 204));
    Add(l, "service", NULL, 2, g, HLine(g, 0, 256, 236));
    Add(l, "ferry", NULL, 2, g, HLine(g, 0, 256, 250));   // not drawn
    End(tile, l);
    Begin(l, "waterway", "name");
    Add(l, "river", NULL, 2, g, HLine(g, 0, 256, 220));
    End(tile, l);

    Begin(l, "transportation_name", "name");
    // a short step, then the long segment the name goes on
    unsigned int st[] = { 9, Zz(0), Zz(164 * 16), 2 | 2 << 3, Zz(16 * 16), Zz(0), Zz(240 * 16), Zz(0) };
    Add(l, "primary", "Main St", 2, st, sizeof(st) / 4);
    Add(l, "primary", "Main St", 2, g, HLine(g, 0, 256, 210));      // the same street close by: not again
    Add(l, "secondary", "Side St", 2, g, HLine(g, 0, 256, 240));
    End(tile, l);
    Begin(l, "poi", "name");
    Add(l, "fuel", "Fuel", 1, g, Point(g, 64, 144));
    Add(l, "parking", NULL, 1, g, Point(g, 192, 144));
    Add(l, "cafe", "Cafe", 1, g, Point(g, 100, 60));   // not drawn
    End(tile, l);
    Begin(l, "housenumber", "housenumber");
    Add(l, NULL, "12", 1, g, Point(g, 176, 144));
    End(tile, l);
    Begin(l, "place", "name");
    Add(l, "city", "City", 1, g, Point(g, 128, 64));
    if (village)
        Add(l, "village", "Vil", 1, g, Point(g, 128, 64));   // under the city: not drawn
    End(tile, l);
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

// A quarter's tile: 176 house numbers "1", 16 by 11, 12 by 10 px apart around the tile's centre (z16: 48 by 40 px)
enum { kHouseCols = 16, kHouseRows = 11 };

static int MakeHouses(unsigned char* out)
{
    Pbw tile = { { 0 }, 0 };
    Layer l;
    Begin(l, "housenumber", "housenumber");
    Value(l.w, "1");
    unsigned int tags[] = { 1, 0 }, g[3];
    for (int j = 0; j < kHouseRows; j++)
        for (int i = 0; i < kHouseCols; i++)
            Feature(l.w, tags, 2, 1, g, Point(g, 38 + 12 * i, 78 + 10 * j));
    End(tile, l);
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

// A small-scale tile's places: five along the middle row, one below
enum { kPlaces = 6 };
static const int kPlaceXY[kPlaces][2] = { { 30, 128 }, { 80, 128 }, { 130, 128 }, { 180, 128 }, { 230, 128 },
                                          { 130, 200 } };

// OpenMapTiles' places (rank and capital as numbers): a country's capital (rank 1, capital 2), a region's centre
// (rank 10, capital 3), a big city (rank 7), a middle one (rank 8), a town (rank 8), a big city of rank 1
static int MakeCountry(unsigned char* out)
{
    static const char* kName[kPlaces] = { "Cap", "Reg", "Big", "Mid", "Twn", "One" };
    static const char* kClass[kPlaces] = { "city", "city", "city", "city", "town", "city" };
    static const unsigned int kRank[kPlaces] = { 1, 10, 7, 8, 8, 1 }, kCapital[kPlaces] = { 2, 3, 0, 0, 0, 0 };
    Pbw tile = { { 0 }, 0 }, l = { { 0 }, 0 };
    Str(l, 1, "place");
    Str(l, 3, "class");
    Str(l, 3, "name");
    Str(l, 3, "rank");
    Str(l, 3, "capital");
    int nv = 0;
    for (int i = 0; i < kPlaces; i++) {
        Value(l, kClass[i]);
        Value(l, kName[i]);
        Pbw v = { { 0 }, 0 };   // the rank: a uint value (field 5)
        Var(v, 5 << 3);
        Var(v, kRank[i]);
        Bytes(l, 4, v.b, v.n);
        unsigned int tags[8] = { 0, nv, 1, nv + 1, 2, nv + 2, 3, nv + 3 };
        int nt = 6;
        if (kCapital[i]) {
            Pbw c = { { 0 }, 0 };
            Var(c, 5 << 3);
            Var(c, kCapital[i]);
            Bytes(l, 4, c.b, c.n);
            nt = 8;
        }
        nv += nt / 2;
        unsigned int g[3];
        Point(g, kPlaceXY[i][0], kPlaceXY[i][1]);
        Feature(l, tags, nt, 1, g, 3);
    }
    Bytes(tile, 3, l.b, l.n);
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

// ---- a layer with any keys; each feature brings its own values: a string, "%n" an int, "#n" a uint, "?n" a bool,
// NULL no such tag
struct Keyed { Pbw w; int nk, nv; };

static void BeginKeys(Keyed& l, const char* name, const char* const* keys, int nk)
{
    l.w.n = 0;
    l.nk = nk;
    l.nv = 0;
    Str(l.w, 1, name);
    for (int i = 0; i < nk; i++)
        Str(l.w, 3, keys[i]);
}

static void AddVals(Keyed& l, const char* const* vals, int type, const unsigned int* geom, int ngeom)
{
    unsigned int tags[16];
    int nt = 0;
    for (int i = 0; i < l.nk; i++) {
        if (!vals[i])
            continue;
        Pbw v = { { 0 }, 0 };
        const char* kNumbers = "%#?";   // an int value (field 4), a uint (5), a bool (7)
        const char* num = vals[i][0] ? strchr(kNumbers, vals[i][0]) : NULL;
        if (num) {
            Var(v, (num == kNumbers ? 4 : num == kNumbers + 1 ? 5 : 7) << 3);
            Var(v, (unsigned int)atoi(vals[i] + 1));
        } else
            Str(v, 1, vals[i]);
        Bytes(l.w, 4, v.b, v.n);
        tags[nt++] = i;
        tags[nt++] = l.nv++;
    }
    Feature(l.w, tags, nt, type, geom, ngeom);
}

// The same town in OpenMapTiles' schema (OpenFreeMap, planetiler) and in Protomaps' (their planet build, cut into a
// region's pack): a row's feature in each, a named road in Protomaps also its street's name (OpenMapTiles: a
// transportation_name feature). Shapes in the z14 tile's pixels: 'b' box a,b - c,d; 'h' line a - b at height c;
// 'p' point a,b.
struct Twin { const char *omt, *cls, *pm, *kind, *detail, *name; char shape; short a, b, c, d; };
static const Twin kTwins[] = {
    { "building", NULL, "buildings", "building", NULL, NULL, 'b', 32, 32, 64, 64 },
    { "water", NULL, "water", "water", NULL, NULL, 'b', 64, 32, 128, 64 },
    { "park", "park", "landuse", "park", NULL, NULL, 'b', 128, 32, 192, 64 },
    { "landcover", "wood", "landuse", "wood", NULL, NULL, 'b', 192, 32, 256, 64 },
    { "landcover", "grass", "landuse", "grass", NULL, NULL, 'b', 128, 96, 192, 128 },
    { "landuse", "residential", "landuse", "residential", NULL, NULL, 'b', 0, 96, 64, 128 },
    { "landuse", "retail", "landuse", "retail", NULL, NULL, 'b', 64, 96, 128, 128 },
    { "transportation", "motorway", "roads", "highway", "motorway", "M5", 'h', 0, 256, 136 },
    { "transportation", "trunk", "roads", "major_road", "trunk_link", NULL, 'h', 0, 256, 146 },
    { "transportation", "primary", "roads", "major_road", "primary", "Main St", 'h', 0, 256, 158 },
    { "transportation", "secondary", "roads", "major_road", "secondary_link", NULL, 'h', 0, 256, 170 },
    { "transportation", "tertiary", "roads", "major_road", "tertiary", "Side St", 'h', 0, 256, 182 },
    { "transportation", "minor", "roads", "minor_road", "residential", "Lane", 'h', 0, 256, 194 },
    { "transportation", "service", "roads", "minor_road", "service", "Yard", 'h', 0, 256, 206 },   // (no name drawn)
    { "transportation", "path", "roads", "path", "footway", NULL, 'h', 0, 256, 214 },
    { "transportation", "rail", "roads", "rail", "tram", NULL, 'h', 0, 256, 222 },
    { "transportation", "ferry", "roads", "ferry", NULL, NULL, 'h', 0, 256, 244 },
    { "waterway", "river", "water", "river", NULL, NULL, 'h', 0, 256, 232 },
    { "poi", "fuel", "pois", "fuel", NULL, "Fuel", 'p', 64, 80 },
    { "place", "city", "places", "locality", "city", "City", 'p', 128, 64 },
    { "place", "village", "places", "locality", "village", "Vil", 'p', 200, 80 },
    { "place", "suburb", "places", "neighbourhood", "suburb", "Sub", 'p', 40, 80 },
};
static const int kNTwins = sizeof(kTwins) / sizeof(kTwins[0]);

static int TwinShape(const Twin& t, unsigned int* g, int& type)
{
    type = t.shape == 'b' ? 3 : t.shape == 'h' ? 2 : 1;
    return t.shape == 'b' ? Box(g, t.a, t.b, t.c, t.d) : t.shape == 'h' ? HLine(g, t.a, t.b, t.c) : Point(g, t.a, t.b);
}

static const char* TwinLayer(const Twin& t, bool pm, bool names)
{
    return names ? (t.name && !strcmp(t.omt, "transportation") ? "transportation_name" : NULL) : pm ? t.pm : t.omt;
}

// The rows of one layer (the first row naming it, and the rest), the OpenMapTiles' street names a layer of their own
static void TwinLayerOut(Pbw& tile, bool pm, bool names, const char* layer)
{
    static const char* kOmt[] = { "class", "name" };
    static const char* kPm[] = { "kind", "kind_detail", "name" };   // (a name taken from a key not there: wrong)
    Keyed l;
    BeginKeys(l, layer, pm ? kPm : kOmt, pm ? 3 : 2);
    for (int j = 0; j < kNTwins; j++) {
        const Twin& t = kTwins[j];
        const char* lj = TwinLayer(t, pm, names);
        if (!lj || strcmp(lj, layer))
            continue;
        const char* nm = names || pm || strcmp(t.omt, "transportation") ? t.name : NULL;
        const char* vals[3] = { pm ? t.kind : t.cls, pm ? t.detail : nm, nm };
        unsigned int g[16];
        int type, ng = TwinShape(t, g, type);
        AddVals(l, vals, type, g, ng);
    }
    Bytes(tile, 3, l.w.b, l.w.n);
}

static int MakeTwin(unsigned char* out, bool pm)
{
    Pbw tile = { { 0 }, 0 };
    for (int i = 0; i < kNTwins; i++) {
        bool first = true;
        for (int k = 0; k < i; k++)
            first = first && strcmp(TwinLayer(kTwins[k], pm, false), TwinLayer(kTwins[i], pm, false)) != 0;
        if (first)
            TwinLayerOut(tile, pm, false, TwinLayer(kTwins[i], pm, false));
    }
    if (!pm)
        TwinLayerOut(tile, false, true, "transportation_name");
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

// Protomaps' places as MakeCountry's: the country's capital "yes", a region's centre "4" (both strings), a city of
// 240 000 people, a city of 150 000, a town, a city of a million
static int MakePmCountry(unsigned char* out)
{
    static const char* kKeys[] = { "kind", "name", "kind_detail", "capital", "population" };
    static const char* kRows[kPlaces][5] = { { "locality", "Cap", "city", "yes", "#9000000" },
                                             { "locality", "Reg", "city", "4", "#200000" },
                                             { "locality", "Big", "city", NULL, "%240000" },   // (an int)
                                             { "locality", "Mid", "city", NULL, "#150000" },
                                             { "locality", "Twn", "town", NULL, "#50000" },
                                             { "locality", "One", "city", NULL, "#1000000" } };
    Pbw tile = { { 0 }, 0 };
    Keyed l;
    BeginKeys(l, "places", kKeys, 5);
    for (int i = 0; i < kPlaces; i++) {
        unsigned int g[3];
        Point(g, kPlaceXY[i][0], kPlaceXY[i][1]);
        AddVals(l, kRows[i], 1, g, 3);
    }
    Bytes(tile, 3, l.w.b, l.w.n);
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

// ---- the frame the labels are drawn on (800 x 480, black)
struct Frame { Image im; unsigned short* bits; HDC dc; HGDIOBJ old; };

static void Open(Frame& f)
{
    memset(&f, 0, sizeof(f));
    f.bits = ImageCreate(800, 480, &f.im);
    memset(f.bits, 0, 800 * 480 * 2);
    f.dc = CreateCompatibleDC(NULL);
    f.old = SelectObject(f.dc, f.im.bmp);
}

static void Close(Frame& f)
{
    SelectObject(f.dc, f.old);
    DeleteDC(f.dc);
    ImageFree(&f.im);
}

// The labels with the view's centre on the z14 tile's pixel (px, py) of data tile (tx, ty)
static void Labels(Frame& f, int tx, int ty, int px, int py, int z, double angle, const RECT& mr,
                   const RECT* avoid, int navoid)
{
    memset(f.bits, 0, 800 * 480 * 2);
    double s = (double)(1 << (z - 14));
    VLabelsDraw(f.dc, mr, 400, 240, (tx * 256.0 + px) * s, (ty * 256.0 + py) * s, angle, z, avoid, navoid);
    GdiFlush();
}

static int Count(const Frame& f, int x0, int y0, int x1, int y1, unsigned short c)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            n += f.bits[(479 - y) * 800 + x] == c;
    return n;
}

static int Lit(const Frame& f, int x0, int y0, int x1, int y1)   // anything drawn
{
    return (x1 - x0) * (y1 - y0) - Count(f, x0, y0, x1, y1, 0);
}

static unsigned short Px(const Image& im, int x, int y)   // the DIB's rows are bottom-up
{
    DIBSECTION ds;
    GetObject(im.bmp, sizeof(ds), &ds);
    return ((unsigned short*)ds.dsBm.bmBits)[(im.h - 1 - y) * im.w + x];
}

// The places' names by zoom (the user 2026-10-27): capitals from z3, regions' centres and big cities from z7, every
// city and town from z10 - a tile of MakeCountry's places, schema's in the checks' names
static void PlacesByZoom(const unsigned char* tile, int len, int col, const char* schema)
{
    static const int kZoom[3] = { 3, 7, 10 };
    static const bool kShown[3][kPlaces] = { { true, false, false, false, false, false },
                                             { true, true, true, false, false, true },
                                             { true, true, true, true, true, true } };
    static const char* kWhat[3] = { "z3: the capital only", "z7: the capital, the region's centre, the big city",
                                    "z10: every city and town" };
    const unsigned short white = RGB565(255, 255, 255);
    Frame h;
    Open(h);
    RECT whole = { 0, 0, 800, 480 };
    for (int k = 0; k < 3; k++) {
        int n = (1 << (kZoom[k] - 1)) + col;   // a tile in the middle of the world (another column per schema)
        MapRequest q = { SRC_OSM, kZoom[k], n, n - col };
        Image ti = { 0 };
        bool ok = VTileRender(q, tile, len, &ti);
        ImageFree(&ti);
        memset(h.bits, 0, 800 * 480 * 2);
        VLabelsDraw(h.dc, whole, 400, 240, n * 256.0 + 128, (n - col) * 256.0 + 128, 0, kZoom[k], NULL, 0);
        GdiFlush();
        for (int i = 0; i < kPlaces; i++) {
            int sx = 272 + kPlaceXY[i][0], sy = 112 + kPlaceXY[i][1];
            ok = ok && (kShown[k][i] ? Count(h, sx - 20, sy - 12, sx + 20, sy + 12, white) > 10
                                     : Lit(h, sx - 12, sy - 12, sx + 12, sy + 12) == 0);
        }
        char what[128];
        sprintf(what, "%s %s", schema, kWhat[k]);
        Check(ok, what);
    }
    Close(h);
}

static bool SameImage(const Image& a, const Image& b)
{
    DIBSECTION da, db;
    GetObject(a.bmp, sizeof(da), &da);
    GetObject(b.bmp, sizeof(db), &db);
    return a.w == b.w && a.h == b.h && !memcmp(da.dsBm.bmBits, db.dsBm.bmBits, a.w * a.h * 2);
}

// The same town in both schemas (kTwins): the same tiles at z14 and z15, the same names at z14 and z16
static void Twins()
{
    static unsigned char omt[4096], pmt[4096];
    int no = MakeTwin(omt, false), np = MakeTwin(pmt, true);
    static const int kQuarter[3][3] = { { 14, 0, 0 }, { 15, 0, 1 }, { 15, 1, 0 } };   // z, then the z15 quarter
    for (int i = 0; i < 3; i++) {
        int s = kQuarter[i][0] - 14;
        MapRequest a = { SRC_OSM, kQuarter[i][0], (9300 << s) + kQuarter[i][1], (5000 << s) + kQuarter[i][2] };
        MapRequest b = { SRC_OSM, kQuarter[i][0], (9400 << s) + kQuarter[i][1], (5000 << s) + kQuarter[i][2] };
        Image ia = { 0 }, ib = { 0 };
        bool ok = VTileRender(a, omt, no, &ia) && VTileRender(b, pmt, np, &ib) && SameImage(ia, ib);
        Check(ok, i ? "Protomaps: the town's z15 quarter as OpenMapTiles'" : "Protomaps: the town's z14 tile as OpenMapTiles'");
        ImageFree(&ia);
        ImageFree(&ib);
    }
    Frame f, g;
    Open(f);
    Open(g);
    RECT mr = { 0, 0, 800, 480 };
    static const int kAt[2] = { 14, 16 };
    for (int i = 0; i < 2; i++) {
        Labels(f, 9300, 5000, 128, 160, kAt[i], 0, mr, NULL, 0);
        Labels(g, 9400, 5000, 128, 160, kAt[i], 0, mr, NULL, 0);
        Check(Lit(f, 0, 0, 800, 480) > 500 && !memcmp(f.bits, g.bits, 800 * 480 * 2),
              i ? "Protomaps: the town's z16 names as OpenMapTiles'" : "Protomaps: the town's z14 names as OpenMapTiles'");
    }
    Close(f);
    Close(g);
}

// A region's pack's z15 tiles (Protomaps: the house numbers, the car parks) drawn for z15 and up instead of the z14
// tile; the z14 tile's names only in the quarters no z15 tile is drawn for. Tile (9500, 5000): its z14 data
// (OpenMapTiles) has house numbers in each quarter, on the quarters' borders (128 px: the right, the lower one) and
// just before them (100 px: still the top left);
// its top left z15 child (Protomaps) a building, a street, the house number "34" and a car park, its bottom right one
// the house number "90".
struct Num { const char* nm; short x, y; };
static const Num kNums14[] = { { "12", 40, 40 }, { "78", 192, 40 }, { "56", 192, 192 }, { "x2", 128, 200 },
                               { "y2", 200, 128 }, { "x1", 100, 40 },
                               { "y1", 40, 100 } };

static int MakeDeep15(unsigned char* out, bool topLeft)
{
    unsigned int g[16];
    Pbw t = { { 0 }, 0 };
    static const char* kBKeys[] = { "kind", "name", "addr_housenumber" };
    static const char* kHouse[2][3] = { { "address", NULL, "90" }, { "address", NULL, "34" } };
    static const char* kBuilding[] = { "building", "Hall", NULL };   // (a name key in the layer: not the number's)
    Keyed b;
    BeginKeys(b, "buildings", kBKeys, 3);
    AddVals(b, kBuilding, 3, g, Box(g, 0, 0, 64, 64));               // the z15 tile's pixels: 0..256
    AddVals(b, kHouse[topLeft], 1, g, topLeft ? Point(g, 176, 176) : Point(g, 64, 64));
    Bytes(t, 3, b.w.b, b.w.n);
    if (topLeft) {
        static const char* kKeys[] = { "kind", "name" };
        static const char* kPark[] = { "parking", NULL };
        static const char* kLane[] = { "minor_road", "Lane" };
        Keyed p, r;
        BeginKeys(p, "pois", kKeys, 2);
        AddVals(p, kPark, 1, g, Point(g, 64, 176));
        Bytes(t, 3, p.w.b, p.w.n);
        BeginKeys(r, "roads", kKeys, 2);
        AddVals(r, kLane, 2, g, HLine(g, 0, 256, 40));
        Bytes(t, 3, r.w.b, r.w.n);
    }
    memcpy(out, t.b, t.n);
    return t.n;
}

static void Deep()
{
    unsigned int g[16];
    Pbw t14 = { { 0 }, 0 };
    Layer l;
    Begin(l, "housenumber", "housenumber");
    for (int i = 0; i < (int)(sizeof(kNums14) / sizeof(kNums14[0])); i++)
        Add(l, NULL, kNums14[i].nm, 1, g, Point(g, kNums14[i].x, kNums14[i].y));
    End(t14, l);
    static unsigned char tl[2048], br[2048];
    int ntl = MakeDeep15(tl, true), nbr = MakeDeep15(br, false);

    MapRequest r14 = { SRC_OSM, 14, 9500, 5000 }, r16 = { SRC_OSM, 16, 38000, 20000 };
    MapRequest r16b = { SRC_OSM, 16, 38002, 20002 };
    MapRequest d15 = { SRC_OSM, VEC_ZBASE + 15, 19000, 10000 }, d15b = { SRC_OSM, VEC_ZBASE + 15, 19001, 10001 };
    Check(SameKey(VTileDataAt(r16, VEC_PACKZ), d15) && VTileDataAt(r16, VEC_MAXZ).z == VEC_ZBASE + 14,
          "z16: drawn from its z15 ancestor in a pack, else from the z14 one");
    Image im = { 0 };
    Check(VTileRender(r14, t14.b, t14.n, &im), "deep: the z14 tile drawn");
    ImageFree(&im);
    Check(VTileRenderFrom(r16, d15, tl, ntl, &im) && Px(im, 100, 100) == RGB565(224, 217, 208)
          && Px(im, 200, 200) != RGB565(224, 217, 208), "z16 from the z15 tile: its building, larger");
    ImageFree(&im);
    Check(VTileRenderFrom(r16b, d15b, br, nbr, &im), "z16 from the other z15 tile");
    ImageFree(&im);
    const unsigned short house = RGB565(120, 112, 104), blue = RGB565(46, 110, 190), white = RGB565(255, 255, 255);
    Frame f;
    Open(f);
    RECT mr = { 0, 0, 800, 480 };
    Labels(f, 9500, 5000, 64, 64, 15, 0, mr, NULL, 0);
    Check(Count(f, 360, 142, 440, 162, white) > 20, "z15: the z15 tile's street name");
    Labels(f, 9500, 5000, 64, 64, 17, 0, mr, NULL, 0);
    Check(Count(f, 572, 422, 612, 442, house) > 3, "z17: the z15 tile's house number");
    Check(Lit(f, 188, 38, 228, 58) == 0, "z17: not the z14 tile's in the z15 tile's quarter");
    Check(Count(f, 134, 422, 155, 443, blue) > 100, "z17: the z15 tile's car park");
    Labels(f, 9500, 5000, 192, 40, 17, 0, mr, NULL, 0);
    Check(Count(f, 383, 230, 418, 250, house) > 3, "z17: the z14 tile's house number where no z15 tile is");
    static const int kHidden[5][2] = { { 192, 192 }, { 128, 200 }, { 200, 128 }, { 100, 40 }, { 40, 100 } };
    bool hidden = true;
    for (int i = 0; i < 5; i++) {
        Labels(f, 9500, 5000, kHidden[i][0], kHidden[i][1], 17, 0, mr, NULL, 0);
        hidden = hidden && Lit(f, 380, 230, 420, 250) == 0;
    }
    Check(hidden, "z17: the z14 tile's numbers in the z15 tiles' quarters, at their borders too, not drawn");
    Close(f);
}

// Roads by their level (the user 2026-10-27, the first step of the 3D view): a road on the ground across the z14 tile
// at y 128, over it a bridge at x 128, under it a tunnel at x 64 (in the tile: the bridge first, the tunnel last) -
// OpenMapTiles' "brunnel", Protomaps' is_bridge / is_tunnel (bools)
static int MakeLevels(unsigned char* out, bool pm)
{
    static const char* kOmt[] = { "class", "brunnel" };
    static const char* kPm[] = { "kind", "kind_detail", "is_bridge", "is_tunnel" };
    static const char* kRows[2][3][4] = { { { "primary", "bridge" }, { "primary", NULL }, { "primary", "tunnel" } },
                                          { { "major_road", "primary", "?1", NULL }, { "major_road", "primary", NULL, NULL },
                                            { "major_road", "primary", NULL, "?1" } } };
    Pbw tile = { { 0 }, 0 };
    Keyed l;
    BeginKeys(l, pm ? "roads" : "transportation", pm ? kPm : kOmt, pm ? 4 : 2);
    for (int i = 0; i < 3; i++) {
        unsigned int g[6] = { 9, Zz(128 * 16), 0, 2 | 1 << 3, 0, Zz(256 * 16) };   // (the bridge: down at x 128)
        if (i == 1)
            HLine(g, 0, 256, 128);
        else if (i == 2)
            g[1] = Zz(64 * 16);
        AddVals(l, kRows[pm][i], 2, g, 6);
    }
    Bytes(tile, 3, l.w.b, l.w.n);
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

static int Column(const Image& im, int x, int y0, int y1, unsigned short c)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        n += Px(im, x, y) == c;
    return n;
}

static int Row(const Image& im, int y, int x0, int x1, unsigned short c)
{
    int n = 0;
    for (int x = x0; x < x1; x++)
        n += Px(im, x, y) == c;
    return n;
}

static void Levels()
{
    static unsigned char omt[4096], pmt[4096];
    int no = MakeLevels(omt, false), np = MakeLevels(pmt, true);
    const unsigned short casing = RGB565(224, 182, 120);
    MapRequest a = { SRC_OSM, 14, 9600, 5000 }, b = { SRC_OSM, 14, 9700, 5000 }, n = { SRC_OSM, 14, 9800, 5000 };
    Image ia = { 0 }, ib = { 0 };
    g_cfg.night = 0;
    Check(VTileRender(a, omt, no, &ia) && Row(ia, 128, 118, 138, casing) == 2,
          "the bridge's casing over the road it crosses: 1 px each side");
    Check(Column(ia, 64, 116, 140, casing) >= 2, "the road's casing over the tunnel under it");
    Check(Px(ia, 64, 40) == RGB565(249, 234, 205), "the tunnel paler: half the background");
    Check(Row(ia, 40, 54, 74, casing) == 0, "the tunnel without a casing");
    const unsigned short shadow = RGB565(206, 200, 190);   // the user 2026-10-28: a bridge's height shown by its shadow
    Check(Row(ia, 40, 133, 145, shadow) >= 4 && Row(ia, 40, 110, 123, shadow) == 0,
          "the bridge's shadow beside it, to one side (south-east), over the ground");
    Check(Row(ia, 40, 120, 150, shadow) == 6 && Px(ia, 138, 40) == shadow,
          "the shadow: the bridge's casing wide, 3/4 of its width aside");
    Check(Column(ia, 136, 123, 134, shadow) == 11, "the shadow over the road under the bridge, its casing too");
    Check(Px(ia, 136, 12) != shadow && Px(ia, 136, 13) == shadow,
          "the shadow shifted down too (south-east), its ends cut back to the bridge's");
    Check(Row(ia, 40, 50, 80, shadow) == 0 && Column(ia, 20, 116, 150, shadow) == 0,
          "no shadow of the tunnel, of the road on the ground");
    Check(VTileRender(b, pmt, np, &ib) && SameImage(ia, ib), "Protomaps' bridge and tunnel as OpenMapTiles'");
    ImageFree(&ib);
    g_cfg.night = 1;   // (no casings at night: the bridge's own, of the background)
    Check(VTileRender(n, omt, no, &ib) && Row(ib, 128, 118, 138, RGB565(17, 19, 23)) >= 2,
          "night: the bridge parted from the road it crosses");
    Check(Row(ib, 40, 133, 145, RGB565(6, 7, 9)) >= 4, "night: the bridge's shadow darker than the night's ground");
    Check(Column(ib, 64, 118, 138, RGB565(17, 19, 23)) == 0, "night: no casing of the ground road over the tunnel");
    g_cfg.night = 0;
    ImageFree(&ia);
    ImageFree(&ib);
}

// The third person's view (the user 2026-10-27): the flat map tilted, the car at its row and size, the far smaller and
// higher, nothing past the fog (the visibility: a circle around the car)
static bool Near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

static void TiltChecks()
{
    Tilt t, t70;
    TiltMake(&t, 120, 150, 300, 60);    // the car 120 px below the flat map's centre, on the screen 150; the fog at
    TiltMake(&t70, 120, 150, 300, 70);  // 300 px ahead; the camera 60 degrees from straight down (70: lower)
    double sx, sy, fy, k, fade, sx2, sy2;
    Check(TiltPoint(t, 0, 120, &sx, &sy) && Near(sx, 0) && Near(sy, 150), "tilt: the car at its row");
    Check(TiltPoint(t, 10, 120, &sx, &sy) && Near(sx, 10), "tilt: at the car's row the map's own size");
    Check(TiltPoint(t, 0, 119, &sx, &sy) && fabs(150 - sy - 1 / tan(60 * M_PI / 180)) < 0.005 &&
          TiltPoint(t70, 0, 119, &sx2, &sy2) && sy2 > sy, "tilt: at the car the depth shortened by the camera's angle");
    Check(TiltPoint(t, 10, 20, &sx, &sy) && TiltPoint(t, 10, -60, &sx2, &sy2) && sy2 < sy && sy < 150 &&
          sx2 < sx && sx < 10, "tilt: the farther the higher on the screen and the smaller");
    Check(TiltPoint(t, 0, 120 - 299, &sx, &sy) && !TiltPoint(t, 0, 120 - 301, &sx, &sy), "tilt: the fog at 300 px ahead");
    Check(!TiltPoint(t, 0, 120 - 300, &sx, &sy), "tilt: the fog's very edge not seen");
    TiltProject(t, 0, 120 - 300, &sx, &sy);                              // (a line cut there: its end drawn)
    Check(Near(sy, t.fr) && Near(sx, 0), "tilt: the fog's edge projected to the fog's row");
    Check(TiltPoint(t, 1000, 20, &sx, &sy), "tilt: the fog by the depth only: far aside seen");
    Check(TiltPoint(t, 0, 400, &sx, &sy) && sy > 240 && !TiltPoint(t, 0, 120 + 300, &sx, &sy),
          "tilt: behind the car below the screen, behind the fog's depth none");
    Check(TiltRow(t, 100, &fy, &k, &fade) && TiltPoint(t, 30 * k, fy, &sx, &sy) && Near(sx, 30) && Near(sy, 100),
          "tilt: a screen row back to the flat map");
    Check(TiltRow(t, 140, &fy, &k, &fade) && fade == 0, "tilt: no fog near the car");
    double row75 = t.cs - t.rc + t.rc * t.f / (225 + t.f), row40 = t.cs - t.rc + t.rc * t.f / (120 + t.f);
    Check(TiltRow(t, row75, &fy, &k, &fade) && Near(fade, 0.5) && TiltRow(t, row40, &fy, &k, &fade) && fade == 0,
          "tilt: the fog thickening from half its depth (the user 2026-10-28)");
    unsigned char c[3];
    TiltFog(t, t.fr + 10, false, c);
    Check(c[0] == 243 && c[1] == 240 && c[2] == 235, "fog: by day the map's cream at the ground");
    TiltFog(t, t.fr - 200, false, c);
    Check(c[0] == 170 && c[1] == 205 && c[2] == 238, "fog: by day sky blue high above");
    TiltFog(t, t.fr - 60, false, c);
    Check(c[0] > 170 && c[0] < 243 && c[2] > 235 && c[2] < 238 + 1, "fog: by day a gradient between");
    TiltFog(t, t.fr - 200, true, c);
    unsigned char d[3];
    TiltFog(t, t.fr + 10, true, d);
    Check(!c[0] && !c[1] && !c[2] && !d[0] && !d[1] && !d[2], "fog: at night just black");
    double fogRow = 150 - t.rc * 300 / (300 + t.f);
    Check(!TiltRow(t, fogRow - 1, &fy, &k, &fade) && TiltRow(t, fogRow + 1, &fy, &k, &fade),
          "tilt: above the fog's row only fog");
    Check(Near(t.fr, fogRow), "tilt: the fog's row kept");
    Check(!TiltRow(t, t.cs - t.rc - 1, &fy, &k, &fade), "tilt: above the horizon only fog");
    double x0 = 0, y0 = 120, x1 = 0, y1 = -580;
    Check(TiltClip(t, &x0, &y0, &x1, &y1) && Near(y0, 120) && Near(y1, 120 - 300), "tilt: a line cut at the fog");
    x0 = 100, y0 = 120, x1 = 700, y1 = -480;
    Check(TiltClip(t, &x0, &y0, &x1, &y1) && Near(x0, 100) && Near(x1, 400) && Near(y1, -180),
          "tilt: a line aslant cut where it reaches the fog's depth");
    x0 = 0, y0 = 500, x1 = 0, y1 = 300;
    Check(TiltClip(t, &x0, &y0, &x1, &y1) && Near(y0, 420) && Near(y1, 300), "tilt: a line from behind cut there");
    x0 = -400, y0 = 20, x1 = 400, y1 = 20;
    Check(TiltClip(t, &x0, &y0, &x1, &y1) && Near(x0, -400) && Near(x1, 400), "tilt: a line across seen whole");
    x0 = 0, y0 = -300, x1 = 0, y1 = -400;
    Check(!TiltClip(t, &x0, &y0, &x1, &y1), "tilt: a line beyond the fog: none");
    x0 = 0, y0 = 100, x1 = 1, y1 = 100;
    Check(TiltClip(t, &x0, &y0, &x1, &y1) && Near(x0, 0) && Near(x1, 1), "tilt: a line of 1 px kept");
    x0 = 0, y0 = -179.5, x1 = 0, y1 = -180.5;  // 1 px deep across the fog's depth
    Check(TiltClip(t, &x0, &y0, &x1, &y1) && Near(y1, -180), "tilt: a line 1 px deep cut at the fog");
    x0 = -10, y0 = -200, x1 = 10, y1 = -200;
    Check(!TiltClip(t, &x0, &y0, &x1, &y1), "tilt: a line across beyond the fog: none");
}

// A road on the ground, then its bridge, then the ground again, in one line at y 64 (the joints at x 100 and 180): the
// bridge's casing along its sides only - no casing across the road at the joints (the user 2026-10-28: the bridge one
// with its road)
static int MakeJoint(unsigned char* out)
{
    static const char* kKeys[] = { "class", "brunnel" };
    static const char* kRows[3][2] = { { "primary", NULL }, { "primary", "bridge" }, { "primary", NULL } };
    static const int kX[4] = { 0, 100, 180, 256 };
    Pbw tile = { { 0 }, 0 };
    Keyed l;
    BeginKeys(l, "transportation", kKeys, 2);
    for (int i = 0; i < 3; i++) {   // (the bridge: a first piece of 2 px, shorter than its casing's cut)
        unsigned int g[8] = { 9, Zz(kX[i] * 16), Zz(64 * 16), 2 | 2 << 3, Zz(2 * 16), 0, Zz((kX[i + 1] - kX[i] - 2) * 16), 0 };
        if (i != 1)
            HLine(g, kX[i], kX[i + 1], 64);
        AddVals(l, kRows[i], 2, g, i == 1 ? 8 : 6);
    }
    unsigned int dead[6];   // a road on the ground ending in the tile: its casing round its end
    HLine(dead, 20, 100, 150);
    AddVals(l, kRows[0], 2, dead, 6);
    HLine(dead, 200, 204, 200);   // a bridge shorter than its casing's cut
    AddVals(l, kRows[1], 2, dead, 6);
    Bytes(tile, 3, l.w.b, l.w.n);
    memcpy(out, tile.b, tile.n);
    return tile.n;
}

static void Joints()
{
    static unsigned char d[4096];
    int n = MakeJoint(d);
    const unsigned short casing = RGB565(224, 182, 120), road = RGB565(255, 228, 176);
    MapRequest r = { SRC_OSM, 14, 9900, 5000 };
    Image im = { 0 };
    bool ok = VTileRender(r, d, n, &im);
    int across = 0;
    for (int x = 94; x < 107; x++)
        across += Column(im, x, 61, 68, casing) + Column(im, x + 80, 61, 68, casing);
    Check(ok && across == 0 && Row(im, 64, 90, 190, road) == 100, "a bridge's joints with its road: no casing across");
    Check(Px(im, 140, 59) == casing && Px(im, 140, 68) == casing, "the bridge's casing along its sides");
    Check(Row(im, 61, 94, 107, road) == 13 && Row(im, 67, 174, 187, road) == 13,
          "the bridge's road whole up to its ends, its edges too");
    Check(Px(im, 104, 150) == casing && Px(im, 103, 150) == road, "a road's end on the ground: its casing round it");
    Check(Row(im, 59, 88, 112, casing) == 21 && Row(im, 59, 168, 196, casing) == 25,
          "the bridge's casing cut at its ends only a little (3 px of its top side)");
    Check(Row(im, 70, 168, 196, RGB565(206, 200, 190)) == 12 && Row(im, 67, 168, 196, RGB565(206, 200, 190)) == 0,
          "its shadow ends with it: not over the road on the ground after it");
    int shortCasing = 0;
    for (int y = 190; y < 211; y++)
        shortCasing += Row(im, y, 190, 216, casing) + Row(im, y, 190, 216, RGB565(206, 200, 190));
    Check(shortCasing == 0 && Px(im, 202, 200) == road, "a bridge shorter than its casing's cut: no casing, no shadow");
    ImageFree(&im);
}

// The junction card (the user 2026-10-28, on in the settings): the route round a complex maneuver - 110 m before it,
// 70 m after - on the map turned so the way in points up
static void YoChecks()   // a device's font without Ё (Shturmann NSL 500, 2026-10-29: squares): Е instead
{
    wchar_t s[] = L"\x0401\x043B\x043A\x0438 \x0451\x0436 \x0415";
    NoYo(s);
    Check(!wcscmp(s, L"\x0415\x043B\x043A\x0438 \x0435\x0436 \x0415"), "yo: capital and small yo as ye");
}

static void WheelChecks()
{
    Check(WheelZoom(8, 2) == 1 && WheelZoom(9, 2) == -1 && WheelZoom(8, 3) == 1 && WheelZoom(9, 3) == -1,
          "wheel: up / down held - zoom in / out");
    Check(WheelZoom(0x0A, 1) == 1 && WheelZoom(0x0B, 1) == -1, "wheel: the MCU's held up / down (0x0A / 0x0B)");
    Check(WheelZoom(0x12, 2) == 1 && WheelZoom(0x14, 2) == -1, "wheel: phone / hang up held - as before");
    Check(!WheelZoom(8, 1) && !WheelZoom(9, 1) && !WheelZoom(0x12, 1) && !WheelZoom(0x14, 1) && !WheelZoom(7, 2),
          "wheel: short presses and other keys - VestaHome's");
}

static void JunctionChecks()
{
    Check(JunctionComplex("fork") && JunctionComplex("off ramp") && JunctionComplex("on ramp") &&
          JunctionComplex("roundabout") && JunctionComplex("rotary") && JunctionComplex("exit roundabout") &&
          JunctionComplex("merge") && !JunctionComplex("turn") && !JunctionComplex("new name") &&
          !JunctionComplex("arrive") && !JunctionComplex("depart"), "junction: only the complex maneuvers");
    Check(JunctionShown(true, true, 300, false) && !JunctionShown(true, true, 301, false) &&
          !JunctionShown(false, true, 100, false) && !JunctionShown(true, false, 100, false) &&
          !JunctionShown(true, true, 100, true), "junction: on in the settings, complex, within 300 m, on the route");
    Check(JunctionZoom(15, true, true, 300, false) == 17 && JunctionZoom(16, true, true, 0, false) == 17,
          "auto zoom: z17 within 300 m of a complex maneuver, heading-up");
    Check(JunctionZoom(15, true, true, 301, false) == 15 && JunctionZoom(15, false, true, 100, false) == 15 &&
          JunctionZoom(15, true, false, 100, false) == 15 && JunctionZoom(15, true, true, 100, true) == 15 &&
          JunctionZoom(18, true, true, 100, false) == 18 && JunctionZoom(17, true, true, 100, false) == 17,
          "auto zoom: else the user's (farther, north-up, a plain turn, off the route, closer already)");
    // a line north 300 m then east 300 m (at 53 N), the maneuver at the corner (point 3)
    const double la = 53.0, dla = 100 / 110540.0, dlo = 100 / (111320.0 * cos(la * M_PI / 180));
    double ll[] = { 50, la, 50, la + dla, 50, la + 2 * dla, 50, la + 3 * dla, 50 + dlo, la + 3 * dla,
                    50 + 2 * dlo, la + 3 * dla, 50 + 3 * dlo, la + 3 * dla };
    double out[32], course;
    int at, n = JunctionSlice(ll, 7, 3, 110, 70, out, 16, &at, &course);
    Check(n == 4 && at == 2 && fabs(out[1] - (la + 1.9 * dla)) < 1e-7 && fabs(out[0] - 50) < 1e-9 &&
          fabs(out[6] - (50 + 0.7 * dlo)) < 1e-7 && fabs(out[7] - (la + 3 * dla)) < 1e-9,
          "junction: the route from 110 m before the maneuver to 70 m after, its ends cut on the line");
    Check(fabs(course) < 0.01, "junction: the way in's course (north: 0)");
    double want[8] = { 50, la + 1.9 * dla, 50, la + 2 * dla, 50, la + 3 * dla, 50 + 0.7 * dlo, la + 3 * dla };
    bool same = true;
    for (int i = 0; i < 8; i++)
        same = same && fabs(out[i] - want[i]) < 1e-7;
    Check(same, "junction: every point of the piece");
    for (int i = 0; i < 32; i++)
        out[i] = -1;
    n = JunctionSlice(ll, 7, 3, 110, 70, out, 2, &at, &course);
    Check(n == 2 && out[4] == -1 && out[5] == -1 && fabs(course) < 0.01,
          "junction: no more points than room for (the maneuver's left out: the line's first piece's course)");
    n = JunctionSlice(ll, 7, 1, 110, 70, out, 16, &at, &course);
    Check(n == 3 && at == 1 && out[0] == 50 && out[1] == la && fabs(course) < 0.01,
          "junction: less than 110 m from the line's start: from the start");
    n = JunctionSlice(ll, 7, 4, 110, 70, out, 16, &at, &course);
    double want4 = atan2(100 * cos((la + 3 * dla) * M_PI / 180) / cos(la * M_PI / 180), 10.0) * 180 / M_PI;
    Check(fabs(course - want4) < 0.05 && n == 4 && at == 2,
          "junction: the way in from 110 m before to the maneuver (the approach, not its last bend)");
    n = JunctionSlice(ll + 4, 5, 0, 110, 70, out, 16, &at, &course);   // from the corner's point before: north, then east
    Check(fabs(course) < 0.01, "junction: at the start the course of the first piece, not of the next");
    n = JunctionSlice(ll + 6, 4, 0, 110, 70, out, 16, &at, &course);   // from the corner east
    Check(fabs(course - 90) < 0.05, "junction: at the start of a line going east: 90");
    n = JunctionSlice(ll + 8, 2, 0, 110, 70, out, 16, &at, &course);
    Check(n == 2 && fabs(course - 90) < 0.05, "junction: a line of two points");
    double one[2] = { 50, la };
    n = JunctionSlice(one, 1, 0, 110, 70, out, 16, &at, &course);
    Check(n == 1 && at == 0 && course == 0, "junction: a line of one point");
    n = JunctionSlice(ll, 7, 6, 110, 70, out, 16, &at, &course);
    Check(n == 3 && at == 2 && fabs(course - 90) < 0.01, "junction: at the line's end no after, the course east 90");
    n = JunctionSlice(ll, 7, 0, 110, 70, out, 16, &at, &course);
    Check(n == 2 && at == 0 && fabs(course) < 0.01, "junction: at the line's start the course of its first piece");
    // the turned map: a 4 x 4 canvas, the card 2 x 2 turned 90 degrees round their centres
    unsigned short src[16], dst[4];
    for (int i = 0; i < 16; i++)
        src[i] = (unsigned short)i;
    JunctionTurn(src, 4, 4, 4, 2, 2, dst, 2, 2, 2, 1, 1, 90, 0xFFFF, 0);   // up-left on the card: north-east on the map
    Check(dst[0] == src[1 * 4 + 3] && dst[1] == src[2 * 4 + 3] && dst[2] == src[1 * 4 + 2] && dst[3] == src[2 * 4 + 2],
          "junction: the canvas turned into the card (a course of 90: east up)");
    unsigned short flip[16], up[4];                                        // rows bottom-up (a DIB's): negative strides
    for (int i = 0; i < 16; i++)
        flip[i] = src[(3 - i / 4) * 4 + i % 4];
    JunctionTurn(flip + 12, 4, 4, -4, 2, 2, up + 2, 2, 2, -2, 1, 1, 90, 0xFFFF, 0);
    Check(up[2] == dst[0] && up[3] == dst[1] && up[0] == dst[2] && up[1] == dst[3], "junction: bottom-up rows too");
    JunctionTurn(src, 4, 4, 4, 0, 0, dst, 2, 2, 2, 1, 1, 0, 0xFFFF, 0);
    Check(dst[0] == 0xFFFF && dst[1] == 0xFFFF && dst[2] == 0xFFFF && dst[3] == src[0],
          "junction: off the canvas the background (left, above)");
    JunctionTurn(src, 4, 4, 4, 3, 3, dst, 2, 2, 2, 0, 0, 0, 0xFFFF, 0);
    Check(dst[0] == src[15] && dst[1] == 0xFFFF && dst[2] == 0xFFFF && dst[3] == 0xFFFF,
          "junction: off the canvas the background (right, below)");
    static unsigned short big[64 * 64];
    for (int i = 0; i < 64 * 64; i++)
        big[i] = (unsigned short)i;
    unsigned short far1;
    JunctionTurn(big, 64, 64, 64, 32, 62, &far1, 1, 1, 1, 60, 0, 90, 0xFFFF, 0);   // 60 px left on the card: 60 north
    Check(far1 == big[2 * 64 + 32], "junction: the turn exact 60 px away");
    static unsigned short card[16 * 16];   // round corners of radius 6: the corners' pixels left as they were
    for (int i = 0; i < 256; i++)
        card[i] = 1;
    JunctionTurn(big, 64, 64, 64, 32, 32, card, 16, 16, 16, 8, 8, 0, 0xFFFF, 6);
    Check(card[0] == 1 && card[3] == 1 && card[4] != 1 && card[16] == 1 && card[17] == 1 && card[18] != 1 &&
          card[3 * 16] == 1 && card[4 * 16] != 1 && card[15] == 1 && card[15 * 16] == 1 && card[255] == 1 &&
          card[15 * 16 + 3] == 1 && card[15 * 16 + 4] != 1 && card[8 * 16] != 1 && card[8 * 16 + 15] != 1 &&
          card[6 * 16 + 6] != 1, "junction: the card's corners round");
    Check(CornerInset(6, 0, 0, 16) == 4 && CornerInset(6, 1, 0, 16) == 2 && CornerInset(6, 3, 0, 16) == 1 &&
          CornerInset(6, 4, 0, 16) == 0 && CornerInset(6, 15, 0, 16) == 4 && CornerInset(6, 11, 0, 16) == 0 &&
          CornerInset(6, 8, 0, 16) == 0 && CornerInset(0, 0, 0, 16) == 0 && CornerInset(6, 10, 10, 26) == 4,
          "the rounded corners' inset by the row");
}

// The tilted frame out of the rings (ring.cpp): the near ring (the view's zoom) where it is loaded, the far one (two
// zooms out: 4 world px a ring px) past it, the fog above and fading in over the far rows
static unsigned short RingPx(const RingView& r, double wx, double wy)
{
    int x = ((int)floor(wx) >> r.shift) % r.size, y = ((int)floor(wy) >> r.shift) % r.size;
    return r.top[y * r.stride + x];
}

static void TiltWarpChecks()
{
    Check(Blend565(RGB565(0, 0, 0), RGB565(248, 252, 248), 0) == RGB565(0, 0, 0) &&
          Blend565(RGB565(0, 0, 0), RGB565(248, 252, 248), 32) == RGB565(248, 252, 248) &&
          Blend565(RGB565(0, 0, 0), RGB565(248, 252, 248), 16) == RGB565(120, 124, 120), "the 565 blend by 32nds");
    unsigned short ba = RGB565(200, 100, 40), bb = RGB565(40, 200, 160), bm = Blend565(ba, bb, 8);   // each channel its own
    Check((bm >> 11) == 25 + (5 - 25) * 8 / 32 && ((bm >> 5) & 63) == 25 + (50 - 25) * 8 / 32 &&
          (bm & 31) == 5 + (20 - 5) * 8 / 32, "the 565 blend: red, green, blue apart");
    static unsigned short nearBits[64 * 64], farBits[64 * 64], frame[60 * 40 + 1], up[60 * 40 + 1], sky[400 * 40];
    for (int i = 0; i < 64 * 64; i++) {
        nearBits[i] = (unsigned short)(i & 0x7FFF);
        farBits[i] = (unsigned short)(0x8000 | i);
    }
    const double vx = 1000.5, vy = 2000.5;
    RingView rings[2] = { { nearBits, 64, 64, 0, 980, 1980, 1021, 2021 }, { farBits, 64, 64, 2, 800, 1800, 1200, 2200 } };
    Tilt t;
    TiltMake(&t, 10, 10, 100, 60);   // the car 10 px below the centre, the fog 100 px ahead
    TiltWarp(rings, 2, t, 40, 60, vx, vy, 0, false, frame, 40);
    Check(frame[40 * 40 + 20] == RingPx(rings[0], vx, vy + 10) && frame[40 * 40 + 35] == RingPx(rings[0], vx + 15, vy + 10),
          "tilt frame: the car's row from the near ring, the map's own size");
    double fy, k, fade;
    int row = 0;
    for (int y = 0; y < 60 && !row; y++)   // the first row seen past the near ring (more than 20 px ahead)
        if (TiltRow(t, y - 30, &fy, &k, &fade) && fy < -20 && fade == 0)
            row = y;
    TiltRow(t, row - 30, &fy, &k, &fade);
    Check(row && frame[row * 40 + 20] == RingPx(rings[1], vx, vy + fy) &&
          frame[row * 40 + 30] == RingPx(rings[1], vx + 10 * k, vy + fy), "tilt frame: past the near ring the far one");
    unsigned char c[3];
    TiltFog(t, -30, false, c);
    Check(frame[20] == RGB565(c[0], c[1], c[2]) && frame[39] == RGB565(c[0], c[1], c[2]), "tilt frame: above the fog its colour");
    int fr = -1;
    for (int y = 0; y < 60 && fr < 0; y++)
        if (TiltRow(t, y - 30, &fy, &k, &fade) && fade > 0.2 && fade < 0.8)
            fr = y;
    TiltRow(t, fr - 30, &fy, &k, &fade);
    TiltFog(t, fr - 30, false, c);
    Check(fr >= 0 && frame[fr * 40 + 20] == Blend565(RingPx(rings[1], vx, vy + fy), RGB565(c[0], c[1], c[2]),
                                                     (int)(fade * 32 + 0.5)), "tilt frame: the fog fading in");
    TiltWarp(rings, 2, t, 40, 60, vx, vy, 90, true, frame, 40);   // course east: the screen's right is the south
    Check(frame[40 * 40 + 25] == RingPx(rings[0], vx - 10, vy + 5) && frame[20] == 0,
          "tilt frame: turned by the course; at night the fog black");
    RingView narrow = { nearBits, 64, 64, 0, 990, 1990, 1011, 2011 };   // only the near ring, loaded 10 px round
    TiltWarp(&narrow, 1, t, 40, 60, vx, vy, 0, false, frame, 40);
    TiltFog(t, 10, false, c);
    Check(frame[40 * 40 + 35] == RGB565(c[0], c[1], c[2]) && frame[40 * 40 + 25] == RingPx(narrow, vx + 5, vy + 10),
          "tilt frame: nothing loaded there - the fog's colour");
    frame[60 * 40] = up[60 * 40] = 0x1234;                              // a DIB's bottom-up rows: the same frame
    TiltWarp(rings, 2, t, 40, 60, vx, vy, 0, false, up + 59 * 40, -40);
    TiltWarp(rings, 2, t, 40, 60, vx, vy, 0, false, frame, 40);
    bool same = frame[60 * 40] == 0x1234 && up[60 * 40] == 0x1234;     // (and nothing past the rows written)
    for (int y = 0; y < 60; y++)
        same = same && !memcmp(up + (59 - y) * 40, frame + y * 40, 80);
    Check(same, "tilt frame: bottom-up rows too, each row its width only");
    RingView edge[2] = { { nearBits, 64, 64, 0, 990, 1990, 1011, 2010 }, { nearBits, 64, 64, 0, 990, 2010, 1011, 2030 } };
    TiltWarp(edge, 1, t, 40, 60, vx, vy, 0, false, frame, 40);          // the car's row at y 2010: the ring's end
    TiltFog(t, 10, false, c);
    unsigned short f10 = RGB565(c[0], c[1], c[2]);
    Check(frame[40 * 40 + 10] == f10 && frame[40 * 40 + 30] == f10 && frame[40 * 40 + 31] == f10,
          "tilt frame: a ring's last row and column not past it");
    TiltWarp(edge + 1, 1, t, 40, 60, vx, vy, 0, false, frame, 40);      // its first row 2010, its columns 990..1010
    Check(frame[40 * 40 + 10] == RingPx(edge[1], vx - 10, vy + 10) && frame[40 * 40 + 9] == f10 &&
          frame[40 * 40 + 30] == RingPx(edge[1], vx + 10, vy + 10) && frame[40 * 40 + 31] == f10,
          "tilt frame: a ring's first row and column in it, from x0 to x1 - 1");
    TiltWarp(rings, 2, t, 40, 60, vx, vy, 90, false, frame, 40);       // far, turned: the course exact
    TiltRow(t, row - 30, &fy, &k, &fade);
    Check(frame[row * 40 + 39] == RingPx(rings[1], vx - fy, vy + 19 * k), "tilt frame: far pixels turned exactly");
    static unsigned short wide[60 * 400];                               // a wide frame: the course exact 200 px aside
    RingView all = { farBits, 64, 64, 2, 0, 0, 4000, 4000 };
    TiltWarp(&all, 1, t, 400, 60, vx, vy, 90, false, wide, 400);
    Check(wide[row * 400 + 399] == RingPx(all, vx - fy, vy + 199 * k), "tilt frame: far pixels turned exactly, far aside");
    TiltWarp(rings, 2, t, 40, 400, vx, vy, 0, false, sky, 40);          // a tall frame: the sky's blue at its top
    Check(sky[0] == RGB565(170, 205, 238), "tilt frame: the sky's blue high up");
}

// VTileLocal: a shown tile from the data at hand - the parsed tile without reading, the pack's z15 before its z14,
// the cache's file; none: false (to download). z14 data tile (40, 20): MakeTile (water in its top left
// quarter); its top left z15 child: the quarter's houses (no water) - a z16 tile drawn from it is land, from the z14 one
// water.
static void LocalChecks(const unsigned char* tile, int tileLen, const unsigned char* houses, int housesLen)
{
    const unsigned short water = RGB565(168, 204, 226), land = RGB565(243, 240, 235);
    MapRequest d = { SRC_OSM, VEC_ZBASE + 14, 40, 20 }, p = { SRC_OSM, VEC_ZBASE + 15, 80, 40 };
    MapRequest r16 = { SRC_OSM, 16, 160, 80 }, r16b = { SRC_OSM, 16, 161, 81 }, r14 = { SRC_OSM, 14, 40, 20 };
    Image im = { 0 };
    Check(!VTileLocal(r14, &im) && !im.bmp && s_reads == 0, "local: no data - nothing drawn, nothing read");
    CachePath(s_file, d);
    s_fileData = tile;
    s_fileLen = tileLen;
    Check(VTileLocal(r14, &im) && s_reads == 1 && Px(im, 64, 64) == water, "local: the cache's file");
    ImageFree(&im);
    Check(VTileLocal(r16, &im) && s_reads == 1 && Px(im, 128, 128) == water,
          "local: z16 from the parsed z14 tile, nothing read again");
    ImageFree(&im);
    s_packKey[0] = p;
    s_packData[0] = houses;
    s_packLen[0] = housesLen;
    s_nPack = 1;
    Check(VTileLocal(r16, &im) && s_reads == 2 && Px(im, 128, 128) == land, "local: the pack's z15 tile first");
    ImageFree(&im);
    Check(VTileLocal(r16b, &im) && s_reads == 2 && Px(im, 128, 128) == land,
          "local: the parsed z15 tile, nothing read again");
    ImageFree(&im);
    s_file[0] = 0;
    s_packKey[1] = d;
    s_packData[1] = tile;
    s_packLen[1] = tileLen;
    s_nPack = 2;
    MapRequest r15 = { SRC_OSM, 15, 81, 41 };
    Check(VTileLocal(r15, &im) && Px(im, 128, 128) != water, "local: z15 with no pack z15 tile - the z14 one");
    ImageFree(&im);
    // a pack's tile not a vector tile (the user's card 2026-10-29: blobs 512 bytes off - a layer's middle first):
    // as none, the z14 one drawn
    static const unsigned char broken[] = { 0x82, 0xD0, 0xBE, 0xD0, 0xB2, 0x22, 0x21, 0x0A, 0x1F, 0xD0 };
    MapRequest pb = { SRC_OSM, VEC_ZBASE + 15, 82, 40 }, db = { SRC_OSM, VEC_ZBASE + 14, 41, 20 };
    s_packKey[0] = pb;
    s_packData[0] = broken;
    s_packLen[0] = sizeof(broken);
    s_packKey[1] = db;
    MapRequest r16c = { SRC_OSM, 16, 164, 80 };
    Check(VTileLocal(r16c, &im) && Px(im, 128, 128) == water, "local: a broken pack tile - the z14 one instead");
    ImageFree(&im);
    s_packData[1] = broken;
    s_packLen[1] = sizeof(broken);
    MapRequest r14b = { SRC_OSM, 14, 42, 20 }, db2 = { SRC_OSM, VEC_ZBASE + 14, 42, 20 };
    s_packKey[1] = db2;
    Check(!VTileLocal(r14b, &im) && !im.bmp, "local: only a broken tile - none (to download)");
    s_nPack = 0;
    s_reads = 0;
}

int main()
{
    VTileInit();
    TiltChecks();
    unsigned char data[4096];
    int len = MakeTile(data);
    static unsigned char quarter[4096];
    LocalChecks(data, len, quarter, MakeHouses(quarter));
    const unsigned short water = RGB565(168, 204, 226), road = RGB565(255, 228, 176), land = RGB565(243, 240, 235);

    // z14: the data tile itself
    MapRequest r = { SRC_OSM, 14, 9907, 5118 };
    Image im = { 0 };
    Check(VTileRender(r, data, len, &im), "z14 drawn");
    Check(Px(im, 64, 64) == water, "z14: water in the top left quarter");
    Check(Px(im, 200, 40) == land, "z14: land elsewhere");
    Check(Px(im, 128, 192) == road, "z14: the road across at 3/4");
    ImageFree(&im);

    // z15: the quarters of it, larger
    MapRequest tl = { SRC_OSM, 15, 9907 * 2, 5118 * 2 }, br = { SRC_OSM, 15, 9907 * 2 + 1, 5118 * 2 + 1 };
    Check(VTileRender(tl, data, len, &im) && Px(im, 250, 250) == water, "z15 top left: all water");
    ImageFree(&im);
    Check(VTileRender(br, data, len, &im) && Px(im, 10, 128) == road && Px(im, 10, 60) == land,
          "z15 bottom right: the road at its middle");
    ImageFree(&im);

    // night: its own palette
    g_cfg.night = 1;
    Check(VTileRender(r, data, len, &im) && Px(im, 64, 64) == RGB565(13, 34, 50), "night: dark water");
    ImageFree(&im);
    g_cfg.night = 0;

    // the town's name over the frame: the view's centre on the tile's centre, north up
    Image frame = { 0 };
    unsigned short* bits = ImageCreate(800, 480, &frame);
    for (int i = 0; i < 800 * 480; i++)
        bits[i] = 0;
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(dc, frame.bmp);
    RECT mr = { 0, 0, 800, 480 };
    VLabelsDraw(dc, mr, 400, 240, 9907 * 256.0 + 128, 5118 * 256.0 + 128, 0, 14, NULL, 0);
    GdiFlush();
    int lit = 0;   // the name's halo (white) around the point: the screen's (336, 336)
    for (int y = 320; y < 352; y++)
        for (int x = 300; x < 372; x++)
            lit += bits[(479 - y) * 800 + x] == RGB565(255, 255, 255);
    Check(lit > 20, "the town's name at its point");
    int stray = 0;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 800; x++)
            stray += bits[(479 - y) * 800 + x] != 0;
    Check(stray == 0, "nothing else drawn");
    SelectObject(dc, old);
    DeleteDC(dc);
    ImageFree(&frame);

    // the town's tile: every kind of area and road in the day palette
    static unsigned char city[8192], city2[8192];
    int clen = MakeCity(city, true), clen2 = MakeCity(city2, false);
    MapRequest c = { SRC_OSM, 14, 9000, 5000 };
    Check(VTileRender(c, city, clen, &im), "town z14 drawn");
    Check(Px(im, 48, 48) == land, "z14: no buildings yet");
    Check(Px(im, 160, 48) == RGB565(206, 232, 196), "z14: the park");
    Check(Px(im, 224, 48) == land, "z14: the wood not drawn (the user 2026-10-28: no greenery, Skoda Flow's map)");
    Check(Px(im, 32, 112) == RGB565(236, 232, 226), "z14: the residential area");
    Check(Px(im, 96, 112) == RGB565(234, 228, 232), "z14: the shops' area");
    Check(Px(im, 160, 112) == land, "z14: the grass not drawn");
    Check(Px(im, 224, 112) == land, "z14: farmland not drawn");
    Check(Px(im, 96, 48) == land, "z14: farmland's landcover not drawn");
    Check(Px(im, 128, 144) == RGB565(255, 206, 150), "z14: the trunk road");
    Check(Px(im, 128, 164) == RGB565(255, 247, 214), "z14: the tertiary road");
    Check(Px(im, 128, 184) == RGB565(255, 255, 255), "z14: the minor road");
    int casing = 0;
    for (int y = 176; y < 192; y++)
        casing += Px(im, 128, y) == RGB565(214, 208, 200);
    Check(casing >= 2, "z14: the minor road's casing");
    Check(Px(im, 128, 204) == RGB565(184, 180, 174), "z14: the railway");
    Check(Px(im, 128, 220) == water, "z14: the river");
    Check(Px(im, 128, 236) == land, "z14: no service roads yet");
    Check(Px(im, 128, 250) == land, "z14: the ferry not drawn");
    ImageFree(&im);
    MapRequest ctl = { SRC_OSM, 15, 18000, 10000 }, cbl = { SRC_OSM, 15, 18000, 10001 };
    Check(VTileRender(ctl, NULL, 0, &im) && Px(im, 96, 96) == RGB565(224, 217, 208), "z15: the building");
    ImageFree(&im);
    Check(VTileRender(cbl, NULL, 0, &im) && Px(im, 128, 216) == RGB565(255, 255, 255), "z15: the service road");
    ImageFree(&im);

    // the town's names: the view's centre on the tile's centre
    MapRequest c2 = { SRC_OSM, 14, 9100, 5000 };
    Check(VTileRender(c2, city2, clen2, &im), "town without the village drawn");
    ImageFree(&im);
    const unsigned short white = RGB565(255, 255, 255), blue = RGB565(46, 110, 190);
    Frame f, g;
    Open(f);
    Open(g);
    Labels(f, 9000, 5000, 128, 128, 14, 0, mr, NULL, 0);
    Check(Count(f, 380, 268, 436, 284, white) > 20, "z14: the street's name along it");
    Check(Lit(f, 370, 314, 430, 330) == 0, "z14: the same street close by not named again");
    Check(Count(f, 370, 344, 430, 360, white) > 20, "z14: the other street's name");
    Check(Count(f, 326, 246, 347, 267, blue) > 100, "z14: the filling station's sign");
    Check(Lit(f, 306, 268, 366, 284) == 0, "z14: no station's name under its sign yet");
    Check(Lit(f, 454, 246, 475, 267) == 0, "z14: no car park signs yet");
    Check(Count(f, 370, 166, 430, 186, white) > 20, "z14: the city's name");
    Tilt tNear, tFar;                  // the third person's view: the city's name 64 px ahead of the view's centre
    TiltMake(&tFar, 0, 0, 200, 60);   // the fog 200 px ahead: the name seen, tNearer the centre (the depth shortened)
    TiltMake(&tNear, 0, 0, 100, 60);  // the fog 100 px ahead: 64 px is where it thickens - no name
    double nx, ny;
    TiltPoint(tFar, 0, -64, &nx, &ny);
    g_tilt = &tFar;
    Labels(f, 9000, 5000, 128, 128, 14, 0, mr, NULL, 0);
    int at = (int)(240 + ny);
    Check(Count(f, 370, at - 10, 430, at + 10, white) > 20 && Lit(f, 370, 166, 430, 186) == 0,
          "tilted: the city's name at its tilted place");
    TiltPoint(tFar, 0, 36, &nx, &ny);   // the street 36 px below the centre: its name along it, still level
    at = (int)(240 + ny);
    Check(Count(f, 370, at - 8, 436, at + 8, white) > 200, "tilted: the street's name along it, level");
    g_tilt = &tNear;
    Labels(f, 9000, 5000, 128, 128, 14, 0, mr, NULL, 0);
    Check(Lit(f, 300, 100, 500, 240) == 0, "tilted: no names where the fog thickens");
    g_tilt = NULL;
    Labels(f, 9000, 5000, 128, 128, 14, 0, mr, NULL, 0);   // (the flat frame back: the next check compares it)
    Labels(g, 9100, 5000, 128, 128, 14, 0, mr, NULL, 0);
    Check(!memcmp(f.bits, g.bits, 800 * 480 * 2), "z14: the village under the city not drawn");
    RECT all = { 0, 0, 800, 480 };
    Labels(f, 9000, 5000, 128, 128, 14, 0, mr, &all, 1);
    Check(Lit(f, 0, 0, 800, 480) == 0, "nothing over the controls");
    RECT left = { 0, 0, 400, 480 };
    Labels(f, 9000, 5000, 128, 128, 14, 0, left, NULL, 0);
    Check(Lit(f, 400, 0, 800, 480) == 0 && Count(f, 326, 246, 347, 267, blue) > 100, "only names whole on the map");
    Labels(f, 9000, 5000, 128, 128, 14, 90, mr, NULL, 0);
    Check(Count(f, 428, 200, 446, 264, white) > 20 && Lit(f, 380, 268, 436, 284) == 0,
          "turned 90 degrees: the street's name turned with it");
    Labels(f, 9000, 5000, 64, 144, 16, 0, mr, NULL, 0);
    Check(Count(f, 370, 252, 430, 268, white) > 10, "z16: the station's name under its sign");
    Labels(f, 9000, 5000, 192, 144, 17, 0, mr, NULL, 0);
    Check(Count(f, 390, 231, 411, 251, blue) > 100, "z17: the car park's sign");
    Check(Count(f, 255, 230, 290, 250, RGB565(120, 112, 104)) > 3, "z17: the house number");
    Labels(f, 9000, 5000, 176, 144, 16, 0, mr, NULL, 0);   // (the user 2026-10-29: from z16)
    Check(Count(f, 0, 0, 800, 480, RGB565(120, 112, 104)) > 3, "z16: the house number");
    Labels(f, 9000, 5000, 176, 144, 15, 0, mr, NULL, 0);
    Check(Count(f, 0, 0, 800, 480, RGB565(120, 112, 104)) == 0, "z15: no house numbers");
    static unsigned char houses[4096];
    MapRequest hq = { SRC_OSM, 14, 9200, 5000 };
    Check(VTileRender(hq, houses, MakeHouses(houses), &im), "the quarter's tile drawn");
    ImageFree(&im);
    Labels(f, 9200, 5000, 128, 128, 16, 0, mr, NULL, 0);   // the last row: the 161st to 176th house numbers
    Check(Count(f, 0, 426, 800, 454, RGB565(120, 112, 104)) > 3 * kHouseCols, "z16: 176 house numbers all drawn");
    Close(f);
    Close(g);

    unsigned char country[4096], pmCountry[4096];
    int nc = MakeCountry(country), npc = MakePmCountry(pmCountry);
    PlacesByZoom(country, nc, 0, "OpenMapTiles");
    PlacesByZoom(pmCountry, npc, 3, "Protomaps");
    Twins();
    Deep();
    Levels();
    Joints();
    JunctionChecks();
    WheelChecks();
    YoChecks();
    TiltWarpChecks();
    RECT whole = { 0, 0, 800, 480 };
    // the Russian name when the tile has one (name:ru), else the local one: a frame named so equals one whose tile
    // holds that text as its plain name
    Frame r1, r2;
    Open(r1);
    Open(r2);
    for (int k = 0; k < 2; k++) {
        Pbw tile = { { 0 }, 0 }, l = { { 0 }, 0 };
        Str(l, 1, "place");
        Str(l, 3, "class");
        Str(l, 3, "name");
        Str(l, 3, "name:ru");
        Str(l, 3, "capital");
        Value(l, "city");
        Value(l, k ? "Kiev" : "Kyiv");
        Value(l, "Kiev");
        Pbw c = { { 0 }, 0 };
        Var(c, 5 << 3);
        Var(c, 2);
        Bytes(l, 4, c.b, c.n);
        unsigned int tags[8] = { 0, 0, 1, 1, 2, 2, 3, 3 }, g[3];
        Point(g, 128, 128);
        Feature(l, tags, k ? 4 : 8, 1, g, 3);   // the second: only the class and the plain name "Kiev" - not a capital,
        Bytes(tile, 3, l.b, l.n);              // shown at z10
        MapRequest q = { SRC_OSM, 10, 600 + k, 300 };
        Image ti = { 0 };
        VTileRender(q, tile.b, tile.n, &ti);
        ImageFree(&ti);
        Frame& fr = k ? r2 : r1;
        VLabelsDraw(fr.dc, whole, 400, 240, (600 + k) * 256.0 + 128, 300 * 256.0 + 128, 0, 10, NULL, 0);
        GdiFlush();
    }
    bool same = true;   // around the name (the second view also sees the first tile, 256 px left)
    for (int y = 220; y < 260; y++)
        same = same && !memcmp(r1.bits + (479 - y) * 800 + 300, r2.bits + (479 - y) * 800 + 300, 200 * 2);
    Check(Count(r1, 360, 228, 440, 252, white) > 10 && same, "the place's Russian name when it has one");
    Close(r1);
    Close(r2);

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
