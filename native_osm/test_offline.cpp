// The offline router and search (offline.cpp, offsearch.cpp) on the test map (offline/test/tiny.osm, built by
// test_offline.bat into obj\test\OSMapsData\tiny): asked as OSMapsCE's download thread asks them, checked as the
// driver would see the answer. Exit code: the number of failed checks.
#include "yamaps.h"
#include <stdio.h>

Config  g_cfg;
wchar_t g_dir[MAX_PATH];

void PathInDir(wchar_t* out, const wchar_t* name)
{
    _snwprintf(out, MAX_PATH, L"%s%s", g_dir, name);
    out[MAX_PATH - 1] = 0;
}

void Log(const char*, ...)
{
}

static int s_failed, s_checks;

static void Check(bool ok, const char* what, const char* body)
{
    s_checks++;
    if (ok)
        return;
    s_failed++;
    printf("FAIL: %s\n  %.400s\n", what, body ? body : "(no answer)");
}

static char* Route(const char* lonLat, const char* extra = "")
{
    char path[300], *body = NULL;
    int len = 0;
    _snprintf(path, sizeof(path), "/routed-car/route/v1/driving/%s?overview=full&geometries=polyline6&steps=true%s",
              lonLat, extra);
    return OfflineRoute(path, &body, &len) ? body : NULL;
}

// The line as route.cpp reads it: the JSON string unescaped, then Google's polyline (1e-6 degrees, lat first)
static int Line(const char* body, int* out, int max)
{
    const char* g = body ? strstr(body, "\"geometry\":\"") : NULL;
    if (!g)
        return 0;
    char enc[8192];
    int n = 0;
    for (g += 12; *g && *g != '"' && n < (int)sizeof(enc) - 1; g++)
        enc[n++] = *g == '\\' ? *++g : *g;
    enc[n] = 0;
    int pts = 0, v[2] = { 0, 0 };
    for (const char* p = enc; *p && pts < max;) {
        for (int k = 0; k < 2; k++) {
            int shift = 0, r = 0, c;
            do {
                c = *p++ - 63;
                r |= (c & 0x1f) << shift;
                shift += 5;
            } while (c >= 0x20 && *p);
            v[k] += (r & 1) ? ~(r >> 1) : (r >> 1);
        }
        out[pts * 2] = v[0];
        out[pts * 2 + 1] = v[1];
        pts++;
    }
    return pts;
}

static bool First(const char* body, int lat, int lon)   // the line starts there (within 2e-6 degrees)
{
    int p[2];
    return Line(body, p, 1) == 1 && abs(p[0] - lat) <= 2 && abs(p[1] - lon) <= 2;
}

static int Points(const char* body)
{
    static int p[4096 * 2];
    return Line(body, p, 4096);
}

static char* Ask(const char* photonPath)
{
    char* body = NULL;
    int len = 0;
    return OfflineSearch(photonPath, &body, &len) ? body : NULL;
}

static double Num(const char* body, const char* key)
{
    const char* p = body ? strstr(body, key) : NULL;
    return p ? atof(p + strlen(key)) : -1;
}

// Where the answer (UTF-8) holds the text (the source's Cyrillic: wide literals - VS2005 recodes narrow ones)
static const char* Find(const char* body, const wchar_t* text)
{
    char u[300];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u, sizeof(u), NULL, NULL);
    return body ? strstr(body, u) : NULL;
}

static bool Has(const char* body, const wchar_t* text)
{
    return Find(body, text) != NULL;
}

static int Count(const char* body, const char* s)
{
    int n = 0;
    for (const char* p = body; p && (p = strstr(p, s)) != NULL; p++)
        n++;
    return n;
}

int main()
{
    GetModuleFileName(NULL, g_dir, MAX_PATH);
    wchar_t* s = wcsrchr(g_dir, L'\\');
    if (s)   // as from the program's folder obj\test\app\: its ..\OSMapsData is obj\test\OSMapsData
        wcscpy(s + 1, L"app\\");

    // along the main street, west to east: ~400 m, no turn on the way
    char* b = Route("50.000,53.000;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 380 && Num(b, "\"distance\":") < 420, "main street: ~400 m", b);
    Check(b && Count(b, "\"type\":\"turn\"") == 0 && Count(b, "\"type\":\"depart\"") == 1 &&
          Count(b, "\"type\":\"arrive\"") == 1, "main street: depart, arrive, no turns", b);
    free(b);

    // from the east end to the upper corner: right onto the one-way street (east -> west, then north)
    b = Route("50.006,53.000;50.004,53.002");
    Check(b && Num(b, "\"distance\":") > 330 && Num(b, "\"distance\":") < 380, "to the one-way: ~356 m", b);
    Check(Has(b, L"\"name\":\"Односторонняя улица\",\"ref\":\"\",\"maneuver\":{\"type\":\"turn\",\"modifier\":\"right\""),
          "to the one-way: turn right onto it", b);
    free(b);

    // back: not against the one-way - round by the upper and the side streets (~624 m)
    b = Route("50.004,53.002;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 590 && Num(b, "\"distance\":") < 660, "back: round the one-way, ~624 m", b);
    free(b);

    // a point outside every region: not answered (OSRM is asked)
    b = Route("37.6,55.7;37.7,55.8");
    Check(!b, "outside the regions: no answer", b);
    free(b);

    // off the road: the ends snapped onto it (the line starts at the start's projection)
    b = Route("50.001,53.0003;50.005,53.0003");
    Check(b && Num(b, "\"distance\":") > 262 && Num(b, "\"distance\":") < 273, "off the road: ~268 m along it", b);
    Check(b && First(b, 53000000, 50001000), "off the road: the line starts on the road", b);
    free(b);

    // the curved street: its inner points in the line, both ways
    b = Route("50.006,53.000;50.010,53.000");
    Check(b && Num(b, "\"distance\":") > 388 && Num(b, "\"distance\":") < 398, "curve: ~393 m", b);
    Check(b && Points(b) == 4, "curve: 4 points", b);
    free(b);
    b = Route("50.010,53.000;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 388 && Num(b, "\"distance\":") < 398 && First(b, 53000000, 50010000),
          "curve backwards: ~393 m from its east end", b);
    free(b);

    // on one piece of road: just along it
    b = Route("50.0075,53.0012;50.0085,53.0012");
    Check(b && Num(b, "\"distance\":") > 62 && Num(b, "\"distance\":") < 72 && Count(b, "\"maneuver\"") == 2,
          "one piece: ~67 m, depart and arrive only", b);
    free(b);

    // the car's course: going west it starts west (and turns back), without one - the shortest
    b = Route("50.003,53.000;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 195 && Num(b, "\"distance\":") < 207, "no course: ~201 m", b);
    free(b);
    b = Route("50.003,53.000;50.006,53.000", "&bearings=270,60;");
    Check(b && Num(b, "\"distance\":") > 300, "course west: starts west, longer", b);
    free(b);

    // a left turn; the roundabout's exit by its number
    b = Route("50.000,53.000;50.002,53.002");
    Check(Has(b, L"\"name\":\"Боковая улица\",\"ref\":\"\",\"maneuver\":{\"type\":\"turn\",\"modifier\":\"left\""),
          "left onto the side street", b);
    free(b);
    b = Route("50.010,53.000;50.015,53.000");
    Check(Has(b, L"\"name\":\"Восточная улица\",\"ref\":\"\",\"maneuver\":{\"type\":\"roundabout\"") &&
          Has(b, L"\"exit\":2"), "roundabout: the 2nd exit onto the east street", b);
    free(b);

    // the search: the town first, a house by street and number, the station by its kind
    b = Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=53.0&lon=50.0");   // Тестовск
    Check(Has(b, L"{\"features\":[{\"geometry\":{\"coordinates\":[50.003000,53.001000]},\"properties\":{\"name\":\"Тестовск\""),
          "search: the town first", b);
    free(b);
    b = Ask("/api/?q=%D1%83%D0%BB+%D0%B3%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F+5&limit=7&lat=53.0&lon=50.0");   // ул главная 5
    Check(Has(b, L"\"street\":\"Главная улица\",\"housenumber\":\"5\""), "search: the house", b);
    free(b);
    b = Ask("/api/?q=%D0%B0%D0%B7%D1%81&limit=7&lat=53.0&lon=50.0");   // азс
    Check(Has(b, L"Тест-АЗС (АЗС)"), "search: the station by its kind", b);
    free(b);
    b = Ask("/api/?q=%D0%BD%D0%B5%D1%82%D1%82%D0%B0%D0%BA%D0%BE%D0%B9&limit=7&lat=53.0&lon=50.0");   // неттакой
    Check(!b, "search: nothing found - no answer (Photon is asked)", b);
    free(b);

    b = Ask("/api/?q=%D0%B3%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F+7%D0%BA2&limit=7&lat=53.0&lon=50.0");   // главная 7к2
    Check(Has(b, L"\"street\":\"Главная улица\",\"housenumber\":\"7к2\""), "search: a house with a letter", b);
    free(b);
    b = Ask("/api/?q=%D1%83%D0%BB%D0%B8%D1%86%D0%B0&limit=7&lat=53.0&lon=50.0");   // улица: a kind word only
    Check(!b, "search: the kind word alone - no answer", b);
    free(b);
    b = Ask("/api/?q=%D1%82%D0%B5%D1%81%D1%82&limit=7&lat=53.0&lon=50.0");   // тест: the town and the station
    const char* town = Find(b, L"\"name\":\"Тестовск\""), *fuel = Find(b, L"\"name\":\"Тест-АЗС (АЗС)\"");
    Check(town && fuel && town < fuel, "search: both found, the town before the station", b);
    free(b);

    // the address of a point: the house within 80 m
    b = Ask("/reverse?lon=50.00212&lat=53.00012&limit=1");
    Check(Has(b, L"\"street\":\"Главная улица\",\"housenumber\":\"5\""), "reverse: the house", b);
    free(b);

    b = Ask("/reverse?lon=50.0005&lat=53.0002&limit=1");   // no house within 80 m: the street
    Check(Has(b, L"\"name\":\"Главная улица\",\"street\":\"\""), "reverse: the street", b);
    free(b);
    b = Ask("/reverse?lon=50.012&lat=53.000&limit=1");     // in the roundabout: the road nearest has no name
    Check(Has(b, L"\"name\":\"Тестовск\",\"street\":\"\",\"housenumber\":\"\",\"city\":\"\""),
          "reverse: no house, no named road - the town", b);
    free(b);
    b = Ask("/reverse?lon=50.0&lat=53.05&limit=1");        // 5.5 km off: nothing
    Check(!b, "reverse: nothing near - no answer", b);
    free(b);

    // the region's vector tiles (offline/test/make_test_tiles.py: the town's z14 tile and its z13 parent)
    int tx = (int)((50.003 + 180) / 360 * 16384);
    double la = 53.001 * M_PI / 180;
    int ty = (int)((1 - log(tan(la) + 1 / cos(la)) / M_PI) / 2 * 16384);
    MapRequest t14 = { SRC_OSM, VEC_ZBASE + 14, tx, ty }, t13 = { SRC_OSM, VEC_ZBASE + 13, tx >> 1, ty >> 1 };
    MapRequest next = { SRC_OSM, VEC_ZBASE + 14, tx + 1, ty }, moscow = { SRC_OSM, VEC_ZBASE + 14, 9907, 5118 };
    unsigned char* data = NULL;
    int len = 0;
    Check(VPackTile(t14, &data, &len) && len == 7 && !memcmp(data, "tile 14", 7), "pack: the town's z14 tile", NULL);
    free(data);
    data = NULL;
    Check(VPackTile(t13, &data, &len) && len == 7 && !memcmp(data, "tile 13", 7), "pack: its z13 parent", NULL);
    free(data);
    data = NULL;
    Check(!VPackTile(next, &data, &len), "pack: a tile not in it - no answer (OpenFreeMap is asked)", NULL);
    Check(!VPackTile(moscow, &data, &len), "pack: outside every region's pack - no answer", NULL);

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
