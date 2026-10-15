#include "yamaps.h"

// ---------------------------------------------------------------- UI layout
// 800x480 in the VestaHome style (mockup VestaHome/skin/maps-skin-v2.pdf): the map over the whole screen, a
// see-through status bar on top. Always over the map: "+" / "-" on the right and the GPS mode on the left,
// 72 px see-through tiles. A tap on the map raises the bottom bar - as VestaHome's: home on the left, settings
// on the right, between them traffic, its provider and night, signs without captions - which goes down after
// kDockMs without a touch or on the next tap. Exit is on the settings screen. No pan arrows: the map is
// dragged (the PC keys still move it). The bars and tiles darken the map under them (Darken), their signs are
// pictures with alpha blended by hand (CE GDI has no usable alpha on a 16-bit screen). Pictures: make_images.py.

enum { B_SETTINGS, B_NIGHT, B_EXIT, B_TRF, B_TSRC, B_GPS, B_LEFT, B_RIGHT, B_UP, B_DOWN, B_MINUS, B_PLUS,
       B_HOME, B_COUNT };

enum { kTile = 72, kSign = 48, kStatusH = 44, kDockY = 404, kDockMs = 6000 };

struct Button {
    int            x, y;      // tiles over the map; the bottom bar's slots are laid out in ButtonRect
    const wchar_t* name[3];   // sign picture base names per state (toggle buttons have 2-3)
    Overlay*       ov[3][2];  // [state][pressed]: the sign with its alpha
};

static Button s_btn[B_COUNT] = {
    { 0, 0, { L"Bset" } },
    { 0, 0, { L"Bnight_on", L"Bnight_off" } },
    { 0, 0, { NULL } },   // exit: a button of the settings screen (Esc on the PC)
    { 0, 0, { L"Btrf_on", L"Btrf_off" } },
    { 0, 0, { L"Btsrc_ya", L"Btsrc_2g" } },   // traffic provider, only while traffic is on
    { 16, 312, { L"Bgps_on", L"Bgps_off", L"Bgps_none" } },
    { 0, 0, { NULL } },   // pan: keys only
    { 0, 0, { NULL } },
    { 0, 0, { NULL } },
    { 0, 0, { NULL } },
    { 712, 312, { L"Bminus" } },
    { 712, 232, { L"Bplus" } },
    { 0, 0, { L"Bhome" } },
};
static const int kDockOrder[] = { B_HOME, B_TRF, B_TSRC, B_NIGHT, B_SETTINGS };
static const int kDockCount = sizeof(kDockOrder) / sizeof(kDockOrder[0]);
static Overlay*  s_arrow[2][2];   // settings: back / stepper arrows ([left, right][pressed])

static const wchar_t* kWeekDays[7] = { L"Вс", L"Пн", L"Вт", L"Ср", L"Чт", L"Пт", L"Сб" };
static const wchar_t* kTrafficTitle[TRF_COUNT] = { L"Яндекс", L"2ГИС" };
static const int      kTrafficSrc[TRF_COUNT] = { SRC_YANDEX, SRC_2GIS };

static const COLORREF kText    = RGB(0xf3, 0xf1, 0xec);   // VestaHome palette
static const COLORREF kTextDim = RGB(0xa3, 0xa8, 0xb0);
static const COLORREF kTextErr = RGB(255, 120, 120);
static const COLORREF kMapBg   = RGB(228, 226, 222);
static const COLORREF kInk     = RGB(0x0b, 0x0c, 0x0e);   // bars
static const COLORREF kPanel   = RGB(0x17, 0x19, 0x1c);   // tiles, settings
static const COLORREF kPanel2  = RGB(0x22, 0x25, 0x2a);   // settings controls
static const COLORREF kLine    = RGB(0x34, 0x38, 0x3f);   // 2 px borders
static const COLORREF kOrange  = RGB(0xff, 0x6b, 0x1a);   // the one accent: pressed / on

enum { TIMER_TICK = 1, TIMER_USER = 2 };
static const UINT kUserDelayMs = 900;    // wait for the user to stop pressing buttons

// GPS button modes (g_cfg.follow)
enum { FOLLOW_OFF, FOLLOW_NORTH, FOLLOW_HEADING };
static const double kCarAhead        = 0.25; // heading-up: view center this part of map height ahead of the car
                                             // (0.25 of 480: the car stays above the bottom bar)
static const double kHeadingMinSpeed = 5;    // km/h: below it the course is noise, keep the last heading
static const int    kCanvas          = 960;  // unrotated canvas >= map diagonal (800x480: 933)
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
static HFONT      s_fontBig, s_fontSmall, s_fontTiny, s_fontStatus, s_fontClock, s_fontLabel;
static bool       s_dockShown;          // the bottom bar is up
static DWORD      s_dockTick;           // last touch of it: it goes down kDockMs later

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

// The settings screen as VestaHome's: a top bar with "back", one panel of rows; a row's stepper (port, speed,
// cache) applies at once - no "Apply" / "Close". Rows at kSetRow0 + i * kSetRowH.
enum { S_PORT_PREV, S_PORT_NEXT, S_BAUD_PREV, S_BAUD_NEXT, S_CACHE_PREV, S_CACHE_NEXT,
       S_SCAN, S_EXIT, S_BACK, S_COUNT };
enum { kSetRow0 = 76, kSetRowH = 64 };
static const RECT kSetCtl[S_COUNT] = {
    { 512, 84, 560, 128 }, { 712, 84, 760, 128 },     // the steppers' arrows: rows 0..2
    { 512, 148, 560, 192 }, { 712, 148, 760, 192 },
    { 512, 212, 560, 256 }, { 712, 212, 760, 256 },
    { 616, 280, 760, 324 },                            // find GPS
    { 600, 408, 768, 452 },                            // exit the program
    { 8, 4, 64, 52 },                                  // back (top bar)
};
static const wchar_t* kSetCaption[S_COUNT] = { NULL, NULL, NULL, NULL, NULL, NULL, L"ПОИСК", L"ЗАВЕРШИТЬ", NULL };

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
// 7 days). net.cpp asks with If-None-Match, so an unchanged tile is not downloaded. Files saved while the date was unknown carry the unit's wrong clock (before
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

static void GpsLogLine(const char* line)
{
    Log("%s", line);
}

// VestaCore GPS: reads the port, or follows VestaHome when that one reads it already (shared memory).
static void EnsureGpsRunning()
{
    if (s_gpsRunning)
        return;
    g_cfg.gpsEnabled = 1;
    GpsConfig c;
    memset(&c, 0, sizeof(c));
    c.notify = s_wnd;
    c.msg = WM_APP_GPS;
    c.port = g_cfg.gpsPort;
    c.baud = (DWORD)g_cfg.gpsBaud;
    c.app = L"OSMapsCE";
    c.killExe = g_cfg.killProcess;
    c.kill = KillProcess;
    c.log = GpsLogLine;
    c.exeDir = g_dir;
    GpsStart(&c);
    s_gpsRunning = true;
}

// Every step applies at once, as on VestaHome's settings.
static void ApplySettings()
{
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
}

static void OnSettingsCtl(int id)
{
    switch (id) {
    case S_PORT_PREV:  s_setPort = s_setPort > 1 ? s_setPort - 1 : 9; ApplySettings(); break;
    case S_PORT_NEXT:  s_setPort = s_setPort < 9 ? s_setPort + 1 : 1; ApplySettings(); break;
    case S_BAUD_PREV:  s_setBaud = (s_setBaud + kSetBaudCount - 1) % kSetBaudCount; ApplySettings(); break;
    case S_BAUD_NEXT:  s_setBaud = (s_setBaud + 1) % kSetBaudCount; ApplySettings(); break;
    case S_CACHE_PREV: if (s_setCache > 0) s_setCache--; ApplySettings(); break;
    case S_CACHE_NEXT: if (s_setCache < kSetCacheCount - 1) s_setCache++; ApplySettings(); break;
    case S_SCAN:
        EnsureGpsRunning();
        GpsScan();
        break;
    case S_EXIT:
        DestroyWindow(s_wnd);
        return;
    case S_BACK:
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

// "Home": back to the launcher, the map stays open behind it. VestaHome (the home shell, kept on screen under
// the programs it starts) is brought to the front; VestaLaunch (hidden while its program runs) is asked to show
// itself (WM_VESTA_SHOW, as its second start does). Both bring the map back by raising its window: it stays
// visible, so "already running" finds it. Without either the map goes to the bottom and the shell shows.
#define VESTAHOME_CLASS L"VestaHome_Wnd"
#define VESTALAUNCH_CLASS L"VestaLaunch_Wnd"
#define WM_VESTA_SHOW (WM_APP + 3)

static void GoHome()
{
    s_dockShown = false;
    HWND home = FindWindow(VESTAHOME_CLASS, NULL), launch = FindWindow(VESTALAUNCH_CLASS, NULL);
    if (home && IsWindowVisible(home)) {
#ifdef UNDER_CE
        SetWindowPos(home, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
#endif
        SetForegroundWindow(home);
        Log("ui: home -> VestaHome");
    } else if (launch) {
        PostMessage(launch, WM_VESTA_SHOW, 0, 0);
        Log("ui: home -> VestaLaunch");
    } else {
        Log("ui: home -> no launcher, map to the bottom");
    }
    // the launcher made the map topmost when it raised it: step down to the bottom (this also drops topmost),
    // or the map keeps covering the launcher; the launcher raises it again from "already running"
    SetWindowPos(s_wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void OnButton(int b)
{
    s_dockTick = GetTickCount();   // a touch of the bottom bar keeps it up
    switch (b) {
    case B_HOME:
        GoHome();
        InvalidateRect(s_wnd, NULL, FALSE);
        return;
    case B_SETTINGS:
        s_settings = !s_settings;
        s_dockShown = false;
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

// Where a button is now; false when it isn't on the screen (the settings screen hides them all, the bottom
// bar's ones are there only while it is up, the traffic provider only while traffic is on).
static bool ButtonRect(int b, RECT* r)
{
    if (s_settings)
        return false;
    if (b == B_GPS || b == B_MINUS || b == B_PLUS) {
        SetRect(r, s_btn[b].x, s_btn[b].y, s_btn[b].x + kTile, s_btn[b].y + kTile);
        return true;
    }
    if (!s_dockShown)
        return false;
    int n = 0, slot = -1;
    for (int i = 0; i < kDockCount; i++) {
        if (kDockOrder[i] == B_TSRC && !g_cfg.traffic)
            continue;
        if (kDockOrder[i] == b)
            slot = n;
        n++;
    }
    if (slot < 0)
        return false;
    int w = (s_cw - 16) / n;
    SetRect(r, 8 + slot * w, kDockY + 4, 8 + (slot + 1) * w, s_ch - 4);
    return true;
}

static int HitButton(int x, int y)
{
    RECT r;
    for (int i = 0; i < B_COUNT; i++)
        if (ButtonRect(i, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return i;
    return -1;
}

static void DockShow(bool show)
{
    s_dockShown = show;
    s_dockTick = GetTickCount();
}

static bool InMap(int x, int y)
{
    return x >= g_cfg.mapX && x < g_cfg.mapX + g_cfg.mapW && y >= g_cfg.mapY && y < g_cfg.mapY + g_cfg.mapH;
}

// ---------------------------------------------------------------- drawing

// A sign picture (PNG with alpha) as an overlay: blended over the frame by BlendSign.
static Overlay* LoadSign(const wchar_t* name, int pressed)
{
    wchar_t file[64], path[MAX_PATH];
    _snwprintf(file, 64, L"Images\\%s_%c.png", name, pressed ? L'P' : L'N');
    file[63] = 0;
    PathInDir(path, file);
    unsigned char* data;
    int len;
    Overlay* ov = NULL;
    if (ReadWholeFile(path, &data, &len)) {
        ov = OverlayFromMemory(data, len);
        free(data);
    }
    if (!ov)
        Log("ui: no image %S", path);
    return ov;
}

static void LoadImages()
{
    wchar_t path[MAX_PATH];
    PathInDir(path, L"Images\\MapGL.png");
    if (!ImageFromFile(path, &s_bg))
        Log("ui: no background %S", path);

    for (int i = 0; i < B_COUNT; i++)
        for (int state = 0; state < 3; state++)
            if (s_btn[i].name[state])
                for (int p = 0; p < 2; p++)
                    s_btn[i].ov[state][p] = LoadSign(s_btn[i].name[state], p);
    for (int p = 0; p < 2; p++) {
        s_arrow[0][p] = LoadSign(L"Sleft", p);
        s_arrow[1][p] = LoadSign(L"Sright", p);
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

// ---- see-through bars and tiles, drawn straight into the frame's pixels (s_backBits)

static int BackStride()
{
    return ((s_cw * 2 + 3) & ~3) / 2;
}

// The map under r at a quarter of its brightness (two shifts and a mask per RGB565 pixel), rounded corners
// of the given radius. Without the frame's pixels (no DIB): filled with the bar colour instead.
static void Darken(HDC dc, const RECT& r, int radius)
{
    if (!s_backBits) {
        HBRUSH b = CreateSolidBrush(kInk);
        FillRect(dc, &r, b);
        DeleteObject(b);
        return;
    }
#ifndef UNDER_CE
    GdiFlush();   // desktop GDI may batch drawing into the DIB
#endif
    int stride = BackStride();
    for (int y = r.top < 0 ? 0 : r.top; y < r.bottom && y < s_ch; y++) {
        int inset = 0, dy = y < r.top + radius ? r.top + radius - y : y >= r.bottom - radius ? y - (r.bottom - radius - 1) : 0;
        if (dy > 0) {
            double d = dy - 0.5;
            inset = (int)(radius - sqrt(radius * radius - d * d) + 0.5);
        }
        unsigned short* p = s_backBits + (s_ch - 1 - y) * stride;
        int x1 = r.right - inset > s_cw ? s_cw : r.right - inset;
        for (int x = r.left + inset < 0 ? 0 : r.left + inset; x < x1; x++)
            p[x] = (unsigned short)((p[x] >> 2) & 0x39E7);
    }
}

// A sign picture blended into the frame at (x, y) by its alpha.
static void BlendSign(HDC dc, const Overlay* ov, int x, int y)
{
    if (!ov || !s_backBits)
        return;
#ifndef UNDER_CE
    GdiFlush();
#endif
    int stride = BackStride();
    for (int oy = 0; oy < ov->h; oy++) {
        int sy = y + oy;
        if (sy < 0 || sy >= s_ch)
            continue;
        const unsigned char* p = (const unsigned char*)ov + ov->rowOfs[oy];
        int runs = *(const unsigned short*)p;
        p += 4;
        unsigned short* drow = s_backBits + (s_ch - 1 - sy) * stride;
        int sx = x;
        for (int r = 0; r < runs; r++) {
            const unsigned short* rh = (const unsigned short*)p;
            sx += rh[0];
            int count = rh[1];
            const unsigned char* s = p + 4;
            p += 4 + count * 4;
            for (int i = 0; i < count; i++, sx++, s += 4) {
                if (sx < 0 || sx >= s_cw)
                    continue;
                int a = s[3];
                unsigned short d = drow[sx];
                int dr = (d >> 8) & 0xF8, dg = (d >> 3) & 0xFC, db = (d << 3) & 0xF8;
                dr += (s[0] - dr) * a / 255;
                dg += (s[1] - dg) * a / 255;
                db += (s[2] - db) * a / 255;
                drow[sx] = RGB565(dr, dg, db);
            }
        }
    }
}

// The 2 px border of a tile, inside nothing (Darken made it see-through).
static void Outline(HDC dc, const RECT& r, COLORREF c, int radius)
{
    HPEN pen = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left + 1, r.top + 1, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
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
    HBRUSH brush = CreateSolidBrush(fresh ? kOrange : RGB(140, 140, 140));   // VestaHome accent
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

// OSM licence: attribution must be visible on the map, not hidden behind controls (above the bottom bar
// while it is up).
static void DrawAttribution(HDC dc, RECT mr)
{
    if (s_dockShown)
        mr.bottom = kDockY;
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

    // the rotation buffer (768 KB at 800 x 480) is kept only while heading-up is on
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

// A rounded tile in the VestaHome style: fill + 2 px border.
static void Tile(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 2, border);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, radius, radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(brush);
}

// Settings controls: a dark tile with a border, pressed = orange with a dark caption.
static void DrawCtl(HDC dc, const RECT& r, bool pressed, HFONT font, const wchar_t* caption)
{
    Tile(dc, r, pressed ? kOrange : kPanel2, pressed ? kOrange : kLine, 20);
    Text(dc, font, pressed ? kInk : kText, r.left, r.top, r.right, r.bottom, DT_CENTER, caption);
}

// A row of the settings panel: label, a muted line under it, a separator under the row.
static void SetRow(HDC dc, int y, const wchar_t* label, const wchar_t* sub, bool sep)
{
    Text(dc, s_fontLabel, kText, 36, y + 6, 500, y + 32, DT_LEFT, label);
    Text(dc, s_fontSmall, kTextDim, 36, y + 32, 500, y + 54, DT_LEFT, sub);
    if (sep) {
        RECT l = { 24, y + kSetRowH - 2, s_cw - 24, y + kSetRowH };
        HBRUSH b = CreateSolidBrush(kLine);
        FillRect(dc, &l, b);
        DeleteObject(b);
    }
}

// A stepper "< value >" as VestaHome's; prev / next are the S_* zones of its arrows.
static void Stepper(HDC dc, int prev, int next, const wchar_t* value)
{
    const RECT& a = kSetCtl[prev];
    const RECT& b = kSetCtl[next];
    RECT box = { a.left, a.top, b.right, b.bottom };
    Tile(dc, box, kInk, kLine, 20);
    for (int k = 0; k < 2; k++) {
        const RECT& z = k ? b : a;
        bool down = s_setPressed == (k ? next : prev);
        if (down) {
            RECT in = { z.left + 3, z.top + 3, z.right - 3, z.bottom - 3 };
            Tile(dc, in, kOrange, kOrange, 16);
        }
        const Overlay* ov = s_arrow[k][down ? 1 : 0];
        if (ov)
            BlendSign(dc, ov, (z.left + z.right - ov->w) / 2, (z.top + z.bottom - ov->h) / 2);
    }
    Text(dc, s_fontBig, kText, a.right, a.top, b.left, a.bottom, DT_CENTER, value);
}

// Over the whole screen (the map's bars and buttons are off meanwhile), as VestaHome's settings: a top bar
// with "back", the clock, one panel with the rows; centred on the 800 px screen.
static void DrawSettings(HDC dc)
{
    RECT all = { 0, 0, s_cw, s_ch }, top = { 0, 0, s_cw, 54 }, topLine = { 0, 54, s_cw, 56 };
    HBRUSH ink = CreateSolidBrush(kInk), line = CreateSolidBrush(kLine);
    FillRect(dc, &all, ink);
    FillRect(dc, &top, ink);
    FillRect(dc, &topLine, line);
    DeleteObject(ink);
    DeleteObject(line);

    wchar_t s[128];
    const RECT& back = kSetCtl[S_BACK];
    bool backDown = s_setPressed == S_BACK;
    if (backDown)
        Tile(dc, back, kOrange, kOrange, 24);
    if (s_arrow[0][backDown ? 1 : 0])
        BlendSign(dc, s_arrow[0][backDown ? 1 : 0], (back.left + back.right - s_arrow[0][0]->w) / 2,
                  (back.top + back.bottom - s_arrow[0][0]->h) / 2);
    Text(dc, s_fontBig, kText, 76, 0, 500, 54, DT_LEFT, L"НАСТРОЙКИ КАРТЫ");
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 128, L"%02d:%02d", t.wHour, t.wMinute);
    Text(dc, s_fontClock, kText, s_cw - 160, 0, s_cw - 14, 54, DT_RIGHT, s);

    RECT panel = { 16, 68, s_cw - 16, s_ch - 12 };
    Tile(dc, panel, kPanel, kLine, 32);

    // the receiver's state, under "GPS port"
    COLORREF c = kText;
    if (!s_gpsRunning)
        wcscpy(s, L"GPS выключен (gps=0)");
    else if (s_gps.status == GPS_OPEN)
        _snwprintf(s, 128, L"открыт · %d спутн.%s", s_gps.sats, s_gps.valid ? L" · фикс" : L"");
    else if (s_gps.status == GPS_BUSY)
        wcscpy(s, L"занят другой программой");
    else if (s_gps.status == GPS_SCANNING)
        _snwprintf(s, 128, L"поиск: %s %lu ...", s_gps.port, s_gps.baud);
    else
        wcscpy(s, L"не открывается");
    s[127] = 0;
    SetRow(dc, kSetRow0, L"Порт GPS", s, true);
    _snwprintf(s, 128, L"COM%d", s_setPort);
    Stepper(dc, S_PORT_PREV, S_PORT_NEXT, s);

    SetRow(dc, kSetRow0 + kSetRowH, L"Скорость порта", L"или как у оболочки", true);
    if (kSetBauds[s_setBaud])
        _snwprintf(s, 128, L"%lu", kSetBauds[s_setBaud]);
    else
        wcscpy(s, L"не менять");
    Stepper(dc, S_BAUD_PREV, S_BAUD_NEXT, s);

    if (g_cfg.cacheKb >= 0)
        _snwprintf(s, 128, L"занято %ld МБ", g_cfg.cacheKb / 1024);
    else
        wcscpy(s, L"занято ?");
    SetRow(dc, kSetRow0 + 2 * kSetRowH, L"Кэш карт", s, true);
    int mb = kSetCacheMb[s_setCache];
    if (mb >= 1000)
        _snwprintf(s, 128, L"%d ГБ", mb / 1000);
    else
        _snwprintf(s, 128, L"%d МБ", mb);
    Stepper(dc, S_CACHE_PREV, S_CACHE_NEXT, s);

    SetRow(dc, kSetRow0 + 3 * kSetRowH, L"Найти GPS", L"перебрать COM1…COM9 и скорости", false);
    DrawCtl(dc, kSetCtl[S_SCAN], s_setPressed == S_SCAN, s_fontLabel, kSetCaption[S_SCAN]);
    DrawCtl(dc, kSetCtl[S_EXIT], s_setPressed == S_EXIT, s_fontLabel, kSetCaption[S_EXIT]);

    // live state of the receiver
    if (!s_gpsRunning) {
        wcscpy(s, L"GPS выключен (gps=0): включится любым шагом порта или скорости");
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
    Text(dc, s_fontSmall, c, 36, 332, 580, 350, DT_LEFT, s);

    _snwprintf(s, 128, L"NMEA: %d строк, с ошибкой %d", s_gps.lines, s_gps.badLines);
    Text(dc, s_fontSmall, s_gps.lines ? kText : kTextDim, 36, 350, 580, 368, DT_LEFT, s);

    if (s_gps.valid)
        _snwprintf(s, 128, L"Координаты: %.5f, %.5f  (%d спутн.)", s_gps.lat, s_gps.lon, s_gps.sats);
    else
        _snwprintf(s, 128, L"Координат пока нет (%d спутн.)", s_gps.sats);
    s[127] = 0;
    Text(dc, s_fontSmall, s_gps.valid ? kText : kTextDim, 36, 368, 580, 386, DT_LEFT, s);

    MultiByteToWideChar(CP_ACP, 0, s_gps.last, -1, s, 128);
    s[127] = 0;
    Text(dc, s_fontSmall, kTextDim, 36, 386, 580, 404, DT_LEFT, s[0] ? s : L"-");

    if (s_gps.found == 1) {
        _snwprintf(s, 128, L"Найден GPS: %s %lu, настройки сохранены", s_gps.port, s_gps.baud);
        Text(dc, s_fontSmall, RGB(120, 220, 120), 36, 404, 580, 422, DT_LEFT, s);
    } else if (s_gps.found == -1) {
        Text(dc, s_fontSmall, kTextErr, 36, 404, 580, 422, DT_LEFT, L"GPS не найден на COM1..COM9");
    } else {
        Text(dc, s_fontSmall, kTextDim, 36, 404, 580, 422, DT_LEFT,
             L"Нет строк - не тот порт. Строки с ошибкой - не та скорость.");
    }

    _snwprintf(s, 128, L"Запросов: Яндекс %d/%d, 2ГИС %d/%d, OSM %d",
               *Counter(SRC_YANDEX), Limit(SRC_YANDEX), *Counter(SRC_2GIS), Limit(SRC_2GIS), g_cfg.osmCount);
    s[127] = 0;
    Text(dc, s_fontSmall, kTextDim, 36, 422, 580, 440, DT_LEFT, s);
}

// The status bar over the map: GPS on the left, the traffic / network state, a big clock on the right
// (as the top bar of VestaHome).
static void DrawStatus(HDC dc)
{
    RECT bar = { 0, 0, s_cw, kStatusH - 2 }, line = { 0, kStatusH - 2, s_cw, kStatusH };
    Darken(dc, bar, 0);   // see-through: the map shows dimmed under it
    HBRUSH l = CreateSolidBrush(kLine);
    FillRect(dc, &line, l);
    DeleteObject(l);

    wchar_t s[96];
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 96, L"%02d:%02d", t.wHour, t.wMinute);
    Text(dc, s_fontClock, kText, s_cw - 160, 0, s_cw - 14, kStatusH - 2, DT_RIGHT, s);

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
    Text(dc, s_fontStatus, gc, 14, 0, 320, kStatusH - 2, DT_LEFT, s);

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
    if (g_cfg.traffic && uc == kText && !s_inflight) {   // which provider, next to the age
        wchar_t t2[96];
        _snwprintf(t2, 96, L"%s · %s", kTrafficTitle[g_cfg.trafficSrc], s);
        t2[95] = 0;
        wcscpy(s, t2);
    }
    Text(dc, s_fontStatus, uc == kText ? kTextDim : uc, 330, 0, s_cw - 170, kStatusH - 2, DT_LEFT, s);
}

// The sign of a button in the middle of r (a missing picture: the button's name as text).
static void DrawSign(HDC dc, int b, const RECT& r, int pressed)
{
    const Overlay* ov = s_btn[b].ov[ButtonState(b)][pressed];
    if (ov)
        BlendSign(dc, ov, (r.left + r.right - ov->w) / 2, (r.top + r.bottom - ov->h) / 2);
    else
        Text(dc, s_fontSmall, kText, r.left, r.top, r.right, r.bottom, DT_CENTER, s_btn[b].name[0] + 1);
}

static void DrawButtons(HDC dc)
{
    RECT r;
    if (s_dockShown && !s_settings) {
        RECT bar = { 0, kDockY, s_cw, s_ch }, line = { 0, kDockY, s_cw, kDockY + 2 };
        // the orange part of the line shrinks to the moment the bar goes down
        DWORD el = GetTickCount() - s_dockTick;
        RECT left = { 0, kDockY, el >= (DWORD)kDockMs ? 0 : (int)(s_cw * (kDockMs - el) / kDockMs), kDockY + 2 };
        Darken(dc, bar, 0);
        HBRUSH l = CreateSolidBrush(kLine), o = CreateSolidBrush(kOrange);
        FillRect(dc, &line, l);
        FillRect(dc, &left, o);
        DeleteObject(l);
        DeleteObject(o);
    }
    for (int i = 0; i < B_COUNT; i++) {
        if (!ButtonRect(i, &r))
            continue;
        int pressed = s_pressed == i && s_pressedInside ? 1 : 0;
        if (i == B_GPS || i == B_MINUS || i == B_PLUS) {   // a see-through tile over the map
            if (pressed) {
                Tile(dc, r, kOrange, kOrange, 24);
            } else {
                Darken(dc, r, 12);
                Outline(dc, r, kLine, 12);
            }
            DrawSign(dc, i, r, pressed);
            continue;
        }
        // a slot of the bottom bar: only the sign (a mode that is on: the orange one), pressed = orange tile
        if (pressed) {
            RECT p = { r.left + 6, r.top + 2, r.right - 6, r.bottom - 2 };
            Tile(dc, p, kOrange, kOrange, 24);
        }
        DrawSign(dc, i, r, pressed);
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
    if (s_settings) {
        DrawSettings(dc);
    } else {
        DrawStatus(dc);
        DrawButtons(dc);
    }

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
    s_fontStatus = MakeFont(17, FW_NORMAL);
    s_fontClock = MakeFont(30, FW_BOLD);
    s_fontLabel = MakeFont(19, FW_BOLD);
    // the map takes the whole screen (map_x/y/w/h of the ini are YaMapsCE's: its pictures are 600 x 450)
    g_cfg.mapX = 0;
    g_cfg.mapY = 0;
    g_cfg.mapW = s_cw;
    g_cfg.mapH = s_ch;
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
            if (s_dockShown) {   // the bar's line shrinks every second; it goes down after kDockMs
                if (s_pressed < 0 && GetTickCount() - s_dockTick >= (DWORD)kDockMs)
                    s_dockShown = false;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
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
            } else {   // a tap on the map raises (and refreshes the traffic) / lowers the bottom bar
                DockShow(!s_dockShown);
                if (s_dockShown)
                    Ensure(REQ_FORCE);
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
    if (prev) {   // it may be behind another window or at the bottom (OSMapsCE's "home"): to the top
        ShowWindow(prev, SW_SHOW);
        SetWindowPos(prev, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(prev);
        return 0;
    }

    LogInit();
    Log("OSMapsCE start, dir %S", g_dir);
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
    // End the process for real even if a worker didn't stop in time (NetStop gives a stuck socket
    // only 1 s): the GPS port is released only when the process is gone. Config and log are already
    // written (OnDestroy, unbuffered WriteFile).
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
