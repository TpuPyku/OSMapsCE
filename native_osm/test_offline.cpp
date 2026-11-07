// The offline router and search (offline.cpp, offsearch.cpp) on the test map (offline/test/tiny.osm, built by
// test_offline.bat into obj\test\OSMapsData\tiny): asked as OSMapsCE's download thread asks them, checked as the
// driver would see the answer. Exit code: the number of failed checks.
#include "yamaps.h"
#include <stdio.h>
#include "fling.h"
#include "zoomanim.h"
#include "trafshade.h"
#include "layers.h"

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
// test zip (206); s_failOnce - the next range request times out (a lost link); s_cancelNow - the screen's "ОТМЕНА"
// pressed meanwhile; s_seen - the region the job said it downloads
static char* s_zip;
static int   s_zipLen, s_asked, s_failOnce, s_cancelNow, s_seen;
// the storage's signed links: s_expireAt - after that many range answers every link given so far expires (403)
static int   s_stored, s_expireAt, s_sigFloor;
// Yandex Disk (the user 2026-10-30: the regions whole there): s_withDisk - the catalog tells its folder, zz whole and
// the overview; its API answers a public file's link (dl.example), which serves full.zip / overview.zip in ranges
static char* s_full;
static char* s_ov;
static int   s_fullLen, s_ovLen, s_withDisk, s_apiFail, s_ovAsks, s_catFails;
static char  s_apiQuery[512];

static char* FileBytes(const wchar_t* name, int* len)   // a file of obj\test (malloc)
{
    wchar_t path[MAX_PATH];
    PathInDir(path, name);
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    *len = 0;
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    *len = (int)GetFileSize(f, NULL);
    char* b = (char*)malloc(*len);
    DWORD got;
    ReadFile(f, b, *len, &got, NULL);
    CloseHandle(f);
    return b;
}

static int Body(const char* s, unsigned char** body, int* len, int* status)   // a JSON answer, 200
{
    free(*body);
    *body = (unsigned char*)_strdup(s);
    *len = (int)strlen(s);
    *status = 200;
    return NET_OK;
}

static int Ranged(const char* data, int n, const HttpExtra* x, unsigned char** body, int* len, int* status)
{
    int a = 0, b = 0;
    sscanf(x->headers, "Range: bytes=%d-%d", &a, &b);
    if (a >= n) {
        *status = 416;
        return NET_OK;
    }
    b = b < n ? b : n - 1;
    free(*body);
    *body = (unsigned char*)malloc(b - a + 2);
    memcpy(*body, data + a, b - a + 1);
    *len = b - a + 1;
    *status = 206;
    return NET_OK;
}

static int DiskGet(const char* host, const char* path, unsigned char** body, int* len, int* status,
                   const HttpExtra* x)
{
    if (!strcmp(host, "cloud-api.yandex.net")) {
        strncpy(s_apiQuery, path, sizeof(s_apiQuery) - 1);
        s_ovAsks += strstr(path, "overview.zip") != NULL;
        if (s_apiFail || strstr(path, "yy.zip")) {   // (yy: not there whole)
            *status = 404;
            return NET_OK;
        }
        return Body(strstr(path, "overview.zip") ? "{\"href\":\"https:\\/\\/dl.example\\/ov?sig=1\"}"
                                                 : "{\"href\":\"https:\\/\\/dl.example\\/full?sig=1\"}", body, len, status);
    }
    if (!x->headers)
        return NET_TIMEOUT;
    return strstr(path, "/ov") ? Ranged(s_ov, s_ovLen, x, body, len, status)
                               : Ranged(s_full, s_fullLen, x, body, len, status);
}

static int FakeGet(const char* host, bool, const char* path, unsigned char** body, int* len, int* status,
                   const HttpExtra* x)
{
    s_asked++;
    *body = (unsigned char*)malloc(1);
    *len = 0;
    if (!strcmp(host, "github.com") && strstr(path, "catalog.json") && s_catFails-- > 0) {
        *status = 0;   // (the link lost: GitHub's storage over our TLS, 2026-10-31 - one in five)
        return NET_TIMEOUT;
    }
    if (!strcmp(host, "github.com") && strstr(path, "catalog.json")) {   // the catalog: two regions, zz's zip ours
        static const char* kCat = "{\"regions\":[{\"name\":\"tiny\",\"title\":\"T\",\"date\":\"2026-10-20\",\"zip\":"
                                  "\"tiny.zip\",\"size\":10},{\"name\":\"zz\",\"title\":\"Z\",\"date\":\"2026-10-29\","
                                  "\"zip\":\"zz.zip\",\"size\":2048}]}";
        static const char* kDiskCat = "{\"disk\":\"https://disk.example/d/x y\",\"overview\":{\"zip\":\"overview.zip\","
                                      "\"size\":2048,\"date\":\"2026-10-30\"},\"regions\":[{\"name\":\"yy\",\"title\":\"Y\",\"date\":\"2026-10-29\","
                                      "\"zip\":\"yy.zip\",\"size\":2048},{\"name\":\"zz\",\"title\":\"Z\",\"date\":\"2026-10-30\","
                                      "\"zip\":\"zz.zip\",\"size\":2048,\"full\":{\"zip\":\"zz.zip\",\"size\":300000}}]}";
        return Body(s_withDisk ? kDiskCat : kCat, body, len, status);
    }
    if (s_withDisk && (!strcmp(host, "cloud-api.yandex.net") || !strcmp(host, "dl.example")))
        return DiskGet(host, path, body, len, status, x);
    if (!strcmp(host, "github.com")) {
        *status = 302;
        _snprintf(x->location, x->locCap, "https://store.example/f%s?sig=%d", strrchr(path, '/'), s_asked);
        return NET_OK;
    }
    if (strcmp(host, "store.example") || !x->headers || s_failOnce-- > 0) {
        *status = 0;
        return NET_TIMEOUT;
    }
    const char* sig = strstr(path, "sig=");
    if (sig && atoi(sig + 4) < s_sigFloor) {
        *status = 403;   // (a link expired)
        return NET_OK;
    }
    if (s_expireAt && ++s_stored == s_expireAt)
        s_sigFloor = s_asked + 1;   // (this answer still given; the links given so far expire after it)
    s_seen = MapsBusyRegion();
    if (s_cancelNow)
        MapsCancel();
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
    // mapped when the memory is short (Shturmann), read into memory when it is plenty (the head unit: 350 MB free -
    // HU 2026-10-31, a way across Moscow took 7.8 min paging the mapped 96 MB graph in from the card)
    Check(OffMapWanted(96 << 20, 62 << 20) && !OffMapWanted(96 << 20, 350 << 20) && OffMapWanted(100, 299) &&
          !OffMapWanted(100, 300), "file: mapped unless three times its size is free", NULL);
    g_offFreeMem = 1 << 20;
    bool read = OffOpen(&f, path, head, 4) && !f.view && OffPart(&f, 4, 8) && f.nparts == 1;
    OffClose(&f);
    Check(read, "file: memory plenty - not mapped, its parts read", NULL);
    g_offFreeMem = 1;
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
    static const wchar_t* kNames[6] = { L"route.grf", L"search.idx", L"region.txt", L"trunk.grf", L"tiles.vtp",
                                        L"overview.vtp" };
    wchar_t path[MAX_PATH];
    for (int i = 0; i < 6; i++) {
        _snwprintf(path, MAX_PATH, L"%s\\%s", dir, kNames[i]);
        DeleteFile(path);
    }
    RemoveDirectory(dir);
}

// The region whole from Yandex Disk (the user 2026-10-30: GitHub's downloads slow): the catalog's folder - a public
// file's link asked of the disk's API (public_key and path encoded), the zip in ranges, its big files stored - unpacked
// piece by piece (tiles.vtp over a few pieces); the overview after it when the card has none
static void DiskChecks()
{
    s_full = FileBytes(L"..\\full.zip", &s_fullLen);
    s_ov = FileBytes(L"..\\overview.zip", &s_ovLen);
    s_withDisk = 1;
    MapRegion m;
    Check(MapsList() && WaitMaps() && MapsCount() == 2 && MapsAt(1, &m) && m.fullKb == 292,
          "disk: the catalog's region whole, its size", NULL);
    wchar_t zz[MAX_PATH], ov[MAX_PATH], yy[MAX_PATH];
    PathInDir(zz, L"OSMapsData\\zz");
    Remove(zz);
    wchar_t oldTrunk[MAX_PATH], oldGraph[MAX_PATH];   // a card from before: the trunk roads' own folder
    PathInDir(oldTrunk, L"OSMapsData\\trunk");
    CreateDirectory(oldTrunk, NULL);
    PathInDir(oldGraph, L"OSMapsData\\trunk\\trunk.grf");
    HANDLE og = CreateFile(oldGraph, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CloseHandle(og);
    Check(MapsGetRegion(1) && WaitMaps() && MapsAt(1, &m) && MapState(m) == MAP_OK, "disk: the region downloaded", NULL);
    Check(strstr(s_apiQuery, "/v1/disk/public/resources/download?public_key=https%3A%2F%2Fdisk.example%2Fd%2Fx%20y") &&
          strstr(s_apiQuery, "&path=%2Foverview.zip"), "disk: the link asked of its API, encoded", s_apiQuery);
    int n = 0;
    char* tiles = FileBytes(L"OSMapsData\\zz\\tiles.vtp", &n);
    bool same = n == 251000;
    for (int i = 0; same && i < n; i++)
        same = (unsigned char)tiles[i] == i % 251;
    free(tiles);
    Check(same && FileIs(L"OSMapsData\\zz\\search.idx", "IDXIDXIDXIDXIDXIDXIDXIDXIDXIDX", 30),
          "disk: the region whole - its tiles too, unpacked piece by piece", NULL);
    Check(FileIs(L"OSMapsData\\zz\\parking.lay", "PARKPARKPARK", 12), "disk: Moscow's park lines come with its region", NULL);
    char* ovb = FileBytes(L"OSMapsData\\overview\\overview.vtp", &n);
    Check(n == 800 && !memcmp(ovb, "OVERVIEW", 8), "disk: the overview after it", NULL);
    free(ovb);
    char* grf = FileBytes(L"OSMapsData\\overview\\trunk.grf", &n);
    Check(n == 400 && !memcmp(grf, "OSMG", 4), "disk: the trunk roads in the same pack", NULL);
    free(grf);
    Check(GetFileAttributes(oldTrunk) == 0xFFFFFFFF, "disk: the trunk roads' old folder gone", NULL);
    Check(s_ovAsks == 1, "disk: one archive for both", NULL);
    wchar_t tdate[MAX_PATH];
    PathInDir(tdate, L"..\\OSMapsData\\trunk\\region.txt");   // (the test's own graph found first: of that build too)
    HANDLE h = CreateFile(tdate, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD put;
    WriteFile(h, "name=trunk\ndate=2026-10-30\n", 27, &put, NULL);
    CloseHandle(h);
    Remove(zz);
    Check(MapsGetRegion(1) && WaitMaps() && s_ovAsks == 1, "disk: the pack there, of the catalog's build - not asked again", NULL);
    h = CreateFile(tdate, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(h, "name=trunk\ndate=2026-10-01\n", 27, &put, NULL);
    CloseHandle(h);
    wchar_t odate[MAX_PATH];
    PathInDir(odate, L"OSMapsData\\overview\\region.txt");
    h = CreateFile(odate, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(h, "name=overview\ndate=2026-10-01\n", 30, &put, NULL);
    CloseHandle(h);
    Remove(zz);
    Check(MapsGetRegion(1) && WaitMaps() && s_ovAsks == 2, "disk: the graph of another build - the pack downloaded again", NULL);
    DeleteFile(tdate);
    PathInDir(yy, L"OSMapsData\\yy");
    Remove(yy);
    Check(MapsGetRegion(0) && WaitMaps() && FileIs(L"OSMapsData\\yy\\search.idx", "IDXIDXIDXIDXIDXIDXIDXIDXIDXIDX", 30),
          "disk: a region not there whole - its graph and search from the release", NULL);
    Remove(yy);
    Remove(zz);
    s_apiFail = 1;
    MapsAt(1, &m);
    Check(MapsGetRegion(1) && WaitMaps() && MapsAt(1, &m) && MapState(m) == MAP_NEW,
          "disk: no link from its API - not written", NULL);
    s_apiFail = 0;
    Remove(zz);
    PathInDir(ov, L"OSMapsData\\overview");
    Remove(ov);
    s_withDisk = 0;
    free(s_full);
    free(s_ov);
    Check(MapsList() && WaitMaps() && MapsCount() == 2, "disk: (the catalog as before)", NULL);
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
    Check(s_asked <= s_zipLen / 100 + 3, "download: the storage's link kept for the next ranges (the user's Yandex Disk "
          "2026-10-31: a redirect a piece - a quarter of the time)", NULL);
    DeleteFile(part);
    s_asked = 0;
    s_stored = 0;
    s_expireAt = 3;
    Check(MapsFetch("https://github.com/x/releases/download/maps/tiny.zip", part, 100) &&
          FileIs(L"..\\dl.zip.part", s_zip, s_zipLen), "download: the kept link expired - asked again, the file whole", NULL);
    s_expireAt = 0;
    s_sigFloor = 0;

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
    Check(MapsInstall("https://github.com/x/releases/download/maps/tiny.zip", dir, "zipped", MAPS_GRAPH) &&
          FileIs(L"..\\dldata\\zipped\\region.txt", "name=zipped\ndate=2026-10-29\n", 28) &&
          FileIs(L"..\\dldata\\zipped\\search.idx", "IDXIDXIDXIDXIDXIDXIDXIDXIDXIDX", 30),
          "download: a region installed into its folder", NULL);
    wchar_t left[MAX_PATH];
    PathInDir(left, L"..\\dldata\\zipped.zip");
    Check(GetFileAttributes(left) == 0xFFFFFFFF, "download: the zip removed after", NULL);

    // the job, as the screen starts it: the catalog, then a region (into the program's OSMapsData here - no card),
    // no pack after it: this catalog has no disk
    Check(MapsBusyRegion() == -1, "job: no region downloaded before any", NULL);
    s_catFails = 2;
    Check(MapsList() && WaitMaps() && MapsCount() == 2, "job: the catalog loaded (the link lost twice: asked again)", NULL);
    s_catFails = 3;
    Check(MapsList() && WaitMaps() && MapsCount() == 0, "job: the link lost for good - no catalog", NULL);
    s_catFails = 0;
    Check(MapsList() && WaitMaps() && MapsCount() == 2, "job: the catalog again", NULL);
    MapRegion m;
    Check(MapsAt(0, &m) && !strcmp(m.name, "tiny") && MapState(m) == MAP_OK && MapsAt(1, &m) && MapState(m) == MAP_NEW
          && !MapsAt(2, &m), "job: the card's states", NULL);
    s_asked = 0;
    Check(MapsGetRegion(1) && WaitMaps() && MapsAt(1, &m) && !strcmp(m.have, "2026-10-29") && MapState(m) == MAP_OK,
          "job: a region downloaded, the card's state after", NULL);
    Check(FileIs(L"OSMapsData\\zz\\search.idx", "IDXIDXIDXIDXIDXIDXIDXIDXIDXIDX", 30), "job: into the program's folder",
          NULL);
    Check(s_seen == 1 && MapsBusyRegion() == -1, "job: the region being downloaded told (its row's ОТМЕНА), none after",
          NULL);
    wchar_t text[128];
    int gotKb = 0, total = 0;
    Check(MapsJob(text, 128, &gotKb, &total) == MAPS_IDLE && total == 2 && wcsstr(text, L"Z"), "job: its result told",
          NULL);
    Check(!MapsGetRegion(5), "job: no such region", NULL);

    // cancelled (the user 2026-10-30): stopped, nothing installed, the part kept for the next time; not sticking
    wchar_t zz[MAX_PATH], zzPart[MAX_PATH];
    PathInDir(zz, L"OSMapsData\\zz");
    Remove(zz);
    PathInDir(zzPart, L"OSMapsData\\zz.zip.part");
    s_cancelNow = 1;
    Check(MapsGetRegion(1) && WaitMaps() && MapsAt(1, &m) && MapState(m) == MAP_NEW &&
          GetFileAttributes(zzPart) != 0xFFFFFFFF, "cancel: stopped, not installed, the part kept", NULL);
    Check(MapsJob(text, 128, &gotKb, &total) == MAPS_IDLE && wcsstr(text, L"остановлена"), "cancel: told", NULL);
    s_cancelNow = 0;
    Check(MapsGetRegion(1) && WaitMaps() && MapsAt(1, &m) && MapState(m) == MAP_OK, "cancel: the next one goes on",
          NULL);
    DiskChecks();
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
// The roads' levels (the user 2026-11-02: a way across Moscow never ended - other navigators take seconds): near the
// ends every road, farther from 5 km the tertiary and up, past 20 km the main roads only; a search with none found
// asks again with fewer limits (level 1: the tertiary and up past 5 km; 0: every road)
static void LevelChecks()
{
    Check(RoadTier(0) == 0 && RoadTier(1) == 0 && RoadTier(2) == 0 && RoadTier(9) == 0 && RoadTier(11) == 0,
          "levels: motorway, trunk, primary and their links - the main roads", NULL);
    Check(RoadTier(3) == 1 && RoadTier(4) == 1 && RoadTier(12) == 1 && RoadTier(13) == 1,
          "levels: secondary, tertiary and their links", NULL);
    Check(RoadTier(5) == 2 && RoadTier(6) == 2 && RoadTier(8) == 2 && RoadTier(14) == 2 && RoadTier(15) == 2,
          "levels: unclassified, residential, service, road, track - the minor", NULL);
    Check(HierAllows(2, 4999, 2) && !HierAllows(2, 5000, 2) && HierAllows(1, 19999, 2) && !HierAllows(1, 20000, 2) &&
          HierAllows(0, 1000000, 2), "levels: every road near the ends, the tertiary to 20 km, then the main", NULL);
    Check(HierAllows(1, 50000, 1) && !HierAllows(2, 5000, 1) && HierAllows(2, 4999, 1) && HierAllows(2, 1000000, 0),
          "levels: asked again - the tertiary and up, at last every road", NULL);
}

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
    static const char* kMaybe[2] = { "?nope.bin", "region.txt" };
    Check(ZipExtract(zip, dir, kMaybe, 2), "maps: \"?\" - it may be missing, the rest goes on", NULL);
    static const char* kLayers[1] = { "?layers.lay" };
    wchar_t full[MAX_PATH];
    PathInDir(full, L"..\\full.zip");
    Check(ZipExtract(full, dir, kLayers, 1) && FileIs(L"..\\unzip\\layers.lay", "LAYLAYLAYLAY", 12), "maps: \"?\" - there: unpacked", NULL);
}

static bool Writable(const wchar_t* name)   // the data folder's file can be opened for writing (nothing holds it)
{
    wchar_t path[MAX_PATH];
    PathInDir(path, name);
    HANDLE f = CreateFile(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(f);
    return true;
}

// A region downloaded again while its graph and index are in use (the user 2026-10-30: Moscow's oblast "failed" -
// its route.grf held by the route just made, the zip not unpacked over it): OfflineDrop lets them go
// The map's inertia (fling.cpp; the user 2026-11-04: "rubber" scrolling - the map goes on after the finger, by its
// speed, and slows down)
static void Swipe(Fling* f, DWORD t0, int n, int stepMs, int dxPerStep)   // the finger: n moves to the east
{
    FlingReset(f);
    for (int i = 0; i <= n; i++)
        FlingAdd(f, t0 + i * stepMs, 100 + i * dxPerStep, 200);
}

static double Glide(Fling* f, int stepMs, int steps, double* dy)
{
    double sx = 0, sy = 0, x, y;
    for (int i = 0; i < steps && FlingStep(f, stepMs, &x, &y); i++) {
        sx += x;
        sy += y;
    }
    *dy = sy;
    return sx;
}

static void FlingChecks()
{
    Fling f;
    double dy;
    Swipe(&f, 1000, 10, 10, 10);   // 1 px/ms
    Check(FlingStart(&f, 1100), "fling: a quick swipe goes on", NULL);
    double sx = Glide(&f, 20, 200, &dy);
    Check(sx > 250 && sx < 325 && fabs(dy) < 0.5, "fling: ~ speed x 325 ms on, the finger's way only", NULL);
    Check(!FlingStep(&f, 20, &sx, &dy), "fling: then it stops", NULL);

    Swipe(&f, 1000, 10, 10, 10);
    Check(!FlingStart(&f, 1200), "fling: the finger held still before the release - none", NULL);
    Swipe(&f, 1000, 10, 10, 1);   // 0.1 px/ms
    Check(!FlingStart(&f, 1100), "fling: a slow drag - none", NULL);
    FlingReset(&f);
    FlingAdd(&f, 1000, 100, 200);
    Check(!FlingStart(&f, 1000), "fling: a tap - none", NULL);

    Swipe(&f, 1000, 10, 10, 200);   // 20 px/ms
    FlingStart(&f, 1100);
    double x1, y1;
    FlingStep(&f, 1, &x1, &y1);
    Check(x1 > 3.5 && x1 <= 4.0, "fling: its speed capped (4 px/ms)", NULL);

    Fling a, b;   // the same way whatever the frames
    Swipe(&a, 1000, 10, 10, 10);
    FlingStart(&a, 1100);
    b = a;
    double ya, yb, xa = Glide(&a, 10, 400, &ya), xb = Glide(&b, 80, 50, &yb);
    Check(fabs(xa - xb) < 3, "fling: short frames and long ones - the same way", NULL);

    FlingReset(&f);   // slow, then quick: the speed is the last 100 ms' (1 px/ms), not the whole way's
    for (int i = 0; i <= 10; i++)
        FlingAdd(&f, 1000 + i * 20, 100 + i * 2, 200);
    for (int i = 1; i <= 5; i++)
        FlingAdd(&f, 1200 + i * 20, 120 + i * 20, 200);
    FlingStart(&f, 1300);
    FlingStep(&f, 1, &x1, &y1);
    Check(x1 > 0.95 && x1 < 1.05, "fling: the speed of the last 100 ms only", NULL);

    FlingReset(&f);   // a pause, then one quick point: the last point counts, not the one before
    for (int i = 0; i <= 5; i++)
        FlingAdd(&f, 1000 + i * 10, 100 + i * 10, 200);
    FlingAdd(&f, 1095, 200, 200);
    Check(FlingStart(&f, 1100), "fling: the newest point's time decides the pause", NULL);

    FlingReset(&f);   // two points are enough
    FlingAdd(&f, 1000, 100, 200);
    FlingAdd(&f, 1050, 150, 200);
    Check(FlingStart(&f, 1050), "fling: two points - a swipe", NULL);
    FlingReset(&f);   // two at the same moment: none (no division by 0)
    FlingAdd(&f, 1000, 100, 200);
    FlingAdd(&f, 1000, 150, 200);
    Check(!FlingStart(&f, 1000), "fling: two points at once - none", NULL);
    Check(!FlingStep(&f, 20, &x1, &y1) && x1 == 0 && y1 == 0, "fling: not started - nothing moves", NULL);

    FlingReset(&f);   // diagonal, 3 + 3 px/ms: the speed is its length (4.24, capped to 4)
    for (int i = 0; i <= 10; i++)
        FlingAdd(&f, 1000 + i * 10, 100 + i * 30, 200 + i * 30);
    Check(FlingStart(&f, 1100), "fling: a quick diagonal goes on", NULL);
    FlingStep(&f, 1, &x1, &y1);
    Check(x1 > 2.6 && x1 < 2.9 && y1 > 2.6 && y1 < 2.9, "fling: a diagonal capped by its length", NULL);

    Swipe(&f, 1000, 10, 10, -10);   // to the west, and up
    for (int i = 0; i <= 10; i++)
        FlingAdd(&f, 1000 + i * 10, 100 - i * 10, 200 - i * 5);
    FlingStart(&f, 1100);
    sx = Glide(&f, 20, 200, &dy);
    Check(sx < -250 && dy < -120 && dy > -170, "fling: west and up, both ways", NULL);
}

static bool Near(double a, double b)
{
    return a > b - 0.02 && a < b + 0.02;
}

static void ZoomChecks()   // the zoom's change shown smoothly (zoomanim.cpp)
{
    ZoomAnim a = { false, 0, 0, 0 };
    double s = 0;
    ZoomAnimStart(&a, 1000, 1);
    Check(ZoomAnimScale(&a, 1000, &s) && Near(s, 1), "zoom: closer - the frame as it was at first", NULL);
    Check(ZoomAnimScale(&a, 1000 + kZoomMs / 2, &s) && Near(s, 1.414), "zoom: halfway in time - halfway in scale", NULL);
    double s1, s2;
    ZoomAnimScale(&a, 1010, &s1);
    ZoomAnimScale(&a, 1020, &s2);
    Check(s2 - s1 < 0.01 && s1 >= 1, "zoom: eased at the start", NULL);
    ZoomAnimScale(&a, 1000 + kZoomMs - 1, &s);
    Check(s > 1.98 && s <= 2, "zoom: about twice at the end", NULL);
    Check(!ZoomAnimScale(&a, 1000 + kZoomMs, &s) && !a.on && Near(s, 2), "zoom: then over - the new zoom's frame", NULL);
    ZoomAnimStart(&a, 2000, -1);
    Check(ZoomAnimScale(&a, 2000 + kZoomMs / 2, &s) && Near(s, 0.707), "zoom: farther - the frame shrinks (over again)",
          NULL);
    ZoomAnimStart(&a, 3000, -1);   // another level meanwhile: from where it is, on to a quarter
    ZoomAnimScale(&a, 3000 + kZoomMs / 2, &s1);
    ZoomAnimStart(&a, 3000 + kZoomMs / 2, -1);
    ZoomAnimScale(&a, 3000 + kZoomMs / 2, &s2);
    Check(Near(s1, s2), "zoom: a level more meanwhile - no jump", NULL);
    Check(ZoomAnimScale(&a, 3000 + kZoomMs / 2 + kZoomMs - 1, &s) && s > 0.24 && s < 0.26,
          "zoom: ... and on to both levels", NULL);
    ZoomAnimStart(&a, 6000, 1);   // the last one over by the clock, never asked: a new one from the frame as it is
    ZoomAnimStart(&a, 6000 + kZoomMs * 3, 1);
    Check(ZoomAnimScale(&a, 6000 + kZoomMs * 3, &s) && Near(s, 1), "zoom: after a finished one - anew", NULL);
    ZoomAnimStart(&a, 7000, 1);   // the new zoom's points (the car) onto the frame shown: half as far at first, then as is
    ZoomAnimScale(&a, 7000, &s);
    Check(Near(ZoomAnimNew(&a, s), 0.5), "zoom: the new frame's points - half way out at first", NULL);
    ZoomAnimScale(&a, 7000 + kZoomMs - 1, &s);
    Check(ZoomAnimNew(&a, s) > 0.99 && ZoomAnimNew(&a, s) <= 1.0, "zoom: ... where they are at the end", NULL);
    ZoomAnimStart(&a, 8000, 1);   // asked late: the end's scale, not further
    Check(!ZoomAnimScale(&a, 8000 + kZoomMs * 3 / 2, &s) && Near(s, 2), "zoom: asked late - the end's scale", NULL);
    ZoomAnimStart(&a, 9000, 1);   // the next one just as this one ends: anew
    ZoomAnimStart(&a, 9000 + kZoomMs, 1);
    Check(ZoomAnimScale(&a, 9000 + kZoomMs, &s) && Near(s, 1), "zoom: one more just at the end - anew", NULL);
    ZoomAnim off = { false, 0, 0, 0 };
    Check(!ZoomAnimScale(&off, 5000, &s) && Near(s, 1), "zoom: not started - none", NULL);

    RECT map = { 0, 0, 800, 480 }, r;
    ZoomAnimRect(2, 400, 240, map, &r);
    Check(r.left == -400 && r.top == -240 && r.right == 1200 && r.bottom == 720, "zoom: twice around the center", NULL);
    ZoomAnimRect(2, 400, 400, map, &r);   // the car near the bottom: it stays where it is
    Check(r.left == -400 && r.top == -400 && r.right == 1200 && r.bottom == 560, "zoom: twice around the car", NULL);
    ZoomAnimRect(0.5, 400, 400, map, &r);
    Check(r.left == 200 && r.top == 200 && r.right == 600 && r.bottom == 440, "zoom: half around the car", NULL);
    RECT part = { 100, 50, 700, 450 };
    ZoomAnimRect(1, 300, 200, part, &r);
    Check(r.left == 100 && r.top == 50 && r.right == 700 && r.bottom == 450, "zoom: scale 1 - the map's own rect", NULL);
}

static void DropChecks()
{
    char* r = Route("50.000,53.000;50.006,53.000");
    char* a = Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=53.0&lon=50.0");   // Тестовск
    Check(r && a && !Writable(L"..\\OSMapsData\\tiny\\route.grf") && !Writable(L"..\\OSMapsData\\tiny\\search.idx"),
          "drop: the graph and the index held while loaded", r);
    free(r);
    free(a);
    OfflineDrop();
    Check(Writable(L"..\\OSMapsData\\tiny\\route.grf") && Writable(L"..\\OSMapsData\\tiny\\search.idx"),
          "drop: free to be written over", NULL);
    r = Route("50.000,53.000;50.006,53.000");
    Check(r != NULL, "drop: loaded again when asked", r);
    free(r);
    free(Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=53.0&lon=50.0"));
    r = Route("50.000,53.000;50.006,53.000");   // (the user's Shturmann 2026-10-31: a long way ran out of memory)
    Check(r && Writable(L"..\\OSMapsData\\tiny\\search.idx") && !Writable(L"..\\OSMapsData\\tiny\\route.grf"),
          "drop: a route lets the search's index go first", r);
    free(r);
}

static void PutFile(const wchar_t* name, const void* data, DWORD n)
{
    wchar_t path[MAX_PATH];
    PathInDir(path, name);
    HANDLE f = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    WriteFile(f, data, n, &wr, NULL);
    CloseHandle(f);
}

static int ReadBad(const void* data, DWORD n)
{
    wchar_t path[MAX_PATH];
    PathInDir(path, L"..\\bad.lay");
    PutFile(L"..\\bad.lay", data, n);
    int box[4] = { 53000000, 50000000, 53001000, 50001000 };
    LayerPoint p[4];
    int got = LayersRead(path, box, p, 4);
    DeleteFile(path);
    return got;
}

static void LayerFileChecks()   // layers.lay as make_layers.py writes it
{
    wchar_t path[MAX_PATH];
    PathInDir(path, L"..\\OSMapsData\\tiny\\layers.lay");
    LayerPoint p[8];
    int west[4] = { 53000000, 50000000, 53002000, 50004000 }, both[4] = { 53000000, 50000000, 53002000, 50015000 };
    int n = LayersRead(path, west, p, 8);
    Check(sizeof(LayerPoint) == 12, "layers: a point is 12 bytes", NULL);
    Check(n == 3, "layers: the box's cells whole, the next cell not", NULL);
    Check(LayersRead(path, both, p, 8) == 4, "layers: two cells", NULL);
    int kinds = 0, paid = 0;
    for (int i = 0; i < 4; i++) {
        kinds |= 1 << p[i].kind;
        if (p[i].fee == FEE_PAID)
            paid = p[i].places;
    }
    Check(kinds == (1 << LAY_SIGNAL | 1 << LAY_BUMP | 1 << LAY_PARKING), "layers: the kinds", NULL);
    Check(paid == 40, "layers: a paid car park's places", NULL);
    Check(LayersRead(path, both, p, 2) == 2, "layers: no more than asked", NULL);
    int north[4] = { 53030000, 50000000, 53040000, 50015000 }, east[4] = { 53000000, 50020000, 53002000, 50030000 };
    Check(LayersRead(path, north, p, 8) == 0 && LayersRead(path, east, p, 8) == 0, "layers: no cells there - none", NULL);
    int cellEnd[4] = { 53000000, 50000000, 53009999, 50009999 };
    Check(LayersRead(path, cellEnd, p, 8) == 3, "layers: a box up to a cell's last - not the next rows, not the next cell", NULL);
    int rows[4] = { 53000000, 50000000, 53015000, 50009999 };
    Check(LayersRead(path, rows, p, 8) == 4 && p[0].lat == 53000000 && p[3].lat == 53010500, "layers: a second row after the first, the right points", NULL);
    int later[4] = { 53010000, 50000000, 53025000, 50009999 };
    p[2].lat = -1;
    Check(LayersRead(path, later, p, 2) == 2 && p[1].lat == 53020500 && p[2].lat == -1, "layers: the second row - only what room is left", NULL);
    int last[4] = { 53020000, 50000000, 53025000, 50009999 };
    Check(LayersRead(path, last, p, 8) == 2, "layers: the file's last cell - up to its last point", NULL);
    Check(LayersRead(L"nope\\layers.lay", both, p, 8) == -1, "layers: no file", NULL);
    unsigned head[6] = { 0x4C4D534F, 1, 1, 0, 0, 0 };   // 'OSML', version, cells, points; a cell
    Check(ReadBad("OSM", 3) == -1, "layers: cut short", NULL);
    Check(ReadBad(head, 20) == -1, "layers: a cell's bytes past what it tells", NULL);
    Check(ReadBad(head, 24) == 0, "layers: exactly the cells' bytes read", NULL);
    head[2] = 0;
    Check(ReadBad(head, 16) == 0, "layers: an empty region's file", NULL);
    head[1] = 2;
    Check(ReadBad(head, 16) == -1, "layers: another version", NULL);
    head[1] = 1;
    head[0] = 0;
    Check(ReadBad(head, 16) == -1, "layers: another file", NULL);
    head[0] = 0x4C4D534F;
    head[3] = 1;
    Check(ReadBad(head, 16) == -1, "layers: points told, none there", NULL);
}

static void LayerBoxChecks()   // any side of the box to another cell: read anew
{
    int n = -1;
    int both[4] = { 53000000, 50000000, 53002000, 50015000 }, eastOnly[4] = { 53000000, 50010000, 53002000, 50015000 };
    LayersIn(both, &n);
    LayersIn(eastOnly, &n);
    Check(n == 1, "layers: its west side moved", NULL);
    int low[4] = { 52990000, 50000000, 53009999, 50015000 }, high[4] = { 52990000, 50000000, 53010500, 50015000 };
    LayersIn(low, &n);
    Check(n == 4, "layers: a box wider than the points", NULL);
    LayersIn(high, &n);
    Check(n == 5, "layers: its north side moved", NULL);
    int south[4] = { 53010000, 50000000, 53010500, 50015000 };
    LayersIn(south, &n);
    Check(n < 5, "layers: its south side moved", NULL);
}

static void LayerRegionChecks()   // the region's under the box, kept
{
    LayersDrop();
    int n = -1;
    int west[4] = { 53000000, 50000000, 53002000, 50004000 }, both[4] = { 53000000, 50000000, 53002000, 50015000 };
    const LayerPoint* p = LayersIn(west, &n);
    Check(p && n == 3, "layers: the region's under the box", NULL);
    int same[4] = { 53001000, 50001000, 53003000, 50005000 };
    wchar_t path[MAX_PATH], kept[MAX_PATH];
    PathInDir(path, L"..\\OSMapsData\\tiny\\layers.lay");
    PathInDir(kept, L"..\\OSMapsData\\tiny\\layers.keep");
    Check(MoveFile(path, kept) != 0, "layers: nothing held open between the calls (a download can write over it)", NULL);
    LayersIn(same, &n);
    Check(n == 3, "layers: the same cells - kept, not read", NULL);
    LayersIn(both, &n);
    Check(n == 0, "layers: the file gone, other cells - none", NULL);
    MoveFile(kept, path);
    LayersDrop();
    LayersIn(both, &n);
    Check(n == 4, "layers: dropped - read anew", NULL);
    LayersPause(true);
    LayersIn(west, &n);
    Check(n == 4, "layers: a region being written - the points in hand stay", NULL);
    LayersPause(false);
    LayersIn(both, &n);
    Check(n == 4, "layers: after it", NULL);
    LayersIn(west, &n);
    Check(n == 3, "layers: after it, other cells read", NULL);
    int nowhere[4] = { 10000000, 10000000, 10001000, 10001000 };
    LayersIn(nowhere, &n);
    Check(n == 0, "layers: no region there - none", NULL);
    LayersIn(west, &n);
    Check(n == 3, "layers: back in the region, not held off by the failed try", NULL);
    LayersPause(true);
    LayersIn(west, &n);
    MoveFile(path, kept);
    LayersPause(false);
    LayersIn(west, &n);
    Check(n == 0, "layers: after a region was written - the same cells read anew", NULL);
    MoveFile(kept, path);
    LayersPause(true);
    LayersDrop();
    LayersIn(west, &n);
    Check(n == 0, "layers: dropped while a region is written - none in hand", NULL);
    LayersPause(false);
    LayerBoxChecks();
    LayersDrop();
}

static void ParkLineChecks()   // Moscow's paid street parking as make_parklines.py writes it (parking.lay)
{
    wchar_t path[MAX_PATH], kept[MAX_PATH], other[MAX_PATH];
    PathInDir(path, L"..\\OSMapsData\\tiny\\parking.lay");
    PathInDir(kept, L"..\\OSMapsData\\tiny\\parking.keep");
    PathInDir(other, L"..\\OSMapsData\\tiny\\layers.lay");
    ParkLine l[8];
    LayerPoint p[8];
    int west[4] = { 53000000, 50000000, 53002000, 50004000 }, both[4] = { 53000000, 50000000, 53002000, 50015000 };
    Check(sizeof(ParkLine) == 20, "park lines: a piece is 20 bytes", NULL);
    Check(ParkLinesRead(path, west, l, 8) == 1 && l[0].lat == 53000500 && l[0].lon == 50001000 && l[0].dLat == 0 && l[0].dLon == 1000, "park lines: a cell's piece, where", NULL);
    Check(l[0].price[0] == 4 && l[0].price[1] == 4 && l[0].price[3] == 0, "park lines: its prices", NULL);
    Check(ParkLinesRead(path, both, l, 8) == 3 && l[2].dLat == 200 && l[2].dLon == 500 && l[2].price[1] == 38 && l[2].price[4] == 45, "park lines: two cells, in the file's order", NULL);
    Check(ParkLinesRead(other, both, l, 8) == -1 && LayersRead(path, both, p, 8) == -1, "park lines and layers: the one's reader refuses the other's file", NULL);
    LayersDrop();
    int n = -1;
    const ParkLine* in = ParkLinesIn(both, &n);
    Check(in && n == 3 && in[0].lat == 53000500, "park lines: the region's under the box", NULL);
    MoveFile(path, kept);
    ParkLinesIn(both, &n);
    Check(n == 3, "park lines: the same cells - kept, the file not held", NULL);
    ParkLinesIn(west, &n);
    Check(n == 0, "park lines: a region without them (all but Moscow's) - none", NULL);
    MoveFile(kept, path);
    LayersDrop();
    ParkLinesIn(west, &n);
    Check(n == 1, "park lines: dropped - read anew", NULL);
    LayersPause(true);
    ParkLinesIn(both, &n);
    Check(n == 1, "park lines: a region being written - those in hand stay", NULL);
    MoveFile(path, kept);
    LayersPause(false);
    ParkLinesIn(west, &n);
    Check(n == 0, "park lines: after a region was written - read anew", NULL);
    MoveFile(kept, path);
    LayersDrop();
}

static void ParkPriceChecks()   // Sunday 0: as GetLocalTime tells
{
    ParkLine l = { 0, 0, 0, 0, { 6, 8, 7, 1, 2, 3 } };
    Check(ParkPrice(l, 1, 0) == 60 && ParkPrice(l, 1, 7) == 60, "park price: a weekday before 8", NULL);
    Check(ParkPrice(l, 1, 8) == 80 && ParkPrice(l, 5, 20) == 80, "park price: Monday to Friday, 8 to 21", NULL);
    Check(ParkPrice(l, 1, 21) == 70 && ParkPrice(l, 5, 23) == 70, "park price: a weekday from 21", NULL);
    Check(ParkPrice(l, 6, 7) == 10 && ParkPrice(l, 0, 0) == 10, "park price: the weekend before 8", NULL);
    Check(ParkPrice(l, 6, 8) == 20 && ParkPrice(l, 0, 20) == 20, "park price: Saturday, Sunday, 8 to 21", NULL);
    Check(ParkPrice(l, 6, 21) == 30 && ParkPrice(l, 0, 23) == 30, "park price: the weekend from 21", NULL);
    ParkLine top = { 0, 0, 0, 0, { 255, 0, 0, 0, 0, 0 } };
    Check(ParkPrice(top, 1, 0) == 2550, "park price: up to the byte's whole", NULL);
}

static void LayerAheadChecks()
{
    LayerPoint p[3] = { { 53000000, 50005000, LAY_BUMP, 0, 0 }, { 53000000, 50004000, LAY_SIGNAL, 0, 0 }, { 53000000, 50007000, LAY_BUMP, 0, 0 } };
    int d = 0;
    Check(LayerAhead(p, 3, LAY_BUMP, 53000000, 50003000, 90, 200, &d) && d > 125 && d < 142, "ahead: east, the nearest of its kind", NULL);
    Check(LayerAhead(p, 3, LAY_SIGNAL, 53000000, 50003000, 90, 200, &d) && d > 60 && d < 75, "ahead: another kind", NULL);
    Check(!LayerAhead(p, 3, LAY_BUMP, 53000000, 50003000, 90, 100, &d), "ahead: too far yet", NULL);
    Check(!LayerAhead(p, 3, LAY_BUMP, 53000000, 50003000, 270, 200, &d), "ahead: behind", NULL);
    Check(!LayerAhead(p, 3, LAY_BUMP, 53000000, 50003000, 0, 200, &d), "ahead: to the side", NULL);
    Check(!LayerAhead(p, 3, LAY_BUMP, 53000400, 50003000, 90, 200, &d), "ahead: on the next street (44 m off the line)", NULL);
    Check(LayerAhead(p, 3, LAY_BUMP, 53000100, 50003000, 90, 200, &d), "ahead: a little off the line (GPS)", NULL);
    Check(LayerAhead(p, 3, LAY_BUMP, 53000000, 50006000, 90, 200, &d) && d > 60 && d < 75, "ahead: the one passed no more", NULL);
    Check(LayerAhead(p, 3, LAY_BUMP, 53000000, 50006000, 270, 200, &d) && d > 60 && d < 75, "ahead: the other way - the other one", NULL);
    Check(!LayerAhead(p, 0, LAY_BUMP, 53000000, 50003000, 90, 200, &d), "ahead: no points", NULL);
    Check(LayerAhead(p, 3, LAY_BUMP, 53000000, 50005000, 90, 200, &d) && d == 0, "ahead: right on it", NULL);
    LayerPoint distant = { 52999784, 50014944, LAY_BUMP, 0, 0 };   // (1000 m east, 24 m south: the degrees' metres at this latitude)
    Check(LayerAhead(&distant, 1, LAY_BUMP, 53000000, 50000000, 90, 2000, &d) && d >= 999 && d <= 1001, "ahead: far on, the line's edge, the distance exact", NULL);
    LayerPoint ne = { 53000636, 50001057, LAY_BUMP, 0, 0 };    // (100 m north-east)
    Check(LayerAhead(&ne, 1, LAY_BUMP, 53000000, 50000000, 45, 200, &d) && d >= 99 && d <= 101, "ahead: a diagonal course", NULL);
    int t = 1, c = 0;
    OverlayNext(&t, &c);
    Check(!t && c, "overlay: traffic -> car parks", NULL);
    OverlayNext(&t, &c);
    Check(!t && !c, "overlay: car parks -> neither", NULL);
    OverlayNext(&t, &c);
    Check(t && !c, "overlay: neither -> traffic", NULL);
}

static void TrafficLookChecks()   // how the traffic is shown (trafshade.cpp)
{
    TrafficLook t = TrafficLookOf(true, false, false, true);
    Check(t.onMap && !t.mapDim && !t.routeDim, "traffic: by day - on the map, bright", NULL);
    t = TrafficLookOf(true, false, true, true);
    Check(t.onMap && t.mapDim && !t.routeDim, "traffic: at night - the map's dimmed, the route's bright", NULL);
    t = TrafficLookOf(true, false, true, false);
    Check(t.onMap && !t.mapDim, "traffic: the raster's night - its own dimming, not twice", NULL);
    t = TrafficLookOf(true, true, false, true);
    Check(!t.onMap && !t.routeDim, "traffic: on the route alone - not on the map", NULL);
    t = TrafficLookOf(true, true, true, false);
    Check(!t.onMap && t.routeDim, "traffic: on the route alone at night - the route's dimmed", NULL);
    t = TrafficLookOf(false, false, true, true);
    Check(!t.onMap && !t.mapDim && !t.routeDim, "traffic: off - nowhere", NULL);
    Check(TrafficShade(RGB(200, 100, 20), false) == RGB(200, 100, 20), "traffic: a colour kept", NULL);
    Check(TrafficShade(RGB(200, 100, 20), true) == RGB(200 * kTrafficNightPct / 100, 100 * kTrafficNightPct / 100,
                                                        20 * kTrafficNightPct / 100), "traffic: a colour dimmed", NULL);
}

int main()
{
    GetModuleFileName(NULL, g_dir, MAX_PATH);
    wchar_t* s = wcsrchr(g_dir, L'\\');
    if (s)   // as from the program's folder obj\test\app\: its ..\OSMapsData is obj\test\OSMapsData
        wcscpy(s + 1, L"app\\");

    OffVolumes(false);   // (this PC's own disks' maps - a card in its reader - left out)
    OffFileChecks();
    LevelChecks();
    JsonChecks();
    MapsChecks();
    DownloadChecks();
    DropChecks();
    FlingChecks();
    ZoomChecks();
    TrafficLookChecks();
    LayerFileChecks();
    LayerRegionChecks();
    ParkLineChecks();
    ParkPriceChecks();
    LayerAheadChecks();

    // along the main street, west to east: ~400 m, no turn on the way
    char* b = Route("50.000,53.000;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 380 && Num(b, "\"distance\":") < 420, "main street: ~400 m", b);
    Check(b && Count(b, "\"type\":\"turn\"") == 0 && Count(b, "\"type\":\"depart\"") == 1 &&
          Count(b, "\"type\":\"arrive\"") == 1, "main street: depart, arrive, no turns", b);
    free(b);

    // a point by a road joined to no other (the user's log 2026-10-29: the Kremlin - "2 nodes settled", or the whole
    // region searched for 67 s): the nearest road of the network instead
    b = Route("50.006,53.000;50.003,52.9988");
    Check(b && !Has(b, L"Островная улица") && Num(b, "\"distance\":") > 150,
          "a road joined to no other: the way to the network's nearest road", b);
    free(b);

    // toll roads (the toll region, 60 N: A - B on a toll road ~555 m, round it ~1.7 km): taken; kept off when asked
    b = Route("50.000,60.000;50.010,60.000");
    Check(b && Num(b, "\"distance\":") < 600 && Has(b, L"Платная дорога"), "toll: the toll road, the shortest", b);
    free(b);
    b = Route("50.000,60.000;50.010,60.000", "&exclude=toll");
    Check(b && Num(b, "\"distance\":") > 1500 && !Has(b, L"Платная дорога"), "toll: kept off when asked - round it", b);
    free(b);
    b = Route("50.010,60.000;50.000,60.000", "&exclude=toll");
    Check(b && Num(b, "\"distance\":") > 1500 && !Has(b, L"Платная дорога"), "toll: kept off the other way too", b);
    free(b);
    b = Route("50.105,60.000;50.115,60.000", "&exclude=toll");   // by the far toll road alone: on it all the same
    Check(b && Num(b, "\"distance\":") > 500 && Num(b, "\"distance\":") < 600, "toll: no free road near - the toll one", b);
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
    Check(Has(b, L"\"name\":\"Тестовск\"") && !Has(b, L"Другоград") && !Has(b, L"Главная улица"),
          "search: this region's first, the others not mixed in (the town's name alone: the town, not its streets)", b);
    free(b);
    // here only a street of that name, there the town itself (the user 2026-10-29: "Жигулёвск" from Moscow - Moscow's
    // Жигулёвская streets, not the town): the regions' answers together, the town first
    b = Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=58.0&lon=50.0");   // Тестовск
    const char* there = Find(b, L"\"name\":\"Тестовск\""), *here = Find(b, L"\"name\":\"Тестовская улица\"");
    Check(there && here && there < here && Num(b, "\"coordinates\":[") > 0,
          "search: no exact match here - the other regions too, the exact one first", b);
    free(b);
    // a place's name in the query: near it, the streets not its own (the user 2026-10-29: "балашиха кольцевая 5" -
    // that Кольцевая is the next settlement's in OSM)
    b = Ask("/api/?q=%D0%B4%D0%B0%D0%BB%D1%8C%D0%BD%D0%B5%D0%B5%20%D0%B3%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F"
            "&limit=7&lat=58.0&lon=50.0");   // дальнее главная
    Check(Has(b, L"\"name\":\"Главная улица\"") && Has(b, L"\"city\":\"Другоград\""),
          "search: a village named - the street near it, its own town's", b);
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

    // the overview pack (the PC app's, 2026-10-30: the CIS's small zooms whole): its depth, its tiles; the regions' not
    // mistaken for it, nor it for a region's
    MapRequest o8 = { SRC_OSM, VEC_ZBASE + 8, 154, 79 }, o0 = { SRC_OSM, VEC_ZBASE + 0, 0, 0 };
    MapRequest o9 = { SRC_OSM, VEC_ZBASE + 9, 308, 158 };
    Check(VOverviewZ() == 8, "overview: its deepest zoom", NULL);
    Check(VOverviewTile(o8, &data, &len) && len == 4 && !memcmp(data, "ov 8", 4), "overview: Moscow's z8 tile", NULL);
    free(data);
    data = NULL;
    Check(VOverviewTile(o0, &data, &len) && len == 4 && !memcmp(data, "ov 0", 4), "overview: the z0 tile", NULL);
    free(data);
    data = NULL;
    Check(!VOverviewTile(o9, &data, &len), "overview: deeper than it - none", NULL);
    Check(!VPackTile(o8, &data, &len) && VPackTile(t14, &data, &len) && !memcmp(data, "tile 14", 7),
          "overview: not a region's pack; the region's still found", NULL);
    free(data);
    data = NULL;

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
