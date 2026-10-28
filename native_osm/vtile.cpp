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
enum { K_RESID, K_INDUSTRIAL, K_PARK, K_WATER, K_BUILDING,
       K_WATERWAY, K_RAIL, K_PATH, K_SERVICE, K_MINOR, K_SECONDARY, K_PRIMARY, K_MAJOR, K_PLACE, K_ROADNAME, K_HOUSENUM, K_FUEL, K_PARKING, K_COUNT };
static const int kFirstLine = K_WATERWAY;

// The two palettes of design v3 (VestaHome/skin/make_osm_v3_mockup.py, 2026-10-26): the day warm and light, the night
// its own dark one (not the raster map's inverted lightness), roads without casings. K_PLACE, K_ROADNAME, K_HOUSENUM:
// the text, c* its halo; K_FUEL, K_PARKING: the sign's colour.
struct Style { unsigned char r, g, b; int width; bool cased; unsigned char cr, cg, cb; };   // width 0: area
static const Style kStyle[2][K_COUNT] = { {
    { 236, 232, 226, 0 }, { 234, 228, 232, 0 },
    { 206, 232, 196, 0 }, { 168, 204, 226, 0 }, { 224, 217, 208, 0 },
    { 168, 204, 226, 2 }, { 184, 180, 174, 2 }, { 206, 196, 184, 1 },
    { 255, 255, 255, 2, true, 214, 208, 200 }, { 255, 255, 255, 5, true, 214, 208, 200 },
    { 255, 247, 214, 7, true, 222, 204, 160 }, { 255, 228, 176, 8, true, 224, 182, 120 },
    { 255, 206, 150, 9, true, 214, 150, 90 },
    { 40, 44, 50, 0, true, 255, 255, 255 }, { 66, 70, 76, 0, true, 255, 255, 255 },
    { 120, 112, 104, 0, true, 243, 240, 235 },
    { 46, 110, 190, 0, true, 255, 255, 255 }, { 46, 110, 190, 0, true, 255, 255, 255 },
}, {
    { 22, 25, 30, 0 }, { 25, 25, 31, 0 },
    { 21, 37, 29, 0 }, { 13, 34, 50, 0 }, { 31, 35, 42, 0 },
    { 13, 34, 50, 2 }, { 48, 52, 58, 2 }, { 32, 36, 42, 1 },
    { 38, 43, 50, 2 }, { 44, 50, 58, 5 }, { 62, 70, 80, 7 }, { 84, 80, 68, 8 }, { 112, 88, 58, 9 },
    { 205, 208, 214, 0, true, 11, 12, 14 }, { 150, 156, 166, 0, true, 11, 12, 14 },
    { 110, 116, 126, 0, true, 17, 19, 23 },
    { 58, 118, 196, 0, true, 11, 12, 14 }, { 58, 118, 196, 0, true, 11, 12, 14 },
} };
static const unsigned char kBg[2][3] = { { 243, 240, 235 }, { 17, 19, 23 } };
static const unsigned char kShadow[2][3] = { { 206, 200, 190 }, { 6, 7, 9 } };   // a bridge's shadow on the ground
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
    unsigned char minz;         // the name shown from this zoom (places: by their size, PlaceMinZoom)
    signed char level;          // -1 a tunnel, 0 the ground, 1 a bridge (Level)
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

// The next field: true = it is the length-delimited field `field` (into s), else it is skipped
static bool NextField(Pb& b, int field, Pb& s)
{
    unsigned int tag = Varint(b);
    if ((int)(tag >> 3) == field && (tag & 7) == 2) {
        s = Sub(b);
        return true;
    }
    Skip(b, tag & 7);
    return false;
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

// What a feature is drawn as, by its layer, class (NULL: any) and geometry type (1 point, 2 line, 3 area), and its
// name's rank (0 the biggest: a city, a main road .. 2); the first row that matches wins, none or kind -1: not drawn.
// Two schemas: OpenMapTiles' (OpenFreeMap, planetiler) and Protomaps' v4 (their planet build cut into a region's
// pack): its class is "kind" or the finer "kind_detail" (a row's class matches either), its roads carry their names
// (street: the street name's rank + 1, 0 none - OpenMapTiles has them in transportation_name).
struct KindRow { const char* layer; const char* cls; unsigned char type; signed char kind, rank; unsigned char street; };
static const KindRow kKinds[] = {
    { "buildings", NULL, 3, K_BUILDING, 2 },
    { "landuse", "park", 3, K_PARK, 2 },   // (no woods, fields, grass: the user 2026-10-28, Skoda Flow's map)
    { "landcover", "urban_area", 3, K_RESID, 2 },
    { "water", NULL, 2, K_WATERWAY, 2 },
    { "roads", "highway", 2, K_MAJOR, 2, 1 }, { "roads", "trunk", 2, K_MAJOR, 2, 1 },
    { "roads", "trunk_link", 2, K_MAJOR, 2, 1 }, { "roads", "primary", 2, K_PRIMARY, 2, 1 },
    { "roads", "primary_link", 2, K_PRIMARY, 2, 1 }, { "roads", "secondary", 2, K_SECONDARY, 2, 2 },
    { "roads", "secondary_link", 2, K_SECONDARY, 2, 2 }, { "roads", "tertiary", 2, K_SECONDARY, 2, 2 },
    { "roads", "tertiary_link", 2, K_SECONDARY, 2, 2 }, { "roads", "service", 2, K_SERVICE, 2 },
    { "roads", "minor_road", 2, K_MINOR, 2, 3 }, { "roads", "path", 2, K_PATH, 2 }, { "roads", "rail", 2, K_RAIL, 2 },
    { "places", "city", 1, K_PLACE, 0 }, { "places", "town", 1, K_PLACE, 0 }, { "places", "village", 1, K_PLACE, 1 },
    { "places", "suburb", 1, K_PLACE, 1 }, { "places", "quarter", 1, K_PLACE, 1 }, { "places", NULL, 1, K_PLACE, 2 },
    { "pois", "fuel", 1, K_FUEL, 0 }, { "pois", "parking", 1, K_PARKING, 1 },
    { "buildings", "address", 1, K_HOUSENUM, 2 },   // (z15: the house numbers' points)
    { "water", NULL, 3, K_WATER, 2 }, { "building", NULL, 3, K_BUILDING, 2 }, { "park", NULL, 3, K_PARK, 2 },
    { "landuse", "residential", 3, K_RESID, 2 }, { "landuse", "suburb", 3, K_RESID, 2 },
    { "landuse", "neighbourhood", 3, K_RESID, 2 },
    { "landuse", "industrial", 3, K_INDUSTRIAL, 2 }, { "landuse", "commercial", 3, K_INDUSTRIAL, 2 },
    { "landuse", "retail", 3, K_INDUSTRIAL, 2 }, { "landuse", "railway", 3, K_INDUSTRIAL, 2 },
    { "waterway", NULL, 2, K_WATERWAY, 2 },
    { "transportation", "motorway", 2, K_MAJOR, 2 }, { "transportation", "trunk", 2, K_MAJOR, 2 },
    { "transportation", "primary", 2, K_PRIMARY, 2 },
    { "transportation", "secondary", 2, K_SECONDARY, 2 }, { "transportation", "tertiary", 2, K_SECONDARY, 2 },
    { "transportation", "minor", 2, K_MINOR, 2 }, { "transportation", "service", 2, K_SERVICE, 2 },
    { "transportation", "track", 2, K_PATH, 2 }, { "transportation", "path", 2, K_PATH, 2 },
    { "transportation", "rail", 2, K_RAIL, 2 }, { "transportation", "transit", 2, K_RAIL, 2 },
    { "place", "city", 1, K_PLACE, 0 }, { "place", "town", 1, K_PLACE, 0 },
    { "place", "suburb", 1, K_PLACE, 1 }, { "place", "village", 1, K_PLACE, 1 }, { "place", "quarter", 1, K_PLACE, 1 },
    { "place", NULL, 1, K_PLACE, 2 },
    { "transportation_name", "motorway", 2, K_ROADNAME, 0 }, { "transportation_name", "trunk", 2, K_ROADNAME, 0 },
    { "transportation_name", "primary", 2, K_ROADNAME, 0 },
    { "transportation_name", "secondary", 2, K_ROADNAME, 1 }, { "transportation_name", "tertiary", 2, K_ROADNAME, 1 },
    { "transportation_name", "minor", 2, K_ROADNAME, 2 },
    { "transportation_name", NULL, 2, -1, 2 },   // (service roads, paths: no names)
    { "housenumber", NULL, 1, K_HOUSENUM, 2 },
    { "poi", "fuel", 1, K_FUEL, 0 }, { "poi", "parking", 1, K_PARKING, 1 },   // for the driver: filling stations, car parks
};

static const KindRow* Row(const Pb& layer, const Pb& cls, const Pb& detail, int geomType)
{
    for (int i = 0; i < (int)(sizeof(kKinds) / sizeof(kKinds[0])); i++) {
        const KindRow& k = kKinds[i];
        if (k.type == geomType && StrEq(layer, k.layer) && (!k.cls || StrEq(cls, k.cls) || StrEq(detail, k.cls)))
            return k.kind < 0 ? NULL : &k;
    }
    return NULL;
}

// The tags read: a feature's class (Protomaps: "kind", its finer "kind_detail"), its name and the Russian one (when
// there is one: "Киев", not "Київ"), a house number (its own key), a bridge or tunnel (OpenMapTiles' "brunnel") -
// strings; a place's rank, capital level and population, Protomaps' is_bridge, is_tunnel - numbers (0: none)
enum { T_CLASS, T_DETAIL, T_NAME, T_NAMERU, T_HOUSE, T_BRUNNEL, T_STRINGS, T_RANK = T_STRINGS, T_CAPITAL, T_POPULATION,
       T_BRIDGE, T_TUNNEL, T_COUNT };
static const char* const kTagKeys[T_COUNT][2] = { { "class", "kind" }, { "kind_detail" }, { "name" }, { "name:ru" },
                                                   { "housenumber", "addr_housenumber" }, { "brunnel" }, { "rank" },
                                                   { "capital" }, { "population" }, { "is_bridge" }, { "is_tunnel" } };

// A layer's name and the indexes of its keys of the tags read (-1 none); its values in s_vals (the strings) and
// s_nums (as numbers)
struct LayerHead { Pb name; int nv, key[T_COUNT]; };
static Pb s_keys[512], s_vals[2048];   // (not on the stack: CE threads have little; s_cs held)
static int s_nums[2048];

static void Keep(Pb* a, int& n, int cap, const Pb& v)
{
    if (n < cap)
        a[n++] = v;
}

static Pb StringValue(Pb v)
{
    Pb sv = { 0, 0 }, s;
    while (v.p < v.end)
        if (NextField(v, 1, s))
            sv = s;
    return sv;
}

// Protomaps' capital, a string: a level's one digit ("4" a region's centre), "yes" 2 (a country's: OSM's admin
// level 2); other strings: not a level 2..4
static int StringNumber(const Pb& s)
{
    if (StrEq(s, "yes"))
        return 2;
    return s.end - s.p == 1 ? *s.p - '0' : 0;
}

static int NumberValue(Pb v)   // int (field 4), uint (5), sint (6), bool (7), a string (1: StringNumber); else 0
{
    int n = 0;
    while (v.p < v.end) {
        unsigned int tag = Varint(v);
        int field = tag >> 3;
        if (tag == (1 << 3 | 2)) {   // (field 1, length-delimited)
            n = StringNumber(Sub(v));
            continue;
        }
        if ((tag & 7) != 0) {
            Skip(v, tag & 7);
            continue;
        }
        unsigned int x = Varint(v);
        if (field == 4 || field == 5 || field == 7)
            n = (int)x;
        else if (field == 6)
            n = (int)((x >> 1) ^ (0 - (x & 1)));
    }
    return n;
}

static bool IsKey(const Pb& key, int tag)
{
    const char* const* k = kTagKeys[tag];
    return StrEq(key, k[0]) || (k[1] && StrEq(key, k[1]));
}

static void KeyIndexes(LayerHead& h, int nk)
{
    for (int tag = 0; tag < T_COUNT; tag++) {
        h.key[tag] = -1;
        for (int i = 0; i < nk; i++)
            if (IsKey(s_keys[i], tag))
                h.key[tag] = i;
    }
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
        else if (field == 4 && h.nv < 2048) {
            s_nums[h.nv] = NumberValue(s);
            Keep(s_vals, h.nv, 2048, StringValue(s));
        }
    }
    KeyIndexes(h, nk);
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

// A feature's tags (T_*): the strings, the numbers; the name the Russian one when it has one
struct Tags { Pb s[T_STRINGS]; int n[T_COUNT - T_STRINGS]; };

static void ReadTags(Pb tags, const LayerHead& h, Tags& t)
{
    while (tags.p < tags.end) {
        unsigned int k = Varint(tags), v = Varint(tags);
        for (int tag = 0; tag < T_COUNT && v < (unsigned)h.nv; tag++)
            if ((int)k == h.key[tag] && tag < T_STRINGS)
                t.s[tag] = s_vals[v];
            else if ((int)k == h.key[tag])
                t.n[tag - T_STRINGS] = s_nums[v];
    }
    if (t.s[T_NAMERU].p)
        t.s[T_NAME] = t.s[T_NAMERU];
}

// A region's centre or a city of ~240 000 people and up (OpenMapTiles has no population: its rank 7 or bigger)
static bool BigCity(const Tags& t)
{
    int rank = t.n[T_RANK - T_STRINGS], capital = t.n[T_CAPITAL - T_STRINGS];
    bool city = StrEq(t.s[T_CLASS], "city") || StrEq(t.s[T_DETAIL], "city");
    return t.n[T_POPULATION - T_STRINGS] >= 240000 || (city && ((rank > 0 && rank <= 7) || capital == 3 || capital == 4));
}

// The zoom a place's name shows from (the user 2026-10-27): a country's capital from z3, a big city (BigCity) from z7,
// other cities, towns, villages, suburbs from z10, the rest from z14.
static int PlaceMinZoom(const Tags& t, int nameRank)
{
    if (t.n[T_CAPITAL - T_STRINGS] == 2)
        return 3;
    if (BigCity(t))
        return 7;
    return nameRank < 2 ? 10 : 14;
}

static int Level(const Tags& t)   // -1 a tunnel, 0 the ground, 1 a bridge
{
    if (StrEq(t.s[T_BRUNNEL], "bridge") || t.n[T_BRIDGE - T_STRINGS])
        return 1;
    return StrEq(t.s[T_BRUNNEL], "tunnel") || t.n[T_TUNNEL - T_STRINGS] ? -1 : 0;
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

// A Protomaps road's street name: a feature of its own on the road's geometry (OpenMapTiles' transportation_name)
static bool StreetName(VTile& t, const Feat& road, const KindRow& row, const Pb& nm)
{
    if (!row.street)   // (no name: Named drops it)
        return true;
    Feat s = road;
    s.kind = K_ROADNAME;
    s.rank = (unsigned char)(row.street - 1);
    return !Named(t, s, K_ROADNAME, nm) || Push(t.f, t.nf, t.fcap, s);
}

// One feature into the tile (or skipped); false = out of memory
static bool DecodeFeature(Pb f, const LayerHead& h, VTile& t)
{
    Pb tags = { 0, 0 }, geom = { 0, 0 };
    Tags tg;
    memset(&tg, 0, sizeof(tg));
    int type = 0;
    ReadFeature(f, tags, type, geom);
    ReadTags(tags, h, tg);
    const KindRow* row = Row(h.name, tg.s[T_CLASS], tg.s[T_DETAIL], type);
    if (!row)
        return true;
    Feat ft;
    ft.kind = (unsigned char)row->kind;
    ft.nparts = 0;
    ft.part0 = t.nparts / 2;
    ft.x0 = ft.y0 = 32767;
    ft.x1 = ft.y1 = -32768;
    ft.name = -1;
    ft.rank = (unsigned char)row->rank;
    ft.minz = (unsigned char)(row->kind == K_PLACE ? PlaceMinZoom(tg, row->rank) : 0);
    ft.level = (signed char)Level(tg);
    if (!DecodeGeometry(geom, t, ft))
        return false;
    if (!ft.nparts || !Named(t, ft, row->kind, tg.s[row->kind == K_HOUSENUM ? T_HOUSE : T_NAME]))
        return true;
    return Push(t.f, t.nf, t.fcap, ft) && StreetName(t, ft, *row, tg.s[T_NAME]);
}

static bool DecodeLayer(Pb b, VTile& t)
{
    LayerHead h;
    ReadLayerHead(b, h);
    Pb f;
    bool ok = true;
    while (ok && b.p < b.end)
        if (NextField(b, 2, f))
            ok = DecodeFeature(f, h, t);
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
struct VLabel { unsigned char kind, rank, minz; short x, y, dx, dy; int name; };
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
    l.minz = f.minz;
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
    Pb b = { data, data + len }, layer;
    bool ok = true;
    while (ok && b.p < b.end)
        if (NextField(b, 3, layer))
            ok = DecodeLayer(layer, t);
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

// The next feature of kind k from fi whose box is near the drawn tile (margin in px); t.nf: none
static int NextShown(const VTile& t, const Xf& xf, int k, int margin, int fi)
{
    while (fi < t.nf && (t.f[fi].kind != k || !BoxVisible(xf, t.f[fi], margin)))
        fi++;
    return fi;
}

// One part of an area as edges: its ring, closed
static void AddRing(int& ne, const VTile& t, const Xf& xf, const Feat& f, int pi)
{
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

// The areas, kind by kind (the later over the earlier)
static void FillAreas(const VTile& t, const Xf& xf, int z, const Style* pal, unsigned short* bits)
{
    for (int k = 0; k < kFirstLine; k++) {
        if (z < MinZoom(k))
            continue;
        unsigned short c = RGB565(pal[k].r, pal[k].g, pal[k].b);
        for (int fi = NextShown(t, xf, k, 0, 0); fi < t.nf; fi = NextShown(t, xf, k, 0, fi + 1)) {
            int ne = 0;
            for (int pi = 0; pi < t.f[fi].nparts; pi++)
                AddRing(ne, t, xf, t.f[fi], pi);
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
static void StrokePart(HDC dc, const VTile& t, const Xf& xf, const Feat& f, int pi)
{
    int p0 = t.parts[(f.part0 + pi) * 2], pn = t.parts[(f.part0 + pi) * 2 + 1];
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

// The lines of kind k and of a level with a pen w px wide
static void StrokeKind(HDC dc, const VTile& t, const Xf& xf, int k, int level, int w, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, w, color);
    HGDIOBJ op = SelectObject(dc, pen);
    for (int fi = NextShown(t, xf, k, w, 0); fi < t.nf; fi = NextShown(t, xf, k, w, fi + 1))
        for (int pi = 0; pi < t.f[fi].nparts && t.f[fi].level == level; pi++)
            StrokePart(dc, t, xf, t.f[fi], pi);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// A line's colour: a casing's (a bridge's without one: the background's - the gap over what it crosses), a tunnel's
// half the background
static COLORREF LineColor(const Style& s, const unsigned char* bg, bool casing, int level)
{
    if (casing)
        return s.cased ? RGB(s.cr, s.cg, s.cb) : RGB(bg[0], bg[1], bg[2]);
    if (level < 0)
        return RGB((s.r + bg[0]) / 2, (s.g + bg[1]) / 2, (s.b + bg[2]) / 2);
    return RGB(s.r, s.g, s.b);
}

// The lines of a level, kind by kind: the casings (2 px wider, of the cased kinds and every bridge; none under the
// ground), or the lines themselves
static void StrokeLines(HDC dc, const VTile& t, const Xf& xf, int z, const Style* pal, const unsigned char* bg,
                        int level, bool casing)
{
    for (int k = kFirstLine; k < K_PLACE; k++) {
        const Style& sty = pal[k];
        if ((casing && !(level > 0 || (sty.cased && level == 0))) || z < MinZoom(k))
            continue;
        StrokeKind(dc, t, xf, k, level, LineWidth(sty.width, z) + (casing ? 2 : 0), LineColor(sty, bg, casing, level));
    }
}

// The bridges' shadows (the user 2026-10-28: their height seen, in the third person's view too): each bridge's line
// as wide as its casing, shifted south-east by 3/4 of its width - drawn over the ground, under the bridges
static void StrokeShadows(HDC dc, const VTile& t, const Xf& xf, int z, const Style* pal, const unsigned char* c)
{
    for (int k = kFirstLine; k < K_PLACE; k++) {
        if (z < MinZoom(k))
            continue;
        int w = LineWidth(pal[k].width, z), off = (w * 3 + 2) / 4;
        Xf sh = xf;
        sh.ox -= off * 16;
        sh.oy -= off * 16;
        StrokeKind(dc, t, sh, k, 1, w + 2, RGB(c[0], c[1], c[2]));
    }
}

static void Draw(const VTile& t, const Xf& xf, int z, bool night, HDC dc, unsigned short* bits)
{
    const Style* pal = kStyle[night ? 1 : 0];
    const unsigned char* bc = kBg[night ? 1 : 0];
    unsigned short bg = RGB565(bc[0], bc[1], bc[2]);
    for (int i = 0; i < TILE * TILE; i++)
        bits[i] = bg;
    // areas, kind by kind, then the lines with their casings level by level: the tunnels, the ground, the bridges'
    // shadows and the bridges (the labels: over the frame, VLabelsDraw)
    FillAreas(t, xf, z, pal, bits);
#ifndef UNDER_CE
    GdiFlush();   // the desktop's GDI may batch: our pixels before its lines (CE draws at once)
#endif
    for (int level = -1; level <= 1; level++) {
        if (level > 0)
            StrokeShadows(dc, t, xf, z, pal, kShadow[night ? 1 : 0]);
        StrokeLines(dc, t, xf, z, pal, bc, level, true);
        StrokeLines(dc, t, xf, z, pal, bc, level, false);
    }
#ifndef UNDER_CE
    GdiFlush();
#endif
}

// The data tile a shown tile is drawn from: its own up to zoom maxz, else its ancestor of that zoom.
MapRequest VTileDataAt(const MapRequest& r, int maxz)
{
    int dz = r.z > maxz ? r.z - maxz : 0;
    MapRequest k = { SRC_OSM, VEC_ZBASE + r.z - dz, r.x >> dz, r.y >> dz };
    return k;
}

MapRequest VTileData(const MapRequest& r)   // OpenFreeMap's (and the cache's): up to z14
{
    return VTileDataAt(r, VEC_MAXZ);
}

bool VTileRender(const MapRequest& r, const unsigned char* data, int len, Image* out)
{
    return VTileRenderFrom(r, VTileData(r), data, len, out);
}

bool VTileRenderFrom(const MapRequest& r, const MapRequest& key, const unsigned char* data, int len, Image* out)
{
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

static bool Overlap(const Placed& a, const Placed& b)
{
    return b.l < a.r && b.r > a.l && b.t < a.b && b.b > a.t;
}

// The box whole on the map and over none of the placed ones
static bool Free(const LabelView& v, const Placed& box)
{
    const RECT& mr = *v.mr;
    if (box.l < mr.left || box.t < mr.top || box.r > mr.right || box.b > mr.bottom)
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
    { K_PLACE, 0, 0 }, { K_PLACE, 1, 0 }, { K_PLACE, 2, 0 },   // (each place from its own zoom: PlaceMinZoom)
    { K_FUEL, 0, 14 }, { K_PARKING, 1, 17 },
    { K_ROADNAME, 0, 14 }, { K_ROADNAME, 1, 14 }, { K_ROADNAME, 2, 15 },
    { K_HOUSENUM, 2, 15 },   // (the user 2026-10-27: from z15)
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

// A z14 tile's label in a quarter a pack's z15 tile has its own labels for (drawn instead: no name twice)
static bool Deeper(const VLabelSet& set, const VLabel& lb)
{
    if (set.key.z != VEC_ZBASE + VEC_PACKZ - 1)
        return false;
    MapRequest quarter = { SRC_OSM, VEC_ZBASE + VEC_PACKZ, set.key.x * 2 + (lb.x >= kExtent / 2),
                           set.key.y * 2 + (lb.y >= kExtent / 2) };
    VLabelSet* oldest;
    return Recent(s_labels, kLabelSets, quarter, oldest) != NULL;
}

static void DrawSet(LabelView& v, const LabelPass& p, HFONT pf, const VLabelSet& set)   // one tile's labels of a pass
{
    for (int li = 0; li < set.n && v.np < v.cap; li++) {
        const VLabel& lb = set.l[li];
        double x, y;
        if (lb.rank == p.rank && lb.kind == p.kind && lb.minz <= v.z && OnScreen(v, set, lb, x, y) && !Deeper(set, lb))
            PlaceLabel(v, p.kind, pf, set, lb, x, y);
    }
}

static void DrawPass(LabelView& v, const LabelPass& p, int zd)   // the labels of a pass, of all tiles of zoom zd
{
    HFONT pf = PassFont(p.kind, p.rank);
    v.unit = (double)(1 << (v.z - zd)) * TILE / kExtent;   // world px per tile unit
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
    v.z = z;
    v.pal = kStyle[g_cfg.night ? 1 : 0];
    v.font0 = NULL;
    v.np = 0;
    AddAvoid(v, avoid, navoid);
    v.cap = v.np + (z >= 15 ? 400 : 160);   // (close up: the house numbers too)
    SetBkMode(dc, TRANSPARENT);
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < (int)(sizeof(kPasses) / sizeof(kPasses[0])); i++)
        if (z >= kPasses[i].minZoom) {
            if (z >= VEC_PACKZ)
                DrawPass(v, kPasses[i], VEC_PACKZ);   // a pack's z15 tiles first (Deeper: their quarters theirs)
            DrawPass(v, kPasses[i], zd);
        }
    LeaveCriticalSection(&s_cs);
    if (v.font0)
        SelectObject(dc, v.font0);
    LogLabelTime(GetTickCount() - t0, v.np - navoid);
}
