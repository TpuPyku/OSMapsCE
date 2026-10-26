#include "yamaps.h"
#include "vversion.h"   // VestaCore: the version "major.minor.YYMMDD" (the date: this file's build)
static const wchar_t* const kRelease = L"1.03";   // raise by hand: minor - significant changes, major - big releases

// ---------------------------------------------------------------- UI layout
// 800x480 in the VestaHome style (mockup VestaHome/skin/maps-skin-v2.pdf): the map over the whole screen, a
// see-through status bar on top. Always over the map: "+" / "-" on the right and the GPS mode on the left,
// 72 px see-through tiles. A tap on the map raises the bottom bar - as VestaHome's: home on the left, settings
// on the right, between them traffic, its provider and night, signs without captions - which goes down after
// kDockMs without a touch or on the next tap. Exit is on the settings screen. No pan arrows: the map is
// dragged (the PC keys still move it). The bars and tiles darken the map under them (Darken), their signs are
// pictures with alpha blended by hand (CE GDI has no usable alpha on a 16-bit screen). Pictures: make_images.py.

enum { B_SETTINGS, B_NIGHT, B_EXIT, B_TRF, B_TSRC, B_GPS, B_LEFT, B_RIGHT, B_UP, B_DOWN, B_MINUS, B_PLUS,
       B_HOME, B_SEARCH, B_ROUTEOFF, B_COUNT };

enum { kTile = 72, kRound = 60, kSign = 48, kStatusH = 44, kDockY = 404, kDockMs = 6000, kDockH = 76, kDockStep = 16,
       kDockAnimMs = 30 };   // the bar slides in / out by kDockStep px every kDockAnimMs

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
    { 724, 286, { L"Bgps_on", L"Bgps_off", L"Bgps_none" } },
    { 0, 0, { NULL } },   // pan: keys only
    { 0, 0, { NULL } },
    { 0, 0, { NULL } },
    { 0, 0, { NULL } },
    { 724, 218, { L"Bminus" } },
    { 724, 150, { L"Bplus" } },
    { 0, 0, { L"Bhome" } },
    { 0, 0, { L"Bsearch" } },   // search a place, a route to it (route.cpp)
    { 0, 0, { L"Broff" } },     // the route off, only while there is one
};
static const int kDockOrder[] = { B_HOME, B_TRF, B_TSRC, B_SEARCH, B_ROUTEOFF, B_NIGHT, B_SETTINGS };
static const int kDockCount = sizeof(kDockOrder) / sizeof(kDockOrder[0]);
static Overlay*  s_arrow[2][2];   // the steppers' arrows ([left, right][pressed]); the search's "back"
static Overlay*  s_backSign[2];   // VestaHome's "back" (undo-2) at the bottom left: settings, the route editor

static const wchar_t* kWeekDays[7] = { L"Вс", L"Пн", L"Вт", L"Ср", L"Чт", L"Пт", L"Сб" };
static const wchar_t* kTrafficTitle[TRF_COUNT] = { L"Яндекс", L"2ГИС" };
static const int      kTrafficSrc[TRF_COUNT] = { SRC_YANDEX, SRC_2GIS };

static const COLORREF kText    = RGB(0xf5, 0xf3, 0xee);   // VestaHome's palette (design 0.6, its skin.ini)
static const COLORREF kTextDim = RGB(0xaa, 0xb0, 0xb8);
static const COLORREF kTextErr = RGB(255, 120, 120);
static const COLORREF kMapBg   = RGB(228, 226, 222);
static const COLORREF kInk     = RGB(0x0e, 0x10, 0x13);   // the background, the bottom bar
static const COLORREF kPanel   = RGB(0x1a, 0x1d, 0x22);   // big panels (settings)
static const COLORREF kPanel2  = RGB(0x23, 0x27, 0x2e);   // cards, round buttons, controls: VestaHome's tiles
static const COLORREF kLine    = RGB(0x2a, 0x2f, 0x37);   // separators
// The one accent (pressed / on / chosen, the car and its glow): VestaHome's theme (the user's rule 2026-10-26) -
// its "welcome" / "theme" (add-on API) or, without VestaHome running, screen.accent of its VestaHome.ini; Марс else.
static COLORREF       kOrange  = RGB(0xff, 0x6b, 0x1a);
static const COLORREF kHair    = RGB(64, 68, 77);         // night: a card's 1 px edge over the dark map

enum { TIMER_TICK = 1, TIMER_USER = 2, TIMER_DOCK = 3, TIMER_LONG = 4 };
static const UINT kUserDelayMs = 900;    // wait for the user to stop pressing buttons

// GPS button modes (g_cfg.follow)
enum { FOLLOW_OFF, FOLLOW_NORTH, FOLLOW_HEADING };
static const double kCarAhead        = 0.25; // heading-up: view center this part of map height ahead of the car
                                             // (0.25 of 480: the car stays above the bottom bar)
static const double kHeadingMinSpeed = 5;    // km/h: below it the course is noise, keep the last heading
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
static HFONT      s_fontBig, s_fontSmall, s_fontTiny, s_fontStatus, s_fontClock, s_fontLabel, s_fontCap;
static HFONT      s_fontHuge, s_fontKm, s_fontStreet, s_fontSpeed;   // design v3: the turn's distance, the trip
static bool       s_dockShown;          // the bottom bar is up
static int        s_dockOff;            // how far it is pushed down while sliding (0 = all up, kDockH = gone)
static int        s_dockDir;            // sliding: -1 in, +1 out, 0 still
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
static Image      s_rotated;            // the turned map frame, only while heading-up (the ring: below)
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

// Search screen (route.cpp does the requests): a top bar with "back" and the text, under it the keyboard or the
// places found; a place tapped - the route to it from the car (no fix: from the map's center).
static bool       s_search;
static bool       s_showHits;       // the places found instead of the keyboard
static wchar_t    s_query[64];
static int        s_kbLang;         // 0 Russian, 1 Latin
static int        s_kbPressed = -1;
enum { K_LANG = 100, K_SPACE, K_BS, K_FIND, K_BACK, K_FIELD, K_HIT = 200 };
static const wchar_t* kKeys[2][4] = {
    { L"1234567890-", L"йцукенгшщзхъ", L"фывапролджэ", L"ячсмитьбю.," },
    { L"1234567890-", L"qwertyuiop", L"asdfghjkl", L"zxcvbnm.," },
};
enum { kKbY = 66, kKbRowH = 81, kHitY = 62, kHitH = 58 };

// The route editor (a long press on the map, the user's design 2026-10-26): "from" (the car by default, or an
// address, or a point on the map), up to three stops, "to"; a field tapped opens its choices. With a route on, a
// long press offers a stop there or a new route there. Slots: 0 from, 1..3 the stops, 4 to.
enum { P_BACK = 1, P_ROW = 10, P_DEL = 20, P_ADDVIA = 30, P_GO, P_CH_GPS = 40, P_CH_SEARCH, P_CH_MAP, P_CH_CANCEL,
       P_PICK_CANCEL = 50, M_VIA = 60, M_NEW, M_CANCEL, P_TRIP_X = 80 };
static bool       s_plan;
static int        s_planChoose = -1;   // the slot whose choices are open
static int        s_planPressed = -1;
static int        s_pickSlot = -1;     // the map picks a point for this slot
static int        s_searchSlot = -1;   // the search picks a place for this slot
static bool       s_lpMenu;            // the long press menu (a route is on)
static double     s_lpLon, s_lpLat;
static bool       s_longDone;          // the press became a long one: its release does nothing more
static DWORD      s_downTick;          // when the finger went down on the map
static const wchar_t* s_planMsg;
enum { kRowY = 66, kRowH = 56, kLongMs = 650 };
static RoutePlace* PlanSlot(int slot);
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
    { 600, 416, 784, 468 },                            // exit the program
    { 8, 412, 80, 480 },                               // back: VestaHome's, at the bottom left
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

// What a frame shows that changes by itself (2026-10-22, PLAN: "standing, no frame until something changed"):
// the map's center and the car on it at pixel precision, the heading, the bar's GPS and traffic / network texts,
// the clock's minute, night. With the GPS ticking once a second while the car stands, the frame stays the same and
// is not drawn again; tiles arriving, taps, the bar and the settings draw as before.
static DWORD s_paintSig;   // FrameSig() of the frame last drawn

static DWORD FrameSig()
{
    double x, y, cx = 0, cy = 0;
    GeoToWorld(g_cfg.lon, g_cfg.lat, g_cfg.z, &x, &y);
    if (s_gps.valid)
        GeoToWorld(s_gps.lon, s_gps.lat, g_cfg.z, &cx, &cy);
    SYSTEMTIME t;
    GetLocalTime(&t);
    int v[] = { (int)x, (int)y, (int)cx, (int)cy, g_cfg.z, (int)s_heading, s_gps.valid, s_gps.sats,
                (int)(s_gps.speedKmh + 0.5), s_gps.status, s_gps.lines > 0, s_gpsRunning, s_limitHit,
                s_lastErr && GetTickCount() - s_lastErrTick < 60000, s_inflight, (int)(GetTickCount() / 60000),
                t.wMinute, g_cfg.night, s_settings, (int)RouteSig(), s_search, SearchState(), s_plan, s_pickSlot, s_lpMenu };
    DWORD h = 2166136261u;   // FNV-1a over them
    for (int i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++)
        h = (h ^ (DWORD)v[i]) * 16777619u;
    return h;
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
        g_cfg.yandexCount = g_cfg.twogisCount = g_cfg.osmCount = g_cfg.apiCount = 0;
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

// The night mode on / off (the map's filter, the settings); NightShare is the caller's (a tap, not VestaHome's change).
static void NightSet(bool night)
{
    g_cfg.night = night;
    if (!night) {
        free(s_nightLut);
        s_nightLut = NULL;
    }
    ConfigSave();
    InvalidateRect(s_wnd, NULL, FALSE);
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
    case B_SEARCH:
        s_search = true;
        s_dockShown = false;
        s_showHits = SearchState() == SEARCH_DONE && SearchCount() > 0;
        InvalidateRect(s_wnd, NULL, FALSE);
        return;
    case B_ROUTEOFF:
        RouteClear();
        Log("ui: route off");
        InvalidateRect(s_wnd, NULL, FALSE);
        return;
    case B_NIGHT:
        NightSet(!g_cfg.night);
        NightShare(g_cfg.night != 0);   // VestaHome follows (VestaCore vnight)
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
    if (s_settings || s_search || s_plan)
        return false;
    if (b == B_GPS || b == B_MINUS || b == B_PLUS) {   // round, on the right (design v3)
        SetRect(r, s_btn[b].x, s_btn[b].y, s_btn[b].x + kRound, s_btn[b].y + kRound);
        return true;
    }
    if (!s_dockShown)
        return false;
    // VestaHome's bottom bar has 7 places (x 8 + i * 112, 104 wide, 58 high at 8 px under the bar's top): each
    // button where its counterpart is - home = "Главная", traffic = "Радио", its source = "Медиа",
    // settings = "Настройки", night = "Ночь" (the user's wish 2026-10-18: the same places, not stretched)
    // search and "route off" take the two middle places (VestaHome's phone and apps)
    int slot = b == B_HOME ? 0 : b == B_TRF ? 1 : b == B_TSRC ? 2 : b == B_SEARCH ? 3 : b == B_ROUTEOFF ? 4
             : b == B_SETTINGS ? 5 : b == B_NIGHT ? 6 : -1;
    RouteInfo ri;
    RouteGet(&ri);
    if (slot < 0 || (b == B_TSRC && !g_cfg.traffic) || (b == B_ROUTEOFF && ri.state == ROUTE_NONE))
        return false;
    int top = kDockY + 12 + s_dockOff;
    SetRect(r, 8 + slot * 112, top, 8 + slot * 112 + 104, top + 58);
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

// The bar slides up from the bottom edge and back down (TIMER_DOCK); it counts as up while it slides in.
static bool DockUp()
{
    return s_dockShown && s_dockDir <= 0;
}

static void DockShow(bool show)
{
    s_dockTick = GetTickCount();
    if (show == DockUp())
        return;
    if (show && !s_dockShown) {
        s_dockOff = kDockH;
        s_dockShown = true;
    }
    s_dockDir = show ? -1 : 1;
    SetTimer(s_wnd, TIMER_DOCK, kDockAnimMs, NULL);
}

static void DockStep()
{
    s_dockOff += s_dockDir * kDockStep;
    if (s_dockOff <= 0 || s_dockOff >= kDockH || !s_dockDir) {
        if (s_dockOff >= kDockH)
            s_dockShown = false;   // all the way down: gone
        s_dockOff = 0;
        s_dockDir = 0;
        KillTimer(s_wnd, TIMER_DOCK);
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

static bool InMap(int x, int y)
{
    return x >= g_cfg.mapX && x < g_cfg.mapX + g_cfg.mapW && y >= g_cfg.mapY && y < g_cfg.mapY + g_cfg.mapH;
}

// ---------------------------------------------------------------- drawing

// A sign picture (PNG with alpha) as an overlay: blended over the frame by BlendSign.
// The signs' orange (make_images.py draws the "on" ones in the design's orange) -> the theme's accent.
static void RecolorSign(Overlay* ov)
{
    if (kOrange == RGB(0xff, 0x6b, 0x1a))
        return;
    for (int y = 0; y < ov->h; y++) {
        unsigned char* p = (unsigned char*)ov + ov->rowOfs[y];
        int runs = *(unsigned short*)p;
        p += 4;
        for (int r = 0; r < runs; r++) {
            int count = ((unsigned short*)p)[1];
            unsigned char* c = p + 4;
            for (int i = 0; i < count; i++, c += 4)
                if (c[0] > 200 && c[1] > 60 && c[1] < 160 && c[2] < 100) {
                    c[0] = GetRValue(kOrange);
                    c[1] = GetGValue(kOrange);
                    c[2] = GetBValue(kOrange);
                }
            p += 4 + count * 4;
        }
    }
}

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
    else
        RecolorSign(ov);
    return ov;
}

static void LoadSigns()
{
    for (int i = 0; i < B_COUNT; i++)
        for (int state = 0; state < 3; state++)
            if (s_btn[i].name[state])
                for (int p = 0; p < 2; p++)
                    s_btn[i].ov[state][p] = LoadSign(s_btn[i].name[state], p);
    for (int p = 0; p < 2; p++) {
        s_arrow[0][p] = LoadSign(L"Sleft", p);
        s_arrow[1][p] = LoadSign(L"Sright", p);
        s_backSign[p] = LoadSign(L"Sback", p);
    }
}

static void LoadImages()
{
    wchar_t path[MAX_PATH];
    PathInDir(path, L"Images\\MapGL.png");
    if (!ImageFromFile(path, &s_bg))
        Log("ui: no background %S", path);
    LoadSigns();
}

// The theme's accent changed: the signs again in it.
static void AccentSet(COLORREF c)
{
    if (c == kOrange)
        return;
    kOrange = c;
    for (int i = 0; i < B_COUNT; i++)
        for (int st = 0; st < 3; st++)
            for (int p = 0; p < 2; p++) {
                free(s_btn[i].ov[st][p]);
                s_btn[i].ov[st][p] = NULL;
            }
    for (int k = 0; k < 2; k++)
        for (int p = 0; p < 2; p++) {
            free(s_arrow[k][p]);
            s_arrow[k][p] = NULL;
            free(s_backSign[p]);
            s_backSign[p] = NULL;
        }
    LoadSigns();
    Log("ui: accent %02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    if (s_wnd)
        InvalidateRect(s_wnd, NULL, FALSE);
}

// Without VestaHome running: its setting (a Lada Vesta paint code, VestaHome's kAccents) in its ini next to us
// (\SDMMC\OSMapsCE and \SDMMC\VestaHome; the PC: the repos side by side).
static void AccentFromIni()
{
    static const struct { int code; COLORREF c; } kAcc[] = { { 130, RGB(0xff, 0x6b, 0x1a) }, { 195, RGB(0xdc, 0x3c, 0x3c) },
        { 247, RGB(0xc3, 0xb8, 0x9c) }, { 372, RGB(0x72, 0xc0, 0x59) }, { 476, RGB(0x4f, 0x7a, 0xca) },
        { 619, RGB(0xab, 0xba, 0xd8) }, { 691, RGB(0xee, 0xee, 0xee) } };
    static const wchar_t* kIni[2] = { L"..\\VestaHome\\VestaHome.ini", L"..\\..\\VestaHome\\VestaHome\\VestaHome\\VestaHome.ini" };
    for (int f = 0; f < 2; f++) {
        wchar_t path[MAX_PATH];
        PathInDir(path, kIni[f]);
        unsigned char* data;
        int len;
        if (!ReadWholeFile(path, &data, &len))
            continue;
        char* buf = (char*)realloc(data, len + 1);
        if (!buf) {
            free(data);
            return;
        }
        buf[len] = 0;
        const char* k = strstr(buf, "\nscreen.accent=");
        int code = k ? atoi(k + 15) : 130;
        free(buf);
        for (int i = 0; i < (int)(sizeof(kAcc) / sizeof(kAcc[0])); i++)
            if (kAcc[i].code == code)
                kOrange = kAcc[i].c;   // (before the signs are loaded)
        return;
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
static void Darken(HDC dc, const RECT& r, int radius, int shift = 2)
{
    unsigned short mask = shift == 3 ? 0x18E3 : 0x39E7;   // (RGB565 >> 2 or >> 3, the bits that cross out)
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
            p[x] = (unsigned short)((p[x] >> shift) & mask);
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

// VestaHome's "back" of a nested screen (its BackButton): a disc of the tiles' colour at the bottom left (centre 44, 446,
// radius 26), the turning-back arrow; pressed - the accent.
static const RECT kBackRect = { 8, 412, 80, 480 };

static void BackDisc(HDC dc, bool down)
{
    RECT c = { 18, 420, 70, 472 };
    HBRUSH b = CreateSolidBrush(down ? kOrange : kPanel2);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, c.left, c.top, c.right + 1, c.bottom + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    const Overlay* ov = s_backSign[down ? 1 : 0];
    if (ov)
        BlendSign(dc, ov, 44 - ov->w / 2, 446 - ov->h / 2);
}

// The 1 px edge of a card over the dark night map (by day the shade is enough). Design rule 2026-10-26: buttons
// have no border; a chosen thing - the accent's 2 px (Outline); cards over the map - only this edge at night.
static void Hairline(HDC dc, const RECT& r, int radius)
{
    if (!g_cfg.night)
        return;
    HPEN pen = CreatePen(PS_SOLID, 1, kHair);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// A card over the map (design v3): VestaHome's tile colour, solid (the user 2026-10-26: the panels and the bar as
// VestaHome's), round corners, no border; at night the 1 px edge.
static void Card(HDC dc, const RECT& r, int radius)
{
    HBRUSH b = CreateSolidBrush(kPanel2);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    Hairline(dc, r, radius);
}

// The car's glow: the accent laid softly around it (as VestaHome's saver spot), straight into the frame's pixels.
static void Glow(int cx, int cy)
{
    static unsigned char w[61][61];   // the weight, 0..115, made once
    static bool made;
    if (!made) {
        for (int y = 0; y < 61; y++)
            for (int x = 0; x < 61; x++) {
                double d = sqrt((double)((x - 30) * (x - 30) + (y - 30) * (y - 30))) / 30;
                w[y][x] = (unsigned char)(d >= 1 ? 0 : 115 * (1 - d) * (1 - d));
            }
        made = true;
    }
    if (!s_backBits)
        return;
#ifndef UNDER_CE
    GdiFlush();
#endif
    int ar = GetRValue(kOrange), ag = GetGValue(kOrange), ab = GetBValue(kOrange), stride = BackStride();
    for (int y = 0; y < 61; y++) {
        int sy = cy - 30 + y;
        if (sy < 0 || sy >= s_ch)
            continue;
        unsigned short* row = s_backBits + (s_ch - 1 - sy) * stride;
        for (int x = 0; x < 61; x++) {
            int sx = cx - 30 + x, a = w[y][x];
            if (!a || sx < 0 || sx >= s_cw)
                continue;
            unsigned short d = row[sx];
            int r = (d >> 8) & 0xF8, g = (d >> 3) & 0xFC, b = (d << 3) & 0xF8;
            row[sx] = RGB565(r + (ar - r) * a / 255, g + (ag - g) * a / 255, b + (ab - b) * a / 255);
        }
    }
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
    if (fresh)
        Glow(px, py);
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

// ---- the map: a ring of tiles. The ring canvas is kRing x kRing tiles of the view's zoom; tile (x, y) always
// has its own place, (x mod kRing, y mod kRing), so a world pixel (wx, wy) is the canvas pixel
// (wx mod kRingPx, wy mod kRingPx) and the view moving copies nothing. A slot is drawn only when a tile comes
// into it or what it shows changes: the exact tile instead of a coarser one, new traffic, night, zoom (the
// slot's signature). 5 tiles: any rotation of 800 x 480 fits a circle of 933 px, plus the 256 px the view
// center moves within its tile. Traffic and night are put into the slot once, not on every frame; a frame is
// then only the rotation (heading-up, and only when the view, the angle or a slot changed) or 1-4 blits.
static const int kRing = 5, kRingPx = kRing * TILE;   // 1280 x 1280, 3.2 MB
static Image     s_ring;
static unsigned short* s_ringBits;
struct RingSlot { int x, y; DWORD sig; bool used; };
static RingSlot  s_slot[kRing][kRing];
static DWORD     s_ringStamp;         // grows when a slot was drawn: the rotated frame is out of date
static DWORD     s_rotStamp = 0xFFFFFFFF;
static double    s_rotVx, s_rotVy, s_rotAngle;
static int       s_statSlots;         // slots drawn since the last stat line

static int Mod(int a, int m)
{
    int r = a % m;
    return r < 0 ? r + m : r;
}

static DWORD Mix(DWORD h, DWORD v)
{
    return (h ^ v) * 16777619u;
}

// RGB565 -> night color: lightness inverted with hue and saturation kept (light background turns dark,
// traffic stays green/yellow/red), then dimmed. Built once, the slots use it when drawn at night.
static const int kNightDimPct = 80;

static const unsigned short* NightLut()
{
    if (!s_nightLut) {
        s_nightLut = (unsigned short*)malloc(65536 * sizeof(unsigned short));
        if (!s_nightLut)
            return NULL;
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
    return s_nightLut;
}

// A traffic image of the view's zoom with its place in OSM world pixels. 2GIS tiles share the OSM grid;
// Yandex images are in elliptical Mercator: their place comes from geographic coordinates, the vertical scale
// differs slightly (sy = world px per image row).
struct OvPlace { const CacheEntry* e; double left, top, right, bottom, sy; };

static int OverlayPlaces(CacheEntry** list, int n, OvPlace* out)
{
    int src = TrafficSource(), m = 0;
    for (int i = 0; i < n; i++) {
        const CacheEntry* e = list[i];
        if (e->req.src != src || e->req.z != g_cfg.z || !e->ov)
            continue;
        OvPlace& o = out[m++];
        o.e = e;
        if (src == SRC_2GIS) {
            o.left = (double)e->req.x * TILE;
            o.top = (double)e->req.y * TILE;
            o.sy = 1;
        } else {
            double lon, lat, cx, cy, bottom, unused;
            double yx = (double)e->req.x * YA_W, yy = (double)e->req.y * YA_H;
            WorldToGeoYa(yx, yy, g_cfg.z, &lon, &lat);
            GeoToWorld(lon, lat, g_cfg.z, &cx, &cy);
            WorldToGeoYa(yx, yy - YA_H / 2.0, g_cfg.z, &lon, &lat);
            GeoToWorld(lon, lat, g_cfg.z, &unused, &o.top);
            WorldToGeoYa(yx, yy + YA_H / 2.0, g_cfg.z, &lon, &lat);
            GeoToWorld(lon, lat, g_cfg.z, &unused, &bottom);
            o.left = cx - YA_W / 2.0;
            o.sy = (bottom - o.top) / e->ov->h;
        }
        o.right = o.left + e->ov->w;
        o.bottom = o.top + e->ov->h * o.sy;
    }
    return m;
}

// OSM tiles in memory that cover tile (tx, ty) of the view's zoom: the exact one alone when it is here, otherwise
// the other levels (coarser scaled up, finer scaled down), the farthest levels first so the nearest end on top.
static int BaseCandidates(CacheEntry** list, int n, int tx, int ty, const CacheEntry** out)
{
    MapRequest k = { SRC_OSM, g_cfg.z, tx, ty };
    CacheEntry* exact = CacheFind(k, false);
    if (exact) {
        out[0] = exact;
        return 1;
    }
    int m = 0;
    for (int i = 0; i < n && m < CACHE_MEM; i++) {
        const CacheEntry* e = list[i];
        int dz = g_cfg.z - e->req.z;
        if (e->req.src != SRC_OSM || abs(dz) > kMaxParentLevels)
            continue;
        double f = pow(2.0, dz), l = e->req.x * TILE * f, t = e->req.y * TILE * f, sz = TILE * f;
        if (l >= (tx + 1) * TILE || t >= (ty + 1) * TILE || l + sz <= tx * TILE || t + sz <= ty * TILE)
            continue;
        out[m++] = e;
    }
    for (int i = 1; i < m; i++)
        for (int j = i; j > 0 && abs(out[j]->req.z - g_cfg.z) > abs(out[j - 1]->req.z - g_cfg.z); j--) {
            const CacheEntry* t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return m;
}

// What slot (tx, ty) would show now: when it differs from the slot's, the slot is drawn again.
static DWORD SlotSig(int tx, int ty, bool inWorld, const CacheEntry** base, int nb, const OvPlace* ov, int no)
{
    DWORD h = Mix(Mix(Mix(2166136261u, g_cfg.z), tx), ty);
    h = Mix(Mix(h, inWorld), g_cfg.night);
    for (int i = 0; i < nb; i++)
        h = Mix(Mix(Mix(Mix(Mix(h, base[i]->req.z), base[i]->req.x), base[i]->req.y), base[i]->fetchTick),
                (DWORD)(size_t)base[i]->img.bmp);
    if (g_cfg.traffic) {
        h = Mix(h, 0x7AFF + TrafficSource());
        for (int i = 0; i < no; i++)
            if (ov[i].left < (tx + 1) * TILE && ov[i].right > tx * TILE && ov[i].top < (ty + 1) * TILE &&
                ov[i].bottom > ty * TILE)
                h = Mix(Mix(Mix(Mix(h, ov[i].e->req.x), ov[i].e->req.y), ov[i].e->fetchTick), (DWORD)(size_t)ov[i].e->ov);
    }
    return h;
}

// A traffic image blended into the ring, clipped to `clip` (ring pixels); (left, top) its ring position, sy ring
// px per image row, 1 px per image px horizontally.
static void BlendRing(const Overlay* ov, double left, double top, double sy, const RECT& clip)
{
    int ix = (int)ceil(left);
    int y0 = (int)ceil(top), y1 = (int)floor(top + ov->h * sy);
    if (y0 < clip.top) y0 = clip.top;
    if (y1 > clip.bottom) y1 = clip.bottom;
    for (int y = y0; y < y1; y++) {
        int oy = (int)((y - top) / sy);
        if (oy < 0 || oy >= ov->h)
            continue;
        const unsigned char* p = (const unsigned char*)ov + ov->rowOfs[oy];
        int runs = *(const unsigned short*)p;
        p += 4;
        unsigned short* drow = s_ringBits + (kRingPx - 1 - y) * kRingPx;
        int x = ix;
        for (int r = 0; r < runs; r++) {
            const unsigned short* rh = (const unsigned short*)p;
            x += rh[0];
            int count = rh[1];
            const unsigned char* s = p + 4;
            p += 4 + count * 4;
            for (int i = 0; i < count; i++, x++, s += 4) {
                if (x < clip.left || x >= clip.right)
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

// Draws slot (tx, ty): the background, the base map, the traffic, night.
static void DrawSlot(HDC rdc, HDC src, int tx, int ty, bool inWorld, const CacheEntry** base, int nb,
                     const OvPlace* ov, int no)
{
    int ox = Mod(tx, kRing) * TILE, oy = Mod(ty, kRing) * TILE;
    RECT rc = { ox, oy, ox + TILE, oy + TILE };
    HBRUSH bg = CreateSolidBrush(kMapBg);
    FillRect(rdc, &rc, bg);
    DeleteObject(bg);
    if (inWorld && nb) {
        HRGN rgn = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
        SelectClipRgn(rdc, rgn);
        for (int i = 0; i < nb; i++) {
            const CacheEntry* e = base[i];
            double f = pow(2.0, g_cfg.z - e->req.z);
            double left = ox + e->req.x * TILE * f - tx * TILE, top = oy + e->req.y * TILE * f - ty * TILE;
            double size = TILE * f;
            HGDIOBJ old = SelectObject(src, e->img.bmp);
            if (f == 1) {
                BitBlt(rdc, Round(left), Round(top), TILE, TILE, src, 0, 0, SRCCOPY);
            } else {   // only the part over the slot: a coarse tile scaled 2^6 times would be a 16000 px blit
                int sx0 = (int)floor(((left < rc.left ? rc.left : left) - left) / f);
                int sy0 = (int)floor(((top < rc.top ? rc.top : top) - top) / f);
                int sx1 = (int)ceil(((left + size > rc.right ? rc.right : left + size) - left) / f);
                int sy1 = (int)ceil(((top + size > rc.bottom ? rc.bottom : top + size) - top) / f);
                if (sx1 > TILE) sx1 = TILE;
                if (sy1 > TILE) sy1 = TILE;
                int dx0 = Round(left + sx0 * f), dy0 = Round(top + sy0 * f);
                StretchBlt(rdc, dx0, dy0, Round(left + sx1 * f) - dx0, Round(top + sy1 * f) - dy0,
                           src, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
            }
            SelectObject(src, old);
        }
        SelectClipRgn(rdc, NULL);
        DeleteObject(rgn);
    }
#ifndef UNDER_CE
    GdiFlush();   // desktop GDI may batch drawing into the DIB
#endif
    if (inWorld && g_cfg.traffic)
        for (int i = 0; i < no; i++)
            if (ov[i].left < (tx + 1) * TILE && ov[i].right > tx * TILE && ov[i].top < (ty + 1) * TILE &&
                ov[i].bottom > ty * TILE)
                BlendRing(ov[i].e->ov, ox + ov[i].left - tx * TILE, oy + ov[i].top - ty * TILE, ov[i].sy, rc);
    const unsigned short* lut = g_cfg.night ? NightLut() : NULL;
    if (lut)
        for (int y = rc.top; y < rc.bottom; y++) {
            unsigned short* p = s_ringBits + (kRingPx - 1 - y) * kRingPx + rc.left;
            for (int x = 0; x < TILE; x++)
                p[x] = lut[p[x]];
        }
    s_statSlots++;
}

// The slots under the (rotated) window, brought up to date. Returns how many of them show some map.
static int RingUpdate(HDC dc, double vx, double vy, double angle)
{
    double a = angle * M_PI / 180.0, c = cos(a), s = sin(a);
    double hw = g_cfg.mapW / 2.0, hh = g_cfg.mapH / 2.0;
    double ex = hw * fabs(c) + hh * fabs(s), ey = hw * fabs(s) + hh * fabs(c);   // the window's bounding box
    int i0 = (int)floor((vx - ex) / TILE), i1 = (int)floor((vx + ex) / TILE);
    int j0 = (int)floor((vy - ey) / TILE), j1 = (int)floor((vy + ey) / TILE);
    if (i1 - i0 >= kRing) i1 = i0 + kRing - 1;   // can't be with 800 x 480; never two tiles in one slot
    if (j1 - j0 >= kRing) j1 = j0 + kRing - 1;
    int tiles = 1 << g_cfg.z;
    CacheEntry* list[CACHE_MEM];
    int n = CacheList(list, CACHE_MEM);
    OvPlace ov[CACHE_MEM];
    int no = g_cfg.traffic ? OverlayPlaces(list, n, ov) : 0;
    HDC rdc = NULL, src = NULL;
    HGDIOBJ old = NULL;
    int shown = 0;
    for (int ty = j0; ty <= j1; ty++)
        for (int tx = i0; tx <= i1; tx++) {
            bool inWorld = tx >= 0 && ty >= 0 && tx < tiles && ty < tiles;
            const CacheEntry* base[CACHE_MEM];
            int nb = inWorld ? BaseCandidates(list, n, tx, ty, base) : 0;
            if (nb)
                shown++;
            DWORD sig = SlotSig(tx, ty, inWorld, base, nb, ov, no);
            RingSlot& sl = s_slot[Mod(ty, kRing)][Mod(tx, kRing)];
            if (sl.used && sl.x == tx && sl.y == ty && sl.sig == sig)
                continue;   // the same as it is
            if (!rdc) {
                rdc = CreateCompatibleDC(dc);
                src = CreateCompatibleDC(dc);
                old = SelectObject(rdc, s_ring.bmp);
            }
            DrawSlot(rdc, src, tx, ty, inWorld, base, nb, ov, no);
            sl.x = tx;
            sl.y = ty;
            sl.sig = sig;
            sl.used = true;
            s_ringStamp++;
        }
    if (rdc) {
        SelectObject(rdc, old);
        DeleteDC(rdc);
        DeleteDC(src);
    }
    return shown;
}

// Heading-up: the window turned by -angle out of the ring (16.16 fixed point, the ring wraps), into s_rotated.
static void RotateRing(double vx, double vy, double angle)
{
    const int W = s_rotated.w, H = s_rotated.h, R = kRingPx;
    const int ostride = ((W * 2 + 3) & ~3) / 2;
    double a = angle * M_PI / 180.0, ca = cos(a), sa = sin(a);
    double cx = fmod(vx, (double)R), cy = fmod(vy, (double)R);   // the view center in the ring
    if (cx < 0) cx += R;
    if (cy < 0) cy += R;
    long dx = (long)(ca * 65536), dy = (long)(sa * 65536);
    for (int y = 0; y < H; y++) {
        // screen offset s -> world offset w = Rot(angle) s, then the ring's pixel
        double sx = -W / 2.0, sy = y - H / 2.0;
        long wx = (long)((sx * ca - sy * sa + cx) * 65536);
        long wy = (long)((sx * sa + sy * ca + cy) * 65536);
        unsigned short* d = s_rotatedBits + (H - 1 - y) * ostride;
        for (int x = 0; x < W; x++, wx += dx, wy += dy) {
            int px = (int)(wx >> 16), py = (int)(wy >> 16);
            if (px < 0) px += R; else if (px >= R) px -= R;
            if (py < 0) py += R; else if (py >= R) py -= R;
            d[x] = s_ringBits[(R - 1 - py) * R + px];
        }
    }
}

// North-up: the window straight out of the ring, in 1-4 pieces where it wraps.
static void RingBlit(HDC dc, const RECT& mr, double vx, double vy)
{
    int W = mr.right - mr.left, H = mr.bottom - mr.top;
    int wl = (int)floor(vx - W / 2.0 + 0.5), wt = (int)floor(vy - H / 2.0 + 0.5);
    HDC src = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(src, s_ring.bmp);
    for (int y = 0; y < H;) {
        int ry = Mod(wt + y, kRingPx), h = kRingPx - ry < H - y ? kRingPx - ry : H - y;
        for (int x = 0; x < W;) {
            int rx = Mod(wl + x, kRingPx), w = kRingPx - rx < W - x ? kRingPx - rx : W - x;
            BitBlt(dc, mr.left + x, mr.top + y, w, h, src, rx, ry, SRCCOPY);
            x += w;
        }
        y += h;
    }
    SelectObject(src, old);
    DeleteDC(src);
}

// OSM licence: attribution must be visible on the map, not hidden behind controls (above the bottom bar
// while it is up). At night in the night map's colours.
static void DrawAttribution(HDC dc, RECT mr)
{
    if (s_dockShown)
        mr.bottom = kDockY + s_dockOff;
    const wchar_t* s = !g_cfg.traffic ? L"© OpenStreetMap contributors"
                     : g_cfg.trafficSrc == TRF_2GIS ? L"© OpenStreetMap contributors, пробки © 2ГИС"
                     : L"© OpenStreetMap contributors, пробки © Яндекс";
    SelectObject(dc, s_fontTiny);
    RECT rc = { 0, 0, 0, 0 };
    DrawText(dc, s, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    RECT box = { mr.right - rc.right - 8, mr.bottom - rc.bottom - 4, mr.right, mr.bottom };
    HBRUSH b = CreateSolidBrush(g_cfg.night ? RGB(20, 20, 20) : RGB(255, 255, 255));
    FillRect(dc, &box, b);
    DeleteObject(b);
    Text(dc, s_fontTiny, g_cfg.night ? RGB(160, 160, 160) : RGB(60, 60, 60), box.left + 4, box.top, box.right - 4,
         box.bottom, DT_RIGHT, s);
}

// ---- the route: a blue line with a dark edge, the destination as a ring. Segments are clipped to a box a little
// larger than the screen (GDI coordinates stay small), points closer than 2 px to the last one are left out.
static POINT* s_rpts;
static int    s_rptCap;

static bool ClipSeg(double* x0, double* y0, double* x1, double* y1, double l, double t, double r, double b)
{
    double dx = *x1 - *x0, dy = *y1 - *y0, u0 = 0, u1 = 1;
    double pq[4][2] = { { -dx, *x0 - l }, { dx, r - *x0 }, { -dy, *y0 - t }, { dy, b - *y0 } };
    for (int i = 0; i < 4; i++) {
        double pp = pq[i][0], q = pq[i][1];
        if (pp == 0) {
            if (q < 0)
                return false;
            continue;
        }
        double u = q / pp;
        if (pp < 0) {
            if (u > u1)
                return false;
            if (u > u0)
                u0 = u;
        } else {
            if (u < u0)
                return false;
            if (u < u1)
                u1 = u;
        }
    }
    double ax = *x0, ay = *y0;
    *x0 = ax + u0 * dx;
    *y0 = ay + u0 * dy;
    *x1 = ax + u1 * dx;
    *y1 = ay + u1 * dy;
    return true;
}

// Traffic on the route (the user's wish 2026-10-26: the line hid it): each point of the line takes the colour of
// the traffic overlay under it, looked up a few pixels to the right of the way (both providers draw a direction on
// its right side); no traffic there - the route's blue. Looked up again only when a slot was drawn (new traffic,
// a new view) - per point, while it is in sight.
static const COLORREF kRouteBlue = RGB(66, 146, 255), kRouteEdge = RGB(18, 52, 128);
static const COLORREF kTfFree = RGB(40, 190, 70), kTfSlow = RGB(255, 196, 0), kTfJam = RGB(232, 40, 30),
                      kTfStand = RGB(140, 16, 16);
static COLORREF* s_rcol;
static DWORD*    s_rcolSig;
static int*      s_ridx;
static int       s_rcolCap;
static const CacheEntry* s_tfLast;

static bool OvPixel(const Overlay* ov, int x, int y, COLORREF* out)
{
    if (x < 0 || y < 0 || x >= ov->w || y >= ov->h)
        return false;
    const unsigned char* p = (const unsigned char*)ov + ov->rowOfs[y];
    int runs = *(const unsigned short*)p, sx = 0;
    p += 4;
    for (int r = 0; r < runs; r++) {
        const unsigned short* rh = (const unsigned short*)p;
        sx += rh[0];
        int count = rh[1];
        if (x >= sx && x < sx + count) {
            const unsigned char* c = p + 4 + (x - sx) * 4;
            int mx = c[0] > c[1] ? c[0] : c[1], mn = c[0] < c[1] ? c[0] : c[1];
            mx = c[2] > mx ? c[2] : mx;
            mn = c[2] < mn ? c[2] : mn;
            if (c[3] < 160 || mx - mn < 60)   // see-through, or the lines' white / grey edge: not a traffic colour
                return false;
            // one of the four traffic colours, clean (the lines' anti-aliased pixels come out pale)
            *out = c[1] > c[0] + 30 ? kTfFree : c[1] > 140 ? kTfSlow : c[0] > 150 ? kTfJam : kTfStand;
            return true;
        }
        sx += count;
        p += 4 + count * 4;
    }
    return false;
}

static COLORREF TrafficAt(double lon, double lat, double nx, double ny)
{
    int src = TrafficSource();
    bool ya = src == SRC_YANDEX;
    for (int dz = 0; dz <= 3; dz++) {   // the view's zoom, then the coarser ones still in memory
    int z = g_cfg.z - dz;
    if (z < 2)
        break;
    double wx, wy;
    if (ya)
        GeoToWorldYa(lon, lat, z, &wx, &wy);
    else
        GeoToWorld(lon, lat, z, &wx, &wy);
    for (int k = 1; k <= 7; k += 2) {
        double x = wx + nx * k, y = wy + ny * k, ox, oy;
        MapRequest key;
        key.src = src;
        key.z = z;
        if (ya) {   // Yandex: 600 x 450 images centred at (x * 600, y * 450) of its own world pixels
            key.x = (int)floor(x / YA_W + 0.5);
            key.y = (int)floor(y / YA_H + 0.5);
            ox = key.x * (double)YA_W - YA_W / 2.0;
            oy = key.y * (double)YA_H - YA_H / 2.0;
        } else {
            key.x = (int)floor(x / TILE);
            key.y = (int)floor(y / TILE);
            ox = key.x * (double)TILE;
            oy = key.y * (double)TILE;
        }
        const CacheEntry* e = s_tfLast && SameKey(s_tfLast->req, key) ? s_tfLast : CacheFind(key, false);
        if (!e || !e->ov)
            continue;
        s_tfLast = e;
        COLORREF c;
        if (OvPixel(e->ov, (int)(x - ox), (int)(y - oy), &c))
            return c;
    }
    }
    return kRouteBlue;
}

// ---- the alternatives: each one's time "from - to" by the traffic on it (an estimate, the user's wish
// 2026-10-26: the choice stays the driver's). A segment's share of the server's time, times a factor of its colour:
// free 1, slow 1.5..2.5, jam 2.5..5, standing 4..10; no traffic seen there - 1 (and the share seen is told).
static void Tile(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius);
static void FmtDist(wchar_t* s, int cap, int m);
static void RouteMarker(HDC dc, int x, int y, COLORREF fill, const wchar_t* t);
struct AltEst { int minS, maxS, jamM, seenPct; DWORD sig; };
static AltEst s_est[3];

static void EstimateLine(const double* ll, int np, double timeS, AltEst& e);

static void Estimate(int k, DWORD sig)
{
    AltEst& e = s_est[k];
    if (e.sig == sig)
        return;
    memset(&e, 0, sizeof(e));
    e.sig = sig;
    const double* ll;
    int np;
    double timeS, distM;
    if (RouteAltGet(k, &ll, &np, &timeS, &distM) && np >= 2)
        EstimateLine(ll, np, timeS, e);
}

static void EstimateLine(const double* ll, int np, double timeS, AltEst& e)
{
    double kx = 111320 * cos(ll[1] * M_PI / 180), total = 0;
    for (int i = 1; i < np; i++) {
        double dx = (ll[i * 2] - ll[i * 2 - 2]) * kx, dy = (ll[i * 2 + 1] - ll[i * 2 - 1]) * 110540;
        total += sqrt(dx * dx + dy * dy);
    }
    double mn = 0, mx = 0, seen = 0, jam = 0;
    for (int i = 1; i < np && total > 0; i++) {
        double dx = (ll[i * 2] - ll[i * 2 - 2]) * kx, dy = (ll[i * 2 + 1] - ll[i * 2 - 1]) * 110540;
        double len = sqrt(dx * dx + dy * dy), t = timeS * len / total, f0 = 1, f1 = 1;
        if (len > 0 && g_cfg.traffic) {
            // the right of the way in world pixels: north is -y
            COLORREF c = TrafficAt(ll[i * 2 - 2], ll[i * 2 - 1], -dy / len, -dx / len);
            if (c != kRouteBlue)
                seen += len;
            if (c == kTfSlow) {
                f0 = 1.5;
                f1 = 2.5;
            } else if (c == kTfJam) {
                f0 = 2.5;
                f1 = 5;
                jam += len;
            } else if (c == kTfStand) {
                f0 = 4;
                f1 = 10;
                jam += len;
            }
        }
        mn += t * f0;
        mx += t * f1;
    }
    e.minS = (int)mn;
    e.maxS = (int)mx;
    e.jamM = (int)jam;
    e.seenPct = total > 0 ? (int)(seen * 100 / total) : 0;
}

static bool CardRect(int k, RECT* r)
{
    int n = RouteAlts();
    if (n < 2 || k < 0 || k >= n)
        return false;
    int w = (s_cw - 32 - (n - 1) * 8) / n;
    SetRect(r, 16 + k * (w + 8), 392, 16 + k * (w + 8) + w, s_ch - 8);
    return true;
}

static void FmtMin(wchar_t* s, int cap, int sec)
{
    int m = (sec + 59) / 60;
    if (m >= 60)
        _snwprintf(s, cap, L"%d ч %02d мин", m / 60, m % 60);
    else
        _snwprintf(s, cap, L"%d мин", m);
    s[cap - 1] = 0;
}

static void DrawCards(HDC dc)
{
    DWORD sig = Mix(Mix(Mix(s_ringStamp, g_cfg.z), g_cfg.trafficSrc), g_cfg.traffic) | 1;
    RECT r;
    for (int k = 0; CardRect(k, &r); k++) {
        Estimate(k, sig);
        const AltEst& e = s_est[k];
        const double* ll;
        int np;
        double timeS, distM;
        RouteAltGet(k, &ll, &np, &timeS, &distM);
        bool cur = k == RouteAltCur(), down = s_planPressed == 70 + k;
        Card(dc, r, 14);
        if (cur)   // the chosen one: the accent's border
            Outline(dc, r, kOrange, 14);
        if (down)
            Tile(dc, r, kOrange, kOrange, 28);
        wchar_t t[48], d[24], line[96];
        FmtMin(t, 48, (int)timeS);
        FmtDist(d, 24, (int)distM);
        _snwprintf(line, 96, L"%d · %s · %s", k + 1, t, d);
        line[95] = 0;
        Text(dc, s_fontLabel, down ? kInk : kText, r.left + 12, r.top + 4, r.right - 8, r.top + 28, DT_LEFT, line);
        // the range by the traffic seen on it
        COLORREF c = kTextDim;
        if (!g_cfg.traffic) {
            wcscpy(line, L"пробки выключены");
        } else if (!e.seenPct) {
            wcscpy(line, L"пробок на пути не видно");
        } else {
            wchar_t a[24], b[24];
            FmtMin(a, 24, e.minS);
            FmtMin(b, 24, e.maxS);
            if (e.maxS - e.minS < 120)
                _snwprintf(line, 96, L"с пробками ~%s", a);
            else if (e.maxS < 3600)
                _snwprintf(line, 96, L"с пробками %d–%d мин", (e.minS + 59) / 60, (e.maxS + 59) / 60);
            else
                _snwprintf(line, 96, L"с пробками %s – %s", a, b);
            c = e.jamM > 300 ? kTextErr : e.maxS > timeS * 1.3 ? kOrange : RGB(120, 220, 120);
        }
        line[95] = 0;
        Text(dc, s_fontSmall, down ? kInk : c, r.left + 12, r.top + 28, r.right - 8, r.top + 48, DT_LEFT, line);
        if (g_cfg.traffic && e.seenPct) {
            FmtDist(d, 24, e.jamM);
            if (e.jamM > 300)
                _snwprintf(line, 96, e.seenPct < 90 ? L"пробка %s · видно %d%% пути" : L"пробка %s", d, e.seenPct);
            else
                _snwprintf(line, 96, e.seenPct < 90 ? L"без сильных пробок · видно %d%% пути"
                                                    : L"без сильных пробок", e.seenPct);
            line[95] = 0;
            Text(dc, s_fontSmall, down ? kInk : kTextDim, r.left + 12, r.top + 48, r.right - 8, r.bottom - 4,
                 DT_LEFT, line);
        }
    }
}

// The alternatives' lines under the chosen one: grey, a number in the middle.
static void DrawAltLines(HDC dc, double cx, double cy, double vx, double vy, double angle)
{
    int n = RouteAlts();
    if (n < 2)
        return;
    double ra = angle * M_PI / 180.0, rc = cos(ra), rs = sin(ra);
    double l = g_cfg.mapX - 50, t = g_cfg.mapY - 50, r = g_cfg.mapX + g_cfg.mapW + 50, b = g_cfg.mapY + g_cfg.mapH + 50;
    for (int k = 0; k < n; k++) {
        const double* ll;
        int np;
        double timeS, distM;
        if (k == RouteAltCur() || !RouteAltGet(k, &ll, &np, &timeS, &distM) || np < 2)
            continue;
        HPEN edge = CreatePen(PS_SOLID, 9, RGB(70, 76, 86)), in = CreatePen(PS_SOLID, 5, RGB(160, 170, 186));
        double px = 0, py = 0;
        for (int pass = 0; pass < 2; pass++) {
            HGDIOBJ op = SelectObject(dc, pass ? in : edge);
            for (int i = 0; i < np; i++) {
                double gx, gy;
                GeoToWorld(ll[i * 2], ll[i * 2 + 1], g_cfg.z, &gx, &gy);
                double dx = gx - vx, dy = gy - vy, x = cx + dx * rc + dy * rs, y = cy - dx * rs + dy * rc;
                if (i > 0) {
                    double x0 = px, y0 = py, x1 = x, y1 = y;
                    if (ClipSeg(&x0, &y0, &x1, &y1, l, t, r, b) && (fabs(x1 - x0) + fabs(y1 - y0) >= 1 || i == np - 1)) {
                        POINT seg[2] = { { Round(x0), Round(y0) }, { Round(x1), Round(y1) } };
                        Polyline(dc, seg, 2);
                    } else if (fabs(x1 - x0) + fabs(y1 - y0) < 1) {
                        continue;   // (too short: the next point continues from this one's start)
                    }
                }
                px = x;
                py = y;
            }
            SelectObject(dc, op);
        }
        DeleteObject(edge);
        DeleteObject(in);
        // its number at the middle of the line
        const double* m = ll + (np / 2) * 2;
        double gx, gy;
        GeoToWorld(m[0], m[1], g_cfg.z, &gx, &gy);
        double dx = gx - vx, dy = gy - vy;
        wchar_t num[4];
        _snwprintf(num, 4, L"%d", k + 1);
        RouteMarker(dc, Round(cx + dx * rc + dy * rs), Round(cy - dx * rs + dy * rc), RGB(90, 98, 110), num);
    }
}

static void RoutePass(HDC dc, const int* runs, int nr, int width, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ op = SelectObject(dc, pen);
    for (int i = 0; i < nr; i++)
        Polyline(dc, s_rpts + runs[i * 2], runs[i * 2 + 1]);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// The inner line, piece by piece of one colour.
static void RouteColours(HDC dc, const int* runs, int nr, int width)
{
    for (int i = 0; i < nr; i++) {
        int a = runs[i * 2], n = runs[i * 2 + 1];
        for (int j = 0; j + 1 < n;) {
            COLORREF c = s_rcol[s_ridx[a + j]];
            int k = j + 1;
            while (k + 1 < n && s_rcol[s_ridx[a + k]] == c)
                k++;
            HPEN pen = CreatePen(PS_SOLID, width, c);
            HGDIOBJ op = SelectObject(dc, pen);
            Polyline(dc, s_rpts + a + j, k - j + 1);
            SelectObject(dc, op);
            DeleteObject(pen);
            j = k;
        }
    }
}

// A round marker with a letter or a digit (the start, the stops).
static void RouteMarker(HDC dc, int x, int y, COLORREF fill, const wchar_t* t)
{
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    Ellipse(dc, x - 13, y - 13, x + 14, y + 14);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
    if (t)
        Text(dc, s_fontCap, RGB(255, 255, 255), x - 13, y - 13, x + 14, y + 14, DT_CENTER, t);
}

static void DrawRoute(HDC dc, double cx, double cy, double vx, double vy, double angle)
{
    DrawAltLines(dc, cx, cy, vx, vy, angle);
    double ra = angle * M_PI / 180.0, rc = cos(ra), rs = sin(ra);
    double dlon, dlat;
    bool dest = RouteDest(&dlon, &dlat);
    const double* ll;
    int np = RoutePoints(&ll);
    if (np >= 2 && s_rptCap < np * 2) {
        free(s_rpts);
        free(s_ridx);
        s_rptCap = np * 2;
        s_rpts = (POINT*)malloc(s_rptCap * sizeof(POINT));
        s_ridx = (int*)malloc(s_rptCap * sizeof(int));
        if (!s_rpts || !s_ridx)
            s_rptCap = 0;
    }
    if (np >= 2 && s_rcolCap < np) {
        free(s_rcol);
        free(s_rcolSig);
        s_rcolCap = np;
        s_rcol = (COLORREF*)malloc(np * sizeof(COLORREF));
        s_rcolSig = (DWORD*)calloc(np, sizeof(DWORD));
        if (!s_rcol || !s_rcolSig)
            s_rcolCap = 0;
    }
    if (np >= 2 && s_rpts && s_ridx && s_rcolCap >= np) {
        // the colours' signature: the slots drawn (traffic came), the zoom, the provider, this route
        DWORD sig = Mix(Mix(Mix(Mix(s_ringStamp, g_cfg.z), g_cfg.trafficSrc), g_cfg.traffic), (DWORD)(size_t)ll) | 1;
        double l = g_cfg.mapX - 50, t = g_cfg.mapY - 50, r = g_cfg.mapX + g_cfg.mapW + 50, b = g_cfg.mapY + g_cfg.mapH + 50;
        static int runs[512];
        int nr = 0, n = 0, runStart = 0;
        bool open = false;
        double px = 0, py = 0;
        for (int i = 0; i < np; i++) {
            double gx, gy;
            GeoToWorld(ll[i * 2], ll[i * 2 + 1], g_cfg.z, &gx, &gy);
            double dx = gx - vx, dy = gy - vy;
            double x = cx + dx * rc + dy * rs, y = cy - dx * rs + dy * rc;
            if (i > 0) {
                double x0 = px, y0 = py, x1 = x, y1 = y;
                bool in = ClipSeg(&x0, &y0, &x1, &y1, l, t, r, b);
                if (in) {
                    if (s_rcolSig[i - 1] != sig) {   // the segment's colour: at its start, to the right of it
                        int j = i - 1;
                        COLORREF c = kRouteBlue;
                        if (g_cfg.traffic) {
                            double ax, ay;
                            GeoToWorld(ll[j * 2], ll[j * 2 + 1], g_cfg.z, &ax, &ay);
                            double ex = gx - ax, ey = gy - ay, len = sqrt(ex * ex + ey * ey);
                            if (len > 0)
                                c = TrafficAt(ll[j * 2], ll[j * 2 + 1], -ey / len, ex / len);
                        }
                        s_rcol[j] = c;
                        s_rcolSig[j] = sig;
                    }
                    if (!open) {
                        runStart = n;
                        s_rpts[n].x = Round(x0);
                        s_rpts[n].y = Round(y0);
                        s_ridx[n] = i - 1;
                        n++;
                        open = true;
                    }
                    POINT q = { Round(x1), Round(y1) };
                    const POINT& last = s_rpts[n - 1];
                    bool cut = x1 != x || y1 != y;   // leaves the box here
                    // a point is left out when it is close to the last one - unless the colour changes there
                    if (cut || abs(q.x - last.x) + abs(q.y - last.y) >= 2 || i == np - 1 ||
                        (i + 1 < np && s_rcolSig[i] == sig && s_rcol[i] != s_rcol[s_ridx[n - 1]])) {
                        s_ridx[n] = i;
                        s_rpts[n++] = q;
                    }
                    if (cut) {
                        if (nr < 256) {
                            runs[nr * 2] = runStart;
                            runs[nr * 2 + 1] = n - runStart;
                            nr++;
                        }
                        open = false;
                    }
                } else if (open) {
                    if (nr < 256) {
                        runs[nr * 2] = runStart;
                        runs[nr * 2 + 1] = n - runStart;
                        nr++;
                    }
                    open = false;
                }
            }
            px = x;
            py = y;
        }
        if (open && nr < 256) {
            runs[nr * 2] = runStart;
            runs[nr * 2 + 1] = n - runStart;
            nr++;
        }
        RoutePass(dc, runs, nr, 11, kRouteEdge);
        RouteColours(dc, runs, nr, 7);
    }
    // the markers: a fixed start "А", the stops 1..3, the destination, the long press's point
    RoutePlan* plan = RoutePlanGet();
    RouteInfo ri;
    RouteGet(&ri);
    bool on = ri.state == ROUTE_ON || ri.state == ROUTE_BUSY;
    for (int m = 0; m < 6; m++) {
        double lon, lat;
        COLORREF fill = kRouteEdge;
        wchar_t t[4] = L"";
        if (m == 0) {
            if (!on || plan->from.kind != PL_PLACE)
                continue;
            lon = plan->from.lon;
            lat = plan->from.lat;
            fill = RGB(40, 150, 70);
            wcscpy(t, L"А");
        } else if (m <= kMaxVia) {
            if (!on || m > plan->nvia || plan->via[m - 1].kind != PL_PLACE)
                continue;
            lon = plan->via[m - 1].lon;
            lat = plan->via[m - 1].lat;
            fill = kOrange;
            _snwprintf(t, 4, L"%d", m);
        } else if (m == 4) {
            if (!dest)
                continue;
            lon = dlon;
            lat = dlat;
        } else {
            if (!s_lpMenu)
                continue;
            lon = s_lpLon;
            lat = s_lpLat;
            fill = kOrange;
        }
        double gx, gy;
        GeoToWorld(lon, lat, g_cfg.z, &gx, &gy);
        double dx = gx - vx, dy = gy - vy;
        RouteMarker(dc, Round(cx + dx * rc + dy * rs), Round(cy - dx * rs + dy * rc), fill, t[0] ? t : NULL);
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

    // the rotated frame (768 KB at 800 x 480) is kept only while heading-up is on
    bool rotate = g_cfg.follow == FOLLOW_HEADING;
    if (rotate && !s_rotatedBits) {
        s_rotatedBits = ImageCreate(g_cfg.mapW, g_cfg.mapH, &s_rotated);
        s_rotStamp = 0xFFFFFFFF;
    } else if (!rotate && s_rotatedBits) {
        ImageFree(&s_rotated);
        s_rotatedBits = NULL;
    }

    int drawn = 0;
    if (s_ringBits && (!rotate || s_rotatedBits)) {
        drawn = RingUpdate(dc, vx, vy, angle);
        if (rotate) {   // turned again only when something moved
            if (s_rotStamp != s_ringStamp || s_rotVx != vx || s_rotVy != vy || s_rotAngle != angle) {
                RotateRing(vx, vy, angle);
                s_rotStamp = s_ringStamp;
                s_rotVx = vx;
                s_rotVy = vy;
                s_rotAngle = angle;
            }
            Blit(dc, &s_rotated, mr.left, mr.top);
        } else {
            RingBlit(dc, mr, vx, vy);
        }
    }
    if (!drawn)
        Text(dc, s_fontSmall, g_cfg.night ? RGB(160, 160, 160) : RGB(90, 90, 90), mr.left, mr.top, mr.right,
             mr.bottom, DT_CENTER, s_lastErr ? NetErrorText(s_lastErr) : L"Загрузка карты...");

    DrawAttribution(dc, mr);
    DrawRoute(dc, cx, cy, vx, vy, angle);
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
    Tile(dc, r, pressed ? kOrange : kPanel2, pressed ? kOrange : kPanel2, 20);
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
    Tile(dc, box, kInk, kInk, 20);
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
    RECT all = { 0, 0, s_cw, s_ch };
    HBRUSH ink = CreateSolidBrush(kInk);
    FillRect(dc, &all, ink);
    DeleteObject(ink);

    // as VestaHome's screens: the title at the top left, the clock at the right, no line; "back" at the bottom left
    wchar_t s[128];
    Text(dc, s_fontBig, kText, 20, 0, 500, 56, DT_LEFT, L"Настройки карты");
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 128, L"%02d:%02d", t.wHour, t.wMinute);
    Text(dc, s_fontClock, kText, s_cw - 160, 0, s_cw - 16, 56, DT_RIGHT, s);
    BackDisc(dc, s_setPressed == S_BACK);

    RECT panel = { 16, 64, s_cw - 16, 408 };
    Tile(dc, panel, kPanel, kPanel, 32);

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
    Text(dc, s_fontSmall, c, 36, 330, 580, 348, DT_LEFT, s);

    _snwprintf(s, 128, L"NMEA: %d строк, с ошибкой %d", s_gps.lines, s_gps.badLines);
    Text(dc, s_fontSmall, s_gps.lines ? kText : kTextDim, 36, 348, 580, 366, DT_LEFT, s);

    if (s_gps.valid)
        _snwprintf(s, 128, L"Координаты: %.5f, %.5f  (%d спутн.)", s_gps.lat, s_gps.lon, s_gps.sats);
    else
        _snwprintf(s, 128, L"Координат пока нет (%d спутн.)", s_gps.sats);
    s[127] = 0;
    Text(dc, s_fontSmall, s_gps.valid ? kText : kTextDim, 36, 366, 580, 384, DT_LEFT, s);

    if (s_gps.found == 1) {
        _snwprintf(s, 128, L"Найден GPS: %s %lu, настройки сохранены", s_gps.port, s_gps.baud);
        Text(dc, s_fontSmall, RGB(120, 220, 120), 36, 384, 580, 402, DT_LEFT, s);
    } else if (s_gps.found == -1) {
        Text(dc, s_fontSmall, kTextErr, 36, 384, 580, 402, DT_LEFT, L"GPS не найден на COM1..COM9");
    } else {   // the last line as it came (no fix yet), or the hint
        MultiByteToWideChar(CP_ACP, 0, s_gps.last, -1, s, 128);
        s[127] = 0;
        Text(dc, s_fontSmall, kTextDim, 36, 384, 580, 402, DT_LEFT,
             s[0] && !s_gps.valid ? s : L"Нет строк - не тот порт. Строки с ошибкой - не та скорость.");
    }

    _snwprintf(s, 128, L"Запросов: Яндекс %d/%d, 2ГИС %d/%d, OSM %d, поиск и маршрут %d/%d",
               *Counter(SRC_YANDEX), Limit(SRC_YANDEX), *Counter(SRC_2GIS), Limit(SRC_2GIS), g_cfg.osmCount,
               g_cfg.apiCount, g_cfg.apiLimit);
    s[127] = 0;
    Text(dc, s_fontTiny, kTextDim, 92, 418, 592, 442, DT_LEFT, s);
    _snwprintf(s, 128, L"OSMapsCE %s", AppVersion(kRelease));   // VestaCore vversion.h
    Text(dc, s_fontTiny, kTextDim, 92, 442, 592, 466, DT_LEFT, s);
}

// Distance as the bar shows it: metres by 10 (by 50 above 100), km with a tenth up to 10 km.
static void FmtDist(wchar_t* s, int cap, int m)
{
    if (m < 100)
        _snwprintf(s, cap, L"%d м", m / 10 * 10);
    else if (m < 1000)
        _snwprintf(s, cap, L"%d м", m / 50 * 50);
    else if (m < 10000)
        _snwprintf(s, cap, L"%d,%d км", m / 1000, m % 1000 / 100);
    else
        _snwprintf(s, cap, L"%d км", (m + 500) / 1000);
    s[cap - 1] = 0;
}

// The next turn's sign in r: a stem from below and the arrow the way of the turn; the destination - a ring with
// a dot, a roundabout - a ring with the way out up.
static void DrawTurn(HDC dc, const RECT& r, int turn, COLORREF col = kText)
{
    // drawn for a 76 px box; a smaller one: everything scaled (Q: the length times the box over 76)
    int q = r.right - r.left;
#define Q(v) ((v) * q / 76)
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2 + Q(4);
    HPEN pen = CreatePen(PS_SOLID, Q(7) < 3 ? 3 : Q(7), col);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    if (turn == 999) {
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - Q(20), cy - Q(24), cx + Q(20), cy + Q(16));
        SelectObject(dc, br);
        Ellipse(dc, cx - Q(6), cy - Q(10), cx + Q(6), cy + Q(2));
    } else {
        double a;
        int hx, hy;
        if (turn == 998) {
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx - Q(14), cy - Q(8), cx + Q(14), cy + Q(20));
            POINT st[2] = { { cx, cy - Q(8) }, { cx, cy - Q(26) } };
            Polyline(dc, st, 2);
            SelectObject(dc, br);
            a = 0;
            hx = cx;
            hy = cy - Q(26);
        } else if (turn >= 170 || turn <= -170) {   // U-turn: up and back down on the turn's side
            int k = turn > 0 ? 1 : -1;
            POINT u[4] = { { cx - Q(12) * k, r.bottom - Q(10) }, { cx - Q(12) * k, r.top + Q(20) },
                           { cx + Q(12) * k, r.top + Q(20) }, { cx + Q(12) * k, cy + Q(4) } };
            Polyline(dc, u, 4);
            a = M_PI;
            hx = cx + Q(12) * k;
            hy = cy + Q(4);
        } else {
            a = turn * M_PI / 180.0;
            POINT st[3] = { { cx, r.bottom - Q(10) }, { cx, cy },
                            { cx + (int)(sin(a) * Q(22)), cy - (int)(cos(a) * Q(22)) } };
            Polyline(dc, st, 3);
            hx = st[2].x;
            hy = st[2].y;
        }
        // the head: a triangle pointing along a
        double c = cos(a), sn = sin(a);
        POINT h[3] = { { hx + (int)(sn * Q(14)), hy - (int)(c * Q(14)) },
                       { hx + (int)(c * Q(12) - sn * Q(4)), hy + (int)(sn * Q(12) + c * Q(4)) },
                       { hx - (int)(c * Q(12) + sn * Q(4)), hy - (int)(sn * Q(12) - c * Q(4)) } };
        HPEN thin = CreatePen(PS_SOLID, 1, col);
        SelectObject(dc, thin);
        Polygon(dc, h, 3);
        SelectObject(dc, pen);
        DeleteObject(thin);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
#undef Q
}

// The turn (the user 2026-10-26): the big card only before it - 300 m or 20 s at the speed, whichever is more -
// narrower than the mockup's; the rest of the time a compact one, about a quarter of it: the sign and the distance.
// Off the route: the compact card says so in red. Not on (being laid, not found, arrived): a line card.
static void DrawGuide(HDC dc)
{
    RouteInfo ri;
    RouteGet(&ri);
    if (ri.state == ROUTE_NONE)
        return;
    if (ri.state != ROUTE_ON) {
        RECT p = { 16, kStatusH + 12, 400, kStatusH + 64 };
        Card(dc, p, 18);
        Text(dc, s_fontLabel, ri.state == ROUTE_FAIL ? kTextErr : kText, p.left + 18, p.top, p.right - 12, p.bottom,
             DT_LEFT, ri.text);
        return;
    }
    wchar_t d[32], num[16];
    FmtDist(d, 32, ri.nextM);
    const wchar_t* unit = wcsrchr(d, L' ');
    int n = unit ? (int)(unit - d) : (int)wcslen(d);
    wcsncpy(num, d, n);
    num[n] = 0;
    double soonM = s_gps.speedKmh / 3.6 * 20;
    bool big = !ri.off && ri.nextM <= (soonM > 300 ? soonM : 300);
    if (!big) {   // compact: the sign in the accent's square and the distance
        RECT p = { 16, 56, ri.off ? 330 : 182, 112 };
        Card(dc, p, 18);
        RECT sq = { 22, 62, 66, 106 };
        Tile(dc, sq, ri.off ? kTextErr : kOrange, ri.off ? kTextErr : kOrange, 24);
        DrawTurn(dc, sq, ri.turn, kInk);
        Text(dc, ri.off ? s_fontSmall : s_fontKm, ri.off ? kTextErr : kText, 76, 56, p.right - 8, 112, DT_LEFT,
             ri.off ? ri.text : d);
        return;
    }
    // big: 250 px wide
    RECT p = { 16, 56, 266, ri.thenText ? 222 : 188 };
    Card(dc, p, 22);
    RECT sq = { 28, 68, 96, 136 };
    Tile(dc, sq, kOrange, kOrange, 32);
    DrawTurn(dc, sq, ri.turn, kInk);
    SelectObject(dc, s_fontHuge);
    SIZE sz;
    GetTextExtentPoint32(dc, num, n, &sz);
    Text(dc, s_fontHuge, kText, 108, 62, 266, 116, DT_LEFT, num);
    if (unit)
        Text(dc, s_fontKm, kTextDim, 108 + sz.cx + 5, 76, 266, 114, DT_LEFT, unit + 1);
    // "Поверните направо: Московская улица" -> what to do under the distance, the street in the line below
    wchar_t act[112], street[112] = L"";
    wcsncpy(act, ri.text, 111);
    act[111] = 0;
    wchar_t* colon = wcsstr(act, L": ");
    if (colon) {
        wcscpy(street, colon + 2);
        *colon = 0;
    } else {
        wcscpy(street, act);
        act[0] = 0;
    }
    const wchar_t* verb = act;   // "Поверните направо" -> "направо": the sign says the rest
    if (!wcsncmp(act, L"Поверните ", 10))
        verb = act + 10;
    Text(dc, s_fontSmall, kTextDim, 108, 114, 262, 138, DT_LEFT, verb);
    // the street big; a longer one smaller, the longest in two lines
    SelectObject(dc, s_fontStreet);
    GetTextExtentPoint32(dc, street, (int)wcslen(street), &sz);
    HFONT sf = s_fontStreet;
    if (sz.cx > 226) {
        SelectObject(dc, s_fontLabel);
        GetTextExtentPoint32(dc, street, (int)wcslen(street), &sz);
        sf = sz.cx > 226 ? NULL : s_fontLabel;
    }
    if (sf) {
        Text(dc, sf, kText, 28, 144, 258, 180, DT_LEFT, street);
    } else {
        RECT sr = { 28, 142, 258, 184 };
        SelectObject(dc, s_fontSmall);
        SetTextColor(dc, kText);
        DrawText(dc, street, -1, &sr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    if (ri.thenText) {
        RECT l = { 28, 188, 254, 189 };
        HBRUSH b = CreateSolidBrush(kLine);
        FillRect(dc, &l, b);
        DeleteObject(b);
        wchar_t t[128];
        _snwprintf(t, 128, L"затем — %s", ri.thenText);
        t[127] = 0;
        Text(dc, s_fontTiny, kTextDim, 28, 192, 258, 216, DT_LEFT, t);
    }
}

// The trip at the bottom (design v3): the arrival, the minutes (with the range by the traffic seen), the km, and
// "x" - the route off. Not while the bottom bar or the alternatives' cards are up.
static AltEst s_estCur;

static bool TripShown()
{
    RouteInfo ri;
    RouteGet(&ri);
    return ri.state == ROUTE_ON && !s_dockShown && RouteAlts() < 2 && s_pickSlot < 0 && !s_lpMenu;
}

static void DrawTrip(HDC dc)
{
    RouteInfo ri;
    RouteGet(&ri);
    if (s_gps.valid && !s_dockShown && RouteAlts() < 2) {   // the speed, bottom left
        RECT c = { 20, 394, 94, 468 };
        Card(dc, c, 37);
        wchar_t v[8];
        _snwprintf(v, 8, L"%d", (int)(s_gps.speedKmh + 0.5));
        Text(dc, s_fontSpeed, kText, c.left, c.top + 8, c.right, c.top + 46, DT_CENTER, v);
        Text(dc, s_fontCap, kTextDim, c.left, c.top + 44, c.right, c.top + 62, DT_CENTER, L"км/ч");
    }
    if (!TripShown())
        return;
    RECT p = { 196, 400, 604, 466 };
    Card(dc, p, 33);
    SYSTEMTIME t;
    GetLocalTime(&t);
    int at = (t.wHour * 60 + t.wMinute + (ri.leftS + 59) / 60) % (24 * 60);
    wchar_t big[3][24], under[3][40];
    _snwprintf(big[0], 24, L"%02d:%02d", at / 60, at % 60);
    wcscpy(under[0], L"прибытие");
    int mins = (ri.leftS + 59) / 60;
    if (mins >= 60)
        _snwprintf(big[1], 24, L"%d ч %02d", mins / 60, mins % 60);
    else
        _snwprintf(big[1], 24, L"%d мин", mins);
    wcscpy(under[1], L"в пути");
    const double* ll;
    int np = RoutePoints(&ll);
    if (g_cfg.traffic && np > ri.seg + 1) {   // the range by the traffic on the rest of the way
        DWORD sig = Mix(Mix(Mix(s_ringStamp, g_cfg.z), g_cfg.trafficSrc), ri.seg / 10) | 1;
        if (s_estCur.sig != sig) {
            memset(&s_estCur, 0, sizeof(s_estCur));
            s_estCur.sig = sig;
            EstimateLine(ll + ri.seg * 2, np - ri.seg, ri.leftS, s_estCur);
        }
        if (s_estCur.seenPct && s_estCur.maxS - s_estCur.minS >= 120)
            _snwprintf(under[1], 40, L"с пробками %d–%d", (s_estCur.minS + 59) / 60, (s_estCur.maxS + 59) / 60);
    }
    FmtDist(big[2], 24, ri.leftM);
    wcscpy(under[2], L"осталось");
    for (int i = 0; i < 3; i++) {
        int cx = 262 + i * 120;
        Text(dc, s_fontKm, kText, cx - 62, 406, cx + 62, 432, DT_CENTER, big[i]);
        Text(dc, s_fontCap, kTextDim, cx - 62, 432, cx + 62, 452, DT_CENTER, under[i]);
    }
    RECT x = { 552, 413, 592, 453 };
    bool down = s_planPressed == P_TRIP_X;
    Tile(dc, x, down ? kOrange : kPanel2, down ? kOrange : kPanel2, 40);
    HPEN pen = CreatePen(PS_SOLID, 3, down ? kInk : kText);
    HGDIOBJ op = SelectObject(dc, pen);
    POINT a[2] = { { 565, 426 }, { 580, 441 } }, b2[2] = { { 580, 426 }, { 565, 441 } };
    Polyline(dc, a, 2);
    Polyline(dc, b2, 2);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// ---- the search screen
static int KbRowLen(int row)
{
    return row < 4 ? (int)wcslen(kKeys[s_kbLang][row]) : 4;
}

// Where control id of the search screen is; false if it is not there now.
static bool SearchRect(int id, RECT* r)
{
    if (id == K_BACK) {
        SetRect(r, 8, 8, 64, 48);
        return true;
    }
    if (id == K_FIELD) {
        SetRect(r, 76, 6, s_cw - 12, 50);
        return true;
    }
    if (s_showHits) {
        int i = id - K_HIT;
        if (i < 0 || i >= SearchCount())
            return false;
        SetRect(r, 16, kHitY + i * kHitH, s_cw - 16, kHitY + (i + 1) * kHitH - 4);
        return true;
    }
    int row, col;
    if (id >= K_LANG && id <= K_FIND) {   // the bottom row: language, space, backspace, find
        static const int x[5] = { 8, 156, 524, 656, 792 };
        int k = id - K_LANG;
        SetRect(r, x[k], kKbY + 4 * kKbRowH, x[k + 1] - 6, kKbY + 5 * kKbRowH - 6);
        return true;
    }
    row = id / 16;
    col = id % 16;
    if (row > 3 || col >= KbRowLen(row))
        return false;
    int n = KbRowLen(row), w = (784 - (n - 1) * 6) / n, x0 = 8 + (784 - (n * w + (n - 1) * 6)) / 2;
    SetRect(r, x0 + col * (w + 6), kKbY + row * kKbRowH, x0 + col * (w + 6) + w, kKbY + (row + 1) * kKbRowH - 6);
    return true;
}

static int HitSearch(int x, int y)
{
    RECT r;
    for (int id = 0; id < K_HIT + 8; id++)
        if (SearchRect(id, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return id;
    return -1;
}

// The point the search is near and the route starts at: the car, or the map's center without a fix.
static void SearchFrom(double* lon, double* lat)
{
    *lon = s_gps.valid ? s_gps.lon : g_cfg.lon;
    *lat = s_gps.valid ? s_gps.lat : g_cfg.lat;
}

static void OnSearchKey(int id)
{
    int n = (int)wcslen(s_query);
    if (id == K_BACK) {
        if (s_showHits)
            s_showHits = false;
        else {
            s_search = false;
            if (s_searchSlot >= 0) {   // back to the route editor
                s_searchSlot = -1;
                s_plan = true;
            }
        }
    } else if (id == K_FIELD) {
        s_showHits = false;
    } else if (id == K_LANG) {
        s_kbLang = !s_kbLang;
    } else if (id == K_SPACE) {
        if (n && n < 62 && s_query[n - 1] != L' ')
            wcscat(s_query, L" ");
    } else if (id == K_BS) {
        if (n)
            s_query[n - 1] = 0;
    } else if (id == K_FIND) {
        double lon, lat;
        SearchFrom(&lon, &lat);
        if (n) {
            SearchStart(s_query, lon, lat);
            s_showHits = true;   // (the query is in the log as the request's URL)
        }
    } else if (id >= K_HIT) {
        const SearchHit* h = SearchGet(id - K_HIT);
        if (h && s_searchSlot >= 0) {   // a place for the route editor
            RoutePlace* p = PlanSlot(s_searchSlot);
            p->kind = PL_PLACE;
            wcscpy(p->name, h->name);
            p->lon = h->lon;
            p->lat = h->lat;
            s_searchSlot = -1;
            s_search = false;
            s_plan = true;
        } else if (h) {
            double lon, lat;
            SearchFrom(&lon, &lat);
            RouteTo(h, lon, lat, !s_gps.valid ? -2 : s_gps.speedKmh > 10 ? s_gps.course : -1);
            s_search = false;
            if (!s_gps.valid) {   // no car to follow: show where the route goes
                g_cfg.follow = FOLLOW_OFF;
                g_cfg.lon = h->lon;
                g_cfg.lat = h->lat;
                UserChanged();
            }
        }
    } else if (id / 16 < 4 && n < 62) {
        s_query[n] = kKeys[s_kbLang][id / 16][id % 16];
        s_query[n + 1] = 0;
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

static void DrawSearch(HDC dc)
{
    RECT all = { 0, 0, s_cw, s_ch };
    HBRUSH ink = CreateSolidBrush(kInk);
    FillRect(dc, &all, ink);
    DeleteObject(ink);

    RECT r;
    SearchRect(K_BACK, &r);
    bool down = s_kbPressed == K_BACK;
    Tile(dc, r, down ? kOrange : kPanel2, down ? kOrange : kPanel2, 40);   // (VestaHome's keyboard: "back" up here)
    if (s_arrow[0][0])
        BlendSign(dc, s_arrow[0][down ? 1 : 0], (r.left + r.right - s_arrow[0][0]->w) / 2,
                  (r.top + r.bottom - s_arrow[0][0]->h) / 2);
    SearchRect(K_FIELD, &r);
    Tile(dc, r, kPanel2, s_showHits ? kPanel2 : kOrange, 20);
    wchar_t s[128];
    _snwprintf(s, 128, s_showHits ? L"%s" : L"%s|", s_query);
    s[127] = 0;
    Text(dc, s_fontBig, s_query[0] ? kText : kTextDim, r.left + 16, r.top, r.right - 12, r.bottom, DT_LEFT,
         s_query[0] || s_showHits ? s : L"Адрес или место|");

    if (s_showHits) {
        int st = SearchState(), n = SearchCount();
        const wchar_t* msg = st == SEARCH_BUSY ? L"Поиск…" : st == SEARCH_FAIL ? L"Нет связи с сервером поиска"
                           : !n ? L"Ничего не найдено" : NULL;
        if (msg) {
            Text(dc, s_fontLabel, st == SEARCH_FAIL ? kTextErr : kTextDim, 0, 120, s_cw, 200, DT_CENTER, msg);
            return;
        }
        double lon, lat;
        SearchFrom(&lon, &lat);
        for (int i = 0; i < n; i++) {
            const SearchHit* h = SearchGet(i);
            SearchRect(K_HIT + i, &r);
            Tile(dc, r, s_kbPressed == K_HIT + i ? kOrange : kPanel, s_kbPressed == K_HIT + i ? kOrange : kPanel, 16);
            COLORREF c = s_kbPressed == K_HIT + i ? kInk : kText;
            Text(dc, s_fontLabel, c, r.left + 16, r.top + 4, r.right - 120, r.top + 28, DT_LEFT, h->name);
            Text(dc, s_fontSmall, s_kbPressed == K_HIT + i ? kInk : kTextDim, r.left + 16, r.top + 28, r.right - 120,
                 r.bottom - 4, DT_LEFT, h->sub);
            double dx = (h->lon - lon) * 111320 * cos(lat * M_PI / 180), dy = (h->lat - lat) * 110540;
            wchar_t d[32];
            FmtDist(d, 32, (int)sqrt(dx * dx + dy * dy));
            Text(dc, s_fontLabel, c, r.right - 120, r.top, r.right - 16, r.bottom, DT_RIGHT, d);
        }
        return;
    }
    static const wchar_t* kBottom[4] = { NULL, L"ПРОБЕЛ", L"СТЕРЕТЬ", L"НАЙТИ" };
    for (int id = 0; id <= K_FIND; id++) {
        if (!SearchRect(id, &r))
            continue;
        bool p = s_kbPressed == id;
        wchar_t cap[8] = { 0 };
        if (id < K_LANG)
            cap[0] = kKeys[s_kbLang][id / 16][id % 16];
        else
            wcscpy(cap, id == K_LANG ? (s_kbLang ? L"РУС" : L"ENG") : kBottom[id - K_LANG]);
        COLORREF kc = p || (id == K_FIND && s_query[0]) ? kOrange : kPanel2;
        Tile(dc, r, kc, kc, 16);
        Text(dc, id < K_LANG ? s_fontBig : s_fontLabel, p || (id == K_FIND && s_query[0]) ? kInk : kText, r.left,
             r.top, r.right, r.bottom, DT_CENTER, cap);
    }
}

// ---- the route editor
static RoutePlace* PlanSlot(int slot)
{
    RoutePlan* p = RoutePlanGet();
    return slot == 0 ? &p->from : slot == 4 ? &p->to : &p->via[slot - 1];
}

static int RowSlot(int row)   // rows: from, the stops, to
{
    int n = RoutePlanGet()->nvia;
    return row == 0 ? 0 : row <= n ? row : 4;
}

static int SlotRow(int slot)
{
    return slot == 4 ? RoutePlanGet()->nvia + 1 : slot;
}

static int ChoiceCount()
{
    return s_planChoose == 0 ? 4 : 3;
}

static int ChoiceId(int i)   // "my place" only for "from"
{
    return s_planChoose == 0 ? P_CH_GPS + i : P_CH_SEARCH + i;
}

static bool PlanRect(int id, RECT* r)
{
    RoutePlan* p = RoutePlanGet();
    if (id == P_BACK) {   // VestaHome's: at the bottom left
        *r = kBackRect;
        return true;
    }
    if (s_planChoose >= 0) {   // the choices over the editor: only they answer
        for (int i = 0; i < ChoiceCount(); i++)
            if (ChoiceId(i) == id) {
                SetRect(r, 220, 120 + i * 66, 580, 120 + i * 66 + 56);
                return true;
            }
        return false;
    }
    if (id >= P_ROW && id <= P_ROW + 4) {
        int slot = id - P_ROW;
        if (slot >= 1 && slot <= 3 && slot > p->nvia)
            return false;
        int y = kRowY + SlotRow(slot) * kRowH;
        SetRect(r, 16, y, slot >= 1 && slot <= 3 ? 720 : 784, y + kRowH - 6);
        return true;
    }
    if (id >= P_DEL + 1 && id <= P_DEL + 3) {
        int slot = id - P_DEL;
        if (slot > p->nvia)
            return false;
        int y = kRowY + SlotRow(slot) * kRowH;
        SetRect(r, 728, y, 784, y + kRowH - 6);
        return true;
    }
    if (id == P_ADDVIA) {
        if (p->nvia >= kMaxVia)
            return false;
        SetRect(r, 92, 416, 376, 468);
        return true;
    }
    if (id == P_GO) {
        SetRect(r, 540, 416, 784, 468);
        return true;
    }
    return false;
}

static int HitPlan(int x, int y)
{
    static const int ids[] = { P_BACK, P_ROW, P_ROW + 1, P_ROW + 2, P_ROW + 3, P_ROW + 4, P_DEL + 1, P_DEL + 2,
                               P_DEL + 3, P_ADDVIA, P_GO, P_CH_GPS, P_CH_SEARCH, P_CH_MAP, P_CH_CANCEL };
    RECT r;
    for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++)
        if (PlanRect(ids[i], &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return ids[i];
    return -1;
}

// The screen point -> its place (the inverse of the map's turn).
static void ScreenToGeo(int x, int y, double* lon, double* lat)
{
    double vx, vy;
    ViewWorld(&vx, &vy);
    double a = ViewAngle() * M_PI / 180.0, c = cos(a), sn = sin(a);
    double sx = x - (g_cfg.mapX + g_cfg.mapW / 2.0), sy = y - (g_cfg.mapY + g_cfg.mapH / 2.0);
    WorldToGeo(vx + c * sx - sn * sy, vy + sn * sx + c * sy, g_cfg.z, lon, lat);
}

static RoutePlace MapPlace(double lon, double lat)
{
    RoutePlace p;
    p.kind = PL_PLACE;
    _snwprintf(p.name, 80, L"Точка на карте (%.5f, %.5f)", lat, lon);
    p.name[79] = 0;
    p.lon = lon;
    p.lat = lat;
    return p;
}

// A new plan to a point: from the car, no stops; its address is asked for.
static void PlanNew(double lon, double lat)
{
    RoutePlan* p = RoutePlanGet();
    memset(p, 0, sizeof(*p));
    p->from.kind = PL_GPS;
    p->to = MapPlace(lon, lat);
    PlaceLookup(p->to);
    s_planMsg = NULL;
    s_planChoose = -1;
    s_plan = true;
}

static void OnLongPress(int x, int y)
{
    double lon, lat;
    ScreenToGeo(x, y, &lon, &lat);
    Log("ui: long press %.5f,%.5f", lat, lon);
    RouteInfo ri;
    RouteGet(&ri);
    s_dockShown = false;
    if (ri.state == ROUTE_ON || ri.state == ROUTE_BUSY) {
        s_lpMenu = true;
        s_lpLon = lon;
        s_lpLat = lat;
    } else {
        PlanNew(lon, lat);
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

// The map in the picker mode: a tap is the point for the slot.
static void PickAt(int x, int y)
{
    double lon, lat;
    ScreenToGeo(x, y, &lon, &lat);
    RoutePlace* p = PlanSlot(s_pickSlot);
    *p = MapPlace(lon, lat);
    PlaceLookup(*p);
    s_pickSlot = -1;
    s_plan = true;
}

static void OnPlanCtl(int id)
{
    RoutePlan* p = RoutePlanGet();
    s_planMsg = NULL;
    if (id == P_BACK) {
        if (s_planChoose >= 0)
            s_planChoose = -1;
        else
            s_plan = false;
    } else if (id >= P_ROW && id <= P_ROW + 4) {
        s_planChoose = id - P_ROW;
    } else if (id >= P_DEL + 1 && id <= P_DEL + 3) {
        for (int i = id - P_DEL; i < p->nvia; i++)
            p->via[i - 1] = p->via[i];
        p->nvia--;
    } else if (id == P_ADDVIA) {
        memset(&p->via[p->nvia], 0, sizeof(RoutePlace));
        p->nvia++;
        s_planChoose = p->nvia;   // at once: where is it
    } else if (id == P_GO) {
        for (int i = 0; i < p->nvia;) {   // stops left empty: out
            if (p->via[i].kind == PL_NONE) {
                for (int k = i + 1; k < p->nvia; k++)
                    p->via[k - 1] = p->via[k];
                p->nvia--;
            } else
                i++;
        }
        bool gps = s_gps.valid != 0;
        if (p->to.kind != PL_PLACE)
            s_planMsg = L"Выберите, куда ехать";
        else if (!RouteStart(s_gps.lon, s_gps.lat, gps && s_gps.speedKmh > 10 ? s_gps.course : -1, gps))
            s_planMsg = L"Нет GPS: выберите «Откуда» адресом или точкой на карте";
        else {
            s_plan = false;
            if (p->from.kind == PL_PLACE) {   // a planned route: show its start
                g_cfg.follow = FOLLOW_OFF;
                g_cfg.lon = p->from.lon;
                g_cfg.lat = p->from.lat;
                UserChanged();
            }
        }
    } else if (id == P_CH_GPS) {
        p->from.kind = PL_GPS;
        s_planChoose = -1;
    } else if (id == P_CH_SEARCH) {
        s_searchSlot = s_planChoose;
        s_planChoose = -1;
        s_plan = false;
        s_search = true;
        s_showHits = false;
    } else if (id == P_CH_MAP) {
        s_pickSlot = s_planChoose;
        s_planChoose = -1;
        s_plan = false;
        s_dockShown = false;
    } else if (id == P_CH_CANCEL) {
        s_planChoose = -1;
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

static void DrawPlan(HDC dc)
{
    RoutePlan* p = RoutePlanGet();
    RECT all = { 0, 0, s_cw, s_ch };
    HBRUSH ink = CreateSolidBrush(kInk);
    FillRect(dc, &all, ink);
    DeleteObject(ink);
    RECT r;
    Text(dc, s_fontBig, kText, 20, 0, 500, 56, DT_LEFT, L"Маршрут");
    BackDisc(dc, s_planPressed == P_BACK);

    int choose = s_planChoose;
    s_planChoose = -1;   // (the rows' places)
    for (int row = 0; row < p->nvia + 2; row++) {
        int slot = RowSlot(row);
        PlanRect(P_ROW + slot, &r);
        bool pr = s_planPressed == P_ROW + slot;
        Tile(dc, r, pr ? kOrange : kPanel, choose == slot || pr ? kOrange : kPanel, 16);
        wchar_t label[16];
        if (slot == 0)
            wcscpy(label, L"Откуда");
        else if (slot == 4)
            wcscpy(label, L"Куда");
        else if (p->nvia > 1)   // (the user's word 2026-10-26)
            _snwprintf(label, 16, L"Заехать %d", slot);
        else
            wcscpy(label, L"Заехать");
        const RoutePlace* pl = PlanSlot(slot);
        const wchar_t* v = pl->kind == PL_PLACE ? pl->name : pl->kind == PL_GPS
                         ? (s_gps.valid ? L"Моё местоположение" : L"Моё местоположение (нет GPS)") : L"не выбрано";
        Text(dc, s_fontSmall, pr ? kInk : kTextDim, r.left + 16, r.top, r.left + 110, r.bottom, DT_LEFT, label);
        Text(dc, s_fontLabel, pr ? kInk : pl->kind == PL_NONE ? kTextDim : kText, r.left + 112, r.top, r.right - 12,
             r.bottom, DT_LEFT, v);
        if (slot >= 1 && slot <= 3 && PlanRect(P_DEL + slot, &r)) {
            bool dp = s_planPressed == P_DEL + slot;
            Tile(dc, r, dp ? kOrange : kPanel2, dp ? kOrange : kPanel2, 16);
            Text(dc, s_fontBig, dp ? kInk : kText, r.left, r.top, r.right, r.bottom, DT_CENTER, L"×");
        }
    }
    if (PlanRect(P_ADDVIA, &r))
        DrawCtl(dc, r, s_planPressed == P_ADDVIA, s_fontLabel, L"+ ЗАЕХАТЬ");
    PlanRect(P_GO, &r);
    bool ready = p->to.kind == PL_PLACE && (p->from.kind == PL_PLACE || (p->from.kind == PL_GPS && s_gps.valid));
    COLORREF go = ready || s_planPressed == P_GO ? kOrange : kPanel2;
    Tile(dc, r, go, go, 20);
    Text(dc, s_fontLabel, ready ? kInk : kTextDim, r.left, r.top, r.right, r.bottom, DT_CENTER, L"ПОЕХАЛИ");
    if (s_planMsg)
        Text(dc, s_fontSmall, kTextErr, 24, 360, s_cw - 24, 400, DT_LEFT, s_planMsg);
    else
        Text(dc, s_fontSmall, kTextDim, 24, 360, s_cw - 24, 400, DT_LEFT,
             L"Нажмите на поле: своё место, адрес или точка на карте. Долгое нажатие на карту — маршрут туда.");
    s_planChoose = choose;
    if (choose < 0)
        return;
    // the field's choices over the editor
    RECT box = { 196, 96, 604, 120 + ChoiceCount() * 66 + 14 };
    Tile(dc, box, kPanel, kPanel, 24);
    static const wchar_t* kChoice[4] = { L"МОЁ МЕСТОПОЛОЖЕНИЕ", L"НАЙТИ АДРЕС", L"ТОЧКА НА КАРТЕ", L"ОТМЕНА" };
    for (int i = 0; i < ChoiceCount(); i++) {
        int id = ChoiceId(i);
        PlanRect(id, &r);
        DrawCtl(dc, r, s_planPressed == id, s_fontLabel, kChoice[id - P_CH_GPS]);
    }
}

// The picker's bar over the map and the long press menu.
static bool PickRect(RECT* r)
{
    SetRect(r, s_cw - 168, kStatusH + 12, s_cw - 16, kStatusH + 56);
    return s_pickSlot >= 0;
}

static bool MenuRect(int id, RECT* r)
{
    if (!s_lpMenu || id < M_VIA || id > M_CANCEL)
        return false;
    if (id == M_VIA && RoutePlanGet()->nvia >= kMaxVia)
        return false;
    int i = id - M_VIA;
    SetRect(r, 220, 300 + i * 58, 580, 300 + i * 58 + 50);
    return true;
}

static bool TripShown();

static int HitOverMap(int x, int y)
{
    RECT r;
    if (TripShown() && x >= 548 && x < 596 && y >= 409 && y < 457)
        return P_TRIP_X;
    for (int k = 0; CardRect(k, &r); k++)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return 70 + k;
    if (PickRect(&r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
        return P_PICK_CANCEL;
    for (int id = M_VIA; id <= M_CANCEL; id++)
        if (MenuRect(id, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return id;
    return -1;
}

static void OnOverMap(int id)
{
    if (id == P_TRIP_X) {   // the trip's "x": the route off
        RouteClear();
        Log("ui: route off");
    } else if (id >= 70 && id < 73) {   // an alternative card: that route on
        RouteAltChoose(id - 70);
    } else if (id == P_PICK_CANCEL) {
        s_pickSlot = -1;
        s_plan = true;
    } else if (id == M_VIA) {
        RoutePlace p = MapPlace(s_lpLon, s_lpLat);
        PlaceLookup(p);
        bool gps = s_gps.valid != 0;
        RouteAddVia(p, s_gps.lon, s_gps.lat, gps && s_gps.speedKmh > 10 ? s_gps.course : -1, gps);
        s_lpMenu = false;
    } else if (id == M_NEW) {
        s_lpMenu = false;
        PlanNew(s_lpLon, s_lpLat);
    } else if (id == M_CANCEL) {
        s_lpMenu = false;
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

static void DrawOverMap(HDC dc)
{
    RECT r;
    DrawCards(dc);
    if (PickRect(&r)) {
        RECT bar = { 8, kStatusH + 6, s_cw - 8, kStatusH + 62 };
        Card(dc, bar, 12);
        Outline(dc, bar, kOrange, 12);   // (a mode: the map picks a point)
        static const wchar_t* kWhat[5] = { L"«Откуда»", L"«Заехать 1»", L"«Заехать 2»", L"«Заехать 3»", L"«Куда»" };
        wchar_t s[96];
        _snwprintf(s, 96, L"Нажмите на карту: точка для %s", kWhat[s_pickSlot]);
        s[95] = 0;
        Text(dc, s_fontLabel, kText, bar.left + 16, bar.top, r.left - 8, bar.bottom, DT_LEFT, s);
        DrawCtl(dc, r, s_planPressed == P_PICK_CANCEL, s_fontLabel, L"ОТМЕНА");
    }
    if (s_lpMenu) {
        RECT box = { 204, 288, 596, 300 + 3 * 58 + 4 };
        Card(dc, box, 20);
        static const wchar_t* kMenu[3] = { L"ЗАЕХАТЬ СЮДА", L"МАРШРУТ СЮДА", L"ОТМЕНА" };
        for (int id = M_VIA; id <= M_CANCEL; id++) {
            if (MenuRect(id, &r))
                DrawCtl(dc, r, s_planPressed == id, s_fontLabel, kMenu[id - M_VIA]);
            else if (id == M_VIA) {
                SetRect(&r, 220, 300, 580, 350);
                Text(dc, s_fontSmall, kTextDim, r.left, r.top, r.right, r.bottom, DT_CENTER, L"уже три точки «Заехать»");
            }
        }
    }
}

// The top (design v3): no bar - the map darkens softly towards the edge; on it the chips (GPS; the traffic or the
// network) and the clock.
static void ShadeTop()
{
    static int f[60];
    static bool made;
    if (!made) {
        for (int y = 0; y < 60; y++)
            f[y] = 256 - (int)(200 * pow(1 - y / 60.0, 1.5));
        made = true;
    }
    if (!s_backBits)
        return;
#ifndef UNDER_CE
    GdiFlush();
#endif
    int stride = BackStride();
    for (int y = 0; y < 60 && y < s_ch; y++) {
        unsigned short* row = s_backBits + (s_ch - 1 - y) * stride;
        int k = f[y];
        for (int x = 0; x < s_cw; x++) {
            unsigned d = row[x];
            row[x] = (unsigned short)((((d >> 11) * k >> 8) << 11) | ((((d >> 5) & 63) * k >> 8) << 5) | ((d & 31) * k >> 8));
        }
    }
}

static int Chip(HDC dc, int x, const wchar_t* s, COLORREF fg, COLORREF bg)   // a round-ended label; its right edge
{
    SelectObject(dc, s_fontCap);
    SIZE sz;
    GetTextExtentPoint32(dc, s, (int)wcslen(s), &sz);
    RECT r = { x, 10, x + sz.cx + 24, 34 };
    Tile(dc, r, bg, bg, 24);
    Text(dc, s_fontCap, fg, r.left, r.top, r.right, r.bottom, DT_CENTER, s);
    return r.right;
}

static void DrawStatus(HDC dc)
{
    ShadeTop();
    wchar_t s[96];
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 96, L"%02d:%02d", t.wHour, t.wMinute);
    Text(dc, s_fontClock, kText, s_cw - 160, 0, s_cw - 16, kStatusH, DT_RIGHT, s);

    const COLORREF okBg = RGB(20, 60, 34), okFg = RGB(140, 230, 160), errBg = RGB(70, 24, 24);
    COLORREF fg = kTextDim, bg = kPanel2;
    if (!s_gpsRunning) {
        wcscpy(s, L"GPS выкл.");
    } else if (s_gps.status == GPS_SCANNING) {
        wcscpy(s, L"GPS: поиск порта");
    } else if (s_gps.status == GPS_BUSY) {
        _snwprintf(s, 96, L"GPS: порт %s занят", s_gps.port);
        fg = kTextErr;
        bg = errBg;
    } else if (s_gps.status != GPS_OPEN) {
        _snwprintf(s, 96, L"GPS: нет порта %s", s_gps.port);
        fg = kTextErr;
        bg = errBg;
    } else if (!s_gps.lines) {
        wcscpy(s, L"GPS: нет данных");
        fg = kTextErr;
        bg = errBg;
    } else if (!s_gps.valid) {
        _snwprintf(s, 96, L"GPS: поиск · %d", s_gps.sats);
    } else {
        _snwprintf(s, 96, L"GPS · %d", s_gps.sats);
        fg = okFg;
        bg = okBg;
    }
    s[95] = 0;
    int x = Chip(dc, 16, s, fg, bg) + 8;

    // the network's state, or the traffic: its provider and age
    fg = kTextDim;
    bg = kPanel2;
    if (s_limitHit && g_cfg.traffic) {
        wcscpy(s, L"Лимит запросов!");
        fg = kTextErr;
        bg = errBg;
    } else if (s_lastErr && GetTickCount() - s_lastErrTick < 60000) {
        if (s_lastErr == NET_HTTP)
            _snwprintf(s, 96, L"Ошибка HTTP %d", s_lastHttp);
        else
            _snwprintf(s, 96, L"Ошибка: %s", NetErrorText(s_lastErr));
        fg = kTextErr;
        bg = errBg;
    } else if (s_inflight) {
        wcscpy(s, L"Загрузка…");
    } else if (!g_cfg.traffic) {
        wcscpy(s, L"Пробки выкл.");
    } else {
        MapRequest keys[kMaxKeys];
        CacheEntry* e = NeededKeys(TrafficSource(), keys, kMaxKeys) ? CacheFind(keys[0], false) : NULL;
        if (!e)
            _snwprintf(s, 96, L"Пробки · %s · нет данных", kTrafficTitle[g_cfg.trafficSrc]);
        else
            _snwprintf(s, 96, L"Пробки · %s · %d мин", kTrafficTitle[g_cfg.trafficSrc],
                       (int)((GetTickCount() - e->fetchTick) / 60000));
    }
    s[95] = 0;
    Chip(dc, x, s, fg, bg);
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

// The caption under a sign of the bottom bar.
static const wchar_t* DockCaption(int b)
{
    switch (b) {
    case B_HOME:     return L"ДОМОЙ";
    case B_TRF:      return L"ПРОБКИ";
    case B_TSRC:     return g_cfg.trafficSrc == TRF_2GIS ? L"2ГИС" : L"ЯНДЕКС";
    case B_NIGHT:    return g_cfg.night ? L"ДЕНЬ" : L"НОЧЬ";   // as VestaHome: what a tap turns on
    case B_SETTINGS: return L"НАСТРОЙКИ";
    case B_SEARCH:   return L"ПОИСК";
    case B_ROUTEOFF: return L"СБРОС";
    }
    return L"";
}

static void DrawButtons(HDC dc)
{
    RECT r;
    if (s_dockShown && !s_settings && !s_search) {
        int y = kDockY + s_dockOff;   // sliding in / out
        RECT bar = { 0, y, s_cw, s_ch };
        // as VestaHome's bar: solid; the accent's line on its top shrinks to the moment it goes down
        DWORD el = GetTickCount() - s_dockTick;
        RECT left = { 0, y, el >= (DWORD)kDockMs ? 0 : (int)(s_cw * (kDockMs - el) / kDockMs), y + 2 };
        HBRUSH ib = CreateSolidBrush(kInk), o = CreateSolidBrush(kOrange);
        FillRect(dc, &bar, ib);
        FillRect(dc, &left, o);
        DeleteObject(ib);
        DeleteObject(o);
    }
    for (int i = 0; i < B_COUNT; i++) {
        if (!ButtonRect(i, &r))
            continue;
        int pressed = s_pressed == i && s_pressedInside ? 1 : 0;
        if (i == B_GPS || i == B_MINUS || i == B_PLUS) {   // a dark round button over the map, no border
            if (pressed)
                Tile(dc, r, kOrange, kOrange, kRound);
            else
                Card(dc, r, kRound / 2);
            DrawSign(dc, i, r, pressed);
            continue;
        }
        // a slot of the bottom bar, as VestaHome's: the sign (a mode that is on: the orange one) and its caption
        // under it; pressed = an orange tile with both dark
        if (pressed) {
            RECT p = { r.left + 6, r.top + 2, r.right - 6, r.bottom - 2 };
            Tile(dc, p, kOrange, kOrange, 24);
        }
        // as VestaHome's bar: the 30 px sign 5 px under the slot's top (its 44 px picture centred there), the 12 px
        // bold caption under it
        RECT ic = { r.left, r.top - 2, r.right, r.top + 42 };
        DrawSign(dc, i, ic, pressed);
        bool on = i == B_TRF && g_cfg.traffic;
        Text(dc, s_fontCap, pressed ? kInk : on ? kOrange : kTextDim, r.left, r.top + 39, r.right, r.bottom,
             DT_CENTER, DockCaption(i));
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
    } else if (s_search) {
        DrawSearch(dc);
    } else if (s_plan) {
        DrawPlan(dc);
    } else {
        DrawStatus(dc);
        if (s_pickSlot < 0)
            DrawGuide(dc);
        DrawTrip(dc);
        DrawButtons(dc);
        DrawOverMap(dc);
    }

    BitBlt(hdc, 0, 0, s_cw, s_ch, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
}

// The face (design v3, 2026-10-26): VestaHome's Golos Text - Fonts\ next to the exe (SIL OFL, as VestaHome's), added at
// the start without the registry (the unit resets it); not there - the unit's Tahoma.
static const wchar_t* s_face = L"Tahoma";

static void FontsLoad()
{
    static const wchar_t* files[2] = { L"GolosText-Regular.ttf", L"GolosText-Bold.ttf" };
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        wchar_t path[MAX_PATH], name[64];
        _snwprintf(name, 64, L"Fonts\\%s", files[i]);
        name[63] = 0;
        PathInDir(path, name);
        ok += AddFontResource(path) > 0;
    }
    if (ok == 2)
        s_face = L"Golos Text";
    Log("ui: fonts %S", s_face);
}

static HFONT MakeFont(int height, int weight)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    // no smoothing: on the unit ANTIALIASED_QUALITY comes out as ClearType - coloured fringes in the 16-bit
    // frame (the user's report 2026-10-17; VestaHome found the same)
    lf.lfQuality = NONANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, s_face);
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
    Log("stat: ram %lu%% used, %lu KB free, proc %lu KB free; cache %d KB; paints %d (max %lu ms), tiles drawn %d; "
        "gps %lu bytes, %d lines, %d bad",
        ms.dwMemoryLoad, ms.dwAvailPhys / 1024, ms.dwAvailVirtual / 1024, CacheBytes() / 1024,
        s_statPaints, s_statPaintMax, s_statSlots,
        s_gps.bytes - s_statGpsBytes, s_gps.lines - s_statGpsLines, s_gps.badLines - s_statGpsBad);
    s_statTick = GetTickCount();
    s_statPaints = 0;
    s_statPaintMax = 0;
    s_statSlots = 0;
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

    FontsLoad();
    AccentFromIni();   // (VestaHome, when it runs, says its theme in "welcome")
    s_fontHuge = MakeFont(46, FW_BOLD);
    s_fontKm = MakeFont(21, FW_BOLD);
    s_fontStreet = MakeFont(21, FW_BOLD);
    s_fontSpeed = MakeFont(28, FW_BOLD);
    s_fontBig = MakeFont(22, FW_SEMIBOLD);
    s_fontSmall = MakeFont(15, FW_NORMAL);
    s_fontTiny = MakeFont(12, FW_NORMAL);
    s_fontStatus = MakeFont(17, FW_NORMAL);
    s_fontClock = MakeFont(30, FW_BOLD);
    s_fontLabel = MakeFont(19, FW_BOLD);
    s_fontCap = MakeFont(12, FW_BOLD);   // the bottom bar's captions, as VestaHome's
    // the map takes the whole screen (map_x/y/w/h of the ini are YaMapsCE's: its pictures are 600 x 450)
    g_cfg.mapX = 0;
    g_cfg.mapY = 0;
    g_cfg.mapW = s_cw;
    g_cfg.mapH = s_ch;
    LoadImages();
    s_ringBits = ImageCreate(kRingPx, kRingPx, &s_ring);
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
    KillTimer(hwnd, TIMER_DOCK);
    KillTimer(hwnd, TIMER_LONG);
    ConfigSave();
    if (s_taskbar)
        ShowWindow(s_taskbar, SW_SHOWNORMAL);
    GpsStop();
    NetStop();
    LogStat();
    Log("exit, today: yandex %d, 2gis %d, osm %d", g_cfg.yandexCount, g_cfg.twogisCount, g_cfg.osmCount);
    PostQuitMessage(0);
}

// ---------------------------------------------------------------- VestaHome's map widget
// OSMapsCE as an add-on of VestaHome (VestaHome/docs/plugin-api.md, 4.1; 2026-10-26): when VestaHome runs, OSMapsCE
// says hello and declares the picture widget "Карта" (2 x 2). While the widget is in sight (widget.show) the map is
// drawn into the back buffer each second - minimized too - and its middle (the car) copied as RGB565 into the shared
// memory VestaHome made; a tap on the widget brings OSMapsCE to the front. Without VestaHome nothing of this runs.
#define VHAP_MAGIC 0x56484150   // 'VHAP'
static HWND           s_vh;         // VestaHome's window
static HANDLE         s_vhMap;
static unsigned char* s_vhView;     // 16 bytes ('VHIM', w, h, frame number), then the rows
static int            s_vhW, s_vhH;
static bool           s_vhShown;
static DWORD          s_vhSeq, s_vhHelloTick;

static void VhSend(const wchar_t* fmt, ...)
{
    if (!s_vh || !IsWindow(s_vh)) {
        s_vh = NULL;
        return;
    }
    wchar_t t[256];
    va_list a;
    va_start(a, fmt);
    int n = _vsnwprintf(t, 255, fmt, a);
    va_end(a);
    t[255] = 0;
    if (n < 0)
        n = (int)wcslen(t);
    COPYDATASTRUCT c;
    c.dwData = VHAP_MAGIC;
    c.cbData = n * sizeof(wchar_t);
    c.lpData = t;
    SendMessage(s_vh, WM_COPYDATA, (WPARAM)s_wnd, (LPARAM)&c);
}

static bool VhField(const wchar_t* cmd, const wchar_t* key, wchar_t* out, int cap)
{
    out[0] = 0;
    int kl = (int)wcslen(key);
    for (const wchar_t* p = wcschr(cmd, L'\t'); p; p = wcschr(p + 1, L'\t')) {
        if (wcsncmp(p + 1, key, kl) || p[1 + kl] != L'=')
            continue;
        const wchar_t* v = p + 2 + kl;
        int n = 0;
        while (v[n] && v[n] != L'\t' && n < cap - 1) {
            out[n] = v[n];
            n++;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

static void VhFree()
{
    if (s_vhView)
        UnmapViewOfFile(s_vhView);
    if (s_vhMap)
        CloseHandle(s_vhMap);
    s_vhView = NULL;
    s_vhMap = NULL;
    s_vhShown = false;
}

static int VhReceive(const wchar_t* cmd)   // 2 = a button taken (VestaHome leaves it), 1 otherwise
{
    wchar_t v[64];
    if (!wcsncmp(cmd, L"welcome", 7) || !wcsncmp(cmd, L"theme", 5)) {   // VestaHome's accent: ours (the rule 2026-10-26)
        if (VhField(cmd, L"accent", v, 16) && v[0]) {
            unsigned long c = wcstoul(v, NULL, 16);
            AccentSet(RGB(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF));
        }
    }
    if (!wcsncmp(cmd, L"key", 3) && cmd[3] == L'\t') {
        // The wheel's buttons (the user 2026-10-26): short presses stay VestaHome's (tracks, the phone); held "up" /
        // "down" (0x12 / 0x14) - the zoom, while the map is in front (VestaHome asks only then).
        int code = VhField(cmd, L"code", v, 8) ? _wtoi(v) : 0;
        int st = VhField(cmd, L"st", v, 8) ? _wtoi(v) : 0;
        int kind = VhField(cmd, L"kind", v, 8) ? _wtoi(v) : 0;
        if ((code == 0x12 || code == 0x14) && kind >= 2) {
            if (st == 0) {
                Zoom(code == 0x12 ? 1 : -1);
                UserChanged();
                InvalidateRect(s_wnd, NULL, FALSE);
            }
            return 2;
        }
        return 1;
    }
    if (!wcsncmp(cmd, L"welcome", 7)) {
        VhSend(L"widget\tid=map\ttitle=Карта OSM\ticon=map\tsize=2x2\timage=1");
        VhSend(L"subscribe\twhat=keys");
    } else if (!wcsncmp(cmd, L"widget.size", 11)) {
        VhFree();
        VhField(cmd, L"w", v, 8);
        s_vhW = _wtoi(v);
        VhField(cmd, L"h", v, 8);
        s_vhH = _wtoi(v);
        VhField(cmd, L"shm", v, 64);
        DWORD size = 16 + s_vhW * s_vhH * 2;
        if (s_vhW > 0 && s_vhH > 0 && s_vhW <= s_cw && s_vhH <= s_ch) {   // the same memory: VestaHome made it
            s_vhMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, v);
            s_vhView = s_vhMap ? (unsigned char*)MapViewOfFile(s_vhMap, FILE_MAP_ALL_ACCESS, 0, 0, size) : NULL;
        }
        Log("vestahome: map widget %d x %d %s", s_vhW, s_vhH, s_vhView ? "ok" : "NO MEMORY");
    } else if (!wcsncmp(cmd, L"widget.show", 11)) {
        VhField(cmd, L"value", v, 8);
        s_vhShown = v[0] == L'1';
    } else if (!wcsncmp(cmd, L"widget.tap", 10)) {   // to the front, the whole screen
        ShowWindow(s_wnd, SW_SHOW);
        SetForegroundWindow(s_wnd);
        InvalidateRect(s_wnd, NULL, FALSE);
    } else if (!wcsncmp(cmd, L"bye", 3) || !wcsncmp(cmd, L"quit", 4)) {   // VestaHome gone: OSMapsCE stays
        VhFree();
        s_vh = NULL;
    }
    return 1;
}

// Each second: find VestaHome (again), and a frame for the widget while it is in sight.
static void VhTick()
{
    if (!s_vh || !IsWindow(s_vh)) {
        if (s_vh || s_vhView)
            VhFree();
        s_vh = NULL;
        if (GetTickCount() - s_vhHelloTick < 5000)
            return;
        s_vhHelloTick = GetTickCount();
        s_vh = FindWindow(L"VestaHome_Wnd", NULL);
        if (s_vh)
            VhSend(L"hello\tid=OSMapsCE\tname=OSMapsCE\tver=1\tapi=1\thwnd=%08X", (unsigned)(UINT_PTR)s_wnd);
        return;
    }
    if (!s_vhShown || !s_vhView || !s_backBits || !s_back)
        return;
    HDC wdc = GetDC(s_wnd);
    HDC dc = CreateCompatibleDC(wdc);
    ReleaseDC(s_wnd, wdc);
    HGDIOBJ ob = SelectObject(dc, s_back);
    SetBkMode(dc, TRANSPARENT);
    DrawMap(dc);   // (the next paint of our own window draws its frame anew)
    SelectObject(dc, ob);
    DeleteDC(dc);
    int x0 = g_cfg.mapX + (g_cfg.mapW - s_vhW) / 2, y0 = g_cfg.mapY + (g_cfg.mapH - s_vhH) / 2, stride = BackStride();
    unsigned short* dst = (unsigned short*)(s_vhView + 16);
    for (int y = 0; y < s_vhH; y++)   // the back buffer is bottom-up, the widget's rows top down
        memcpy(dst + y * s_vhW, s_backBits + (s_ch - 1 - (y0 + y)) * stride + x0, s_vhW * 2);
    ((DWORD*)s_vhView)[3] = ++s_vhSeq;
    VhSend(L"widget.frame\tid=map");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COPYDATA: {   // VestaHome's commands
        const COPYDATASTRUCT* c = (const COPYDATASTRUCT*)lp;
        if (!c || c->dwData != VHAP_MAGIC || !c->lpData)
            return FALSE;
        wchar_t cmd[256];
        int n = (int)(c->cbData / sizeof(wchar_t));
        n = n > 255 ? 255 : n;
        memcpy(cmd, c->lpData, n * sizeof(wchar_t));
        cmd[n] = 0;
        return VhReceive(cmd);
    }
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
        s_paintSig = FrameSig();
        s_lastPaintTick = GetTickCount();
        s_statPaints++;
        if (s_lastPaintTick - t0 > s_statPaintMax)
            s_statPaintMax = s_lastPaintTick - t0;
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_DOCK) {
            DockStep();
            return 0;
        }
        if (wp == TIMER_LONG) {   // the finger stayed on the map: a point for a route
            KillTimer(hwnd, TIMER_LONG);
            if (s_dragging && !s_dragMoved) {
                s_longDone = true;
                if (s_pickSlot >= 0)
                    PickAt(s_dragStart.x, s_dragStart.y);
                else
                    OnLongPress(s_dragStart.x, s_dragStart.y);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (wp == TIMER_USER) {
            KillTimer(hwnd, TIMER_USER);
            s_userPending = false;
            Ensure(REQ_USER);
        } else {
            GpsGet(&s_gps);
            g_today = Today();
            VhTick();   // VestaHome's map widget
            int sharedNight = NightShared();   // switched in VestaHome: follow it
            if (sharedNight >= 0 && (sharedNight != 0) != (g_cfg.night != 0))
                NightSet(sharedNight != 0);
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
                if (s_pressed < 0 && s_dockDir == 0 && GetTickCount() - s_dockTick >= (DWORD)kDockMs)
                    DockShow(false);   // slides down
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // the clock (minutes) and the bar's texts: only when they changed
            if (FrameSig() == s_paintSig)
                return 0;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP_GPS:
        GpsGet(&s_gps);
        HttpSetGpsDate(s_gps.date);   // a reliable date for HTTPS certificate checks
        if (wp == 1)
            OnGpsScanDone();
        if (s_gps.valid) {
            if (RouteAlts() >= 2 && s_gps.speedKmh > 15)
                RouteAltsDone();   // driving: the route chosen is the one on
            RouteGps(s_gps.lon, s_gps.lat, s_gps.speedKmh, s_gps.course);
        }
        if (!s_dragging) {
            FollowGps();
            Ensure(REQ_AUTO);
        }
        if (FrameSig() != s_paintSig)   // standing: the same frame
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP_MAP:
        OnMapResult((MapResult*)lp);
        return 0;

    case WM_APP_API:   // search / route answers (route.cpp)
        if (OnApiResult((ApiResult*)lp))
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CHAR:   // the PC's keyboard types into the search
        if (s_search) {
            if (wp == 8)
                OnSearchKey(K_BS);
            else if (wp == 13)
                OnSearchKey(K_FIND);
            else if (wp >= 32 && wcslen(s_query) < 62) {
                size_t n = wcslen(s_query);
                s_query[n] = (wchar_t)wp;
                s_query[n + 1] = 0;
                s_showHits = false;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        SetCapture(hwnd);
        s_pressed = HitButton(x, y);
        if (s_pressed >= 0) {
            s_pressedInside = true;
        } else if (s_settings) {
            s_setPressed = HitSettings(x, y);
        } else if (s_search) {
            s_kbPressed = HitSearch(x, y);
        } else if (s_plan) {
            s_planPressed = HitPlan(x, y);
        } else if ((s_planPressed = HitOverMap(x, y)) >= 0) {
            // the picker's "cancel", the long press menu
        } else if (s_lpMenu) {
            s_lpMenu = false;   // a tap beside the menu closes it
        } else if (InMap(x, y)) {
            SetTimer(hwnd, TIMER_LONG, kLongMs, NULL);
            s_longDone = false;
            s_downTick = GetTickCount();
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
            if (!s_dragMoved && (abs(dx) > 6 || abs(dy) > 6)) {
                s_dragMoved = true;
                KillTimer(hwnd, TIMER_LONG);
            }
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
        } else if (s_kbPressed >= 0) {
            int c = s_kbPressed;
            s_kbPressed = -1;
            if (HitSearch(x, y) == c)
                OnSearchKey(c);
        } else if (s_planPressed >= 0) {
            int c = s_planPressed;
            s_planPressed = -1;
            if (s_plan ? HitPlan(x, y) == c : HitOverMap(x, y) == c) {
                if (s_plan)
                    OnPlanCtl(c);
                else
                    OnOverMap(c);
            }
        } else if (s_dragging) {
            s_dragging = false;
            KillTimer(hwnd, TIMER_LONG);
            if (s_longDone) {
                s_longDone = false;   // done when it fired
            } else if (s_dragMoved) {
                UserChanged();
            } else if (GetTickCount() - s_downTick >= (DWORD)kLongMs) {
                // held long, but the timer did not come in time (the thread was busy: input goes before timers)
                if (s_pickSlot >= 0)
                    PickAt(s_dragStart.x, s_dragStart.y);
                else
                    OnLongPress(s_dragStart.x, s_dragStart.y);
            } else if (s_pickSlot >= 0) {
                PickAt(x, y);
            } else if (RouteAlts() >= 2) {
                RouteAltsDone();
            } else {   // a tap on the map raises (and refreshes the traffic) / lowers the bottom bar
                bool up = !DockUp();
                DockShow(up);
                if (up)
                    Ensure(REQ_FORCE);
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        if (s_search) {   // letters go to WM_CHAR
            if (wp == VK_ESCAPE)
                OnSearchKey(K_BACK);
            return 0;
        }
        if (s_plan && wp == VK_ESCAPE) {
            OnPlanCtl(P_BACK);
            return 0;
        }
        switch (wp) {
        case VK_F3:       OnButton(B_SEARCH); break;   // (a letter would also type itself into the search)
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
