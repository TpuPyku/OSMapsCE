#include "yamaps.h"

// ---------------------------------------------------------------- UI layout
// Button positions match the old SystemInformation layout (Main.ini), 800x480 screen.

enum { B_SETTINGS, B_NIGHT, B_EXIT, B_TRF, B_TSRC, B_GPS, B_LEFT, B_RIGHT, B_UP, B_DOWN, B_MINUS, B_PLUS, B_COUNT };

static const int kBtnSize = 50;

struct Button {
    int            x, y;
    const wchar_t* name[3];   // image base names per state (toggle buttons have 2-3)
    Image          img[3][2]; // [state][pressed]
};

static Button s_btn[B_COUNT] = {
    { 610,  28, { L"Bset" } },
    { 675,  28, { L"Bnight_on", L"Bnight_off" } },
    { 740,  28, { L"Bexit" } },
    { 610,  93, { L"Btrf_on", L"Btrf_off" } },
    { 675,  93, { L"Btsrc_ya", L"Btsrc_2g" } },   // traffic provider, only while traffic is on
    { 740,  93, { L"Bgps_on", L"Bgps_off", L"Bgps_none" } },
    { 615, 290, { L"Bleft" } },
    { 735, 290, { L"Bright" } },
    { 675, 230, { L"Bup" } },
    { 675, 350, { L"Bdown" } },
    { 640, 420, { L"Bminus" } },
    { 710, 420, { L"Bplus" } },
};

static const wchar_t* kWeekDays[7] = { L"Вс", L"Пн", L"Вт", L"Ср", L"Чт", L"Пт", L"Сб" };
static const wchar_t* kTrafficTitle[TRF_COUNT] = { L"Яндекс", L"2ГИС" };
static const int      kTrafficSrc[TRF_COUNT] = { SRC_YANDEX, SRC_2GIS };

static const COLORREF kText    = RGB(255, 255, 255);
static const COLORREF kTextDim = RGB(170, 170, 170);
static const COLORREF kTextErr = RGB(255, 120, 120);
static const COLORREF kMapBg   = RGB(228, 226, 222);

enum { TIMER_TICK = 1, TIMER_USER = 2 };
static const UINT kUserDelayMs = 900;    // wait for the user to stop pressing buttons

// GPS button modes (g_cfg.follow)
enum { FOLLOW_OFF, FOLLOW_NORTH, FOLLOW_HEADING };
static const double kCarAhead        = 0.3;  // heading-up: view center this part of map height ahead of the car
static const double kHeadingMinSpeed = 5;    // km/h: below it the course is noise, keep the last heading
static const int    kCanvas          = 768;  // unrotated canvas >= map diagonal (750)
static const int    kMaxKeys         = 32;   // tiles one view can need from one source
static const int    kMaxParentLevels = 6;    // missing tile: show a coarser cached one, up to 6 levels up

// Small ring of map keys: "not on disk" / "no coarser tile" memos,
// so the card is not searched again every second.
struct KeyRing {
    MapRequest k[64];
    int        n, next;
};

static bool RingHas(const KeyRing& r, const MapRequest& k)
{
    for (int i = 0; i < r.n; i++)
        if (SameKey(r.k[i], k))
            return true;
    return false;
}

static void RingAdd(KeyRing& r, const MapRequest& k)
{
    r.k[r.next] = k;
    r.next = (r.next + 1) % 64;
    if (r.n < 64)
        r.n++;
}

// ---------------------------------------------------------------- state

static HWND       s_wnd;
static HWND       s_taskbar;
static HBITMAP    s_back;
static Image      s_backImg;            // s_back as a 16bpp DIB: the night filter edits its pixels
static unsigned short* s_backBits;
static unsigned short* s_nightLut;      // RGB565 -> night color, only while night is on
static int        s_cw, s_ch;
static Image      s_bg;
static HFONT      s_fontBig, s_fontSmall, s_fontTiny;

static bool       s_inflight;
static DWORD      s_inflightSeq, s_inflightTick;
static int        s_lastErr, s_lastHttp;
static DWORD      s_lastErrTick;
static bool       s_limitHit;
static KeyRing    s_missKeys;           // keys not found on disk
static KeyRing    s_noParentKeys;       // keys with no coarser tile on disk either
static bool       s_userPending;        // user is still pressing buttons (TIMER_USER running)
static double     s_heading;            // heading-up: map rotation, degrees
static Image      s_canvas, s_rotated;  // drawing buffers; s_rotated only while heading-up
static unsigned short* s_canvasBits;
static unsigned short* s_rotatedBits;
static DWORD      s_lastSaveTick;
static DWORD      s_lastPaintTick;
static DWORD      s_statTick;           // once a minute: memory and load to the log
static int        s_statPaints;
static DWORD      s_statPaintMax, s_statGpsBytes;
static int        s_statGpsLines, s_statGpsBad;

static int        s_pressed = -1;
static bool       s_pressedInside;
static bool       s_dragging, s_dragMoved;
static POINT      s_dragStart;
static double     s_dragWx, s_dragWy;

static GpsState   s_gps;
static bool       s_gpsRunning;

// Settings screen (GPS port, cache size), drawn over the map
static bool       s_settings;
static int        s_setPort;        // 1..9 -> COMn:
static int        s_setBaud;        // index in kSetBauds
static int        s_setCache;       // index in kSetCacheMb
static int        s_setPressed = -1;
static const DWORD kSetBauds[] = { 0, 4800, 9600, 19200, 38400, 57600, 115200 };   // 0 = leave as is
static const int   kSetBaudCount = sizeof(kSetBauds) / sizeof(kSetBauds[0]);
static const int   kSetCacheMb[] = { 250, 500, 1000, 2000 };
static const int   kSetCacheCount = sizeof(kSetCacheMb) / sizeof(kSetCacheMb[0]);

enum { S_PORT_PREV, S_PORT_NEXT, S_BAUD_PREV, S_BAUD_NEXT, S_CACHE_PREV, S_CACHE_NEXT,
       S_APPLY, S_SCAN, S_CLOSE, S_COUNT };
static const RECT kSetCtl[S_COUNT] = {
    { 180,  75, 230, 125 }, { 410,  75, 460, 125 },
    { 180, 133, 230, 183 }, { 410, 133, 460, 183 },
    { 180, 191, 230, 241 }, { 410, 191, 460, 241 },
    {  22, 251, 192, 296 }, { 212, 251, 382, 296 }, { 402, 251, 572, 296 },
};
static const wchar_t* kSetCaption[S_COUNT] = { L"<", L">", L"<", L">", L"<", L">",
                                               L"Применить", L"Поиск GPS", L"Закрыть" };

// ---------------------------------------------------------------- view & tiles

static void ViewWorld(double* x, double* y)
{
    GeoToWorld(g_cfg.lon, g_cfg.lat, g_cfg.z, x, y);
}

// Map rotation for drawing: heading-up mode only.
static double ViewAngle()
{
    return g_cfg.follow == FOLLOW_HEADING ? s_heading : 0;
}

// North-up: the car is in the center. Heading-up: the car is near the bottom edge,
// the view center is ahead of it along the course.
static void FollowGps()
{
    if (g_cfg.follow == FOLLOW_OFF || !s_gps.valid)
        return;
    if (s_gps.speedKmh > kHeadingMinSpeed)
        s_heading = s_gps.course;
    double x, y;
    GeoToWorld(s_gps.lon, s_gps.lat, g_cfg.z, &x, &y);
    if (g_cfg.follow == FOLLOW_HEADING) {
        double a = s_heading * M_PI / 180.0, ahead = g_cfg.mapH * kCarAhead;
        x += sin(a) * ahead;
        y -= cos(a) * ahead;
    }
    WorldToGeo(x, y, g_cfg.z, &g_cfg.lon, &g_cfg.lat);
}

static int TrafficSource()
{
    return kTrafficSrc[g_cfg.trafficSrc];
}

// Tiles of a source that touch the (rotated) map window, nearest to the view center first
// (heading-up: the center is ahead of the car, so the road ahead comes first).
// OSM and 2GIS: 256 px tiles; Yandex: 600x450 images in Yandex world pixels.
static int NeededKeys(int src, MapRequest* out, int max)
{
    bool ya = src == SRC_YANDEX;
    double tw = ya ? YA_W : TILE, th = ya ? YA_H : TILE;
    double off = ya ? 0 : 0.5;   // tile center = (index + off) * size
    double vx, vy;
    if (ya)
        GeoToWorldYa(g_cfg.lon, g_cfg.lat, g_cfg.z, &vx, &vy);
    else
        ViewWorld(&vx, &vy);
    double a = ViewAngle() * M_PI / 180.0, c = cos(a), s = sin(a);
    double hw = g_cfg.mapW / 2.0, hh = g_cfg.mapH / 2.0;
    // world bounding box of the (rotated) window
    double ex = hw * fabs(c) + hh * fabs(s), ey = hw * fabs(s) + hh * fabs(c);
    double world = 256.0 * pow(2.0, g_cfg.z);

    double dist[kMaxKeys];
    int n = 0;
    int i0 = (int)floor((vx - ex) / tw - off), i1 = (int)ceil((vx + ex) / tw - off);
    int j0 = (int)floor((vy - ey) / th - off), j1 = (int)ceil((vy + ey) / th - off);
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            double tx = (i + off) * tw, ty = (j + off) * th;
            if (tx < 0 || ty < 0 || tx > world || ty > world)
                continue;
            double dx = tx - vx, dy = ty - vy;
            // separating axes: world x/y, then the window's own axes
            if (fabs(dx) >= ex + tw / 2 || fabs(dy) >= ey + th / 2)
                continue;
            if (fabs(dx * c + dy * s) >= hw + tw / 2 * fabs(c) + th / 2 * fabs(s))
                continue;
            if (fabs(-dx * s + dy * c) >= hh + tw / 2 * fabs(s) + th / 2 * fabs(c))
                continue;
            if (n == max)
                continue;
            MapRequest k = { src, g_cfg.z, i, j };
            double d = dx * dx + dy * dy;
            int p = n++;
            for (; p > 0 && dist[p - 1] > d; p--) {
                out[p] = out[p - 1];
                dist[p] = dist[p - 1];
            }
            out[p] = k;
            dist[p] = d;
        }
    }
    return n;
}

// ---------------------------------------------------------------- request budget

static int Today()
{
    if (s_gps.date)
        return s_gps.date;
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (t.wYear < 2024)
        return 0;   // clock was reset, can't trust it
    return t.wYear * 10000 + t.wMonth * 100 + t.wDay;
}

volatile int g_today;

static void CheckDay()
{
    int d = Today();
    if (d > g_cfg.reqDay) {   // never go back: a wrong clock must not reset the counters
        Log("budget: new day %d (yesterday: yandex %d, 2gis %d, osm %d)",
            d, g_cfg.yandexCount, g_cfg.twogisCount, g_cfg.osmCount);
        g_cfg.reqDay = d;
        g_cfg.yandexCount = g_cfg.twogisCount = g_cfg.osmCount = 0;
        ConfigSave();
    }
}

static int* Counter(int src)
{
    return src == SRC_YANDEX ? &g_cfg.yandexCount : src == SRC_2GIS ? &g_cfg.twogisCount : &g_cfg.osmCount;
}

static int Limit(int src)
{
    return src == SRC_YANDEX ? g_cfg.yandexLimit : g_cfg.twogisLimit;
}

// ---------------------------------------------------------------- map requests

enum { REQ_AUTO, REQ_USER, REQ_FORCE };

// A map tile from the disk is refreshed once it is cache_days old (OSM policy: keep at least
// 7 days). Files saved while the date was unknown carry the unit's wrong clock (before
// 2024): refreshed too, and get a real date then. Without today's date nothing expires.
static bool IsExpired(const CacheEntry* e)
{
    if (!g_cfg.cacheDays || !g_today || !e->fileDay || e->fetchTick)
        return false;
    return e->fileDay < 20240101 || DaysBetween(e->fileDay, g_today) >= g_cfg.cacheDays;
}

static bool IsFresh(const CacheEntry* e)
{
    if (!e)
        return false;
    if (e->req.src == SRC_OSM)
        return !IsExpired(e);
    return e->fetchTick && GetTickCount() - e->fetchTick < (DWORD)g_cfg.trafficTtlMin * 60000;
}

// Downloads an image of the view if really needed. One download at a time: the next
// image is requested when the answer comes (OnMapResult).
// Returns true if the image is fresh in the cache or will be downloaded.
//  REQ_AUTO  - timer/GPS: traffic uses its budget minus the reserve, waits after errors;
//  REQ_USER  - user stopped on a view: full budget;
//  REQ_FORCE - tap on the map: refresh traffic even if fresh (not more than once a minute).
static bool Request(int mode, const MapRequest& k, CacheEntry* e)
{
    DWORD now = GetTickCount();
    if (mode == REQ_FORCE && k.src != SRC_OSM) {
        if (e && e->fetchTick && now - e->fetchTick < 60000)
            return true;
    } else if (IsFresh(e)) {
        return true;
    }

    if (s_inflight && now - s_inflightTick > 40000)
        s_inflight = false;   // answer lost
    if (s_inflight)
        return true;
    if (mode == REQ_AUTO && s_lastErr && now - s_lastErrTick < 30000)
        return false;

    CheckDay();
    if (k.src != SRC_OSM) {
        int limit = Limit(k.src) - (mode == REQ_AUTO ? g_cfg.autoReserve : 0);
        if (*Counter(k.src) >= limit) {
            if (!s_limitHit)
                Log("budget: limit reached for source %d (%d/%d), mode %d", k.src, *Counter(k.src), Limit(k.src), mode);
            s_limitHit = true;
            return false;
        }
        s_limitHit = false;
    }

    s_inflightSeq = NetRequest(k);
    s_inflight = true;
    s_inflightTick = now;
    (*Counter(k.src))++;
    if (k.src != SRC_OSM) {
        ConfigSave();   // the traffic counter must survive a sudden power loss
        s_lastSaveTick = now;
    }
    return true;
}

// A missing tile that can't be downloaded now (no network, error): brings the nearest
// coarser cached tile into memory, the mosaic draws it scaled up.
static void LoadParent(const MapRequest& k)
{
    if (RingHas(s_noParentKeys, k))
        return;
    for (int dz = 1; dz <= kMaxParentLevels && k.z - dz >= 0; dz++) {
        MapRequest p = { SRC_OSM, k.z - dz, k.x >> dz, k.y >> dz };
        if (CacheFind(p, false))
            return;
        if (!RingHas(s_missKeys, p)) {
            if (CacheFind(p, true))
                return;
            RingAdd(s_missKeys, p);
        }
    }
    RingAdd(s_noParentKeys, k);
}

// Brings the map tiles of the view into memory (disk or download), then the traffic
// overlay. Disk decodes are limited per call: each costs some 20-50 ms on the unit.
static void LoadFromDisk(const MapRequest* keys, int n, int maxLoads)
{
    for (int i = 0; i < n && maxLoads > 0; i++) {
        if (CacheFind(keys[i], false) || RingHas(s_missKeys, keys[i]))
            continue;
        maxLoads--;
        if (!CacheFind(keys[i], true))
            RingAdd(s_missKeys, keys[i]);
    }
}

static void Ensure(int mode)
{
    MapRequest keys[kMaxKeys];
    int n = NeededKeys(SRC_OSM, keys, kMaxKeys);
    LoadFromDisk(keys, n, 6);
    bool parentLoaded = false;
    for (int i = 0; i < n; i++) {
        CacheEntry* e = CacheFind(keys[i], false);
        if (!Request(mode, keys[i], e) && !e && !s_userPending && !parentLoaded) {
            LoadParent(keys[i]);
            parentLoaded = true;
        }
    }
    if (g_cfg.traffic) {
        n = NeededKeys(TrafficSource(), keys, kMaxKeys);
        for (int i = 0; i < n; i++)
            Request(i == 0 ? mode : (mode == REQ_FORCE ? REQ_AUTO : mode), keys[i], CacheFind(keys[i], false));
    }
}

static void OnMapResult(MapResult* res)
{
    if (res->seq == s_inflightSeq)
        s_inflight = false;
    if (res->err == NET_OK) {
        CacheAdd(res, GetTickCount());
        s_noParentKeys.n = 0;   // new tile on disk
        s_lastErr = 0;
    } else {
        s_lastErr = res->err;
        s_lastHttp = res->httpStatus;
        s_lastErrTick = GetTickCount();
    }
    ImageFree(&res->img);
    free(res->ov);
    free(res);
    // next image of the view; after an error this waits and shows coarser tiles
    Ensure(REQ_AUTO);
    InvalidateRect(s_wnd, NULL, FALSE);
}

// Called after every user change: show what the disk has now, download later.
static void UserChanged()
{
    MapRequest keys[kMaxKeys];
    LoadFromDisk(keys, NeededKeys(SRC_OSM, keys, kMaxKeys), 6);
    ConfigSave();
    s_lastSaveTick = GetTickCount();
    SetTimer(s_wnd, TIMER_USER, kUserDelayMs, NULL);
    s_userPending = true;
    InvalidateRect(s_wnd, NULL, FALSE);
}

// ---------------------------------------------------------------- GPS settings

static void SettingsLoad()
{
    s_setPort = 6;
    const wchar_t* p = g_cfg.gpsPort;
    if (p[0] == L'C' && p[1] == L'O' && p[2] == L'M' && p[3] >= L'1' && p[3] <= L'9')
        s_setPort = p[3] - L'0';
    s_setBaud = kSetBaudCount - 1;
    for (int i = 0; i < kSetBaudCount; i++)
        if (kSetBauds[i] == (DWORD)g_cfg.gpsBaud)
            s_setBaud = i;
    s_setCache = 1;
    for (int i = 0; i < kSetCacheCount; i++)
        if (kSetCacheMb[i] == g_cfg.cacheMb)
            s_setCache = i;
}

static void EnsureGpsRunning()
{
    if (s_gpsRunning)
        return;
    g_cfg.gpsEnabled = 1;
    GpsStart(s_wnd);
    s_gpsRunning = true;
}

static void OnSettingsCtl(int id)
{
    switch (id) {
    case S_PORT_PREV: s_setPort = s_setPort > 1 ? s_setPort - 1 : 9; break;
    case S_PORT_NEXT: s_setPort = s_setPort < 9 ? s_setPort + 1 : 1; break;
    case S_BAUD_PREV: s_setBaud = (s_setBaud + kSetBaudCount - 1) % kSetBaudCount; break;
    case S_BAUD_NEXT: s_setBaud = (s_setBaud + 1) % kSetBaudCount; break;
    case S_CACHE_PREV: if (s_setCache > 0) s_setCache--; break;
    case S_CACHE_NEXT: if (s_setCache < kSetCacheCount - 1) s_setCache++; break;
    case S_APPLY: {
        wchar_t port[16];
        _snwprintf(port, 16, L"COM%d:", s_setPort);
        if (wcscmp(port, g_cfg.gpsPort) != 0 || (DWORD)g_cfg.gpsBaud != kSetBauds[s_setBaud] || !s_gpsRunning) {
            wcscpy(g_cfg.gpsPort, port);
            g_cfg.gpsBaud = kSetBauds[s_setBaud];
            Log("gps: settings %S %d", g_cfg.gpsPort, g_cfg.gpsBaud);
            EnsureGpsRunning();
            GpsConfigure(g_cfg.gpsPort, g_cfg.gpsBaud);
        }
        g_cfg.cacheMb = kSetCacheMb[s_setCache];
        if (g_cfg.cacheKb < 0 || g_cfg.cacheKb > (LONG)g_cfg.cacheMb * 1024)
            CachePrune();
        ConfigSave();
        break;
    }
    case S_SCAN:
        EnsureGpsRunning();
        GpsScan();
        break;
    case S_CLOSE:
        s_settings = false;
        break;
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

// The GPS thread finished a port search.
static void OnGpsScanDone()
{
    if (s_gps.found != 1)
        return;
    wcscpy(g_cfg.gpsPort, s_gps.port);
    g_cfg.gpsBaud = s_gps.baud;
    ConfigSave();
    SettingsLoad();
}

static int HitSettings(int x, int y)
{
    for (int i = 0; i < S_COUNT; i++)
        if (x >= kSetCtl[i].left && x < kSetCtl[i].right && y >= kSetCtl[i].top && y < kSetCtl[i].bottom)
            return i;
    return -1;
}

// ---------------------------------------------------------------- actions

static void PanBy(double dx, double dy)
{
    double x, y;
    ViewWorld(&x, &y);
    WorldToGeo(x + dx, y + dy, g_cfg.z, &g_cfg.lon, &g_cfg.lat);
    g_cfg.follow = FOLLOW_OFF;
}

static void Zoom(int dz)
{
    int z = g_cfg.z + dz;
    if (z < 2 || z > 19)
        return;
    double x, y;
    ViewWorld(&x, &y);
    double f = pow(2.0, dz);
    g_cfg.z = z;
    WorldToGeo(x * f, y * f, z, &g_cfg.lon, &g_cfg.lat);
    FollowGps();
}

// GPS button: off -> north-up (car in the center) -> heading-up (car at the bottom) -> off
static void NextFollowMode()
{
    g_cfg.follow = (g_cfg.follow + 1) % 3;
    FollowGps();
}

static void OnButton(int b)
{
    switch (b) {
    case B_SETTINGS:
        s_settings = !s_settings;
        if (s_settings)
            SettingsLoad();
        InvalidateRect(s_wnd, NULL, FALSE);
        return;
    case B_NIGHT:
        g_cfg.night = !g_cfg.night;
        if (!g_cfg.night) {
            free(s_nightLut);
            s_nightLut = NULL;
        }
        ConfigSave();
        InvalidateRect(s_wnd, NULL, FALSE);
        return;
    case B_EXIT:   DestroyWindow(s_wnd); return;
    case B_TRF:    g_cfg.traffic = !g_cfg.traffic; break;
    case B_TSRC:   g_cfg.trafficSrc = (g_cfg.trafficSrc + 1) % TRF_COUNT; s_limitHit = false; break;
    case B_GPS:    NextFollowMode(); break;
    case B_LEFT:   PanBy(-g_cfg.mapW / 3.0, 0); break;
    case B_RIGHT:  PanBy(g_cfg.mapW / 3.0, 0); break;
    case B_UP:     PanBy(0, -g_cfg.mapH / 3.0); break;
    case B_DOWN:   PanBy(0, g_cfg.mapH / 3.0); break;
    case B_MINUS:  Zoom(-1); break;
    case B_PLUS:   Zoom(1); break;
    }
    UserChanged();
}

static int ButtonState(int b)
{
    if (b == B_TRF)  return g_cfg.traffic ? 0 : 1;
    if (b == B_NIGHT) return g_cfg.night ? 0 : 1;
    if (b == B_TSRC) return g_cfg.trafficSrc;
    if (b == B_GPS)  return g_cfg.follow == FOLLOW_HEADING ? 0 : (g_cfg.follow == FOLLOW_NORTH ? 1 : 2);
    return 0;
}

// The traffic provider button is shown only while traffic is on.
static bool ButtonVisible(int b)
{
    return b != B_TSRC || g_cfg.traffic;
}

static int HitButton(int x, int y)
{
    for (int i = 0; i < B_COUNT; i++)
        if (ButtonVisible(i) &&
            x >= s_btn[i].x && x < s_btn[i].x + kBtnSize && y >= s_btn[i].y && y < s_btn[i].y + kBtnSize)
            return i;
    return -1;
}

static bool InMap(int x, int y)
{
    return x >= g_cfg.mapX && x < g_cfg.mapX + g_cfg.mapW && y >= g_cfg.mapY && y < g_cfg.mapY + g_cfg.mapH;
}

// ---------------------------------------------------------------- drawing

static void LoadImages()
{
    wchar_t path[MAX_PATH];
    PathInDir(path, L"Images\\MapGL.png");
    if (!ImageFromFile(path, &s_bg))
        Log("ui: no background %S", path);

    for (int i = 0; i < B_COUNT; i++) {
        for (int state = 0; state < 3; state++) {
            const wchar_t* name = s_btn[i].name[state];
            if (!name)
                continue;
            for (int p = 0; p < 2; p++) {
                wchar_t file[64];
                _snwprintf(file, 64, L"Images\\%s_%c.png", name, p ? L'P' : L'N');
                file[63] = 0;
                PathInDir(path, file);
                if (!ImageFromFile(path, &s_btn[i].img[state][p]))
                    Log("ui: no image %S", path);
            }
        }
    }
}

static void Blit(HDC dc, const Image* im, int x, int y)
{
    HDC src = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(src, im->bmp);
    BitBlt(dc, x, y, im->w, im->h, src, 0, 0, SRCCOPY);
    SelectObject(src, old);
    DeleteDC(src);
}

static void Text(HDC dc, HFONT font, COLORREF color, int l, int t, int r, int b, UINT align, const wchar_t* s)
{
    RECT rc = { l, t, r, b };
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawText(dc, s, -1, &rc, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

// Car marker. (cx, cy) - screen point of the view center (vx, vy); angle - map rotation.
static void DrawGpsMarker(HDC dc, double cx, double cy, double vx, double vy, double angle)
{
    if (!s_gps.fixTick)
        return;
    double gx, gy;
    GeoToWorld(s_gps.lon, s_gps.lat, g_cfg.z, &gx, &gy);
    double ra = angle * M_PI / 180.0, rc = cos(ra), rs = sin(ra);
    double dx = gx - vx, dy = gy - vy;
    int px = (int)floor(cx + dx * rc + dy * rs + 0.5);
    int py = (int)floor(cy - dx * rs + dy * rc + 0.5);

    bool fresh = s_gps.valid && GetTickCount() - s_gps.fixTick < 5000;
    HBRUSH brush = CreateSolidBrush(fresh ? RGB(0x20, 0x60, 0xE0) : RGB(140, 140, 140));
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);

    if (s_gps.speedKmh > 3 || g_cfg.follow == FOLLOW_HEADING) {
        // arrow pointing along the course (0 = north, clockwise), relative to the map
        static const int shape[4][2] = { { 0, -16 }, { 11, 12 }, { 0, 6 }, { -11, 12 } };
        double course = g_cfg.follow == FOLLOW_HEADING ? s_heading : s_gps.course;
        double a = (course - angle) * M_PI / 180.0, c = cos(a), s = sin(a);
        POINT pts[4];
        for (int i = 0; i < 4; i++) {
            pts[i].x = px + (int)floor(shape[i][0] * c - shape[i][1] * s + 0.5);
            pts[i].y = py + (int)floor(shape[i][0] * s + shape[i][1] * c + 0.5);
        }
        Polygon(dc, pts, 4);
    } else {
        Ellipse(dc, px - 9, py - 9, px + 9, py + 9);
    }

    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(brush);
}

static int Round(double v)
{
    return (int)floor(v + 0.5);
}

// Base map on the canvas (view center in its middle): OSM tiles in memory, other zoom
// levels scaled, the farthest levels first so the view's own zoom ends up on top.
static int DrawBase(HDC dc, double vx, double vy)
{
    // every tile of the view is here: other levels would be hidden under them, skip them
    MapRequest keys[kMaxKeys];
    int nk = NeededKeys(SRC_OSM, keys, kMaxKeys);
    bool full = true;
    for (int i = 0; i < nk && full; i++)
        full = CacheFind(keys[i], false) != NULL;
    int maxDz = full ? 0 : kMaxParentLevels;

    CacheEntry* list[CACHE_MEM];
    int n = CacheList(list, CACHE_MEM), m = 0;
    for (int i = 0; i < n; i++)
        if (list[i]->req.src == SRC_OSM && abs(list[i]->req.z - g_cfg.z) <= maxDz)
            list[m++] = list[i];
    for (int i = 1; i < m; i++)
        for (int j = i; j > 0 && abs(list[j]->req.z - g_cfg.z) > abs(list[j - 1]->req.z - g_cfg.z); j--) {
            CacheEntry* t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }

    const double C = kCanvas, half = kCanvas / 2.0;
    HDC src = CreateCompatibleDC(dc);
    int drawn = 0;
    for (int i = 0; i < m; i++) {
        const CacheEntry* e = list[i];
        double f = pow(2.0, g_cfg.z - e->req.z);
        double left = half + e->req.x * TILE * f - vx, top = half + e->req.y * TILE * f - vy;
        double size = TILE * f;
        if (left >= C || top >= C || left + size <= 0 || top + size <= 0)
            continue;
        HGDIOBJ old = SelectObject(src, e->img.bmp);
        if (f == 1) {
            BitBlt(dc, Round(left), Round(top), TILE, TILE, src, 0, 0, SRCCOPY);
        } else {
            // only the part of the tile that lands on the canvas: a coarse tile scaled
            // 2^6 times would otherwise be a 16000 px blit
            int sx0 = (int)floor(((left < 0 ? 0 : left) - left) / f);
            int sy0 = (int)floor(((top < 0 ? 0 : top) - top) / f);
            int sx1 = (int)ceil(((left + size > C ? C : left + size) - left) / f);
            int sy1 = (int)ceil(((top + size > C ? C : top + size) - top) / f);
            if (sx1 > TILE) sx1 = TILE;
            if (sy1 > TILE) sy1 = TILE;
            int dx0 = Round(left + sx0 * f), dy0 = Round(top + sy0 * f);
            StretchBlt(dc, dx0, dy0, Round(left + sx1 * f) - dx0, Round(top + sy1 * f) - dy0,
                       src, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
        }
        SelectObject(src, old);
        drawn++;
    }
    DeleteDC(src);
    return drawn;
}

// Blends a transparent overlay into the canvas (RGB565), 1 canvas px per overlay px
// horizontally. (left, top) - canvas position of its top-left corner; sy - canvas px per
// overlay row.
static void BlendOverlay(const Overlay* ov, double left, double top, double sy)
{
    const int C = kCanvas;   // 768 * 2 bytes: rows need no padding
    int ix = (int)ceil(left);
    int y0 = (int)ceil(top), y1 = (int)floor(top + ov->h * sy);
    if (y0 < 0) y0 = 0;
    if (y1 > C) y1 = C;
    for (int y = y0; y < y1; y++) {
        int oy = (int)((y - top) / sy);
        if (oy < 0 || oy >= ov->h)
            continue;
        const unsigned char* p = (const unsigned char*)ov + ov->rowOfs[oy];
        int runs = *(const unsigned short*)p;
        p += 4;
        unsigned short* drow = s_canvasBits + (C - 1 - y) * C;
        int x = ix;
        for (int r = 0; r < runs; r++) {
            const unsigned short* rh = (const unsigned short*)p;
            x += rh[0];
            int count = rh[1];
            const unsigned char* s = p + 4;
            p += 4 + count * 4;
            for (int i = 0; i < count; i++, x++, s += 4) {
                if (x < 0 || x >= C)
                    continue;
                int a = s[3];
                unsigned short d = drow[x];
                int dr = (d >> 8) & 0xF8, dg = (d >> 3) & 0xFC, db = (d << 3) & 0xF8;
                dr += (s[0] - dr) * a / 255;
                dg += (s[1] - dg) * a / 255;
                db += (s[2] - db) * a / 255;
                drow[x] = RGB565(dr, dg, db);
            }
        }
    }
}

// Traffic of the current provider at the view's zoom, stale ones too (better than nothing).
// 2GIS tiles share the OSM grid. Yandex images are in elliptical Mercator: their
// position comes from geographic coordinates, the vertical scale differs slightly.
static void DrawOverlays(double vx, double vy)
{
    int src = TrafficSource();
    const double half = kCanvas / 2.0;
    CacheEntry* list[CACHE_MEM];
    int n = CacheList(list, CACHE_MEM);
    for (int i = 0; i < n; i++) {
        const CacheEntry* e = list[i];
        if (e->req.src != src || e->req.z != g_cfg.z || !e->ov)
            continue;
        if (src == SRC_2GIS) {   // 256 px tiles on the OSM grid
            BlendOverlay(e->ov, half + e->req.x * TILE - vx, half + e->req.y * TILE - vy, 1);
        } else {
            double lon, lat, cx, cy, top, bottom, unused;
            double yx = (double)e->req.x * YA_W, yy = (double)e->req.y * YA_H;
            WorldToGeoYa(yx, yy, g_cfg.z, &lon, &lat);
            GeoToWorld(lon, lat, g_cfg.z, &cx, &cy);
            WorldToGeoYa(yx, yy - YA_H / 2.0, g_cfg.z, &lon, &lat);
            GeoToWorld(lon, lat, g_cfg.z, &unused, &top);
            WorldToGeoYa(yx, yy + YA_H / 2.0, g_cfg.z, &lon, &lat);
            GeoToWorld(lon, lat, g_cfg.z, &unused, &bottom);
            BlendOverlay(e->ov, half + cx - YA_W / 2.0 - vx, half + top - vy, (bottom - top) / e->ov->h);
        }
    }
}

// Turns the north-up canvas (view center in its middle) by -angle into the map-sized
// buffer. GDI on CE can't rotate, so it is done per pixel in 16.16 fixed point.
static void RotateCanvas(double angle)
{
    const int C = kCanvas, W = s_rotated.w, H = s_rotated.h;
    const int cstride = ((C * 2 + 3) & ~3) / 2, ostride = ((W * 2 + 3) & ~3) / 2;
    const unsigned short bg = RGB565(GetRValue(kMapBg), GetGValue(kMapBg), GetBValue(kMapBg));
    double a = angle * M_PI / 180.0, ca = cos(a), sa = sin(a);
    long dx = (long)(ca * 65536), dy = (long)(sa * 65536);
    for (int y = 0; y < H; y++) {
        // screen offset s -> canvas offset w = Rot(angle) s
        double sx = -W / 2.0, sy = y - H / 2.0;
        long wx = (long)((sx * ca - sy * sa + C / 2.0) * 65536);
        long wy = (long)((sx * sa + sy * ca + C / 2.0) * 65536);
        unsigned short* d = s_rotatedBits + (H - 1 - y) * ostride;
        for (int x = 0; x < W; x++, wx += dx, wy += dy) {
            int px = (int)(wx >> 16), py = (int)(wy >> 16);
            d[x] = (px < 0 || py < 0 || px >= C || py >= C) ? bg : s_canvasBits[(C - 1 - py) * cstride + px];
        }
    }
}

// OSM licence: attribution must be visible on the map, not hidden behind controls.
static void DrawAttribution(HDC dc, const RECT& mr)
{
    const wchar_t* s = !g_cfg.traffic ? L"© OpenStreetMap contributors"
                     : g_cfg.trafficSrc == TRF_2GIS ? L"© OpenStreetMap contributors, пробки © 2ГИС"
                     : L"© OpenStreetMap contributors, пробки © Яндекс";
    SelectObject(dc, s_fontTiny);
    RECT rc = { 0, 0, 0, 0 };
    DrawText(dc, s, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    RECT box = { mr.right - rc.right - 8, mr.bottom - rc.bottom - 4, mr.right, mr.bottom };
    HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &box, b);
    DeleteObject(b);
    Text(dc, s_fontTiny, RGB(60, 60, 60), box.left + 4, box.top, box.right - 4, box.bottom, DT_RIGHT, s);
}

// Night map: lightness inverted with hue and saturation kept (light background turns dark,
// traffic stays green/yellow/red), then dimmed. One table lookup per pixel of the frame.
static const int kNightDimPct = 80;

static void NightMap(const RECT& r)
{
    if (!s_backBits)
        return;
    if (!s_nightLut) {
        s_nightLut = (unsigned short*)malloc(65536 * sizeof(unsigned short));
        if (!s_nightLut)
            return;
        for (int v = 0; v < 65536; v++) {
            int c[3] = { ((v >> 8) & 0xF8) | (v >> 13), ((v >> 3) & 0xFC) | ((v >> 9) & 3),
                         ((v << 3) & 0xF8) | ((v >> 2) & 7) };
            int mx = c[0], mn = c[0];
            for (int i = 1; i < 3; i++) {
                if (c[i] > mx) mx = c[i];
                if (c[i] < mn) mn = c[i];
            }
            int shift = 255 - mx - mn;   // HSL lightness L -> 255 - L
            for (int i = 0; i < 3; i++) {
                int t = c[i] + shift;
                t = t < 0 ? 0 : t > 255 ? 255 : t;
                c[i] = t * kNightDimPct / 100;
            }
            s_nightLut[v] = RGB565(c[0], c[1], c[2]);
        }
    }
#ifndef UNDER_CE
    GdiFlush();   // desktop GDI may batch drawing into the DIB
#endif
    int stride = ((s_cw * 2 + 3) & ~3) / 2;
    for (int y = r.top; y < r.bottom; y++) {
        unsigned short* p = s_backBits + (s_ch - 1 - y) * stride;
        for (int x = r.left; x < r.right; x++)
            p[x] = s_nightLut[p[x]];
    }
}

static void DrawMap(HDC dc)
{
    RECT mr = { g_cfg.mapX, g_cfg.mapY, g_cfg.mapX + g_cfg.mapW, g_cfg.mapY + g_cfg.mapH };
    HRGN rgn = CreateRectRgn(mr.left, mr.top, mr.right, mr.bottom);
    SelectClipRgn(dc, rgn);

    double vx, vy;
    ViewWorld(&vx, &vy);
    double cx = g_cfg.mapX + g_cfg.mapW / 2.0;
    double cy = g_cfg.mapY + g_cfg.mapH / 2.0;
    double angle = ViewAngle();

    // the rotation buffer (~540 KB) is kept only while heading-up is on
    bool rotate = g_cfg.follow == FOLLOW_HEADING;
    if (rotate && !s_rotatedBits) {
        s_rotatedBits = ImageCreate(g_cfg.mapW, g_cfg.mapH, &s_rotated);
    } else if (!rotate && s_rotatedBits) {
        ImageFree(&s_rotated);
        s_rotatedBits = NULL;
    }

    int drawn = 0;
    if (s_canvasBits && (!rotate || s_rotatedBits)) {
        HDC cdc = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(cdc, s_canvas.bmp);
        RECT all = { 0, 0, kCanvas, kCanvas };
        HBRUSH bg = CreateSolidBrush(kMapBg);
        FillRect(cdc, &all, bg);
        DeleteObject(bg);
        drawn = DrawBase(cdc, vx, vy);
        SelectObject(cdc, old);
        DeleteDC(cdc);
#ifndef UNDER_CE
        GdiFlush();   // desktop GDI may batch drawing into the DIB
#endif
        if (g_cfg.traffic)
            DrawOverlays(vx, vy);
        if (rotate) {
            RotateCanvas(angle);
            Blit(dc, &s_rotated, mr.left, mr.top);
        } else {   // north-up: the middle of the canvas as it is
            HDC src = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(src, s_canvas.bmp);
            BitBlt(dc, mr.left, mr.top, g_cfg.mapW, g_cfg.mapH, src,
                   (kCanvas - g_cfg.mapW) / 2, (kCanvas - g_cfg.mapH) / 2, SRCCOPY);
            SelectObject(src, old);
            DeleteDC(src);
        }
    }
    if (!drawn)
        Text(dc, s_fontSmall, RGB(90, 90, 90), mr.left, mr.top, mr.right, mr.bottom, DT_CENTER,
             s_lastErr ? NetErrorText(s_lastErr) : L"Загрузка карты...");

    DrawAttribution(dc, mr);
    if (g_cfg.night)
        NightMap(mr);
    DrawGpsMarker(dc, cx, cy, vx, vy, angle);

    SelectClipRgn(dc, NULL);
    DeleteObject(rgn);
}

// White rounded button like the image buttons.
static void DrawCtl(HDC dc, const RECT& r, bool pressed, HFONT font, const wchar_t* caption)
{
    HBRUSH brush = CreateSolidBrush(pressed ? RGB(190, 190, 190) : RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 14, 14);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(brush);
    Text(dc, font, RGB(90, 90, 90), r.left, r.top, r.right, r.bottom, DT_CENTER, caption);
}

static void DrawSettings(HDC dc)
{
    RECT mr = { g_cfg.mapX, g_cfg.mapY, g_cfg.mapX + g_cfg.mapW, g_cfg.mapY + g_cfg.mapH };
    HBRUSH bg = CreateSolidBrush(RGB(32, 36, 44));
    FillRect(dc, &mr, bg);
    DeleteObject(bg);

    wchar_t s[128];
    Text(dc, s_fontBig, kText, 22, 34, 580, 70, DT_LEFT, L"Настройки");
    Text(dc, s_fontSmall, kText, 22, 75, 170, 125, DT_LEFT, L"Порт GPS");
    _snwprintf(s, 128, L"COM%d:", s_setPort);
    Text(dc, s_fontBig, kText, 240, 75, 400, 125, DT_CENTER, s);
    Text(dc, s_fontSmall, kText, 22, 133, 170, 183, DT_LEFT, L"Скорость");
    if (kSetBauds[s_setBaud])
        _snwprintf(s, 128, L"%lu", kSetBauds[s_setBaud]);
    else
        wcscpy(s, L"не менять");
    Text(dc, s_fontBig, kText, 240, 133, 400, 183, DT_CENTER, s);
    Text(dc, s_fontSmall, kText, 22, 191, 170, 241, DT_LEFT, L"Кэш карт");
    int mb = kSetCacheMb[s_setCache];
    if (mb >= 1000)
        _snwprintf(s, 128, L"%d ГБ", mb / 1000);
    else
        _snwprintf(s, 128, L"%d МБ", mb);
    Text(dc, s_fontBig, kText, 240, 191, 400, 241, DT_CENTER, s);
    if (g_cfg.cacheKb >= 0)
        _snwprintf(s, 128, L"занято %ld МБ", g_cfg.cacheKb / 1024);
    else
        wcscpy(s, L"занято ?");
    Text(dc, s_fontSmall, kTextDim, 470, 191, 598, 241, DT_LEFT, s);

    for (int i = 0; i < S_COUNT; i++)
        DrawCtl(dc, kSetCtl[i], s_setPressed == i, i < S_APPLY ? s_fontBig : s_fontSmall, kSetCaption[i]);

    // live state of the receiver
    COLORREF c = kText;
    if (!s_gpsRunning) {
        wcscpy(s, L"GPS выключен (gps=0), нажмите Применить");
        c = kTextDim;
    } else {
        switch (s_gps.status) {
        case GPS_OPEN:     _snwprintf(s, 128, L"Порт %s %lu открыт", s_gps.port, s_gps.baud); break;
        case GPS_BUSY:     _snwprintf(s, 128, L"Порт %s занят другой программой", s_gps.port); c = kTextErr; break;
        case GPS_SCANNING: _snwprintf(s, 128, L"Поиск: %s %lu ...", s_gps.port, s_gps.baud); break;
        default:           _snwprintf(s, 128, L"Порт %s не открывается", s_gps.port); c = kTextErr; break;
        }
    }
    s[127] = 0;
    Text(dc, s_fontSmall, c, 22, 306, 580, 330, DT_LEFT, s);

    _snwprintf(s, 128, L"NMEA: %d строк, с ошибкой %d", s_gps.lines, s_gps.badLines);
    Text(dc, s_fontSmall, s_gps.lines ? kText : kTextDim, 22, 330, 580, 354, DT_LEFT, s);

    if (s_gps.valid)
        _snwprintf(s, 128, L"Координаты: %.5f, %.5f  (%d спутн.)", s_gps.lat, s_gps.lon, s_gps.sats);
    else
        _snwprintf(s, 128, L"Координат пока нет (%d спутн.)", s_gps.sats);
    s[127] = 0;
    Text(dc, s_fontSmall, s_gps.valid ? kText : kTextDim, 22, 354, 580, 378, DT_LEFT, s);

    MultiByteToWideChar(CP_ACP, 0, s_gps.last, -1, s, 128);
    s[127] = 0;
    Text(dc, s_fontSmall, kTextDim, 22, 378, 580, 402, DT_LEFT, s[0] ? s : L"-");

    if (s_gps.found == 1) {
        _snwprintf(s, 128, L"Найден GPS: %s %lu, настройки сохранены", s_gps.port, s_gps.baud);
        Text(dc, s_fontSmall, RGB(120, 220, 120), 22, 404, 580, 428, DT_LEFT, s);
    } else if (s_gps.found == -1) {
        Text(dc, s_fontSmall, kTextErr, 22, 404, 580, 428, DT_LEFT, L"GPS не найден на COM1..COM9");
    } else {
        Text(dc, s_fontSmall, kTextDim, 22, 404, 580, 452, DT_LEFT | DT_WORDBREAK,
             L"Нет строк - не тот порт. Строки с ошибкой - не та скорость.");
    }
}

static void DrawStatus(HDC dc)
{
    wchar_t s[96];
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 96, L"%s %02d.%02d.%04d  %02d:%02d:%02d", kWeekDays[t.wDayOfWeek % 7],
               t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute, t.wSecond);
    Text(dc, s_fontBig, kText, 10, 0, 330, 28, DT_LEFT, s);

    COLORREF gc = kText;
    if (!s_gpsRunning) {
        wcscpy(s, L"GPS выкл.");
        gc = kTextDim;
    } else if (s_gps.status == GPS_SCANNING) {
        wcscpy(s, L"GPS: поиск порта...");
    } else if (s_gps.status == GPS_BUSY) {
        _snwprintf(s, 96, L"GPS: порт %s занят", s_gps.port);
        gc = kTextErr;
    } else if (s_gps.status != GPS_OPEN) {
        _snwprintf(s, 96, L"GPS: нет порта %s", s_gps.port);
        gc = kTextErr;
    } else if (!s_gps.lines) {
        wcscpy(s, L"GPS: нет данных");
        gc = kTextErr;
    } else if (!s_gps.valid) {
        _snwprintf(s, 96, L"GPS: поиск (%d спутн.)", s_gps.sats);
        gc = kTextDim;
    } else {
        _snwprintf(s, 96, L"GPS: %d спутн., %d км/ч", s_gps.sats, (int)(s_gps.speedKmh + 0.5));
    }
    s[95] = 0;
    Text(dc, s_fontSmall, gc, 330, 0, 560, 28, DT_LEFT, s);

    // network state, or the age of the traffic nearest to the car
    COLORREF uc = kText;
    if (s_limitHit && g_cfg.traffic) {
        wcscpy(s, L"Лимит запросов!");
        uc = kTextErr;
    } else if (s_lastErr && GetTickCount() - s_lastErrTick < 60000) {
        if (s_lastErr == NET_HTTP)
            _snwprintf(s, 96, L"Ошибка HTTP %d", s_lastHttp);
        else
            _snwprintf(s, 96, L"Ошибка: %s", NetErrorText(s_lastErr));
        uc = kTextErr;
    } else if (s_inflight) {
        wcscpy(s, L"Загрузка...");
    } else if (!g_cfg.traffic) {
        wcscpy(s, L"Пробки выкл.");
        uc = kTextDim;
    } else {
        MapRequest keys[kMaxKeys];
        CacheEntry* e = NeededKeys(TrafficSource(), keys, kMaxKeys) ? CacheFind(keys[0], false) : NULL;
        if (!e) {
            wcscpy(s, L"Пробки: нет данных");
            uc = kTextDim;
        } else {
            _snwprintf(s, 96, L"Пробки: %d мин назад", (int)((GetTickCount() - e->fetchTick) / 60000));
            uc = IsFresh(e) ? kText : kTextDim;
        }
    }
    s[95] = 0;
    Text(dc, s_fontSmall, uc, 560, 0, 795, 28, DT_RIGHT, s);

    // info block in the right panel
    _snwprintf(s, 96, L"Масштаб: %d", g_cfg.z);
    Text(dc, s_fontSmall, kText, 612, 155, 795, 175, DT_LEFT, s);
    if (g_cfg.traffic) {
        int src = TrafficSource();
        _snwprintf(s, 96, L"Пробки: %s", kTrafficTitle[g_cfg.trafficSrc]);
        Text(dc, s_fontSmall, kText, 612, 175, 795, 195, DT_LEFT, s);
        _snwprintf(s, 96, L"Запросов: %d из %d", *Counter(src), Limit(src));
        Text(dc, s_fontSmall, *Counter(src) >= Limit(src) - g_cfg.autoReserve ? kTextErr : kTextDim,
             612, 195, 795, 215, DT_LEFT, s);
    } else {
        Text(dc, s_fontSmall, kTextDim, 612, 175, 795, 195, DT_LEFT, L"Пробки выкл.");
        _snwprintf(s, 96, L"Плиток OSM: %d", g_cfg.osmCount);
        Text(dc, s_fontSmall, kTextDim, 612, 195, 795, 215, DT_LEFT, s);
    }
}

static void DrawButtons(HDC dc)
{
    for (int i = 0; i < B_COUNT; i++) {
        if (!ButtonVisible(i))
            continue;
        // the settings button stays highlighted while its screen is open
        int pressed = ((s_pressed == i && s_pressedInside) || (i == B_SETTINGS && s_settings)) ? 1 : 0;
        const Image* im = &s_btn[i].img[ButtonState(i)][pressed];
        if (im->bmp) {
            Blit(dc, im, s_btn[i].x, s_btn[i].y);
        } else {
            RECT r = { s_btn[i].x, s_btn[i].y, s_btn[i].x + kBtnSize, s_btn[i].y + kBtnSize };
            FillRect(dc, &r, (HBRUSH)GetStockObject(pressed ? GRAY_BRUSH : DKGRAY_BRUSH));
            Text(dc, s_fontSmall, kText, r.left, r.top, r.right, r.bottom, DT_CENTER, s_btn[i].name[0] + 1);
        }
    }
}


static void Paint(HDC hdc)
{
    HDC dc = CreateCompatibleDC(hdc);
    HGDIOBJ oldBmp = SelectObject(dc, s_back);
    HGDIOBJ oldFont = SelectObject(dc, s_fontSmall);
    SetBkMode(dc, TRANSPARENT);

    if (s_bg.bmp) {
        Blit(dc, &s_bg, 0, 0);
    } else {
        RECT r = { 0, 0, s_cw, s_ch };
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    DrawMap(dc);
    if (s_settings)
        DrawSettings(dc);
    DrawButtons(dc);
    DrawStatus(dc);

    BitBlt(hdc, 0, 0, s_cw, s_ch, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
}

static HFONT MakeFont(int height, int weight)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, L"Tahoma");
    return CreateFontIndirect(&lf);
}

// ---------------------------------------------------------------- window

// Free memory of the unit and of this process, what was drawn and read since the last line.
// A shrinking "proc" between lines with the same work is a leak.
static void LogStat()
{
    MEMORYSTATUS ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    Log("stat: ram %lu%% used, %lu KB free, proc %lu KB free; cache %d KB; paints %d (max %lu ms); "
        "gps %lu bytes, %d lines, %d bad",
        ms.dwMemoryLoad, ms.dwAvailPhys / 1024, ms.dwAvailVirtual / 1024, CacheBytes() / 1024,
        s_statPaints, s_statPaintMax,
        s_gps.bytes - s_statGpsBytes, s_gps.lines - s_statGpsLines, s_gps.badLines - s_statGpsBad);
    s_statTick = GetTickCount();
    s_statPaints = 0;
    s_statPaintMax = 0;
    s_statGpsBytes = s_gps.bytes;
    s_statGpsLines = s_gps.lines;
    s_statGpsBad = s_gps.badLines;
}

static void OnCreate(HWND hwnd)
{
    s_wnd = hwnd;
    RECT rc;
    GetClientRect(hwnd, &rc);
    s_cw = rc.right;
    s_ch = rc.bottom;
    s_backBits = ImageCreate(s_cw, s_ch, &s_backImg);
    s_back = s_backImg.bmp;
    if (!s_back) {   // still works, just without the night filter
        HDC hdc = GetDC(hwnd);
        s_back = CreateCompatibleBitmap(hdc, s_cw, s_ch);
        ReleaseDC(hwnd, hdc);
    }

    s_fontBig = MakeFont(22, FW_SEMIBOLD);
    s_fontSmall = MakeFont(15, FW_NORMAL);
    s_fontTiny = MakeFont(12, FW_NORMAL);
    LoadImages();
    s_canvasBits = ImageCreate(kCanvas, kCanvas, &s_canvas);
    CacheInit();

#ifdef UNDER_CE
    s_taskbar = FindWindow(L"HHTaskBar", NULL);
    if (s_taskbar && g_cfg.hideTaskbar)
        ShowWindow(s_taskbar, SW_HIDE);
#endif

    NetStart(hwnd);
    if (g_cfg.gpsEnabled)
        EnsureGpsRunning();
    SetTimer(hwnd, TIMER_TICK, 1000, NULL);
    s_lastSaveTick = GetTickCount();
    g_today = Today();
    Ensure(REQ_AUTO);
}

static void OnDestroy(HWND hwnd)
{
    KillTimer(hwnd, TIMER_TICK);
    KillTimer(hwnd, TIMER_USER);
    ConfigSave();
    if (s_taskbar)
        ShowWindow(s_taskbar, SW_SHOWNORMAL);
    GpsStop();
    NetStop();
    LogStat();
    Log("exit, today: yandex %d, 2gis %d, osm %d", g_cfg.yandexCount, g_cfg.twogisCount, g_cfg.osmCount);
    PostQuitMessage(0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_DESTROY:
        OnDestroy(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DWORD t0 = GetTickCount();
        Paint(hdc);
        EndPaint(hwnd, &ps);
        s_lastPaintTick = GetTickCount();
        s_statPaints++;
        if (s_lastPaintTick - t0 > s_statPaintMax)
            s_statPaintMax = s_lastPaintTick - t0;
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_USER) {
            KillTimer(hwnd, TIMER_USER);
            s_userPending = false;
            Ensure(REQ_USER);
        } else {
            GpsGet(&s_gps);
            g_today = Today();
            if (!s_dragging)
                Ensure(REQ_AUTO);
            if (g_cfg.cacheKb > (LONG)g_cfg.cacheMb * 1024)
                CachePrune();   // trims to 90% of the limit, so this is rare
            // remember the GPS-driven position now and then: power may vanish any moment
            if (GetTickCount() - s_lastSaveTick > 5 * 60 * 1000) {
                ConfigSave();
                s_lastSaveTick = GetTickCount();
            }
            if (GetTickCount() - s_statTick >= 60000)
                LogStat();
            // the clock needs a frame a second; GPS updates may have drawn one already
            if (GetTickCount() - s_lastPaintTick < 900)
                return 0;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP_GPS:
        GpsGet(&s_gps);
        HttpSetGpsDate(s_gps.date);   // a reliable date for HTTPS certificate checks
        if (wp == 1)
            OnGpsScanDone();
        if (!s_dragging) {
            FollowGps();
            Ensure(REQ_AUTO);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP_MAP:
        OnMapResult((MapResult*)lp);
        return 0;

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        SetCapture(hwnd);
        s_pressed = HitButton(x, y);
        if (s_pressed >= 0) {
            s_pressedInside = true;
        } else if (s_settings) {
            s_setPressed = HitSettings(x, y);
        } else if (InMap(x, y)) {
            s_dragging = true;
            s_dragMoved = false;
            s_dragStart.x = x;
            s_dragStart.y = y;
            ViewWorld(&s_dragWx, &s_dragWy);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (s_pressed >= 0) {
            bool inside = HitButton(x, y) == s_pressed;
            if (inside != s_pressedInside) {
                s_pressedInside = inside;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (s_dragging) {
            int dx = x - s_dragStart.x, dy = y - s_dragStart.y;
            if (!s_dragMoved && (abs(dx) > 6 || abs(dy) > 6))
                s_dragMoved = true;
            if (s_dragMoved) {
                WorldToGeo(s_dragWx - dx, s_dragWy - dy, g_cfg.z, &g_cfg.lon, &g_cfg.lat);
                g_cfg.follow = FOLLOW_OFF;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        ReleaseCapture();
        if (s_pressed >= 0) {
            int b = s_pressed;
            bool inside = HitButton(x, y) == b;
            s_pressed = -1;
            if (inside)
                OnButton(b);
        } else if (s_setPressed >= 0) {
            int c = s_setPressed;
            s_setPressed = -1;
            if (HitSettings(x, y) == c)
                OnSettingsCtl(c);
        } else if (s_dragging) {
            s_dragging = false;
            if (s_dragMoved) {
                UserChanged();
            } else {
                Ensure(REQ_FORCE);   // tap on the map = refresh now
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:     OnButton(B_LEFT); break;
        case VK_RIGHT:    OnButton(B_RIGHT); break;
        case VK_UP:       OnButton(B_UP); break;
        case VK_DOWN:     OnButton(B_DOWN); break;
        case VK_ADD:      OnButton(B_PLUS); break;
        case VK_SUBTRACT: OnButton(B_MINUS); break;
        case 'T':         OnButton(B_TRF); break;
        case 'L':         OnButton(B_TSRC); break;
        case 'G':         OnButton(B_GPS); break;
        case 'S':         OnButton(B_SETTINGS); break;
        case 'N':         OnButton(B_NIGHT); break;
        case VK_ESCAPE:   OnButton(B_EXIT); break;
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

#ifdef UNDER_CE
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
#else
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
#endif
{
    GetModuleFileName(NULL, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash)
        slash[1] = 0;

    // single instance: bring the running copy to front
    HWND prev = FindWindow(APP_CLASS, NULL);
    if (prev) {
        SetForegroundWindow(prev);
        return 0;
    }

    LogInit();
    Log("OsmMapsCE start, dir %S", g_dir);
    if (!ConfigLoad())
        Log("config: not found, using defaults");
    ConfigSave();

    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_CLASS;
#ifndef UNDER_CE
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
#endif
    RegisterClass(&wc);

#ifdef UNDER_CE
    DWORD style = WS_POPUP | WS_VISIBLE;
    int x = 0, y = 0;
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
#else
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    RECT rc = { 0, 0, 800, 480 };
    AdjustWindowRect(&rc, style, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = rc.right - rc.left, h = rc.bottom - rc.top;
#endif
    HWND hwnd = CreateWindowEx(0, APP_CLASS, APP_NAME, style, x, y, w, h, NULL, NULL, inst, NULL);
    if (!hwnd) {
        Log("CreateWindow failed (%lu)", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
