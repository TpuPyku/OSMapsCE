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

int HttpGet(const char*, bool, const char*, unsigned char**, int*, int* status, const char*, char*, int,
            const HttpExtra*)   // (no network here: the downloads are asked of FakeGet)
{
    *status = 0;
    return NET_CONNECT;
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

// A made-up GitHub for the download (offmaps.cpp): github.com redirects to the storage, which answers ranges of the
// test zip (206); s_failOnce - the next range request times out (a lost link)
static char* s_zip;
static int   s_zipLen, s_asked, s_failOnce;

static int FakeGet(const char* host, bool, const char* path, unsigned char** body, int* len, int* status,
                   const HttpExtra* x)
{
    s_asked++;
    *body = (unsigned char*)malloc(1);
    *len = 0;
    if (!strcmp(host, "github.com") && strstr(path, "catalog.json")) {   // the catalog: two regions, zz's zip ours
        static const char* kCat = "{\"regions\":[{\"name\":\"tiny\",\"title\":\"T\",\"date\":\"2026-10-20\",\"zip\":"
                                  "\"tiny.zip\",\"size\":10},{\"name\":\"zz\",\"title\":\"Z\",\"date\":\"2026-10-29\","
                                  "\"zip\":\"zz.zip\",\"size\":2048}]}";
        free(*body);
        *body = (unsigned char*)_strdup(kCat);
        *len = (int)strlen(kCat);
        *status = 200;
        return NET_OK;
    }
    if (!strcmp(host, "github.com")) {
        *status = 302;
        _snprintf(x->location, x->locCap, "https://store.example/f%s?sig=%d", strrchr(path, '/'), s_asked);
        return NET_OK;
    }
    if (strcmp(host, "store.example") || !x->headers || s_failOnce-- > 0) {
        *status = 0;
        return NET_TIMEOUT;
    }
    int a = 0, b = 0;
    sscanf(x->headers, "Range: bytes=%d-%d", &a, &b);
    if (a >= s_zipLen) {
        *status = 416;
        return NET_OK;
    }
    b = b < s_zipLen ? b : s_zipLen - 1;
    free(*body);
    *body = (unsigned char*)malloc(b - a + 2);
    memcpy(*body, s_zip + a, b - a + 1);
    *len = b - a + 1;
    *status = 206;
    return NET_OK;
}

// offdata.cpp's file: mapped (the user 2026-10-29: Moscow's 99 MB graph read into memory failed on a Shturmann) -
// an aligned part straight from the view, an unaligned one copied, one past the end refused
static void OffFileChecks()
{
    wchar_t path[MAX_PATH];
    PathInDir(path, L"..\\region.zip");
    OffFile f;
    char head[4];
    Check(OffOpen(&f, path, head, 4) && f.view && !memcmp(head, "PK\x03\x04", 4), "file: opened, mapped", NULL);
    const char* a = (const char*)OffPart(&f, 4, 8);
    Check(a == (const char*)f.view + 4 && f.nparts == 0, "file: an aligned part - the view itself", NULL);
    const char* u = (const char*)OffPart(&f, 1, 8);
    Check(u && u != (const char*)f.view + 1 && !memcmp(u, (const char*)f.view + 1, 8) && f.nparts == 1,
          "file: an unaligned part - copied", NULL);
    Check(!OffPart(&f, f.size, 4) && f.bad, "file: past the end - refused, marked", NULL);
    OffClose(&f);
    Check(!f.view && !f.file && !f.nparts, "file: closed, all zero", NULL);
}

static bool WaitMaps()   // the job's thread done (5 s at most)
{
    wchar_t t[128];
    int got, total;
    for (int i = 0; i < 100 && MapsJob(t, 128, &got, &total) == MAPS_BUSY; i++)
        Sleep(50);
    return MapsJob(t, 128, &got, &total) == MAPS_IDLE;
}

static void Remove(const wchar_t* dir)   // a folder of the test's last run (its files, then itself)
{
    static const wchar_t* kNames[4] = { L"route.grf", L"search.idx", L"region.txt", L"trunk.grf" };
    wchar_t path[MAX_PATH];
    for (int i = 0; i < 4; i++) {
        _snwprintf(path, MAX_PATH, L"%s\\%s", dir, kNames[i]);
        DeleteFile(path);
    }
    RemoveDirectory(dir);
}

static void DownloadChecks()
{
    wchar_t zip[MAX_PATH], part[MAX_PATH], dir[MAX_PATH];
    PathInDir(zip, L"..\\region.zip");
    HANDLE f = CreateFile(zip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    s_zipLen = (int)GetFileSize(f, NULL);
    s_zip = (char*)malloc(s_zipLen);
    DWORD got;
    ReadFile(f, s_zip, s_zipLen, &got, NULL);
    CloseHandle(f);
    MapsSetGet(FakeGet, 1);

    PathInDir(part, L"..\\dl.zip.part");
    DeleteFile(part);
    s_asked = 0;
    Check(MapsFetch("https://github.com/x/releases/download/maps/tiny.zip", part, 100) &&
          FileIs(L"..\\dl.zip.part", s_zip, s_zipLen), "download: in 100-byte ranges, through the redirect", NULL);
    Check(s_asked >= 2 * (s_zipLen / 100), "download: each range asked through github.com (a fresh signature)", NULL);

    HANDLE h = CreateFile(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);   // a half: resumed from there
    DWORD put;
    WriteFile(h, s_zip, 150, &put, NULL);
    CloseHandle(h);
    s_failOnce = 1;
    Check(MapsFetch("https://github.com/x/releases/download/maps/tiny.zip", part, 100) &&
          FileIs(L"..\\dl.zip.part", s_zip, s_zipLen), "download: resumed after 150 bytes, past a lost link", NULL);
    s_failOnce = 9;
    DeleteFile(part);
    Check(!MapsFetch("https://github.com/x/releases/download/maps/tiny.zip", part, 100),
          "download: the link lost for good - false", NULL);
    s_failOnce = 0;

    PathInDir(dir, L"..\\dldata");
    Check(MapsInstall("https://github.com/x/releases/download/maps/tiny.zip", dir, "zipped", false) &&
          FileIs(L"..\\dldata\\zipped\\region.txt", "name=zipped\ndate=2026-10-29\n", 28) &&
          FileIs(L"..\\dldata\\zipped\\search.idx", "IDXIDXIDXIDXIDXIDXIDXIDXIDXIDX", 30),
          "download: a region installed into its folder", NULL);
    wchar_t left[MAX_PATH];
    PathInDir(left, L"..\\dldata\\zipped.zip");
    Check(GetFileAttributes(left) == 0xFFFFFFFF, "download: the zip removed after", NULL);

    // the job, as the screen starts it: the catalog, then a region (into the program's OSMapsData here - no card),
    // the trunk roads after it when the card has none (they are there: obj\test\OSMapsData\trunk)
    Check(MapsList() && WaitMaps() && MapsCount() == 2, "job: the catalog loaded", NULL);
    MapRegion m;
    Check(MapsAt(0, &m) && !strcmp(m.name, "tiny") && MapState(m) == MAP_OK && MapsAt(1, &m) && MapState(m) == MAP_NEW
          && !MapsAt(2, &m), "job: the card's states", NULL);
    s_asked = 0;
    Check(MapsGetRegion(1) && WaitMaps() && MapsAt(1, &m) && !strcmp(m.have, "2026-10-29") && MapState(m) == MAP_OK,
          "job: a region downloaded, the card's state after", NULL);
    Check(FileIs(L"OSMapsData\\zz\\search.idx", "IDXIDXIDXIDXIDXIDXIDXIDXIDXIDX", 30), "job: into the program's folder",
          NULL);
    wchar_t text[128];
    int gotKb = 0, total = 0;
    Check(MapsJob(text, 128, &gotKb, &total) == MAPS_IDLE && total == 2 && wcsstr(text, L"Z"), "job: its result told",
          NULL);
    Check(!MapsGetRegion(5), "job: no such region", NULL);
    MapsSetGet(NULL, 0);
    free(s_zip);

    // the real one's target (this PC's: a drive with \OSMapsData, a removable one, or the program's folder)
    wchar_t target[MAX_PATH], prog[MAX_PATH];
    OffWriteDir(target);
    PathInDir(prog, L"OSMapsData");
    size_t tl = wcslen(target);
    Check(tl > 11 && !wcscmp(target + tl - 11, L"\\OSMapsData") &&
          (!wcscmp(target, prog) || (target[1] == L':' && tl == 13)), "download: its folder - a drive's or ours", NULL);
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
    wchar_t app[MAX_PATH];
    PathInDir(app, L".");
    CreateDirectory(app, NULL);   // the program's folder (obj\test\app: as the unit's, there before)
    PathInDir(app, L"OSMapsData\\zz");
    Remove(app);                  // the last run's download
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

    OffVolumes(false);   // (this PC's own disks' maps - a card in its reader - left out)
    OffFileChecks();
    JsonChecks();
    MapsChecks();
    DownloadChecks();

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
    wchar_t trunk[MAX_PATH], hidden[MAX_PATH];
    PathInDir(trunk, L"..\\OSMapsData\\trunk\\trunk.grf");
    PathInDir(hidden, L"..\\OSMapsData\\trunk\\trunk.hidden");
    Check(OfflineHasTrunk(), "trunk: the card has the trunk roads", NULL);
    MoveFile(trunk, hidden);
    OffVolumes(false);   // (the folders listed again)
    Check(!OfflineHasTrunk() && !TrunkRoute("50.000,52.995;50.006,53.000"), "trunk: none on the card - no way", NULL);
    MoveFile(hidden, trunk);
    OffVolumes(false);
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
    // the town's name in the query too (the user 2026-10-29: "Жигулёвск Ткачёва 71" - nothing; "Ткачёва 71" found)
    b = Ask("/api/?q=%D1%82%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA%20%D0%B3%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F%207"
            "%D0%BA2&limit=7&lat=53.0&lon=50.0");   // тестовск главная 7к2
    Check(Has(b, L"\"street\":\"Главная улица\",\"housenumber\":\"7к2\""), "search: the town, the street, the house", b);
    free(b);
    b = Ask("/api/?q=%D0%B3%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F%20%D1%82%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA"
            "&limit=7&lat=53.0&lon=50.0");   // главная тестовск
    Check(Has(b, L"\"name\":\"Главная улица\""), "search: the street, then its town", b);
    free(b);
    b = Ask("/api/?q=%D0%B4%D1%80%D1%83%D0%B3%D0%BE%D0%B3%D1%80%D0%B0%D0%B4%20%D0%B3%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F"
            "&limit=7&lat=53.0&lon=50.0");   // другоград главная: another town's name - not this one's street
    Check(!Has(b, L"\"name\":\"Главная улица\",\"street\":\"\",\"housenumber\":\"\",\"city\":\"Тестовск\""),
          "search: another town's name - not this town's street", b);
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
    // here only a street of that name, there the town itself (the user 2026-10-29: "Жигулёвск" from Moscow - Moscow's
    // Жигулёвская streets, not the town): the regions' answers together, the town first
    b = Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=58.0&lon=50.0");   // Тестовск
    const char* there = Find(b, L"\"name\":\"Тестовск\""), *here = Find(b, L"\"name\":\"Тестовская улица\"");
    Check(there && here && there < here && Num(b, "\"coordinates\":[") > 0,
          "search: no exact match here - the other regions too, the exact one first", b);
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
