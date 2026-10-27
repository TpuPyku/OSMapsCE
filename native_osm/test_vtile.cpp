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
bool ReadWholeFile(const wchar_t*, unsigned char**, int*) { return false; }

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

int main()
{
    VTileInit();
    unsigned char data[4096];
    int len = MakeTile(data);
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
    Check(Px(im, 224, 48) == RGB565(198, 222, 186), "z14: the wood");
    Check(Px(im, 32, 112) == RGB565(236, 232, 226), "z14: the residential area");
    Check(Px(im, 96, 112) == RGB565(234, 228, 232), "z14: the shops' area");
    Check(Px(im, 160, 112) == RGB565(216, 234, 202), "z14: the grass");
    Check(Px(im, 224, 112) == land, "z14: farmland not drawn");
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
    Close(f);
    Close(g);

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
