#include "yamaps.h"
#include "vkeys.h"     // VestaCore: the keyboard's text and cursor (VestaHome's)
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
       B_HOME, B_SEARCH, B_ROUTEOFF, B_TILT, B_COUNT };

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
    { 724, 82, { NULL } },      // the third person's view on / off: heading-up from z15 (the user 2026-10-28)
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

enum { TIMER_TICK = 1, TIMER_USER = 2, TIMER_DOCK = 3, TIMER_LONG = 4, TIMER_DISK = 5 };
static const UINT kUserDelayMs = 900;    // wait for the user to stop pressing buttons

// GPS button modes (g_cfg.follow)
enum { FOLLOW_OFF, FOLLOW_NORTH, FOLLOW_HEADING };
static const double kCarAhead        = 0.25; // heading-up: view center this part of map height ahead of the car
// The third person's view (tilt.cpp, ring.cpp; the user 2026-10-27..28): heading-up from z15, the camera's angle from
// straight down by the zoom, the fog this far ahead; the near ring loaded +-kNearHalf round the view's centre, the far
// one (two zooms out) +-kFarHalf of its own px
static const int    kTiltMinZ        = 15;
static const double kTiltFog         = 1150;
static const double kNearHalf        = 380;  // (5 slots: 2 x 380 + the 256 px the centre moves in its tile < 1280)
static const double kFarHalf         = 384;
static const double kAroundHalf      = 200;  // the next zooms' tiles kept ready +-this of their px round the centre
static const UINT   kDiskStepMs      = 30;   // the view's tiles at hand drawn kDiskLoads at a time, a frame between
enum { kDiskLoads = 3 };
                                             // (0.25 of 480: the car stays above the bottom bar)
static const double kHeadingMinSpeed = 5;    // km/h: below it the course is noise, keep the last heading
static const int    kMaxKeys         = 32;   // tiles one view can need from one source
static const int    kMaxParentLevels = 6;    // missing tile: show a coarser cached one, up to 6 levels up

// Small ring of map keys: "not on disk" / "no coarser tile" memos,
// so the card is not searched again every second.
enum { kRingKeys = 128 };   // (the view 32, the far ring 25, the next zooms 2 x 9 and their parents: all fit)
struct KeyRing {
    MapRequest k[kRingKeys];
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
    r.next = (r.next + 1) % kRingKeys;
    if (r.n < kRingKeys)
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
static bool       s_dragTilt;           // the drag began in the third person's view: it stays on
static double     s_dragFx, s_dragFy;   // ... the flat point under the finger then (from the view's centre, turned)
static DWORD      s_freeTick;           // the third person's view moved by hand: when (0: it follows the car)
static const DWORD kFreeMs = 5000;      // then back to the car (the user 2026-10-29)

static GpsState   s_gps;
static bool       s_gpsRunning;

// Settings screen (GPS port, cache size), drawn over the map
static bool       s_settings;

// Search screen (route.cpp does the requests): a top bar with "back" and the text, under it the keyboard or the
// places found; a place tapped - the route to it from the car (no fix: from the map's center).
static bool       s_search;
static bool       s_showHits;       // the places found instead of the keyboard
// The query and its cursor - VestaHome's keyboard (VestaCore vkeys; the user 2026-10-29: a cursor to move): its
// keys, the cursor put and dragged by the finger on the field, the text scrolled to keep the cursor in sight.
static VKeys      s_kb;
static int        s_kbPressed = -1;
enum { kQueryCap = 120 };   // (SearchStart: 256 bytes of UTF-8 - 120 Russian letters fit)
enum { K_SHIFT = 100, K_LANG, K_SPACE, K_BS, K_FIND, K_BACK, K_FIELD, K_HIT = 200 };
enum { kKbY = 66, kKbRowH = 81, kHitY = 62, kHitH = 58 };

// The route editor (a long press on the map, the user's design 2026-10-26): "from" (the car by default, or an
// address, or a point on the map), up to three stops, "to"; a field tapped opens its choices. With a route on, a
// long press offers a stop there or a new route there; on a stop's sign - that stop out or earlier (the user
// 2026-10-29: it set a new stop instead). The trip's card tapped: the editor with the route's points.
// Slots: 0 from, 1..3 the stops, 4 to.
enum { P_BACK = 1, P_ROW = 10, P_DEL = 20, P_ADDVIA = 30, P_GO, P_CH_GPS = 40, P_CH_SEARCH, P_CH_MAP, P_CH_CANCEL,
       P_PICK_CANCEL = 50, M_VIA = 60, M_NEW, M_CANCEL, M_STOP_OUT, M_STOP_UP, M_STOP_CANCEL, P_TRIP_X = 80, P_TRIP };
static bool       s_plan;
static int        s_planChoose = -1;   // the slot whose choices are open
static int        s_planPressed = -1;
static int        s_pickSlot = -1;     // the map picks a point for this slot
static int        s_searchSlot = -1;   // the search picks a place for this slot
static bool       s_lpMenu;            // the long press menu (a route is on)
static double     s_lpLon, s_lpLat;
static int        s_lpStop = -1;       // the stop whose sign was held (its menu), -1: none
enum { kStopNearPx = 40 };             // a long press this near a stop's sign is on it
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

// The settings screen as VestaHome's (the user 2026-10-29: all of it within its rules): the title and the clock on
// top, one panel of six rows within 64..400 (a row's stepper applies at once), "back" / "КАРТЫ" / "ЗАВЕРШИТЬ" in the
// bottom strip; what does not fit - the GPS port's settings, the requests' counters - in a sheet over the bottom
// (VestaHome's windows: full width, the main action right, in the accent), a tap beside it closes it.
// Rows at kSetRow0 + i * kSetRowH (the maps' screen has the same rows).
enum { S_MAPS, S_ROUTE, S_CACHE_PREV, S_CACHE_NEXT, S_LOG_PREV, S_LOG_NEXT, S_GPS,
       S_INFO, S_EXIT, S_BACK, S_PAGE_END,
       S_PORT_PREV = S_PAGE_END, S_PORT_NEXT, S_BAUD_PREV, S_BAUD_NEXT, S_SCAN, S_GPS_DONE, S_GPS_END,
       S_INFO_DONE = S_GPS_END, S_INFO_END,
       S_JUNC_PREV = S_INFO_END, S_JUNC_NEXT, S_TOLL_PREV, S_TOLL_NEXT, S_ROUTE_DONE, S_COUNT };
enum { SHEET_NONE, SHEET_GPS, SHEET_INFO, SHEET_ROUTE };
enum { kSetRow0 = 76, kSetRowH = 50, kSheetY = 200 };
static const RECT kSetCtl[S_COUNT] = {
    { 576, 79, 760, 123 },                             // the maps: their page (the kind, the detail, the regions)
    { 576, 129, 760, 173 },                            // the route: its sheet (the junction card, the tolls)
    { 512, 179, 560, 223 }, { 712, 179, 760, 223 },   // the steppers' arrows: rows 2, 3
    { 512, 229, 560, 273 }, { 712, 229, 760, 273 },
    { 576, 279, 760, 323 },                            // GPS: its sheet
    { 576, 329, 760, 373 },                            // the requests: their sheet
    { 600, 416, 784, 468 },                            // exit the program
    { 8, 412, 80, 480 },                               // back: VestaHome's, at the bottom left
    { 512, 259, 560, 303 }, { 712, 259, 760, 303 },   // the GPS sheet: the port, the speed
    { 512, 309, 560, 353 }, { 712, 309, 760, 353 },
    { 416, 416, 592, 468 },                            // ... find GPS
    { 600, 416, 784, 468 },                            // ... done
    { 600, 416, 784, 468 },                            // the requests' sheet: done
    { 512, 259, 560, 303 }, { 712, 259, 760, 303 },   // the route's sheet: the junction card, the tolls
    { 512, 309, 560, 353 }, { 712, 309, 760, 353 },
    { 600, 416, 784, 468 },                            // ... done
};
static int s_setSheet;   // SHEET_*

// The maps' page (the user 2026-10-30: Settings -> the map's row "КАРТЫ"): the map's kind and detail on top, then the
// regions' maps downloaded here (offmaps.cpp's job; the user 2026-10-29: the graphs for the routes and the search
// without the internet) - the release's catalog by its titles, kMapsRows a page, a row's button downloads it (the
// trunk roads after the first one), "ОТМЕНА" while it goes; "ЗАГРУЗИТЬ" and "ОК" (the same again) asked "ТОЧНО?"
enum { kMapsRows = 4 };
enum { M_KIND_PREV, M_KIND_NEXT, M_DETAIL_PREV, M_DETAIL_NEXT, M_ROW0, M_PREV = M_ROW0 + kMapsRows, M_NEXT, M_LIST,
       M_BACK, M_COUNT };
static const RECT kMapsCtl[M_COUNT] = {
    { 512, 79, 560, 123 }, { 712, 79, 760, 123 },     // the steppers: the kind, the detail
    { 512, 129, 560, 173 }, { 712, 129, 760, 173 },
    { 600, 181, 760, 221 }, { 600, 231, 760, 271 },   // the regions' buttons
    { 600, 281, 760, 321 }, { 600, 331, 760, 371 },
    { 300, 420, 348, 468 }, { 452, 420, 500, 468 },   // the pages
    { 716, 416, 784, 468 },                            // the catalog again (its sign: "refresh")
    { 8, 412, 80, 480 },                               // back to the settings
};
static bool s_maps;            // the maps' screen over the settings
static int  s_mapsPressed = -1, s_mapsPage;
static int  s_mapsConfirm = -1;   // the region whose button asks "ТОЧНО?"

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
    if (s_freeTick && GetTickCount() - s_freeTick < kFreeMs)
        return;   // moved by hand: the view (and its turn) stay a while
    s_freeTick = 0;
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
                t.wMinute, g_cfg.night, s_settings, (int)RouteSig(), s_search, SearchState(), s_plan, s_pickSlot, s_lpMenu, s_lpStop };
    DWORD h = 2166136261u;   // FNV-1a over them
    for (int i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++)
        h = (h ^ (DWORD)v[i]) * 16777619u;
    return h;
}

static int TrafficSource()
{
    return kTrafficSrc[g_cfg.trafficSrc];
}

static bool Tilted()
{
    return g_cfg.tilt && g_cfg.follow == FOLLOW_HEADING && g_cfg.z >= kTiltMinZ;
}

// The view's tilt while the third person's view is on (else NULL): the car keeps its place on the screen; the camera
// 45 degrees from straight down at z15, 55 at z16, 65 closer (the user 2026-10-27)
static const Tilt* ViewTilt()
{
    static Tilt t;
    if (!Tilted())
        return NULL;
    double car = g_cfg.mapH * kCarAhead;
    TiltMake(&t, car, car, kTiltFog, g_cfg.z >= 17 ? 65 : g_cfg.z == 16 ? 55 : 45);
    return &t;
}

static bool ScreenOf(double gx, double gy, double vx, double vy, double cx, double cy, double angle, double* x, double* y);

// Zoom dz from the view's: the tiles +-half of that zoom's px round the view's centre; their count
static int SquareKeys(int dz, double half, MapRequest* out, int max)
{
    int z = g_cfg.z + dz, n = 0, tiles = 1 << z;
    if (z < 2 || z > 19)
        return 0;
    double vx, vy;
    GeoToWorld(g_cfg.lon, g_cfg.lat, z, &vx, &vy);
    for (int j = (int)floor((vy - half) / TILE); j <= (int)floor((vy + half) / TILE); j++)
        for (int i = (int)floor((vx - half) / TILE); i <= (int)floor((vx + half) / TILE); i++)
            if (n < max && i >= 0 && j >= 0 && i < tiles && j < tiles) {
                MapRequest k = { SRC_OSM, z, i, j };
                out[n++] = k;
            }
    return n;
}

// The far ring's tiles (two zooms out round the view's centre) while the third person's view is on; their count
static int FarKeys(MapRequest* out, int max)
{
    return Tilted() ? SquareKeys(-2, kFarHalf, out, max) : 0;
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
    if (Tilted() && !ya) {   // the near ring's square round the view's centre (its own axes: the world's)
        a = 0;
        c = 1;
        s = 0;
        hw = hh = kNearHalf;
    }
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
// Up to maxLoads of the keys not in memory from the disk; the loads left, -1: keys left untried
static int LoadFromDisk(const MapRequest* keys, int n, int maxLoads)
{
    for (int i = 0; i < n; i++) {
        if (CacheFind(keys[i], false) || RingHas(s_missKeys, keys[i]))
            continue;
        if (!maxLoads--)
            return -1;
        if (!CacheFind(keys[i], true))
            RingAdd(s_missKeys, keys[i]);
    }
    return maxLoads;
}

// One step of drawing what is at hand (the user 2026-10-29: no waiting, no scaled tiles where the data is here): the
// view's tiles, then - while the memory has room - the far ring's, the next zooms' round the centre (+ / - show them
// at once). *view: the view's got some; returns true: more to do.
static bool DiskStep(bool* view)
{
    MapRequest keys[kMaxKeys];
    int left = LoadFromDisk(keys, NeededKeys(SRC_OSM, keys, kMaxKeys), kDiskLoads);
    *view = left != kDiskLoads;
    if (left < 0)
        return true;
    for (int k = 0; k < 3 && CacheBytes() + TILE * TILE * 2 <= CACHE_BYTES; k++) {
        int n = k == 0 ? FarKeys(keys, kMaxKeys) : SquareKeys(k == 1 ? 1 : -1, kAroundHalf, keys, kMaxKeys);
        left = LoadFromDisk(keys, n, left);
        if (left < 0)
            return true;
    }
    return false;
}

static void DiskSteps()   // from now on, till done
{
    SetTimer(s_wnd, TIMER_DISK, kDiskStepMs, NULL);
}

static int JunctionKeys(MapRequest* out, double* mx, double* my);

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
    n = FarKeys(keys, kMaxKeys);   // the far ring's, after the view's
    LoadFromDisk(keys, n, 4);
    for (int i = 0; i < n; i++)
        Request(REQ_AUTO, keys[i], CacheFind(keys[i], false));
    double mx, my;   // the junction card's tiles: after the view's (one request at a time: theirs first)
    n = JunctionKeys(keys, &mx, &my);
    LoadFromDisk(keys, n, 2);
    for (int i = 0; i < n; i++)
        Request(REQ_AUTO, keys[i], CacheFind(keys[i], false));
    if (g_cfg.traffic) {
        n = NeededKeys(TrafficSource(), keys, kMaxKeys);
        for (int i = 0; i < n; i++)
            Request(i == 0 ? mode : (mode == REQ_FORCE ? REQ_AUTO : mode), keys[i], CacheFind(keys[i], false));
    }
    DiskSteps();
}

int MapLook()
{
    return (g_cfg.vector ? 1 : 0) + (g_cfg.night ? 2 : 0) + VTileDetail() * 4;
}

static void BaseRedraw();
static int s_detailShown;   // VTileDetail() of the base map's tiles in memory

static void OnMapResult(MapResult* res)
{
    if (res->seq == s_inflightSeq)
        s_inflight = false;
    if (res->err == NET_OK && res->req.src == SRC_OSM && res->look != (g_cfg.vector ? MapLook() : -1)) {
        ImageFree(&res->img);   // drawn for the other kind / palette (switched while it came)
        res->look = -2;
    }
    if (g_cfg.vector && VTileDetail() != s_detailShown) {   // auto chose a level: the full ones drawn again
        s_detailShown = VTileDetail();
        BaseRedraw();
    }
    if (res->look == -2) {
        // dropped: the view asks for it again below
    } else if (res->err == NET_OK) {
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
    DiskSteps();
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

// The base map's tiles out of memory, the view's ones loaded / downloaded again (another kind or palette).
static void BaseRedraw()
{
    CacheDropBase();
    s_missKeys.n = 0;
    s_noParentKeys.n = 0;
    Ensure(REQ_USER);
}

// The base map: OSM's raster tiles or our vector ones (vtile.cpp).
static void MapStyleSet(int vector)
{
    if (g_cfg.vector == vector)
        return;
    g_cfg.vector = vector;
    Log("map: %s", vector ? "vector" : "raster");
    ConfigSave();
    BaseRedraw();
}

// The vector map's detail (0 auto, 1 full, 2 medium, 3 fast; the arrows go round)
static void DetailSet(int detail)
{
    g_cfg.detail = (detail + 4) % 4;
    VTileSetDetail(g_cfg.detail - 1);
    s_detailShown = VTileDetail();
    Log("map: detail %d", g_cfg.detail);
    ConfigSave();
    if (g_cfg.vector)
        BaseRedraw();
}

static void OnSettingsCtl(int id)
{
    switch (id) {
    case S_JUNC_PREV:
    case S_JUNC_NEXT:  g_cfg.junction = !g_cfg.junction; ConfigSave(); break;
    case S_TOLL_PREV:
    case S_TOLL_NEXT:  g_cfg.noToll = !g_cfg.noToll; ConfigSave(); break;
    case S_ROUTE:      s_setSheet = SHEET_ROUTE; break;
    case S_LOG_PREV:
    case S_LOG_NEXT:   g_cfg.log = !g_cfg.log; LogEnable(g_cfg.log != 0); ConfigSave(); break;
    case S_GPS:        s_setSheet = SHEET_GPS; break;
    case S_INFO:       s_setSheet = SHEET_INFO; break;
    case S_GPS_DONE:
    case S_INFO_DONE:
    case S_ROUTE_DONE: s_setSheet = SHEET_NONE; break;
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
    case S_MAPS:
        s_maps = true;
        s_mapsPage = 0;
        s_mapsConfirm = -1;
        if (!MapsCount())
            MapsList();
        break;
    case S_BACK:
        s_settings = false;
        break;
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

// A region's button: its download stopped; "ЗАГРУЗИТЬ" / "ОК" first asked "ТОЧНО?" (confirm: the one asking before)
static void MapsTap(int i, int confirm)
{
    MapRegion m;
    if (MapsBusyRegion() == i)
        MapsCancel();
    else if (MapsAt(i, &m) && MapState(m) != MAP_UPDATE && confirm != i)
        s_mapsConfirm = i;
    else
        MapsGetRegion(i);
}

static void OnMapsCtl(int id)
{
    int pages = (MapsCount() + kMapsRows - 1) / kMapsRows, confirm = s_mapsConfirm;
    s_mapsConfirm = -1;   // (any other tap: not asked any more)
    switch (id) {
    case M_KIND_PREV:
    case M_KIND_NEXT:   MapStyleSet(!g_cfg.vector); break;
    case M_DETAIL_PREV: DetailSet(g_cfg.detail - 1); break;
    case M_DETAIL_NEXT: DetailSet(g_cfg.detail + 1); break;
    case M_PREV:        if (s_mapsPage > 0) s_mapsPage--; break;
    case M_NEXT:        if (s_mapsPage + 1 < pages) s_mapsPage++; break;
    case M_LIST:        MapsList(); break;
    case M_BACK:        s_maps = false; break;
    default:            MapsTap(s_mapsPage * kMapsRows + id - M_ROW0, confirm); break;
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

static int HitRects(const RECT* r, int n, int x, int y)   // the first of the rectangles at the point; -1: none
{
    for (int i = 0; i < n; i++)
        if (x >= r[i].left && x < r[i].right && y >= r[i].top && y < r[i].bottom)
            return i;
    return -1;
}

static int HitMaps(int x, int y)
{
    return HitRects(kMapsCtl, M_COUNT, x, y);
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

// The control at the point: the page's, or the open sheet's alone (beside the sheet: S_GPS_DONE / S_INFO_DONE -
// a tap there closes it)
static int HitSettings(int x, int y)
{
    static const int kFrom[4] = { 0, S_PAGE_END, S_GPS_END, S_INFO_END },
                     kTo[4] = { S_PAGE_END, S_GPS_END, S_INFO_END, S_COUNT };
    int hit = HitRects(kSetCtl + kFrom[s_setSheet], kTo[s_setSheet] - kFrom[s_setSheet], x, y);
    if (hit >= 0)
        return kFrom[s_setSheet] + hit;
    return s_setSheet && y < kSheetY ? kTo[s_setSheet] - 1 : -1;
}

// ---------------------------------------------------------------- actions

static void PanBy(double dx, double dy)
{
    double x, y;
    ViewWorld(&x, &y);
    WorldToGeo(x + dx, y + dy, g_cfg.z, &g_cfg.lon, &g_cfg.lat);
    g_cfg.follow = FOLLOW_OFF;
}

// The flat map's point under a screen point (from the view's centre, the view turned); false: the sky or the fog
static bool FlatAt(int x, int y, double* fx, double* fy)
{
    double sx = x - (g_cfg.mapX + g_cfg.mapW / 2.0), sy = y - (g_cfg.mapY + g_cfg.mapH / 2.0), k, fade;
    const Tilt* t = ViewTilt();
    if (!t || !TiltRow(*t, sy, fy, &k, &fade))
        return false;
    *fx = sx * k;
    return true;
}

// The third person's view dragged (the user 2026-10-29: it stays on): the flat point first under the finger under it
// again; the view comes back to the car kFreeMs after
static void DragTilted(int x, int y)
{
    double fx, fy;
    if (!FlatAt(x, y, &fx, &fy))
        return;
    double dx = s_dragFx - fx, dy = s_dragFy - fy, a = ViewAngle() * M_PI / 180.0, c = cos(a), sn = sin(a);
    WorldToGeo(s_dragWx + c * dx - sn * dy, s_dragWy + sn * dx + c * dy, g_cfg.z, &g_cfg.lon, &g_cfg.lat);
    s_freeTick = GetTickCount();
}

static void ZoomBy(int dz)
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

// Closer before a complex maneuver (junction.cpp JunctionZoom; the user 2026-10-28), back to the user's zoom after it;
// the user zooming meanwhile keeps theirs for that maneuver
static int s_autoFrom;        // the user's zoom while the map is closer by itself (0: it is not)
static int s_autoSkip = -1;   // the maneuver (its point) the user zoomed at

static void AutoZoom()
{
    RouteInfo ri;
    RouteGet(&ri);
    int user = s_autoFrom ? s_autoFrom : g_cfg.z;
    bool on = ri.state == ROUTE_ON && g_cfg.follow == FOLLOW_HEADING && ri.shape != s_autoSkip;
    int want = JunctionZoom(user, on, ri.complex, ri.nextM, ri.off);
    if (want == g_cfg.z)
        return;
    s_autoFrom = want > user ? user : 0;
    ZoomBy(want - g_cfg.z);
}

static void Zoom(int dz)   // the user's
{
    if (s_autoFrom) {
        RouteInfo ri;
        RouteGet(&ri);
        s_autoSkip = ri.shape;
        s_autoFrom = 0;
    }
    ZoomBy(dz);
}

// GPS button: off -> north-up (car in the center) -> heading-up (car at the bottom) -> off
static void NextFollowMode()
{
    g_cfg.follow = (g_cfg.follow + 1) % 3;
    s_freeTick = 0;
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
    if (g_cfg.vector)   // its tiles are drawn in the night palette: drawn again
        BaseRedraw();
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
        s_setSheet = SHEET_NONE;
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
    case B_TILT:   g_cfg.tilt = !g_cfg.tilt; break;
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
    if (b == B_TILT && (g_cfg.follow != FOLLOW_HEADING || g_cfg.z < kTiltMinZ))
        return false;
    if (b == B_GPS || b == B_MINUS || b == B_PLUS || b == B_TILT) {   // round, on the right (design v3)
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
    // VestaHome on \ResidentFlash (since 2026-10-27), or an older install beside us on \SDMMC; the PC: the repos
    static const wchar_t* kIni[3] = { L"\\ResidentFlash\\VestaHome\\VestaHome.ini", L"..\\VestaHome\\VestaHome.ini",
                                      L"..\\..\\VestaHome\\VestaHome\\VestaHome\\VestaHome.ini" };
    for (int f = 0; f < 3; f++) {
        wchar_t path[MAX_PATH];
        if (kIni[f][0] == L'\\') {
#ifndef UNDER_CE
            continue;   // (the PC: no \ResidentFlash)
#endif
            wcscpy(path, kIni[f]);
        } else {
            PathInDir(path, kIni[f]);
        }
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
    wchar_t ye[256];
    if (g_noYo && wcslen(s) < 256) {
        wcscpy(ye, s);
        NoYo(ye);
        s = ye;
    }
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
        int inset = CornerInset(radius, y, r.top, r.bottom);
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

// A card over the map (design v3): the map under it at an eighth of its brightness - see-through (the user
// 2026-10-28: back as the design had it, solid since 2026-10-26 by a misunderstanding), round corners, no border; at
// night the 1 px edge.
static void Card(HDC dc, const RECT& r, int radius)
{
    Darken(dc, r, radius, 3);
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
    double gx, gy, sx, sy;
    GeoToWorld(s_gps.lon, s_gps.lat, g_cfg.z, &gx, &gy);
    if (!ScreenOf(gx, gy, vx, vy, cx, cy, angle, &sx, &sy))
        return;
    int px = (int)floor(sx + 0.5), py = (int)floor(sy + 0.5);

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
// The third person's view (ring.cpp) adds a far ring of the same size two zooms out, only while it is on.
static const int kRing = 5, kRingPx = kRing * TILE;   // 1280 x 1280, 3.2 MB
struct RingSlot { int x, y; DWORD sig; bool used; };
struct Ring {
    Image img;
    unsigned short* bits;
    int z;                            // its zoom (the far ring's: the view's - 2)
    RingSlot slot[kRing][kRing];
    int i0, j0, i1, j1;               // the tiles last brought up to date (the part loaded)
};
static Ring      s_near, s_far;
static DWORD     s_ringStamp;         // grows when a slot was drawn: the rotated frame is out of date
static DWORD     s_rotStamp = 0xFFFFFFFF;
static int       s_rotTilt = -1;      // the zoom the frame was tilted at (-1: flat)
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
static int BaseCandidates(CacheEntry** list, int n, int z, int tx, int ty, const CacheEntry** out)
{
    MapRequest k = { SRC_OSM, z, tx, ty };
    CacheEntry* exact = CacheFind(k, false);
    if (exact) {
        out[0] = exact;
        return 1;
    }
    int m = 0;
    for (int i = 0; i < n && m < CACHE_MEM; i++) {
        const CacheEntry* e = list[i];
        int dz = z - e->req.z;
        if (e->req.src != SRC_OSM || abs(dz) > kMaxParentLevels)
            continue;
        double f = pow(2.0, dz), l = e->req.x * TILE * f, t = e->req.y * TILE * f, sz = TILE * f;
        if (l >= (tx + 1) * TILE || t >= (ty + 1) * TILE || l + sz <= tx * TILE || t + sz <= ty * TILE)
            continue;
        out[m++] = e;
    }
    for (int i = 1; i < m; i++)
        for (int j = i; j > 0 && abs(out[j]->req.z - z) > abs(out[j - 1]->req.z - z); j--) {
            const CacheEntry* t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return m;
}

// What slot (tx, ty) would show now: when it differs from the slot's, the slot is drawn again.
static DWORD SlotSig(int z, int tx, int ty, bool inWorld, const CacheEntry** base, int nb, const OvPlace* ov, int no)
{
    DWORD h = Mix(Mix(Mix(2166136261u, z), tx), ty);
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
static void BlendRing(unsigned short* bits, const Overlay* ov, double left, double top, double sy, const RECT& clip)
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
        unsigned short* drow = bits + (kRingPx - 1 - y) * kRingPx;
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
static void DrawSlot(Ring& r, HDC rdc, HDC src, int tx, int ty, bool inWorld, const CacheEntry** base, int nb,
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
            double f = pow(2.0, r.z - e->req.z);
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
                BlendRing(r.bits, ov[i].e->ov, ox + ov[i].left - tx * TILE, oy + ov[i].top - ty * TILE, ov[i].sy, rc);
    const unsigned short* lut = g_cfg.night && !g_cfg.vector ? NightLut() : NULL;   // (vector: its own night palette)
    if (lut)
        for (int y = rc.top; y < rc.bottom; y++) {
            unsigned short* p = r.bits + (kRingPx - 1 - y) * kRingPx + rc.left;
            for (int x = 0; x < TILE; x++)
                p[x] = lut[p[x]];
        }
    s_statSlots++;
}

// The (rotated) window's bounding box round the view's centre: half its width and height, world px
static void WindowBox(double angle, double* ex, double* ey)
{
    double a = angle * M_PI / 180.0, c = cos(a), s = sin(a);
    double hw = g_cfg.mapW / 2.0, hh = g_cfg.mapH / 2.0;
    *ex = hw * fabs(c) + hh * fabs(s);
    *ey = hw * fabs(s) + hh * fabs(c);
}

// The ring's slots in the box +-ex, +-ey round (vx, vy) (world px of its zoom), brought up to date. Returns how many
// of them show some map. Traffic only on the view's own ring.
static int RingUpdate(Ring& r, HDC dc, double vx, double vy, double ex, double ey)
{
    int i0 = (int)floor((vx - ex) / TILE), i1 = (int)floor((vx + ex) / TILE);
    int j0 = (int)floor((vy - ey) / TILE), j1 = (int)floor((vy + ey) / TILE);
    if (i1 - i0 >= kRing) i1 = i0 + kRing - 1;   // can't be with 800 x 480; never two tiles in one slot
    if (j1 - j0 >= kRing) j1 = j0 + kRing - 1;
    r.i0 = i0;
    r.j0 = j0;
    r.i1 = i1;
    r.j1 = j1;
    int tiles = 1 << r.z;
    CacheEntry* list[CACHE_MEM];
    int n = CacheList(list, CACHE_MEM);
    OvPlace ov[CACHE_MEM];
    int no = g_cfg.traffic && r.z == g_cfg.z ? OverlayPlaces(list, n, ov) : 0;
    HDC rdc = NULL, src = NULL;
    HGDIOBJ old = NULL;
    int shown = 0;
    for (int ty = j0; ty <= j1; ty++)
        for (int tx = i0; tx <= i1; tx++) {
            bool inWorld = tx >= 0 && ty >= 0 && tx < tiles && ty < tiles;
            const CacheEntry* base[CACHE_MEM];
            int nb = inWorld ? BaseCandidates(list, n, r.z, tx, ty, base) : 0;
            if (nb)
                shown++;
            DWORD sig = SlotSig(r.z, tx, ty, inWorld, base, nb, ov, no);
            RingSlot& sl = r.slot[Mod(ty, kRing)][Mod(tx, kRing)];
            if (sl.used && sl.x == tx && sl.y == ty && sl.sig == sig)
                continue;   // the same as it is
            if (!rdc) {
                rdc = CreateCompatibleDC(dc);
                src = CreateCompatibleDC(dc);
                old = SelectObject(rdc, r.img.bmp);
            }
            DrawSlot(r, rdc, src, tx, ty, inWorld, base, nb, ov, no);
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
            d[x] = s_near.bits[(R - 1 - py) * R + px];
        }
    }
}

// A ring's view for TiltWarp: its canvas (a DIB: bottom-up), the part loaded in the view zoom's world px
static RingView RingViewOf(const Ring& r, int shift)
{
    RingView v = { r.bits + (kRingPx - 1) * kRingPx, -kRingPx, kRingPx, shift, (r.i0 * TILE) << shift,
                   (r.j0 * TILE) << shift, ((r.i1 + 1) * TILE) << shift, ((r.j1 + 1) * TILE) << shift };
    return v;
}

// The third person's view: the frame tilted out of the near ring and the far one (ring.cpp), into s_rotated.
static void TiltFrame(const Tilt& t, double vx, double vy, double angle)
{
    const int W = s_rotated.w, H = s_rotated.h, stride = ((W * 2 + 3) & ~3) / 2;
    RingView rings[2] = { RingViewOf(s_near, 0), RingViewOf(s_far, 2) };
    TiltWarp(rings, s_far.bits ? 2 : 1, t, W, H, vx, vy, angle, g_cfg.night != 0, s_rotatedBits + (H - 1) * stride,
             -stride);
}

// A point of the turned map (offsets x, y from the map's centre) on the screen: tilted in the third person's view;
// false: in its fog
static bool Lift(const Tilt* t, double* x, double* y)
{
    return !t || TiltPoint(*t, *x, *y, x, y);
}

// A piece of a line of the turned map (offsets from the map's centre) on the screen: cut to the third person's view's
// fog first; false: none of it seen; cut: its end cut off
static bool LiftSeg(const Tilt* t, double* x0, double* y0, double* x1, double* y1, bool* cut)
{
    *cut = false;
    if (!t)
        return true;
    double ex = *x1, ey = *y1;
    if (!TiltClip(*t, x0, y0, x1, y1))
        return false;
    *cut = *x1 != ex || *y1 != ey;
    TiltProject(*t, *x0, *y0, x0, y0);
    TiltProject(*t, *x1, *y1, x1, y1);
    return true;
}

// A world px of the view's zoom on the screen (turned, tilted); false: in the fog
static bool ScreenOf(double gx, double gy, double vx, double vy, double cx, double cy, double angle, double* x, double* y)
{
    double ra = angle * M_PI / 180.0, rc = cos(ra), rs = sin(ra), dx = gx - vx, dy = gy - vy;
    *x = dx * rc + dy * rs;
    *y = -dx * rs + dy * rc;
    bool seen = Lift(ViewTilt(), x, y);
    *x += cx;
    *y += cy;
    return seen;
}

// North-up: the window straight out of the ring, in 1-4 pieces where it wraps.
static void RingBlit(HDC dc, const RECT& mr, double vx, double vy)
{
    int W = mr.right - mr.left, H = mr.bottom - mr.top;
    int wl = (int)floor(vx - W / 2.0 + 0.5), wt = (int)floor(vy - H / 2.0 + 0.5);
    HDC src = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(src, s_near.img.bmp);
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
    // the vector map: OpenFreeMap's terms - OpenMapTiles' schema and the OSM data credited
    static const wchar_t* kCredit[2][3] = {
        { L"© OpenStreetMap contributors", L"© OpenStreetMap contributors, пробки © Яндекс",
          L"© OpenStreetMap contributors, пробки © 2ГИС" },
        { L"OpenFreeMap © OpenMapTiles © OpenStreetMap", L"OpenFreeMap © OpenMapTiles © OSM, пробки © Яндекс",
          L"OpenFreeMap © OpenMapTiles © OSM, пробки © 2ГИС" } };
    wchar_t s[96];   // the view's zoom first (the user 2026-10-29: as small, in this line)
    _snwprintf(s, 96, L"z%d · %s", g_cfg.z,
               kCredit[g_cfg.vector ? 1 : 0][!g_cfg.traffic ? 0 : g_cfg.trafficSrc == TRF_2GIS ? 2 : 1]);
    s[95] = 0;
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
        const Tilt* tp = ViewTilt();
        HPEN edge = CreatePen(PS_SOLID, 9, RGB(70, 76, 86)), in = CreatePen(PS_SOLID, 5, RGB(160, 170, 186));
        double px = 0, py = 0;
        for (int pass = 0; pass < 2; pass++) {
            HGDIOBJ op = SelectObject(dc, pass ? in : edge);
            for (int i = 0; i < np; i++) {
                double gx, gy;
                GeoToWorld(ll[i * 2], ll[i * 2 + 1], g_cfg.z, &gx, &gy);
                double dx = gx - vx, dy = gy - vy, x = cx + dx * rc + dy * rs, y = cy - dx * rs + dy * rc;
                if (i > 0) {
                    double x0 = px - cx, y0 = py - cy, x1 = x - cx, y1 = y - cy;
                    bool tcut, seen = LiftSeg(tp, &x0, &y0, &x1, &y1, &tcut);   // (the third person's view)
                    x0 += cx;
                    y0 += cy;
                    x1 += cx;
                    y1 += cy;
                    if (seen && ClipSeg(&x0, &y0, &x1, &y1, l, t, r, b) &&
                        (fabs(x1 - x0) + fabs(y1 - y0) >= 1 || i == np - 1)) {
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
        double gx, gy, sx, sy;
        GeoToWorld(m[0], m[1], g_cfg.z, &gx, &gy);
        wchar_t num[4];
        _snwprintf(num, 4, L"%d", k + 1);
        if (ScreenOf(gx, gy, vx, vy, cx, cy, angle, &sx, &sy))
            RouteMarker(dc, Round(sx), Round(sy), RGB(90, 98, 110), num);
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
        const Tilt* tp = ViewTilt();
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
                double x0 = px - cx, y0 = py - cy, x1 = x - cx, y1 = y - cy;
                bool tcut, in = LiftSeg(tp, &x0, &y0, &x1, &y1, &tcut);   // (the third person's view)
                x0 += cx;
                y0 += cy;
                x1 += cx;
                y1 += cy;
                double lx = x1, ly = y1;   // (its end on the screen before the box's cut)
                in = in && ClipSeg(&x0, &y0, &x1, &y1, l, t, r, b);
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
                    bool cut = tcut || x1 != lx || y1 != ly;   // leaves the box (or the fog) here
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
        double gx, gy, sx, sy;
        GeoToWorld(lon, lat, g_cfg.z, &gx, &gy);
        if (ScreenOf(gx, gy, vx, vy, cx, cy, angle, &sx, &sy))
            RouteMarker(dc, Round(sx), Round(sy), fill, t[0] ? t : NULL);
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

    // the third person's view: the far ring (3.2 MB) only while it is on
    const Tilt* tilt = ViewTilt();
    if (tilt && !s_far.bits) {
        memset(s_far.slot, 0, sizeof(s_far.slot));
        s_far.bits = ImageCreate(kRingPx, kRingPx, &s_far.img);
    } else if (!tilt && s_far.bits) {
        ImageFree(&s_far.img);
        s_far.bits = NULL;
    }

    int drawn = 0;
    if (s_near.bits && (!rotate || s_rotatedBits)) {
        double ex = kNearHalf, ey = kNearHalf;
        if (!tilt)
            WindowBox(angle, &ex, &ey);
        s_near.z = g_cfg.z;
        drawn = RingUpdate(s_near, dc, vx, vy, ex, ey);
        if (tilt && s_far.bits) {
            s_far.z = g_cfg.z - 2;
            RingUpdate(s_far, dc, vx / 4, vy / 4, kFarHalf, kFarHalf);
        }
        if (rotate) {   // turned again only when something moved
            int tz = tilt ? g_cfg.z : -1;
            if (s_rotStamp != s_ringStamp || s_rotVx != vx || s_rotVy != vy || s_rotAngle != angle || s_rotTilt != tz) {
                if (tilt)
                    TiltFrame(*tilt, vx, vy, angle);
                else
                    RotateRing(vx, vy, angle);
                s_rotTilt = tz;
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

    if (g_cfg.vector && drawn) {   // the names over the turned frame, upright (vtile.cpp); not under the car
        RECT avoid[1];   // only the car: the controls are dark enough over the names (the user 2026-10-27)
        int na = 0;
        double gx, gy, sx, sy;
        if (s_gps.fixTick) {   // the car
            GeoToWorld(s_gps.lon, s_gps.lat, g_cfg.z, &gx, &gy);
            if (ScreenOf(gx, gy, vx, vy, cx, cy, angle, &sx, &sy))
                SetRect(&avoid[na++], (int)sx - 34, (int)sy - 34, (int)sx + 34, (int)sy + 34);
        }
        g_tilt = tilt;   // (the names tilted with the frame)
        VLabelsDraw(dc, mr, cx, cy, vx, vy, angle, g_cfg.z, avoid, na);
        g_tilt = NULL;
    }
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
    Text(dc, s_fontLabel, kText, 36, y + 3, 500, y + 28, DT_LEFT, label);
    Text(dc, s_fontSmall, kTextDim, 36, y + 27, 500, y + 48, DT_LEFT, sub);
    if (sep) {
        RECT l = { 24, y + kSetRowH - 2, s_cw - 24, y + kSetRowH };
        HBRUSH b = CreateSolidBrush(kLine);
        FillRect(dc, &l, b);
        DeleteObject(b);
    }
}

// A stepper "< value >" as VestaHome's; prev / next are the S_* zones of its arrows.
static void StepperAt(HDC dc, const RECT& a, const RECT& b, int pressed, const wchar_t* value)   // pressed: 0 a, 1 b
{
    RECT box = { a.left, a.top, b.right, b.bottom };
    Tile(dc, box, kInk, kInk, 20);
    for (int k = 0; k < 2; k++) {
        const RECT& z = k ? b : a;
        bool down = pressed == k;
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

static void Stepper(HDC dc, int prev, int next, const wchar_t* value)
{
    StepperAt(dc, kSetCtl[prev], kSetCtl[next], s_setPressed == prev ? 0 : s_setPressed == next ? 1 : -1, value);
}

static void DrawGpsSheet(HDC dc);
static void DrawInfoSheet(HDC dc);
static void DrawRouteSheet(HDC dc);
static void DoneButton(HDC dc, int id);
static const wchar_t* kDetailNames[4] = { L"Авто", L"Полная", L"Средняя", L"Быстрая" };
static const wchar_t* kDetailLow[3] = { L"полная", L"средняя", L"быстрая" };

// The receiver's state in one line: its port and speed, open / busy / searching, the satellites
static void GpsLine(wchar_t* s, int cap)
{
    wchar_t baud[16];
    if (kSetBauds[s_setBaud])
        _snwprintf(baud, 16, L"%lu", kSetBauds[s_setBaud]);
    else
        wcscpy(baud, L"как есть");
    if (!s_gpsRunning)
        _snwprintf(s, cap, L"выключен (gps=0)");
    else if (s_gps.status == GPS_OPEN)
        _snwprintf(s, cap, L"COM%d · %s · %d спутн.%s", s_setPort, baud, s_gps.sats, s_gps.valid ? L" · фикс" : L"");
    else if (s_gps.status == GPS_BUSY)
        _snwprintf(s, cap, L"COM%d занят другой программой", s_setPort);
    else if (s_gps.status == GPS_SCANNING)
        _snwprintf(s, cap, L"поиск: %s %lu…", s_gps.port, s_gps.baud);
    else
        _snwprintf(s, cap, L"COM%d не открывается", s_setPort);
    s[cap - 1] = 0;
}

// Over the whole screen (the map's bars and buttons are off meanwhile), as VestaHome's settings: the title at the
// top left, the clock at the right; the panel of rows; "back" at the bottom left; a sheet over it when open.
static void DrawSettings(HDC dc)
{
    RECT all = { 0, 0, s_cw, s_ch };
    HBRUSH ink = CreateSolidBrush(kInk);
    FillRect(dc, &all, ink);
    DeleteObject(ink);
    wchar_t s[160];
    Text(dc, s_fontBig, kText, 20, 0, 500, 56, DT_LEFT, L"Настройки карты");
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 160, L"%02d:%02d", t.wHour, t.wMinute);
    Text(dc, s_fontClock, kText, s_cw - 160, 0, s_cw - 16, 56, DT_RIGHT, s);
    BackDisc(dc, s_setPressed == S_BACK);
    RECT panel = { 16, 64, s_cw - 16, 400 };
    Tile(dc, panel, kPanel, kPanel, 32);

    if (!g_cfg.vector)
        wcscpy(s, L"растровая, тайлы OSM");
    else if (g_cfg.detail)
        _snwprintf(s, 160, L"векторная · детализация: %s", kDetailLow[g_cfg.detail - 1]);
    else
        _snwprintf(s, 160, L"векторная · детализация: авто (%s)", kDetailLow[VTileDetail()]);
    SetRow(dc, kSetRow0, L"Карта", s, true);
    DrawCtl(dc, kSetCtl[S_MAPS], s_setPressed == S_MAPS, s_fontLabel, L"КАРТЫ");
    _snwprintf(s, 160, L"схема развязки: %s · платные: %s", g_cfg.junction ? L"вкл" : L"выкл",
               g_cfg.noToll ? L"объезжать" : L"можно");
    SetRow(dc, kSetRow0 + kSetRowH, L"Маршрут", s, true);
    DrawCtl(dc, kSetCtl[S_ROUTE], s_setPressed == S_ROUTE, s_fontLabel, L"НАСТРОИТЬ");
    if (g_cfg.cacheKb >= 0)
        _snwprintf(s, 160, L"занято %ld МБ", g_cfg.cacheKb / 1024);
    else
        wcscpy(s, L"занято ?");
    SetRow(dc, kSetRow0 + 2 * kSetRowH, L"Кэш карт", s, true);
    int mb = kSetCacheMb[s_setCache];
    _snwprintf(s, 160, mb >= 1000 ? L"%d ГБ" : L"%d МБ", mb >= 1000 ? mb / 1000 : mb);
    Stepper(dc, S_CACHE_PREV, S_CACHE_NEXT, s);
    // where it is written (the user 2026-10-29: "pressed, no log" - which card, which folder)
    wchar_t path[MAX_PATH];
    if (LogFile(path))   // (a long one: its end, "…" before it - the row's width)
        _snwprintf(s, 160, wcslen(path) > 44 ? L"…%s" : L"%s", path + (wcslen(path) > 44 ? wcslen(path) - 43 : 0));
    else
        wcscpy(s, g_cfg.log ? L"некуда писать: нет карты памяти" : L"OSMapsCE.log: выключен");
    SetRow(dc, kSetRow0 + 3 * kSetRowH, L"Журнал", s, true);
    Stepper(dc, S_LOG_PREV, S_LOG_NEXT, g_cfg.log ? L"Вкл" : L"Выкл");
    GpsLine(s, 160);
    SetRow(dc, kSetRow0 + 4 * kSetRowH, L"GPS", s, true);
    DrawCtl(dc, kSetCtl[S_GPS], s_setPressed == S_GPS, s_fontLabel, L"НАСТРОИТЬ");
    _snwprintf(s, 160, L"OSMapsCE %s", AppVersion(kRelease));   // VestaCore vversion.h
    SetRow(dc, kSetRow0 + 5 * kSetRowH, L"О программе", s, false);
    DrawCtl(dc, kSetCtl[S_INFO], s_setPressed == S_INFO, s_fontLabel, L"ЗАПРОСЫ");
    DrawCtl(dc, kSetCtl[S_EXIT], s_setPressed == S_EXIT, s_fontLabel, L"ЗАВЕРШИТЬ");
    if (s_setSheet == SHEET_GPS)
        DrawGpsSheet(dc);
    else if (s_setSheet == SHEET_INFO)
        DrawInfoSheet(dc);
    else if (s_setSheet == SHEET_ROUTE)
        DrawRouteSheet(dc);
}

static void Sheet(HDC dc, const wchar_t* title)   // a sheet over the bottom: full width, its title
{
    RECT r = { 0, kSheetY, s_cw, s_ch + 40 };   // (the bottom corners off the screen)
    Tile(dc, r, kPanel, kLine, 32);
    Text(dc, s_fontBig, kText, 24, kSheetY + 4, s_cw - 24, kSheetY + 52, DT_LEFT, title);
}

// The GPS port: its number and speed, the lines as they come, the search over COM1..COM9
static void DrawGpsSheet(HDC dc)
{
    wchar_t s[160];
    GpsLine(s, 160);
    Sheet(dc, L"GPS");
    Text(dc, s_fontSmall, kTextDim, 120, kSheetY + 4, s_cw - 24, kSheetY + 52, DT_LEFT, s);
    SetRow(dc, kSetCtl[S_PORT_PREV].top - 3, L"Порт", L"COM1…COM9", true);
    _snwprintf(s, 160, L"COM%d", s_setPort);
    Stepper(dc, S_PORT_PREV, S_PORT_NEXT, s);
    SetRow(dc, kSetCtl[S_BAUD_PREV].top - 3, L"Скорость порта", L"или как у оболочки", false);
    if (kSetBauds[s_setBaud])
        _snwprintf(s, 160, L"%lu", kSetBauds[s_setBaud]);
    else
        wcscpy(s, L"не менять");
    Stepper(dc, S_BAUD_PREV, S_BAUD_NEXT, s);
    if (s_gps.valid)
        _snwprintf(s, 160, L"NMEA %d (ошибок %d) · %.5f, %.5f", s_gps.lines, s_gps.badLines, s_gps.lat, s_gps.lon);
    else
        _snwprintf(s, 160, L"NMEA %d (ошибок %d) · координат пока нет", s_gps.lines, s_gps.badLines);
    s[159] = 0;
    Text(dc, s_fontSmall, s_gps.valid ? kText : kTextDim, 36, 358, s_cw - 36, 380, DT_LEFT, s);
    COLORREF c = kTextDim;
    if (s_gps.found == 1) {
        _snwprintf(s, 160, L"Найден GPS: %s %lu, настройки сохранены", s_gps.port, s_gps.baud);
        c = RGB(120, 220, 120);
    } else if (s_gps.found == -1) {
        wcscpy(s, L"GPS не найден на COM1…COM9");
        c = kTextErr;
    } else {   // the last line as it came (no fix yet), or the hint
        MultiByteToWideChar(CP_ACP, 0, s_gps.last, -1, s, 160);
        if (s_gps.valid)
            s[0] = 0;
        else if (!s[0])
            wcscpy(s, L"Нет строк — не тот порт. Строки с ошибкой — не та скорость.");
    }
    s[159] = 0;
    Text(dc, s_fontSmall, c, 36, 382, s_cw - 36, 404, DT_LEFT, s);
    DrawCtl(dc, kSetCtl[S_SCAN], s_setPressed == S_SCAN, s_fontLabel, L"НАЙТИ GPS");
    DoneButton(dc, S_GPS_DONE);
}

static void DoneButton(HDC dc, int id)   // a sheet's main action: in the accent
{
    const RECT& r = kSetCtl[id];
    Tile(dc, r, kOrange, kOrange, 20);
    Text(dc, s_fontLabel, kInk, r.left, r.top, r.right, r.bottom, DT_CENTER, L"ГОТОВО");
}

// The route's choices: the junction card; the toll roads (the user 2026-10-29) - kept off by the graphs of the card
// (the regions', the trunk roads'); OSRM online cannot
static void DrawRouteSheet(HDC dc)
{
    Sheet(dc, L"Маршрут");
    SetRow(dc, kSetCtl[S_JUNC_PREV].top - 3, L"Схема развязки", L"перед съездом, развилкой, кольцом", true);
    Stepper(dc, S_JUNC_PREV, S_JUNC_NEXT, g_cfg.junction ? L"Вкл" : L"Выкл");
    SetRow(dc, kSetCtl[S_TOLL_PREV].top - 3, L"Платные дороги", L"объезд — по картам на флешке", false);
    Stepper(dc, S_TOLL_PREV, S_TOLL_NEXT, g_cfg.noToll ? L"Объезжать" : L"Можно");
    Text(dc, s_fontSmall, kTextDim, 36, 362, s_cw - 36, 404, DT_LEFT,
         L"Без карт области и магистралей маршрут строит сервер — он платные не исключает.");
    DoneButton(dc, S_ROUTE_DONE);
}

// The day's requests by their kind, against their limits
static void DrawInfoSheet(HDC dc)
{
    Sheet(dc, L"Запросы за день");
    wchar_t s[4][96];
    _snwprintf(s[0], 96, L"Пробки Яндекса: %d из %d", *Counter(SRC_YANDEX), Limit(SRC_YANDEX));
    _snwprintf(s[1], 96, L"Пробки 2ГИС: %d из %d", *Counter(SRC_2GIS), Limit(SRC_2GIS));
    _snwprintf(s[2], 96, L"Плитки OSM: %d", g_cfg.osmCount);
    _snwprintf(s[3], 96, L"Поиск и маршрут: %d из %d", g_cfg.apiCount, g_cfg.apiLimit);
    for (int i = 0; i < 4; i++)
        Text(dc, s_fontLabel, kText, 36, 262 + i * 36, s_cw - 36, 296 + i * 36, DT_LEFT, s[i]);
    DoneButton(dc, S_INFO_DONE);
}

// A region: its title, size and state, its button ("ОТМЕНА" while it downloads, "ТОЧНО?" in the accent when asked)
static void MapsRow(HDC dc, int r, int i, const MapRegion& m)
{
    static const wchar_t* kState[3] = { L"не найдена", L"есть новее", L"последняя версия" };
    static const wchar_t* kBtn[3] = { L"ЗАГРУЗИТЬ", L"ОБНОВИТЬ", L"ОК" };
    wchar_t t[96], sub[64];
    if (!MultiByteToWideChar(CP_UTF8, 0, m.title[0] ? m.title : m.name, -1, t, 96))
        t[0] = 0;
    t[95] = 0;
    int st = MapState(m);
    _snwprintf(sub, 64, L"%d,%d МБ · %s", m.sizeKb / 1024, m.sizeKb % 1024 * 10 / 1024, kState[st]);
    sub[63] = 0;
    SetRow(dc, kSetRow0 + (r + 2) * kSetRowH, t, sub, r < kMapsRows - 1);
    const RECT& b = kMapsCtl[M_ROW0 + r];
    if (s_mapsConfirm == i) {
        Tile(dc, b, kOrange, kOrange, 20);
        Text(dc, s_fontLabel, kInk, b.left, b.top, b.right, b.bottom, DT_CENTER, L"ТОЧНО?");
    } else
        DrawCtl(dc, b, s_mapsPressed == M_ROW0 + r, s_fontLabel, MapsBusyRegion() == i ? L"ОТМЕНА" : kBtn[st]);
}

// The catalog's button: a "refresh" sign - a circle's arc, clockwise, its arrow's head at the end (CE's GDI has no Arc:
// its points)
static void RefreshSign(HDC dc, const RECT& r, COLORREF c)
{
    double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0, rad = 11;
    POINT p[13], head[3];
    for (int k = 0; k < 13; k++) {   // from 315 degrees back round to 45 (the gap on the right)
        double a = (315 - k * 22.5) * M_PI / 180;
        p[k].x = (int)(cx + rad * cos(a));
        p[k].y = (int)(cy - rad * sin(a));
    }
    double a = 45 * M_PI / 180, dx = sin(a), dy = cos(a);   // the way on at the end (clockwise), the screen's y down
    head[0].x = (int)(p[12].x + 7 * dx);
    head[0].y = (int)(p[12].y + 7 * dy);
    head[1].x = (int)(p[12].x - 6 * dy);
    head[1].y = (int)(p[12].y + 6 * dx);
    head[2].x = (int)(p[12].x + 6 * dy);
    head[2].y = (int)(p[12].y - 6 * dx);
    HPEN pen = CreatePen(PS_SOLID, 3, c);
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    Polyline(dc, p, 13);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, head, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void MapsLine(HDC dc)   // the job's line, with the download's progress while it goes
{
    wchar_t text[128], s[192];
    int got, total;
    bool busy = MapsJob(text, 128, &got, &total) == MAPS_BUSY;
    if (busy && total > 0)
        _snwprintf(s, 192, L"%s %d,%d из %d,%d МБ", text, got / 1024, got % 1024 * 10 / 1024, total / 1024,
                   total % 1024 * 10 / 1024);
    else
        wcscpy(s, text);
    s[191] = 0;
    Text(dc, s_fontSmall, busy ? kOrange : kTextDim, 36, 378, s_cw - 36, 404, DT_LEFT, s);
}

// The map's kind and the vector one's detail (the user 2026-10-30: as the device can draw it) - the page's top rows
static void MapsLook(HDC dc)
{
    wchar_t s[64];
    SetRow(dc, kSetRow0, L"Вид", g_cfg.vector ? L"векторная, OpenFreeMap" : L"растровая, тайлы OSM", true);
    StepperAt(dc, kMapsCtl[M_KIND_PREV], kMapsCtl[M_KIND_NEXT], s_mapsPressed - M_KIND_PREV,
              g_cfg.vector ? L"Вектор" : L"Растр");
    if (g_cfg.detail)
        wcscpy(s, L"меньше деталей — быстрее рисуется");
    else
        _snwprintf(s, 64, L"по скорости устройства: сейчас %s", kDetailLow[VTileDetail()]);
    SetRow(dc, kSetRow0 + kSetRowH, L"Детализация", s, true);
    StepperAt(dc, kMapsCtl[M_DETAIL_PREV], kMapsCtl[M_DETAIL_NEXT], s_mapsPressed - M_DETAIL_PREV,
              kDetailNames[g_cfg.detail]);
}

// Over the settings, as they look: the title, the clock, a panel with the rows, the pages and "refresh" below
static void DrawMaps(HDC dc)
{
    RECT all = { 0, 0, s_cw, s_ch };
    HBRUSH ink = CreateSolidBrush(kInk);
    FillRect(dc, &all, ink);
    DeleteObject(ink);
    wchar_t s[32];
    Text(dc, s_fontBig, kText, 20, 0, 500, 56, DT_LEFT, L"Карты");
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 32, L"%02d:%02d", t.wHour, t.wMinute);
    Text(dc, s_fontClock, kText, s_cw - 160, 0, s_cw - 16, 56, DT_RIGHT, s);
    BackDisc(dc, s_mapsPressed == M_BACK);
    RECT panel = { 16, 64, s_cw - 16, 412 };
    Tile(dc, panel, kPanel, kPanel, 32);
    int n = MapsCount(), pages = n ? (n + kMapsRows - 1) / kMapsRows : 1;
    if (s_mapsPage >= pages)
        s_mapsPage = pages - 1;
    MapsLook(dc);
    MapRegion m;
    for (int r = 0; r < kMapsRows && MapsAt(s_mapsPage * kMapsRows + r, &m); r++)
        MapsRow(dc, r, s_mapsPage * kMapsRows + r, m);
    MapsLine(dc);
    DrawCtl(dc, kMapsCtl[M_PREV], s_mapsPressed == M_PREV, s_fontBig, L"\x2039");
    DrawCtl(dc, kMapsCtl[M_NEXT], s_mapsPressed == M_NEXT, s_fontBig, L"\x203A");
    _snwprintf(s, 32, L"%d/%d", s_mapsPage + 1, pages);
    Text(dc, s_fontLabel, kText, 348, 420, 452, 468, DT_CENTER, s);
    DrawCtl(dc, kMapsCtl[M_LIST], s_mapsPressed == M_LIST, s_fontLabel, L"");
    RefreshSign(dc, kMapsCtl[M_LIST], s_mapsPressed == M_LIST ? kInk : kText);
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
// ---- the junction card (junction.cpp; the user 2026-10-28, on in the settings): under the big turn card before a
// complex maneuver, the map at z17 round it turned so the way in points up, the route on it. Built only when what it
// shows changes (the maneuver, its tiles coming, night) - through a 512 x 512 canvas freed right after - and copied
// into each frame.
enum { kJunZ = 17, kJunCanvas = 512, kJunW = 242, kJunH = 142, kJunTiles = 9 };
static unsigned short* s_junCard;   // kJunW x kJunH, rows top-down
static DWORD s_junSig;

// The card's tiles (z17 round the next maneuver) while it is shown, and the maneuver's world pixel; their count
static int JunctionKeys(MapRequest* out, double* mx, double* my)
{
    RouteInfo ri;
    RouteGet(&ri);
    const double* ll;
    int np = RoutePoints(&ll), n = 0, h = kJunCanvas / 2;
    if (!JunctionShown(g_cfg.junction != 0, ri.complex, ri.nextM, ri.off) || ri.shape >= np)
        return 0;
    GeoToWorld(ll[ri.shape * 2], ll[ri.shape * 2 + 1], kJunZ, mx, my);
    for (int ty = (int)floor((*my - h) / TILE); ty <= (int)floor((*my + h - 1) / TILE); ty++)
        for (int tx = (int)floor((*mx - h) / TILE); tx <= (int)floor((*mx + h - 1) / TILE) && n < kJunTiles; tx++) {
            MapRequest k = { SRC_OSM, kJunZ, tx, ty };
            out[n++] = k;
        }
    return n;
}

static DWORD JunctionSig(const MapRequest* keys, int n)
{
    RouteInfo ri;
    RouteGet(&ri);
    const double* ll;
    RoutePoints(&ll);
    DWORD h = Mix(Mix(Mix(2166136261u, ri.shape), (DWORD)(size_t)ll), g_cfg.night);
    for (int i = 0; i < n; i++) {
        CacheEntry* e = CacheFind(keys[i], false);
        h = Mix(h, e ? e->fetchTick + 1 : 0);
    }
    return h;
}

// The route round the maneuver on the canvas: white edge, the accent inside, an arrow at its end; the way in's course
static double JunctionRoute(HDC cdc, double ox, double oy)
{
    RouteInfo ri;
    RouteGet(&ri);
    const double* ll;
    int np = RoutePoints(&ll), at;
    double pts[2 * 64], course;
    int n = JunctionSlice(ll, np, ri.shape, 110, 70, pts, 64, &at, &course);
    POINT p[64];
    for (int i = 0; i < n; i++) {
        double wx, wy;
        GeoToWorld(pts[i * 2], pts[i * 2 + 1], kJunZ, &wx, &wy);
        p[i].x = Round(wx - ox);
        p[i].y = Round(wy - oy);
    }
    for (int pass = 0; pass < 2 && n > 1; pass++) {
        HPEN pen = CreatePen(PS_SOLID, pass ? 9 : 14, pass ? kOrange : RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(cdc, pen);
        Polyline(cdc, p, n);
        SelectObject(cdc, op);
        DeleteObject(pen);
    }
    if (n > 1) {   // the arrow's head along the last piece
        double a = atan2((double)(p[n - 1].y - p[n - 2].y), (double)(p[n - 1].x - p[n - 2].x));
        POINT h[3];
        for (int k = 0; k < 3; k++) {
            double ang = a + (k == 0 ? 0 : k == 1 ? 2.4 : -2.4), len = k == 0 ? 14 : 16;
            h[k].x = p[n - 1].x + Round(cos(ang) * len);
            h[k].y = p[n - 1].y + Round(sin(ang) * len);
        }
        HBRUSH b = CreateSolidBrush(kOrange);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ ob = SelectObject(cdc, b), op = SelectObject(cdc, pen);
        Polygon(cdc, h, 3);
        SelectObject(cdc, op);
        SelectObject(cdc, ob);
        DeleteObject(pen);
        DeleteObject(b);
    }
    return course;
}

// The card's picture made again: the tiles there and the route on a canvas, turned into s_junCard
static void JunctionBuild(HDC dc, const MapRequest* keys, int n, double mx, double my)
{
    Image canvas = { 0 };
    unsigned short* bits = ImageCreate(kJunCanvas, kJunCanvas, &canvas);
    if (!bits)
        return;
    HDC cdc = CreateCompatibleDC(dc), tdc = CreateCompatibleDC(dc);
    HGDIOBJ oc = SelectObject(cdc, canvas.bmp);
    RECT all = { 0, 0, kJunCanvas, kJunCanvas };
    HBRUSH bg = CreateSolidBrush(kMapBg);
    FillRect(cdc, &all, bg);
    DeleteObject(bg);
    double ox = mx - kJunCanvas / 2, oy = my - kJunCanvas / 2;
    for (int i = 0; i < n; i++) {
        CacheEntry* e = CacheFind(keys[i], false);
        if (!e)
            continue;
        HGDIOBJ ot = SelectObject(tdc, e->img.bmp);
        BitBlt(cdc, Round(keys[i].x * TILE - ox), Round(keys[i].y * TILE - oy), TILE, TILE, tdc, 0, 0, SRCCOPY);
        SelectObject(tdc, ot);
    }
    double course = JunctionRoute(cdc, ox, oy);
#ifndef UNDER_CE
    GdiFlush();
#endif
    JunctionTurn(bits + (kJunCanvas - 1) * kJunCanvas, kJunCanvas, kJunCanvas, -kJunCanvas, kJunCanvas / 2,
                 kJunCanvas / 2, s_junCard, kJunW, kJunH, kJunW, kJunW / 2, kJunH * 0.62, course, RGB565(228, 226, 222), 0);
    SelectObject(cdc, oc);
    DeleteDC(cdc);
    DeleteDC(tdc);
    ImageFree(&canvas);
}

static void JunctionFree()
{
    free(s_junCard);
    s_junCard = NULL;
    s_junSig = 0;
}

// The card under the big turn card (its top at y), or nothing (the card's memory freed)
static void DrawJunction(HDC dc, int top)
{
    MapRequest keys[kJunTiles];
    double mx, my;
    int n = JunctionKeys(keys, &mx, &my);
    if (!n || !s_backBits) {
        JunctionFree();
        return;
    }
    if (!s_junCard && !(s_junCard = (unsigned short*)malloc(kJunW * kJunH * 2)))
        return;
    DWORD sig = JunctionSig(keys, n) | 1;
    if (sig != s_junSig) {
        JunctionBuild(dc, keys, n, mx, my);
        s_junSig = sig;
    }
    RECT c = { 16, top, 16 + kJunW + 8, top + kJunH + 8 };
    Card(dc, c, 20);
#ifndef UNDER_CE
    GdiFlush();
#endif
    int stride = BackStride(), x0 = c.left + 4, y0 = c.top + 4;
    for (int y = 0; y < kJunH && y0 + y < s_ch; y++) {
        int in = CornerInset(16, y, 0, kJunH);
        memcpy(s_backBits + (s_ch - 1 - (y0 + y)) * stride + x0 + in, s_junCard + y * kJunW + in, (kJunW - 2 * in) * 2);
    }
}

static void DrawGuide(HDC dc)
{
    RouteInfo ri;
    RouteGet(&ri);
    if (ri.state != ROUTE_ON)
        JunctionFree();
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
        JunctionFree();
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
    DrawJunction(dc, p.bottom + 8);   // (under the card: its own pixels first, the card's GDI after)
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
    return ri.state == ROUTE_ON && !s_dockShown && RouteAlts() < 2 && s_pickSlot < 0 && !s_lpMenu && s_lpStop < 0;
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
enum { kFieldX = 92, kFieldR = 800 - 24 };   // the query's text on the field (from its left: VKeysLayout's x)

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
    if (id >= K_SHIFT && id <= K_FIND) {   // the bottom row: Shift, language, space, backspace, find
        static const int x[6] = { 8, 128, 248, 548, 668, 792 };
        int k = id - K_SHIFT;
        SetRect(r, x[k], kKbY + 4 * kKbRowH, x[k + 1] - 6, kKbY + 5 * kKbRowH - 6);
        return true;
    }
    row = id / 16;
    col = id % 16;
    if (row > 3 || col >= VKeysRowLen(&s_kb, row))
        return false;
    int n = VKeysRowLen(&s_kb, row), w = (784 - (n - 1) * 6) / n, x0 = 8 + (784 - (n * w + (n - 1) * 6)) / 2;
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
    int n = (int)wcslen(s_kb.text), c = s_kb.cur < n ? s_kb.cur : n;
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
    } else if (id == K_SHIFT) {
        s_kb.shift = !s_kb.shift;
    } else if (id == K_LANG) {
        s_kb.en = !s_kb.en;
    } else if (id == K_SPACE) {
        if (c > 0 && s_kb.text[c - 1] != L' ' && s_kb.text[c] != L' ')   // (no doubled spaces, none first)
            VKeysType(&s_kb, L' ');
    } else if (id == K_BS) {
        VKeysBack(&s_kb);
    } else if (id == K_FIND) {
        double lon, lat;
        SearchFrom(&lon, &lat);
        if (n) {
            SearchStart(s_kb.text, lon, lat);
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
    } else if (id / 16 < 4) {
        VKeysType(&s_kb, VKeysChar(&s_kb, id / 16, id % 16));
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

static void Bar(HDC dc, int x, int y, int w, int h, COLORREF c)   // the cursor's line
{
    RECT b = { x, y, x + w, y + h };
    Tile(dc, b, c, c, 0);
}

// The query on the field from the part in sight, the cursor (keyboard shown) where it is, "‹ ›" where more is
static void DrawQuery(HDC dc, const RECT& r)
{
    if (!s_kb.text[0]) {
        Text(dc, s_fontBig, kTextDim, kFieldX, r.top, kFieldR, r.bottom, DT_LEFT,
             s_showHits ? L"" : L"Адрес или место");
        if (!s_showHits)
            Bar(dc, kFieldX, r.top + 10, 2, r.bottom - r.top - 20, kOrange);
        return;
    }
    HGDIOBJ of = SelectObject(dc, s_fontBig);
    int cx = kFieldX + VKeysLayout(&s_kb, dc, kFieldR - kFieldX);
    SelectObject(dc, of);
    wchar_t shown[VKEYS_MAX];
    wcsncpy(shown, s_kb.text + s_kb.view, s_kb.visN);
    shown[s_kb.visN] = 0;
    Text(dc, s_fontBig, kText, kFieldX, r.top, kFieldR + 8, r.bottom, DT_LEFT, shown);
    if (s_kb.view > 0)
        Text(dc, s_fontBig, kOrange, r.left + 2, r.top, kFieldX, r.bottom, DT_CENTER, L"‹");
    if (s_kb.view + s_kb.visN < (int)wcslen(s_kb.text))
        Text(dc, s_fontBig, kOrange, kFieldR, r.top, r.right, r.bottom, DT_CENTER, L"›");
    if (!s_showHits)
        Bar(dc, cx - 1, r.top + 8, 3, r.bottom - r.top - 16, kOrange);
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
    DrawQuery(dc, r);

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
    static const wchar_t* kBottom[5] = { L"Aa", NULL, L"ПРОБЕЛ", L"СТЕРЕТЬ", L"НАЙТИ" };
    for (int id = 0; id <= K_FIND; id++) {
        if (!SearchRect(id, &r))
            continue;
        bool lit = s_kbPressed == id || (id == K_FIND && s_kb.text[0]) || (id == K_SHIFT && s_kb.shift);
        wchar_t cap[8] = { 0 };
        if (id < K_SHIFT)
            cap[0] = VKeysChar(&s_kb, id / 16, id % 16);
        else
            wcscpy(cap, id == K_LANG ? (s_kb.en ? L"РУС" : L"ENG") : kBottom[id - K_SHIFT]);
        COLORREF kc = lit ? kOrange : kPanel2;
        Tile(dc, r, kc, kc, 16);
        Text(dc, id < K_SHIFT ? s_fontBig : s_fontLabel, lit ? kInk : kText, r.left, r.top, r.right, r.bottom,
             DT_CENTER, cap);
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

// The stop whose sign is within kStopNearPx of the place (the view's zoom's pixels), -1: none
static int StopAt(double lon, double lat)
{
    const RoutePlan* p = RoutePlanGet();
    double x, y;
    GeoToWorld(lon, lat, g_cfg.z, &x, &y);
    for (int i = 0; i < p->nvia; i++) {
        double sx, sy;
        GeoToWorld(p->via[i].lon, p->via[i].lat, g_cfg.z, &sx, &sy);
        if (p->via[i].kind == PL_PLACE && fabs(sx - x) < kStopNearPx && fabs(sy - y) < kStopNearPx)
            return i;
    }
    return -1;
}

static void OnLongPress(int x, int y)
{
    double lon, lat;
    ScreenToGeo(x, y, &lon, &lat);
    Log("ui: long press %.5f,%.5f", lat, lon);
    RouteInfo ri;
    RouteGet(&ri);
    s_dockShown = false;
    bool on = ri.state == ROUTE_ON || ri.state == ROUTE_BUSY;
    s_lpStop = on ? StopAt(lon, lat) : -1;   // on a stop's sign: its menu
    if (s_lpStop < 0 && on) {
        s_lpMenu = true;
        s_lpLon = lon;
        s_lpLat = lat;
    } else if (s_lpStop < 0) {
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

static void StopOut(int i)   // the plan's stop i out, the later ones up
{
    RoutePlan* p = RoutePlanGet();
    for (int k = i + 1; k < p->nvia; k++)
        p->via[k - 1] = p->via[k];
    p->nvia--;
}

// The plan's route started (the editor's "go", a stop's menu); a plan not complete: why, in the editor
static void PlanGo()
{
    RoutePlan* p = RoutePlanGet();
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
            if (p->via[i].kind == PL_NONE)
                StopOut(i);
            else
                i++;
        }
        PlanGo();
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
    bool stop = s_lpStop >= 0 && id >= M_STOP_OUT && id <= M_STOP_CANCEL;
    if (!stop && (!s_lpMenu || id < M_VIA || id > M_CANCEL))
        return false;
    if ((id == M_VIA && RoutePlanGet()->nvia >= kMaxVia) || (id == M_STOP_UP && s_lpStop == 0))
        return false;
    int i = stop ? id - M_STOP_OUT - (id > M_STOP_UP && s_lpStop == 0) : id - M_VIA;   // (no "earlier": closed up)
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
    for (int id = M_VIA; id <= M_STOP_CANCEL; id++)
        if (MenuRect(id, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return id;
    if (TripShown() && x >= 196 && x < 604 && y >= 400 && y < 466)   // the trip's card: the route's editor
        return P_TRIP;
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
    } else if (id == M_STOP_OUT || id == M_STOP_UP) {   // the stop out / one earlier, the route again
        RoutePlan* p = RoutePlanGet();
        if (id == M_STOP_OUT)
            StopOut(s_lpStop);
        else {
            RoutePlace t = p->via[s_lpStop - 1];
            p->via[s_lpStop - 1] = p->via[s_lpStop];
            p->via[s_lpStop] = t;
        }
        s_lpStop = -1;
        PlanGo();
    } else if (id == M_STOP_CANCEL) {
        s_lpStop = -1;
    } else if (id == P_TRIP) {
        s_plan = true;
        s_planChoose = -1;
        s_planMsg = NULL;
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
    if (s_lpStop >= 0) {
        int n = s_lpStop > 0 ? 3 : 2;
        RECT box = { 204, 288 - 40, 596, 300 + n * 58 + 4 };
        Card(dc, box, 20);
        wchar_t t[32];
        _snwprintf(t, 32, L"«Заехать %d»", s_lpStop + 1);
        Text(dc, s_fontLabel, kText, box.left, box.top + 4, box.right, box.top + 48, DT_CENTER, t);
        static const wchar_t* kStop[3] = { L"УБРАТЬ ТОЧКУ", L"РАНЬШЕ", L"ОТМЕНА" };
        for (int id = M_STOP_OUT; id <= M_STOP_CANCEL; id++)
            if (MenuRect(id, &r))
                DrawCtl(dc, r, s_planPressed == id, s_fontLabel, kStop[id - M_STOP_OUT]);
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
        RECT bar = { 0, y, s_cw, s_ch }, line = { 0, y, s_cw, y + 2 };
        // see-through again (the user 2026-10-28, as the design had it); the orange part of the line shrinks to the
        // moment the bar goes down
        DWORD el = GetTickCount() - s_dockTick;
        RECT left = { 0, y, el >= (DWORD)kDockMs ? 0 : (int)(s_cw * (kDockMs - el) / kDockMs), y + 2 };
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
        if (i == B_GPS || i == B_MINUS || i == B_PLUS || i == B_TILT) {   // a dark round button over the map, no border
            if (pressed)
                Tile(dc, r, kOrange, kOrange, kRound);
            else
                Card(dc, r, kRound / 2);
            if (i == B_TILT)   // "3D", in the accent while it is on
                Text(dc, s_fontLabel, pressed ? kInk : g_cfg.tilt ? kOrange : kText, r.left, r.top, r.right, r.bottom,
                     DT_CENTER, L"3D");
            else
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
    if (s_settings && s_maps) {
        DrawMaps(dc);
    } else if (s_settings) {
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
    g_noYo = ok != 2;   // (a device's own font may have no yo: Shturmann NSL 500's squares)
    Log("ui: fonts %S%s", s_face, g_noYo ? " (yo as ye)" : "");
}

const wchar_t* MapFace()
{
    return s_face;
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
    VKeysOpen(&s_kb, L"", kQueryCap);
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
    s_near.bits = ImageCreate(kRingPx, kRingPx, &s_near.img);
    CacheInit();
    VTileInit();

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
    KillTimer(hwnd, TIMER_DISK);
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
        // The wheel's buttons (the user 2026-10-26): short presses stay VestaHome's (tracks, the phone); held - the
        // zoom (WheelZoom), while the map is in front (VestaHome asks only then).
        int code = VhField(cmd, L"code", v, 8) ? _wtoi(v) : 0;
        int st = VhField(cmd, L"st", v, 8) ? _wtoi(v) : 0;
        int kind = VhField(cmd, L"kind", v, 8) ? _wtoi(v) : 0;
        int dz = WheelZoom(code, kind);
        if (dz) {
            if (st == 0) {
                Zoom(dz);
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
        if (wp == TIMER_DISK) {
            bool view;
            if (!DiskStep(&view))
                KillTimer(hwnd, TIMER_DISK);
            if (view)
                InvalidateRect(hwnd, NULL, FALSE);
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
            if (s_settings && s_maps)
                InvalidateRect(hwnd, NULL, FALSE);   // the download's progress
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
            AutoZoom();
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
            else if (wp >= 32) {
                VKeysType(&s_kb, (wchar_t)wp);
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
        } else if (s_settings && s_maps) {
            s_mapsPressed = HitMaps(x, y);
        } else if (s_settings) {
            s_setPressed = HitSettings(x, y);
        } else if (s_search) {
            s_kbPressed = HitSearch(x, y);
            if (s_kbPressed == K_FIELD && !s_showHits)   // the cursor to the finger
                VKeysCursorAt(&s_kb, x - kFieldX, kFieldR - kFieldX);
        } else if (s_plan) {
            s_planPressed = HitPlan(x, y);
        } else if ((s_planPressed = HitOverMap(x, y)) >= 0) {
            // the picker's "cancel", the long press menu
        } else if (s_lpMenu || s_lpStop >= 0) {
            s_lpMenu = false;   // a tap beside the menu closes it
            s_lpStop = -1;
        } else if (InMap(x, y)) {
            SetTimer(hwnd, TIMER_LONG, kLongMs, NULL);
            s_longDone = false;
            s_downTick = GetTickCount();
            s_dragging = true;
            s_dragMoved = false;
            s_dragStart.x = x;
            s_dragStart.y = y;
            ViewWorld(&s_dragWx, &s_dragWy);
            s_dragTilt = FlatAt(x, y, &s_dragFx, &s_dragFy);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (s_search && s_kbPressed == K_FIELD && !s_showHits) {   // the cursor follows the finger
            VKeysCursorAt(&s_kb, x - kFieldX, kFieldR - kFieldX);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
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
            if (s_dragMoved && s_dragTilt) {
                DragTilted(x, y);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (s_dragMoved) {
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
        } else if (s_mapsPressed >= 0) {
            int c = s_mapsPressed;
            s_mapsPressed = -1;
            if (HitMaps(x, y) == c)
                OnMapsCtl(c);
            InvalidateRect(hwnd, NULL, FALSE);
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

#ifndef UNDER_CE
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A   // (VS2005's headers declare it only for _WIN32_WINNT >= 0x0400)
#endif
    case WM_MOUSEWHEEL: {   // the PC: the wheel zooms - around the point under the cursor (following the car: around it)
        if (s_settings || s_search || s_plan)
            return 0;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        int dz = (short)HIWORD(wp) > 0 ? 1 : -1;
        if (g_cfg.follow != FOLLOW_OFF || !InMap(pt.x, pt.y)) {
            OnButton(dz > 0 ? B_PLUS : B_MINUS);
            return 0;
        }
        double lon, lat, x, y, a = ViewAngle() * M_PI / 180.0;
        ScreenToGeo(pt.x, pt.y, &lon, &lat);
        int z = g_cfg.z;
        Zoom(dz);
        if (g_cfg.z != z) {   // the place under the cursor back under it
            double sx = pt.x - (g_cfg.mapX + g_cfg.mapW / 2.0), sy = pt.y - (g_cfg.mapY + g_cfg.mapH / 2.0);
            GeoToWorld(lon, lat, g_cfg.z, &x, &y);
            WorldToGeo(x - (cos(a) * sx - sin(a) * sy), y - (sin(a) * sx + cos(a) * sy), g_cfg.z, &g_cfg.lon, &g_cfg.lat);
        }
        UserChanged();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
#endif

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

    bool cfg = ConfigLoad();
    VTileSetDetail(g_cfg.detail - 1);
    s_detailShown = VTileDetail();
    LogInit();   // (after the settings: the log only when asked for)
    Log("OSMapsCE start, dir %S", g_dir);
    if (!cfg)
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
