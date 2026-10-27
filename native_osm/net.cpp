#include "yamaps.h"

// Map downloader: one worker thread, "latest request wins".
// While a download is running new requests overwrite the pending one,
// so fast button presses never queue up more than one extra download.

static CRITICAL_SECTION s_cs;
static HANDLE           s_wake;
static HANDLE           s_thread;
static HWND             s_notify;
static volatile bool    s_stop;
static MapRequest       s_pending;
static bool             s_hasPending;
static DWORD            s_seq;
// search / route / address requests, before the tiles: a few waiting; a newer search or route replaces the waiting
// one of its kind, address lookups all go (each one names its own point)
struct ApiJob { int kind; bool https; DWORD seq; char host[64]; char path[1024]; };
static ApiJob           s_api[4];
static int              s_nApi;
static DWORD            s_apiSeq;

const wchar_t* NetErrorText(int err)
{
    switch (err) {
    case NET_OK:      return L"OK";
    case NET_DNS:     return L"нет сети (DNS)";
    case NET_CONNECT: return L"нет соединения";
    case NET_TIMEOUT: return L"таймаут";
    case NET_HTTP:    return L"ошибка сервера";
    case NET_DECODE:  return L"битая картинка";
    case NET_MEMORY:  return L"мало памяти";
    case NET_TLS:     return L"ошибка HTTPS";
    }
    return L"?";
}

// Cache\<z>\<bx>_<by>\<x>_<y>.img  (OSM tiles only)
void CachePath(wchar_t* out, const MapRequest& r)
{
    wchar_t name[96];
    _snwprintf(name, 96, L"Cache\\%d\\%d_%d\\%d_%d.img", r.z, r.x / CACHE_BLOCK, r.y / CACHE_BLOCK, r.x, r.y);
    name[95] = 0;
    PathInDir(out, name);
}

// The server's ETag is kept at the end of the cache file: <etag><length byte>"ETAG".
// Image decoders stop at the end of the image and ignore it.
static const int kEtagCap = 100;

static void ReadEtag(const wchar_t* path, char* etag)
{
    etag[0] = 0;
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    unsigned char tail[kEtagCap + 5];
    DWORD size = GetFileSize(h, NULL), got = 0;
    DWORD want = size < sizeof(tail) ? size : sizeof(tail);
    if (want >= 5 && SetFilePointer(h, -(LONG)want, NULL, FILE_END) != 0xFFFFFFFF &&
        ReadFile(h, tail, want, &got, NULL) && got == want && !memcmp(tail + got - 4, "ETAG", 4)) {
        int n = tail[got - 5];
        if (n < kEtagCap && n <= (int)got - 5) {
            memcpy(etag, tail + got - 5 - n, n);
            etag[n] = 0;
        }
    }
    CloseHandle(h);
}

// Raw response goes to the cache via a temp file, so a half-written file is never read.
static void SaveToCache(const MapRequest& r, const unsigned char* data, int len, const char* etag)
{
    wchar_t name[64], dir[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH];
    _snwprintf(name, 64, L"Cache\\%d", r.z);
    PathInDir(dir, name);
    CreateDirectory(dir, NULL);
    _snwprintf(name, 64, L"Cache\\%d\\%d_%d", r.z, r.x / CACHE_BLOCK, r.y / CACHE_BLOCK);
    PathInDir(dir, name);
    CreateDirectory(dir, NULL);

    CachePath(path, r);
    PathInDir(tmp, L"Cache\\download.tmp");
    HANDLE h = CreateFile(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, len, &w, NULL) && (int)w == len;
    int n = (int)strlen(etag);
    if (ok && n) {
        unsigned char trailer[kEtagCap + 5];
        memcpy(trailer, etag, n);
        trailer[n] = (unsigned char)n;
        memcpy(trailer + n + 1, "ETAG", 4);
        ok = WriteFile(h, trailer, n + 5, &w, NULL) && (int)w == n + 5;
        len += n + 5;
    }
    StampFile(h, g_today);   // the age check (cache_days) counts from this date
    CloseHandle(h);

    int oldBytes = 0;
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(path, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        oldBytes = (int)fd.nFileSizeLow;
        FindClose(f);
    }
    DeleteFile(path);
    if (!ok || !MoveFile(tmp, path)) {
        Log("cache: cannot save (%lu)", GetLastError());
        DeleteFile(tmp);
        CacheCountSaved(0, oldBytes);
        return;
    }
    CacheCountSaved(len, oldBytes);
}

// ---------------------------------------------------------------- 2GIS
// 2GIS traffic tiles are addressed by project (city) code plus the time of the current
// traffic snapshot. The same server and URLs as the 2GIS RasterJS API library uses; this is
// not a documented public API, it may change.

#include "regions_2gis.inc"

static bool InPolygon(double lon, double lat, const float (*p)[2], int n)
{
    bool c = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((p[i][1] > lat) != (p[j][1] > lat) &&
            lon < (p[j][0] - p[i][0]) * (lat - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0])
            c = !c;
    }
    return c;
}

// Smaller regions come first in the table: a city wins over the region around it.
static const char* RegionAt(double lon, double lat)
{
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        const Region2Gis& r = kRegions[i];
        if (lon >= r.lon0 && lon <= r.lon1 && lat >= r.lat0 && lat <= r.lat1 &&
            InPolygon(lon, lat, kRegionPoints + r.first, r.count))
            return r.code;
    }
    return NULL;
}

static char  s_tsCode[32];   // project the timestamp belongs to
static char  s_ts[24];
static DWORD s_tsTick;

static int TrafficTimestamp(const char* code)
{
    if (!strcmp(code, s_tsCode) && GetTickCount() - s_tsTick < 3 * 60000)
        return NET_OK;
    char path[96];
    _snprintf(path, sizeof(path), "/%s/meta/speed/time/", code);
    unsigned char* body = NULL;
    int len = 0, status = 0;
    int err = HttpGet("traffic0.maps.2gis.com", true, path, &body, &len, &status);
    if (err == NET_OK && status != 200)
        err = NET_HTTP;
    if (err == NET_OK) {
        int n = 0;
        for (const unsigned char* p = body; *p && n < (int)sizeof(s_ts) - 1; p++)
            if (*p >= '0' && *p <= '9')
                s_ts[n++] = (char)*p;
        s_ts[n] = 0;
        strncpy(s_tsCode, code, sizeof(s_tsCode) - 1);
        s_tsTick = GetTickCount();
    }
    free(body);
    return err;
}

// ---------------------------------------------------------------- vector tiles (OpenFreeMap)
// The tiles' path names the data's version: read from its TileJSON once a run ("tiles": [".../planet/<ver>/{z}/{x}/{y}.pbf"]).
static char s_vecPath[96];   // "/planet/<ver>", "" = not read yet

static int VecVersion()
{
    if (s_vecPath[0])
        return NET_OK;
    unsigned char* body = NULL;
    int len = 0, status = 0;
    int err = HttpGet("tiles.openfreemap.org", true, "/planet", &body, &len, &status);
    if (err == NET_OK && status != 200)
        err = NET_HTTP;
    if (err == NET_OK) {
        const char* t = strstr((const char*)body, "\"tiles\"");
        const char* p = t ? strstr(t, "/planet/") : NULL;
        const char* e = p ? strstr(p, "/{z}") : NULL;
        if (e && e - p < (int)sizeof(s_vecPath)) {
            memcpy(s_vecPath, p, e - p);
            s_vecPath[e - p] = 0;
            Log("net: vector tiles %s", s_vecPath);
        } else
            err = NET_DECODE;
    }
    free(body);
    return err;
}

// A shown tile from its data tile: the disk's .pbf, else downloaded (and kept) - then drawn.
static void FetchVector(const MapRequest& r, MapResult* res)
{
    res->look = MapLook();   // (read before drawing: a switch meanwhile drops this tile)
    MapRequest d = VTileData(r);
    wchar_t file[MAX_PATH];
    CachePath(file, d);
    unsigned char* body = NULL;
    int len = 0;
    if (VPackTile(d, &body, &len) || ReadWholeFile(file, &body, &len)) {   // the region's pack, the cache's copy
        if (!VTileRender(r, body, len, &res->img))
            res->err = NET_DECODE;
        free(body);
        return;
    }
    DWORD t0 = GetTickCount();
    res->err = VecVersion();
    if (res->err != NET_OK)
        return;
    char path[160];
    _snprintf(path, sizeof(path), "%s/%d/%d/%d.pbf", s_vecPath, d.z - VEC_ZBASE, d.x, d.y);
    path[sizeof(path) - 1] = 0;
    res->err = HttpGet("tiles.openfreemap.org", true, path, &body, &len, &res->httpStatus);
    if (res->err == NET_OK && res->httpStatus != 200) {
        Log("net: HTTP %d: %.200s", res->httpStatus, body);
        res->err = NET_HTTP;
        if (res->httpStatus == 404)
            s_vecPath[0] = 0;   // a new version: the old one's tiles are gone - read it again
    }
    if (res->err == NET_OK) {
        res->bytes = len;
        SaveToCache(d, body, len, "");
        if (!VTileRender(r, body, len, &res->img))
            res->err = NET_DECODE;
    }
    free(body);
    Log("net: vector %s -> %d (%d bytes, %lu ms)", path, res->err, res->bytes, GetTickCount() - t0);
}

static void Fetch(const MapRequest& r, MapResult* res)
{
    if (r.src == SRC_OSM && g_cfg.vector) {
        FetchVector(r, res);
        return;
    }
    const char* host;
    bool https;
    char path[256];
    if (r.src == SRC_2GIS) {
        double lon, lat;
        WorldToGeo((r.x + 0.5) * TILE, (r.y + 0.5) * TILE, r.z, &lon, &lat);
        const char* code = RegionAt(lon, lat);
        if (!code) {
            res->err = NET_OK;   // no 2GIS city here: an empty overlay, not an error
            Log("net: 2gis: no city at %.4f,%.4f", lat, lon);
            return;
        }
        res->err = TrafficTimestamp(code);
        if (res->err != NET_OK) {
            Log("net: 2gis: no timestamp for %s (%d)", code, res->err);
            return;
        }
        host = "traffic0.maps.2gis.com";
        https = true;
        _snprintf(path, sizeof(path), "/%s/traffic/%d/%d/%d/speed/0/?%s", code, r.z, r.x, r.y, s_ts);
    } else if (r.src == SRC_OSM) {
        host = "tile.openstreetmap.org";
        https = true;
        _snprintf(path, sizeof(path), "/%d/%d/%d.png", r.z, r.x, r.y);
    } else {
        // Yandex Static API, traffic only: a transparent 600x450 image
        double lon, lat;
        WorldToGeoYa((double)r.x * YA_W, (double)r.y * YA_H, r.z, &lon, &lat);
        host = "static-maps.yandex.ru";
        https = false;
        _snprintf(path, sizeof(path), "/1.x/?ll=%.6f,%.6f&z=%d&size=%d,%d&l=trf,trfe&lang=ru_RU",
                  lon, lat, r.z, YA_W, YA_H);
    }
    path[sizeof(path) - 1] = 0;

    // A cached OSM tile (expired, or a tap on the map) is only checked: 304 = not changed.
    wchar_t file[MAX_PATH];
    char cachedTag[kEtagCap] = "", etag[kEtagCap] = "";
    if (r.src == SRC_OSM) {
        CachePath(file, r);
        ReadEtag(file, cachedTag);
    }

    DWORD t0 = GetTickCount();
    unsigned char* body = NULL;
    int bodyLen = 0;
    res->err = HttpGet(host, https, path, &body, &bodyLen, &res->httpStatus, cachedTag, etag, kEtagCap);
    if (res->err == NET_OK && res->httpStatus == 304 && cachedTag[0]) {
        HANDLE h = CreateFile(file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            StampFile(h, g_today);   // fresh for another cache_days
            CloseHandle(h);
        }
        if (!ImageFromFile(file, &res->img))
            res->err = NET_DECODE;
        free(body);
        Log("net: %s%s -> %d (HTTP 304, %lu ms)", host, path, res->err, GetTickCount() - t0);
        return;
    }
    if (res->err == NET_OK && res->httpStatus == 204 && r.src != SRC_OSM) {
        // 2GIS: "no traffic on this tile", an empty overlay
    } else if (res->err == NET_OK && res->httpStatus != 200) {
        Log("net: HTTP %d: %.200s", res->httpStatus, body);
        res->err = NET_HTTP;
    }
    if (res->err == NET_OK) {
        res->bytes = bodyLen;
        if (r.src == SRC_OSM) {
            if (ImageFromMemory(body, bodyLen, &res->img))
                SaveToCache(r, body, bodyLen, etag);
            else
                res->err = NET_DECODE;
        } else if (res->httpStatus != 204) {
            res->ov = OverlayFromMemory(body, bodyLen);
            if (!res->ov)
                res->err = NET_DECODE;
        }
    }
    free(body);
    Log("net: %s%s -> %d (%d bytes, %lu ms)", host, path, res->err, res->bytes, GetTickCount() - t0);
}

static DWORD WINAPI NetThread(LPVOID)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    while (!s_stop) {
        WaitForSingleObject(s_wake, INFINITE);
        for (;;) {
            if (s_stop)
                break;
            EnterCriticalSection(&s_cs);
            bool api = s_nApi > 0;
            ApiJob job;
            if (api) {
                job = s_api[0];
                for (int i = 1; i < s_nApi; i++)
                    s_api[i - 1] = s_api[i];
                s_nApi--;
            }
            LeaveCriticalSection(&s_cs);
            DWORD apiSeq = job.seq;
            if (api) {
                ApiResult* ar = (ApiResult*)calloc(1, sizeof(ApiResult));
                if (!ar)
                    break;
                ar->kind = job.kind;
                ar->seq = apiSeq;
                DWORD t0 = GetTickCount();
                if ((job.kind == API_ROUTE && OfflineRoute(job.path, &ar->body, &ar->len)) ||   // a region's data
                    ((job.kind == API_SEARCH || job.kind == API_REVERSE) && OfflineSearch(job.path, &ar->body, &ar->len))) {
                    ar->err = NET_OK;
                    ar->httpStatus = 200;
                } else
                    ar->err = HttpGet(job.host, job.https, job.path, (unsigned char**)&ar->body, &ar->len,
                                      &ar->httpStatus);
                Log("net: api %s%.120s -> %d (HTTP %d, %d bytes, %lu ms)", job.host, job.path, ar->err,
                    ar->httpStatus, ar->len, GetTickCount() - t0);
                if (s_stop || !PostMessage(s_notify, WM_APP_API, 0, (LPARAM)ar))
                    ApiFree(ar);
                continue;
            }
            EnterCriticalSection(&s_cs);
            bool have = s_hasPending;
            MapRequest r = s_pending;
            DWORD seq = s_seq;
            s_hasPending = false;
            LeaveCriticalSection(&s_cs);
            if (!have)
                break;

            MapResult* res = (MapResult*)calloc(1, sizeof(MapResult));
            if (!res)
                break;
            res->req = r;
            res->seq = seq;
            res->look = -1;
            Fetch(r, res);
            if (s_stop || !PostMessage(s_notify, WM_APP_MAP, 0, (LPARAM)res)) {
                ImageFree(&res->img);
                free(res->ov);
                free(res);
            }
        }
    }
    HttpCloseAll();
    WSACleanup();
    return 0;
}

void NetStart(HWND notify)
{
    InitializeCriticalSection(&s_cs);
    s_notify = notify;
    s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_thread = CreateThread(NULL, 0, NetThread, NULL, 0, NULL);
}

DWORD NetRequest(const MapRequest& r)
{
    EnterCriticalSection(&s_cs);
    s_pending = r;
    s_hasPending = true;
    DWORD seq = ++s_seq;
    LeaveCriticalSection(&s_cs);
    SetEvent(s_wake);
    return seq;
}

DWORD NetApi(int kind, const char* host, bool https, const char* path)
{
    EnterCriticalSection(&s_cs);
    int i = 0;
    while (i < s_nApi && (s_api[i].kind != kind || kind == API_REVERSE))
        i++;
    if (i == s_nApi) {
        if (s_nApi < 4)
            s_nApi++;
        else
            i = 3;
    }
    ApiJob& j = s_api[i];
    j.kind = kind;
    j.https = https;
    strncpy(j.host, host, sizeof(j.host) - 1);
    j.host[sizeof(j.host) - 1] = 0;
    strncpy(j.path, path, sizeof(j.path) - 1);
    j.path[sizeof(j.path) - 1] = 0;
    DWORD seq = j.seq = ++s_apiSeq;
    LeaveCriticalSection(&s_cs);
    SetEvent(s_wake);
    return seq;
}

void ApiFree(ApiResult* r)
{
    if (r) {
        free(r->body);
        free(r);
    }
}

void NetStop()
{
    s_stop = true;
    SetEvent(s_wake);
    // The worker may sit in select() for a while; don't block shutdown on it.
    WaitForSingleObject(s_thread, 1000);
}
