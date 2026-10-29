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

bool ReadWholeFile(const wchar_t*, unsigned char**, int*) { return false; }   // (image.cpp's: its zlib is used here)

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

static char* TrunkRoute(const char* lonLat, const char* extra = "")   // as asked when OSRM did not answer (no internet)
{
    char path[300], *body = NULL;
    int len = 0;
    _snprintf(path, sizeof(path), "/routed-car/route/v1/driving/%s?overview=full&geometries=polyline6&steps=true%s",
              lonLat, extra);
    return OfflineTrunkRoute(path, &body, &len) ? body : NULL;
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

static bool Last(const char* body, int lat, int lon)   // the line ends there (within 2e-6 degrees)
{
    static int p[4096 * 2];
    int n = Line(body, p, 4096);
    return n > 0 && abs(p[n * 2 - 2] - lat) <= 2 && abs(p[n * 2 - 1] - lon) <= 2;
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

static bool FileIs(const wchar_t* name, const char* want, int n)   // a file under the test folder holds want
{
    wchar_t path[MAX_PATH];
    PathInDir(path, name);
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    static char buf[100000];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf), &got, NULL);
    CloseHandle(f);
    return (int)got == n && !memcmp(buf, want, n);
}

// json.cpp (shared now: the catalog read with it): escapes, nesting, a string cut short
static void JsonChecks()
{
    char s[64];
    JStr("\"a\\nb\\t\\\"c\\\\\\/\\u0410\\u00e9z\\r\\b\\f\"", s, sizeof(s));
    Check(!strcmp(s, "a b \"c\\/\xD0\x90\xC3\xA9z"), "json: the escapes (\\u to UTF-8, 2 and 1 bytes)", s);
    JStr("\"\\u20ac\"", s, sizeof(s));
    Check(!strcmp(s, "\xE2\x82\xAC"), "json: a 3-byte \\u", s);
    JStr("\"ab\\", s, sizeof(s));
    Check(!strcmp(s, "ab"), "json: cut after a backslash", s);
    Check(JNum(JKey("{\"a\":[1,{\"b\":\"x]}\\\"\"}],\"c\":2}", "c"), 0) == 2, "json: past nested values and strings",
          NULL);
    Check(!JKey("{\"a\":1}", "b") && !JKey("[1]", "a") && !JFirst("[]") && !JFirst("{}"), "json: none where none",
          NULL);
}

// The regions' maps on the unit (offmaps.cpp, the user 2026-10-29): the release's catalog, what the card has, a
// region's zip unpacked, the download's URLs
static void MapsChecks()
{
    static const char* kCat = "{\"version\":1,\"made\":\"x\",\"regions\":[{\"name\":\"tiny\",\"title\":\"\xD0\xA2\xD0\xB5"
                              "\xD1\x81\xD1\x82\",\"date\":\"2026-10-27\",\"zip\":\"tiny.zip\",\"size\":2048000,"
                              "\"bbox\":[1,2,3,4]},{\"name\":\"zz\",\"title\":\"Z\",\"date\":\"2026-10-28\","
                              "\"zip\":\"zz.zip\",\"size\":100}]}";
    MapRegion r[4];
    int n = MapsCatalog(kCat, r, 4);
    Check(n == 2 && !strcmp(r[0].name, "tiny") && !strcmp(r[0].title, "\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82") &&
          !strcmp(r[0].date, "2026-10-27") && !strcmp(r[0].zip, "tiny.zip") && r[0].sizeKb == 2000 &&
          !strcmp(r[1].name, "zz") && r[1].sizeKb == 0, "maps: the catalog read", NULL);
    Check(MapsCatalog(kCat, r, 1) == 1 && MapsCatalog("{}", r, 4) == 0 && MapsCatalog("garbage", r, 4) == 0,
          "maps: the catalog bounded, nothing from a bad one", NULL);
    n = MapsCatalog(kCat, r, 4);
    MapsHave(r, n);   // tiny on the card (region.txt 2026-10-20), zz not
    Check(!strcmp(r[0].have, "2026-10-20") && !r[1].have[0], "maps: the card's dates", NULL);
    Check(MapState(r[0]) == MAP_UPDATE && MapState(r[1]) == MAP_NEW, "maps: newer in the catalog, not on the card", NULL);
    strcpy(r[0].date, "2026-10-20");
    Check(MapState(r[0]) == MAP_OK, "maps: the same date - up to date", NULL);

    char host[64];
    bool https = false;
    const char* path = NULL;
    Check(UrlSplit("https://release-assets.githubusercontent.com/a/b?x=1", host, 64, &https, &path) &&
          !strcmp(host, "release-assets.githubusercontent.com") && https && !strcmp(path, "/a/b?x=1"),
          "maps: an https URL split", NULL);
    Check(UrlSplit("http://h.org/p", host, 64, &https, &path) && !https && !strcmp(path, "/p") &&
          !UrlSplit("ftp://x/y", host, 64, &https, &path) && !UrlSplit("https://h.org", host, 64, &https, &path),
          "maps: http too, nothing else", NULL);

    static const char* kFiles[3] = { "route.grf", "search.idx", "region.txt" };
    wchar_t zip[MAX_PATH], dir[MAX_PATH];
    PathInDir(zip, L"..\\region.zip");
    PathInDir(dir, L"..\\unzip");
    Check(ZipExtract(zip, dir, kFiles, 3), "maps: a region's zip unpacked", NULL);
    static char grf[256 * 300];
    for (int i = 0; i < (int)sizeof(grf); i++)
        grf[i] = (char)(i % 256);
    Check(FileIs(L"..\\unzip\\route.grf", grf, sizeof(grf)), "maps: a deflated file whole", NULL);
    Check(FileIs(L"..\\unzip\\search.idx", "IDXIDXIDXIDXIDXIDXIDXIDXIDXIDX", 30), "maps: a stored file whole", NULL);
    Check(FileIs(L"..\\unzip\\region.txt", "name=zipped\ndate=2026-10-29\n", 28), "maps: region.txt", NULL);
    static const char* kMissing[1] = { "nope.bin" };
    Check(!ZipExtract(zip, dir, kMissing, 1), "maps: a file the zip has not - false", NULL);
}

int main()
{
    GetModuleFileName(NULL, g_dir, MAX_PATH);
    wchar_t* s = wcsrchr(g_dir, L'\\');
    if (s)   // as from the program's folder obj\test\app\: its ..\OSMapsData is obj\test\OSMapsData
        wcscpy(s + 1, L"app\\");

    JsonChecks();
    MapsChecks();

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

    // 4b: the trunk roads (obj\test\OSMapsData\trunk: tiny's primary street alone) - a start south of every region
    // (no region's answer), from the trunk road nearest it: along the main street, ~400 m from its west end
    b = Route("50.000,52.995;50.006,53.000");
    Check(!b, "trunk: no region holds the start", b);
    free(b);
    b = TrunkRoute("50.000,52.995;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 380 && Num(b, "\"distance\":") < 420 && First(b, 53000000, 50000000),
          "trunk: from the nearest trunk road, ~400 m", b);
    Check(b && Count(b, "\"type\":\"depart\"") == 1 && Count(b, "\"type\":\"arrive\"") == 1,
          "trunk: an OSRM answer (depart, arrive)", b);
    free(b);
    // joined: from the upper street (a residential one - the region's, not a trunk road) to a point south of every
    // region: the region's roads to the trunk, then the trunk - one leg, its line from the start's own street
    b = Route("50.003,53.002;50.012,52.996");
    Check(!b, "joined: no region holds the end", b);
    free(b);
    b = TrunkRoute("50.003,53.002;50.012,52.996");
    Check(b && First(b, 53002000, 50003000), "joined: the line from the start's own street (the region's part)", b);
    Check(b && Count(b, "\"steps\":") == 1 && Count(b, "\"type\":\"depart\"") == 1 &&
          Count(b, "\"type\":\"arrive\"") == 1, "joined: one leg, one depart, one arrive", b);
    // west 67 m, the side street 222, the main 267, the curve 393, the turn 67, the roundabout ~100, the south 155
    Check(b && Num(b, "\"distance\":") > 1230 && Num(b, "\"distance\":") < 1310, "joined: ~1270 m, as walked", b);
    free(b);
    // joined the other way: the trunk from the south, the region's roads to the upper street at the end
    b = TrunkRoute("50.012,52.996;50.003,53.002");
    Check(b && Last(b, 53002000, 50003000) && Count(b, "\"steps\":") == 1 && Count(b, "\"type\":\"depart\"") == 1 &&
          Count(b, "\"type\":\"arrive\"") == 1, "joined backwards: the end on its own street, one leg", b);
    // the south 155, the roundabout one way round 3/4 ~285, the turn 67, the curve 393, the main 133, up the one-way
    // 222 (north: allowed), the upper 67
    Check(b && Num(b, "\"distance\":") > 1290 && Num(b, "\"distance\":") < 1350, "joined backwards: ~1322 m", b);
    free(b);
    // both ends in the region: its roads at both, the trunk between (the upper street to the north street's end)
    b = TrunkRoute("50.003,53.002;50.012,53.002");
    Check(b && First(b, 53002000, 50003000) && Last(b, 53002000, 50012000) && Count(b, "\"steps\":") == 1 &&
          Count(b, "\"type\":\"arrive\"") == 1, "joined both ends: from its street to its street, one leg", b);
    char* whole = Route("50.003,53.002;50.012,53.002");   // the region alone: the same way, the same time and length
    Check(b && whole && fabs(Num(b, "\"distance\":") - Num(whole, "\"distance\":")) < 2 &&
          fabs(Num(b, "\"duration\":") - Num(whole, "\"duration\":")) < 1.0, "joined both ends: as the region's own", b);
    free(whole);
    free(b);
    // the car's course east (the region's part starts its way): to the upper street's corner and back, 2 x 67 m more
    b = TrunkRoute("50.003,53.002;50.012,52.996", "&bearings=90,60;");
    Check(b && Num(b, "\"distance\":") > 1370 && Num(b, "\"distance\":") < 1430 && First(b, 53002000, 50003000),
          "joined, course east: the start's way kept, ~1401 m", b);
    free(b);
    // a stop on the way: not joined - the trunk alone, two legs (from the trunk roads nearest the points)
    b = TrunkRoute("50.000,53.000;50.006,53.000;50.010,53.000");
    Check(b && Count(b, "\"steps\":") == 2 && First(b, 53000000, 50000000), "trunk with a stop: two legs", b);
    free(b);
    // far from every trunk road: no answer
    b = TrunkRoute("37.6,55.7;37.7,55.8");
    Check(!b, "trunk: nothing near - no answer", b);
    free(b);
    b = Route("50.000,53.000;50.006,53.000");   // and the region again after it (the graphs swapped back)
    Check(b && Num(b, "\"distance\":") > 380 && Num(b, "\"distance\":") < 420, "trunk: the region after it", b);
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

    // the other regions on the card (the user 2026-10-29: a town of Samara's region asked from Moscow - "no network")
    b = Ask("/api/?q=%D0%B4%D1%80%D1%83%D0%B3%D0%BE%D0%B3%D1%80%D0%B0%D0%B4&limit=7&lat=53.0&lon=50.0");   // другоград
    Check(Has(b, L"\"name\":\"Другоград\"") && Num(b, "\"coordinates\":[50.") > 0 && Has(b, L",58."),
          "search: a town of another region (twin, 5 degrees north) when this one has none", b);
    free(b);
    b = Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=10.0&lon=10.0");   // Тестовск
    Check(Has(b, L"\"name\":\"Тестовск\""), "search: from where no region is - the regions on the card", b);
    free(b);
    b = Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=53.0&lon=50.0");   // Тестовск again
    Check(Has(b, L"\"name\":\"Тестовск\"") && !Has(b, L"Другоград"), "search: this region's first, the others not mixed in", b);
    free(b);
    b = Ask("/reverse?lon=10.0&lat=10.0&limit=1");
    Check(!b, "reverse: only the point's own region", b);
    free(b);
    b = Ask("/api/?q=%D0%BD%D0%B5%D1%82%D1%82%D0%B0%D0%BA%D0%BE%D0%B9&limit=7&lat=10.0&lon=10.0");   // неттакой
    Check(!b, "search: nothing in any region - no answer (then the network)", b);
    free(b);
    char* nb = NULL;
    int nl = 0;
    Check(!OfflineSearch("/api/?q=%D0%BD%D0%B5%D1%82%D1%82%D0%B0%D0%BA%D0%BE%D0%B9&limit=7&lat=10.0&lon=10.0", &nb, &nl),
          "search: nothing in any region - not answered (the network asked)", nb);

    // the region's vector tiles (offline/test/make_test_tiles.py: the town's z14 tile, its z13 parent, a z15 child)
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
    MapRequest t15 = { SRC_OSM, VEC_ZBASE + 15, 2 * tx, 2 * ty };
    Check(VPackTile(t15, &data, &len) && len == 7 && !memcmp(data, "tile 15", 7), "pack: its z15 child", NULL);
    free(data);
    data = NULL;
    Check(!VPackTile(next, &data, &len), "pack: a tile not in it - no answer (OpenFreeMap is asked)", NULL);
    Check(!VPackTile(moscow, &data, &len), "pack: outside every region's pack (an old pack's not read) - no answer",
          NULL);

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
