// The vector tiles (vtile.cpp) on a hand-made Mapbox Vector Tile: water in the top left quarter, a primary road
// across at 3/4 of the height, a town's point. Drawn as OSMapsCE draws them (VTileRender, VLabelsDraw), checked by
// the pixels the driver would see. Exit code: the number of failed checks. (test_vtile.bat builds and runs it.)
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

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
