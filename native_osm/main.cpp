#include "yamaps.h"
#include "fling.h"
#include "zoomanim.h"
#include "trafshade.h"
#include "vkeys.h"     // VestaCore: the keyboard's text and cursor (VestaHome's)
#include "vfavs.h"     // VestaCore: the favourite places (VestaHome reads them too)
#include "vversion.h"   // VestaCore: the version "major.minor.YYMMDD" (the date: this file's build)
#include "vhap.h"      // VestaCore: VestaHome's add-ons' protocol
#include "lang.h"      // the interface's languages
#include "startpt.h"   // where a route starts
#include "vnav.h"      // VestaCore: the next manoeuvre as VestaHome is told it, its sign
#include "vfont.h"     // VestaCore: the fonts
static const wchar_t* const kRelease = L"2.1";   // raise by hand: minor - significant changes, major - big releases

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

// The map's ground where no tile is yet: the vector map's night palette has its own (not turned by NightLut) - by
// night a light square stood out (the user 2026-11-02: white squares)
static COLORREF MapBg()
{
    return g_cfg.night && g_cfg.vector ? RGB(17, 19, 23) : kMapBg;
}
static const COLORREF kInk     = RGB(0x0e, 0x10, 0x13);   // the background, the bottom bar
static const COLORREF kPanel   = RGB(0x1a, 0x1d, 0x22);   // big panels (settings)
static const COLORREF kPanel2  = RGB(0x23, 0x27, 0x2e);   // cards, round buttons, controls: VestaHome's tiles
static const COLORREF kLine    = RGB(0x2a, 0x2f, 0x37);   // separators
// The one accent (pressed / on / chosen, the car and its glow): VestaHome's theme (the user's rule 2026-10-26) -
// its "welcome" / "theme" (add-on API) or, without VestaHome running, screen.accent of its VestaHome.ini; Марс else.
static COLORREF       kOrange  = RGB(0xff, 0x6b, 0x1a);
static const COLORREF kHair    = RGB(64, 68, 77);         // night: a card's 1 px edge over the dark map

enum { TIMER_TICK = 1, TIMER_USER = 2, TIMER_DOCK = 3, TIMER_LONG = 4, TIMER_DISK = 5, TIMER_GLIDE = 6, TIMER_FLING = 7, TIMER_ZOOM = 8 };
static const UINT kUserDelayMs = 900;    // wait for the user to stop pressing buttons

// GPS button modes (g_cfg.follow)
enum { FOLLOW_OFF, FOLLOW_NORTH, FOLLOW_HEADING };
static const double kCarAhead        = 0.25; // heading-up: view center this part of map height ahead of the car
// The third person's view (tilt.cpp, ring.cpp; the user 2026-10-27..28): heading-up from z15, the camera's angle from
// straight down by the zoom, the fog this far ahead; the near ring loaded +-kNearHalf round the view's centre, the far
// one (the same zoom, the user 2026-10-31: two zooms out was blurred, its roads too wide) +-kFarHalf round a point
// kFarAhead px ahead by the course - together they cover the frame to the fog at z15..17 (2026-10-31, counted for every
// course: on z17 under 1% left, in the fog's haze)
static const int    kTiltMinZ        = 15;
static const double kTiltFog         = 1150;
static const double kNearHalf        = 511;  // (5 slots: 2 x 511 spans 5 tiles at most)
static const double kFarHalf         = 639;  // (6 slots: 2 x 639 spans 6 at most)
static const double kFarAhead        = 600;
static const double kAroundHalf      = 200;  // the next zooms' tiles kept ready +-this of their px round the centre
static const UINT   kDiskStepMs      = 30;   // the view's tiles at hand drawn kDiskLoads at a time, a frame between
enum { kDiskLoads = 3 };
                                             // (0.25 of 480: the car stays above the bottom bar)
static const double kHeadingMinSpeed = 5;    // km/h: below it the course is noise, keep the last heading
static const int    kMaxKeys         = 36;   // tiles one view can need from one source (the far ring: 6 x 6)
static const int    kMaxParentLevels = 6;    // missing tile: show a coarser cached one, up to 6 levels up

// Small ring of map keys: "not on disk" / "no coarser tile" memos,
// so the card is not searched again every second.
enum { kRingKeys = 160 };   // (the view 25, the far ring 36, the next zooms 2 x 9 and their parents: all fit)
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
// The frame's parts timed (the user 2026-11-04: smooth as Navitel - where a frame's 225-320 ms go): the background,
// the tiles into the ring, the frame (turned / tilted / the ring's blit), the names, the route and marks, the panels,
// the copy to the screen. Each part: the time since the last lap; the sum and the maximum go to the log each minute.
enum { PT_BG, PT_RING, PT_FRAME, PT_LABELS, PT_MARKS, PT_UI, PT_OUT, PT_N };
static DWORD      s_ptSum[PT_N], s_ptMax[PT_N], s_ptAt;
static int        s_statGpsLines, s_statGpsBad;

static int        s_pressed = -1;
static bool       s_pressedInside;
static bool       s_dragging, s_dragMoved;
// The map going on after the finger (fling.h; the user 2026-11-04: "rubber" scrolling) - a frame each kFlingMs at most,
// the way by the time that passed; while it goes, as while dragged: no refining, no following, no requests
enum { kFlingMs = 30 };
static Fling      s_fling;
static bool       s_flinging;
static DWORD      s_flingTick;

static bool MapHeld()   // a finger on the map, or the map still going after it
{
    return s_dragging || s_flinging;
}
static POINT      s_dragStart;
static double     s_dragWx, s_dragWy;
static bool       s_dragTilt;           // the drag began in the third person's view: it stays on
static double     s_dragFx, s_dragFy;   // ... the flat point under the finger then (from the view's centre, turned)
static DWORD      s_freeTick;           // the third person's view moved by hand: when (0: it follows the car)
static const DWORD kFreeMs = 5000;      // then back to the car (the user 2026-10-29)

static GpsState   s_gps;               // as shown: the car driving on by the prediction (GlideApply)
static GpsState   s_fix;               // the last fix as it came
static double     s_showLon, s_showLat, s_showCourse;   // the car shown
static DWORD      s_showTick;          // its last step (0: none - put on the fix)
static DWORD      s_paintMs;           // the last frame's drawing (the glide's frames: not more than the CPU allows)
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
enum { K_SHIFT = 100, K_LANG, K_SPACE, K_BS, K_FIND, K_BACK, K_FIELD, K_PANEL, K_HIT = 200, K_FAVDEL = 300 };
enum { kKbY = 66, kKbRowH = 81 };

// The route editor (a long press on the map, the user's design 2026-10-26): "from" (the car by default, or an
// address, or a point on the map), up to three stops, "to"; a field tapped opens its choices. With a route on, a
// long press offers a stop there or a new route there; on a stop's sign - that stop out or earlier (the user
// 2026-10-29: it set a new stop instead). The trip's card tapped: the editor with the route's points.
// Slots: 0 from, 1..3 the stops, 4 to.
enum { P_BACK = 1, P_ROW = 10, P_DEL = 20, P_ADDVIA = 30, P_GO, P_CH_GPS = 40, P_CH_SEARCH, P_CH_MAP, P_CH_CANCEL,
       P_PICK_CANCEL = 50, M_VIA = 60, M_NEW, M_FAV, M_CARD, M_STOP_OUT, M_STOP_UP, M_STOP_CANCEL, P_TRIP_X = 80,
       P_TRIP, F_GO = 90, F_EDIT, F_CANCEL, E_ICON = 100, E_NAME = E_ICON + VFAV_ICONS, E_DEL, E_DONE, E_BOX, W_FIELD = 120, W_FAV, W_BOX = W_FAV + 3 };
static bool       s_plan;
static int        s_planChoose = -1;   // the slot whose choices are open
static int        s_planPressed = -1;
static int        s_pickSlot = -1;     // the map picks a point for this slot
static int        s_searchSlot = -1;   // the search picks a place for this slot
static bool       s_lpMenu;            // the long press menu
static bool       s_lpRoute;           // ... with a route on ("Заехать сюда" too)
static double     s_lpLon, s_lpLat;
static int        s_lpStop = -1;       // the stop whose sign was held (its menu), -1: none
// The favourite places (the user 2026-10-31: short names - Дом, Работа; VestaHome shows the first two): VestaCore
// vfavs, favorites.txt beside the exe. Added from the long press's menu (the keyboard names it), listed in the search
// (its key with the query empty), a tap - the route there, "x" twice - deleted.
static VFavs      s_favs;
static bool       s_favName;           // the keyboard names a new favourite (the point s_lpLon / s_lpLat)
static bool       s_showFavs;          // the list shows the favourites (else the search's hits)
static int        s_favDel = -1;       // the favourite asked "ТОЧНО?" before it goes
// Their marks on the map (the user 2026-11-02): a tap on one - its menu (the route there, change, cancel); "change" -
// its card: the sign, the name (the keyboard), deleted ("ТОЧНО?").
static int        s_favMenu = -1;      // the mark tapped: its menu
static int        s_favEdit = -1;      // the favourite changed: its card
static int        s_favRename = -1;    // the keyboard renames this one (its card again after)
static bool       s_favSure;           // the card's "delete" asked "ТОЧНО?"
static POINT      s_favPt[VFAV_MAX];   // the marks where drawn last (a tap finds them there)
static bool       s_favOn[VFAV_MAX];
static double     s_toLon, s_toLat;    // a route asked for from outside (VestaHome's widget), till a fix or 15 s
static wchar_t    s_toName[80];
static DWORD      s_toTick;            // 0: none asked
enum { kStopNearPx = 40 };             // a long press this near a stop's sign is on it
static bool       s_longDone;          // the press became a long one: its release does nothing more
static DWORD      s_downTick;          // when the finger went down on the map
static const wchar_t* s_planMsg;
enum { kLongMs = 650 };
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
// top, one panel of six rows within 64..400 (a row's stepper applies at once), "back" and the exit's sign (asked in a
// sheet: the user 2026-10-30) in the bottom strip; what does not fit - the GPS port's settings, the requests' counters - in a sheet over the bottom
// (VestaHome's windows: full width, the main action right, in the accent), a tap beside it closes it.
// Rows at kSetRow0 + i * kSetRowH (the maps' screen has the same rows).
enum { S_MAPS, S_ROUTE, S_CACHE_PREV, S_CACHE_NEXT, S_LOG_PREV, S_LOG_NEXT, S_GPS,
       S_INFO, S_LANG_PREV, S_LANG_NEXT, S_EXIT, S_BACK, S_PAGE_END,
       S_PORT_PREV = S_PAGE_END, S_PORT_NEXT, S_BAUD_PREV, S_BAUD_NEXT, S_SCAN, S_GPS_DONE, S_GPS_END,
       S_INFO_DONE = S_GPS_END, S_INFO_END,
       S_JUNC_PREV = S_INFO_END, S_JUNC_NEXT, S_TOLL_PREV, S_TOLL_NEXT, S_ROUTE_DONE, S_ROUTE_END,
       S_EXIT_YES = S_ROUTE_END, S_EXIT_NO, S_COUNT };
enum { SHEET_NONE, SHEET_GPS, SHEET_INFO, SHEET_ROUTE, SHEET_EXIT };
enum { kSetRow0 = 76, kSetRowH = 50, kSheetY = 200 };
static const RECT kSetCtl[S_COUNT] = {
    { 576, 79, 760, 123 },                             // the maps: their page (the kind, the detail, the regions)
    { 576, 129, 760, 173 },                            // the route: its sheet (the junction card, the tolls)
    { 512, 179, 560, 223 }, { 712, 179, 760, 223 },   // the steppers' arrows: rows 2, 3
    { 512, 229, 560, 273 }, { 712, 229, 760, 273 },
    { 576, 279, 760, 323 },                            // GPS: its sheet
    { 576, 329, 760, 373 },                            // the requests: their sheet
    { 444, 416, 492, 468 }, { 644, 416, 692, 468 },   // the language: below the panel
    { 716, 416, 784, 468 },                            // exit the program (its sign; asked in a sheet)
    { 8, 412, 80, 480 },                               // back: VestaHome's, at the bottom left
    { 512, 259, 560, 303 }, { 712, 259, 760, 303 },   // the GPS sheet: the port, the speed
    { 512, 309, 560, 353 }, { 712, 309, 760, 353 },
    { 416, 416, 592, 468 },                            // ... find GPS
    { 600, 416, 784, 468 },                            // ... done
    { 600, 416, 784, 468 },                            // the requests' sheet: done
    { 512, 259, 560, 303 }, { 712, 259, 760, 303 },   // the route's sheet: the junction card, the tolls
    { 512, 309, 560, 353 }, { 712, 309, 760, 353 },
    { 600, 416, 784, 468 },                            // ... done
    { 600, 416, 784, 468 }, { 416, 416, 592, 468 },   // the exit's sheet: exit, cancel (a tap beside it too)
};
static int s_setSheet;   // SHEET_*

// The map's page (the user 2026-10-30: Settings -> the map's row "КАРТЫ"): the map's kind and detail, the regions'
// maps - their own page (the user 2026-10-30): the regions' maps downloaded here (offmaps.cpp's job; the user
// 2026-10-29: the graphs for the routes and the search without the internet) - the release's catalog by its titles,
// kMapsRows a page, a row's button downloads it (the trunk roads after the first one), "ОТМЕНА" while it goes;
// "ЗАГРУЗИТЬ" and "ОК" (the same again) asked "ТОЧНО?"
enum { M_KIND_PREV, M_KIND_NEXT, M_DETAIL_PREV, M_DETAIL_NEXT, M_TRF_PREV, M_TRF_NEXT, M_REGIONS, M_BACK, M_COUNT };
static const RECT kMapsCtl[M_COUNT] = {
    { 512, 79, 560, 123 }, { 712, 79, 760, 123 },     // the steppers: the kind, the detail
    { 512, 129, 560, 173 }, { 712, 129, 760, 173 },
    { 512, 179, 560, 223 }, { 712, 179, 760, 223 },   // the traffic: on the map / on the route alone
    { 576, 229, 760, 273 },                            // the regions' page
    { 8, 412, 80, 480 },                               // back to the settings
};
enum { kMapsRows = 6 };
enum { R_ROW0, R_PREV = R_ROW0 + kMapsRows, R_NEXT, R_LIST, R_BACK, R_COUNT };
static const RECT kRegCtl[R_COUNT] = {
    { 600, 81, 760, 121 }, { 600, 131, 760, 171 }, { 600, 181, 760, 221 },   // the regions' buttons
    { 600, 231, 760, 271 }, { 600, 281, 760, 321 }, { 600, 331, 760, 371 },
    { 300, 420, 348, 468 }, { 452, 420, 500, 468 },                           // the pages
    { 716, 416, 784, 468 },                                                   // the catalog again ("refresh")
    { 8, 412, 80, 480 },                                                      // back to the map's page
};
static bool s_maps;            // the maps' screen over the settings
static bool s_regions;         // ... its regions' page
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
// The GPS's state taken (the fix's message, the second's tick): a new fix - the turn eased from the course shown
static void GpsTake()
{
    GpsGet(&s_fix);
    s_gps = s_fix;
    if (s_gps.valid) {   // (saved with the view, each 5 minutes: StartFrom)
        g_cfg.lastLon = s_gps.lon;
        g_cfg.lastLat = s_gps.lat;
    }
}

// The car between the fixes (the user 2026-10-31: the map jerked, a frame a second; 2026-11-02: the prediction slowed
// towards the second's end and jumped at the next fix - shown a fix late instead, the point ahead known): a new fix -
// from where the mark is to the fix's point at an even speed in the fixes' period (junction.cpp GlideAim / GlideMove),
// there it waits for the next one; the turn eased to the fix's course; standing or slow - the fix as it is
enum { kGlideMs = 50 };   // (a frame at most every 50 ms; fewer when drawing takes longer: TIMER_GLIDE)
static double s_aimVe, s_aimVn;   // the mark's speed to the last fix, m a ms east / north
static DWORD  s_aimFix, s_aimTick, s_period = 1000;   // that fix's tick, when it was aimed at, the fixes' period (ms)

static void GlideAimFix(DWORD now)   // a new fix: the period measured, the mark on its way to it
{
    DWORD p = s_fix.fixTick - s_aimFix;
    if (p > 300 && p < 3000)
        s_period = (s_period * 3 + p) / 4;
    s_aimFix = s_fix.fixTick;
    s_aimTick = now;
    if (!GlideAim(s_showLon, s_showLat, s_fix.lon, s_fix.lat, s_period, &s_aimVe, &s_aimVn)) {
        s_showLon = s_fix.lon;   // (a jump: there at once)
        s_showLat = s_fix.lat;
    }
}

static bool GlideApply()
{
    if (!s_fix.valid || s_fix.speedKmh <= kHeadingMinSpeed) {
        s_showTick = 0;
        return false;
    }
    DWORD now = GetTickCount();
    if (!s_showTick) {
        s_showLon = s_fix.lon;
        s_showLat = s_fix.lat;
        s_showCourse = s_fix.course;
        s_aimFix = s_fix.fixTick;
        s_aimTick = now;
        s_aimVe = s_aimVn = 0;
    } else {
        if (s_fix.fixTick != s_aimFix)
            GlideAimFix(now);
        DWORD step = now - s_showTick, end = s_aimTick + s_period;
        DWORD left = (long)(end - s_showTick) > 0 ? end - s_showTick : 0;
        GlideMove(&s_showLon, &s_showLat, s_aimVe, s_aimVn, step < left ? step : left);
        s_showCourse = GlideCourse(s_showCourse, s_fix.course, step * 2);
    }
    s_showTick = now;
    s_gps.lon = s_showLon;
    s_gps.lat = s_showLat;
    s_gps.course = s_showCourse;
    return true;
}

// Where "my location" is now for a route (startpt.h), and what to call it
static StartPoint StartNow()
{
    return StartFrom(s_gps.valid != 0, s_gps.lon, s_gps.lat, g_cfg.lastLon, g_cfg.lastLat, g_cfg.lon, g_cfg.lat);
}

static const wchar_t* MyPlaceName()
{
    int source = StartNow().source;
    return source == START_CAR ? L"Моё местоположение" : source == START_LAST ? L"Последнее местоположение" : L"Центр карты";
}

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

// Zoom z's tiles +-half of its px round (vx, vy); their count
static int SquareKeysAt(int z, double vx, double vy, double half, MapRequest* out, int max)
{
    int n = 0, tiles = 1 << z;
    if (z < 2 || z > 19)
        return 0;
    for (int j = (int)floor((vy - half) / TILE); j <= (int)floor((vy + half) / TILE); j++)
        for (int i = (int)floor((vx - half) / TILE); i <= (int)floor((vx + half) / TILE); i++)
            if (n < max && i >= 0 && j >= 0 && i < tiles && j < tiles) {
                MapRequest k = { SRC_OSM, z, i, j };
                out[n++] = k;
            }
    return n;
}

// Zoom dz from the view's: the tiles +-half of that zoom's px round the view's centre; their count
static int SquareKeys(int dz, double half, MapRequest* out, int max)
{
    double vx, vy;
    GeoToWorld(g_cfg.lon, g_cfg.lat, g_cfg.z + dz, &vx, &vy);
    return SquareKeysAt(g_cfg.z + dz, vx, vy, half, out, max);
}

// The far ring's tiles (the view's zoom, round the point ahead) while the third person's view is on, the nearest to the
// view's centre first; their count
static int FarKeys(MapRequest* out, int max)
{
    if (!Tilted())
        return 0;
    double vx, vy, ax, ay;
    ViewWorld(&vx, &vy);
    RingAhead(vx, vy, ViewAngle(), kFarAhead, &ax, &ay);
    int n = SquareKeysAt(g_cfg.z, ax, ay, kFarHalf, out, max);
    double d[kMaxKeys];
    for (int i = 0; i < n; i++) {
        double dx = (out[i].x + 0.5) * TILE - vx, dy = (out[i].y + 0.5) * TILE - vy;
        d[i] = dx * dx + dy * dy;
        for (int j = i; j > 0 && d[j] < d[j - 1]; j--) {
            double t = d[j];
            d[j] = d[j - 1];
            d[j - 1] = t;
            MapRequest k = out[j];
            out[j] = out[j - 1];
            out[j - 1] = k;
        }
    }
    return n;
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

static void MissForget()   // once a minute: the tiles once not found tried again (the user 2026-11-03: a black
{                          // square in 3D stayed - one failed load, never tried again)
    static DWORD s_last;
    if (GetTickCount() - s_last < 60000)
        return;
    s_last = GetTickCount();
    s_missKeys.n = 0;
}

// Brings the map tiles of the view into memory (disk or download), then the traffic
// overlay. Disk decodes are limited per call: each costs some 20-50 ms on the unit.
// Up to maxLoads of the keys not in memory from the disk (draft: the vector ones in the fast style); the loads left,
// -1: keys left untried
static int LoadFromDisk(const MapRequest* keys, int n, int maxLoads, bool draft)
{
    // while the map moves (the user 2026-11-05: it stuttered) only the vector drafts - the fast style, the roads
    // and the water (the user: "только дороги") - the rest when it stops
    if (MapHeld() && (!draft || !g_cfg.vector))
        return maxLoads;
    for (int i = 0; i < n; i++) {
        if (CacheFind(keys[i], false) || RingHas(s_missKeys, keys[i]))
            continue;
        if (!maxLoads--)
            return -1;
        if (!CacheFind(keys[i], true, draft))
            RingAdd(s_missKeys, keys[i]);
    }
    return maxLoads;
}

// The view's first draft drawn again in the chosen detail - not while a finger is on the map (each takes a full tile's
// time); true: one was
static bool RefineStep(const MapRequest* keys, int n)
{
    if (MapHeld())
        return false;
    for (int i = 0; i < n; i++) {
        CacheEntry* e = CacheFind(keys[i], false);
        if (e && e->draft) {
            if (!CacheRefine(e))
                e->draft = false;   // (no data any more: left as it is)
            return true;
        }
    }
    return false;
}

static int Unshown(MapRequest* keys, int n);

// One step of drawing what is at hand (the user 2026-10-29: no waiting, no scaled tiles where the data is here): the
// view's tiles - as drafts in the fast style (2026-10-30, as Shturmann), then one by one in the chosen detail - the far
// ring's (a part of the third person's view), then, while the memory has room, the next zooms' round the centre (+ / -
// show them at once). *view: the view's got some; returns true: more to do.
static bool DiskStep(bool* view)
{
    MissForget();
    MapRequest keys[kMaxKeys];
    int n = Unshown(keys, NeededKeys(SRC_OSM, keys, kMaxKeys));
    int loads = MapHeld() ? 1 : kDiskLoads;   // (moving: one a step - the frames first)
    int left = LoadFromDisk(keys, n, loads, true);
    *view = left != loads;
    if (left < 0)
        return true;
    if (RefineStep(keys, n)) {
        *view = true;
        return true;
    }
    left = LoadFromDisk(keys, Unshown(keys, FarKeys(keys, kMaxKeys)), left, false);
    if (left < 0)
        return true;
    for (int k = 1; k < 3 && CacheBytes() + TILE * TILE * 2 <= CACHE_BYTES; k++) {
        left = LoadFromDisk(keys, SquareKeys(k == 1 ? 1 : -1, kAroundHalf, keys, kMaxKeys), left, false);
        if (left < 0)
            return true;
    }
    return false;
}

static void DiskSteps()   // from now on, till done
{
    SetTimer(s_wnd, TIMER_DISK, kDiskStepMs, NULL);
}

static int JunctionKeys(MapRequest* out, double* mx, double* my, bool early);

static bool TrafficWanted()   // the traffic asked for: shown on the map, or a route to colour
{
    RouteInfo ri;
    RouteGet(&ri);
    return g_cfg.traffic && (!g_cfg.trafficRoute || ri.state != ROUTE_NONE || RouteAlts() >= 2);
}

static void Ensure(int mode)
{
    if (MapHeld())   // (no loads, no requests while the map moves: UserChanged when it stops)
        return;
    MapRequest keys[kMaxKeys];
    int n = Unshown(keys, NeededKeys(SRC_OSM, keys, kMaxKeys));
    LoadFromDisk(keys, n, 6, true);
    bool parentLoaded = false;
    for (int i = 0; i < n; i++) {
        CacheEntry* e = CacheFind(keys[i], false);
        if (!Request(mode, keys[i], e) && !e && !s_userPending && !parentLoaded) {
            LoadParent(keys[i]);
            parentLoaded = true;
        }
    }
    n = Unshown(keys, FarKeys(keys, kMaxKeys));   // the far ring's, after the view's
    LoadFromDisk(keys, n, 4, false);
    for (int i = 0; i < n; i++)
        Request(REQ_AUTO, keys[i], CacheFind(keys[i], false));
    double mx, my;   // the junction card's tiles: after the view's (one request at a time: theirs first)
    n = JunctionKeys(keys, &mx, &my, true);
    LoadFromDisk(keys, n, 2, false);
    for (int i = 0; i < n; i++)
        Request(REQ_AUTO, keys[i], CacheFind(keys[i], false));
    if (TrafficWanted()) {
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
static void FlingStop()
{
    s_flinging = false;
    KillTimer(s_wnd, TIMER_FLING);
}

static void UserChanged()
{
    MapRequest keys[kMaxKeys];
    LoadFromDisk(keys, NeededKeys(SRC_OSM, keys, kMaxKeys), 6, true);
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
    case S_LANG_PREV:
    case S_LANG_NEXT:
        LangStep(id == S_LANG_NEXT ? 1 : -1);
        strncpy(g_cfg.lang, LangCode(), sizeof(g_cfg.lang) - 1);
        g_cfg.lang[sizeof(g_cfg.lang) - 1] = 0;
        ConfigSave();
        break;
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
    case S_EXIT:       s_setSheet = SHEET_EXIT; break;
    case S_EXIT_NO:    s_setSheet = SHEET_NONE; break;
    case S_EXIT_YES:
        DestroyWindow(s_wnd);
        return;
    case S_MAPS:
        s_maps = true;
        s_regions = false;
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

static void OnRegionsCtl(int id)
{
    int pages = (MapsCount() + kMapsRows - 1) / kMapsRows, confirm = s_mapsConfirm;
    s_mapsConfirm = -1;   // (any other tap: not asked any more)
    switch (id) {
    case R_PREV: if (s_mapsPage > 0) s_mapsPage--; break;
    case R_NEXT: if (s_mapsPage + 1 < pages) s_mapsPage++; break;
    case R_LIST: MapsList(); break;
    case R_BACK: s_regions = false; break;
    default:     MapsTap(s_mapsPage * kMapsRows + id - R_ROW0, confirm); break;
    }
}

static void OnMapsCtl(int id)
{
    if (s_regions)
        OnRegionsCtl(id);
    else switch (id) {
    case M_KIND_PREV:
    case M_KIND_NEXT:   MapStyleSet(!g_cfg.vector); break;
    case M_DETAIL_PREV: DetailSet(g_cfg.detail - 1); break;
    case M_DETAIL_NEXT: DetailSet(g_cfg.detail + 1); break;
    case M_TRF_PREV:
    case M_TRF_NEXT:
        g_cfg.trafficRoute = !g_cfg.trafficRoute;
        Log("map: traffic %s", g_cfg.trafficRoute ? "on the route alone" : "on the map");
        ConfigSave();
        Ensure(REQ_AUTO);   // (the route alone, none: no requests)
        break;
    case M_BACK:        s_maps = false; break;
    case M_REGIONS:
        s_regions = true;
        s_mapsPage = 0;
        s_mapsConfirm = -1;
        if (!MapsCount())
            MapsList();
        break;
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
    return s_regions ? HitRects(kRegCtl, R_COUNT, x, y) : HitRects(kMapsCtl, M_COUNT, x, y);
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
    static const int kFrom[5] = { 0, S_PAGE_END, S_GPS_END, S_INFO_END, S_ROUTE_END },
                     kTo[5] = { S_PAGE_END, S_GPS_END, S_INFO_END, S_ROUTE_END, S_COUNT };
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

// The zoom's change shown smoothly (the user 2026-11-05; zoomanim.cpp): the map's last frame (kept by DrawMap) scaled
// around the point the zoom keeps - the car while following, else the map's center - then the new zoom's own frame
static ZoomAnim       s_zoom;
static Image          s_zoomImg;          // the map's last frame (768 KB at 800 x 480: kept while the map is drawn)
static unsigned short* s_zoomBits;
static double         s_zoomAx, s_zoomAy;  // the anchor on the screen
enum { kZoomFrameMs = 30 };

static void ZoomAnchor(double* ax, double* ay)   // where the zoom keeps the map: the car while following
{
    *ax = g_cfg.mapX + g_cfg.mapW / 2.0;
    *ay = g_cfg.mapY + g_cfg.mapH / 2.0;
    double gx, gy, vx, vy, sx, sy;
    if (g_cfg.follow == FOLLOW_OFF || !s_gps.valid)
        return;
    GeoToWorld(s_gps.lon, s_gps.lat, g_cfg.z, &gx, &gy);
    ViewWorld(&vx, &vy);
    if (ScreenOf(gx, gy, vx, vy, *ax, *ay, ViewAngle(), &sx, &sy)) {
        *ax = sx;
        *ay = sy;
    }
}

static void ZoomAnimate(int dz)   // (before the zoom changes: the anchor by the old one)
{
    if (!s_zoomBits)
        return;
    DWORD now = GetTickCount();
    if (!s_zoom.on || now - s_zoom.t0 >= (DWORD)kZoomMs)   // a new one (more meanwhile: the same anchor)
        ZoomAnchor(&s_zoomAx, &s_zoomAy);
    ZoomAnimStart(&s_zoom, now, dz);
    SetTimer(s_wnd, TIMER_ZOOM, kZoomFrameMs, NULL);
}

static void ZoomBy(int dz)
{
    int z = g_cfg.z + dz;
    if (z < 2 || z > 19)
        return;
    ZoomAnimate(dz);
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
    int want = JunctionZoom(user, on, ri.complex, ri.nextM, ri.off, JunctionReachM(s_gps.speedKmh));
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
    s = Tr(s);
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

// A card over the map (design v3): the map under it at a quarter of its brightness - see-through (the user
// 2026-10-28: back as the design had it, solid since 2026-10-26 by a misunderstanding; 2026-11-02: every card and round
// button lighter than the bottom bar, all alike), round corners, no border; at night the 1 px edge.
static void Card(HDC dc, const RECT& r, int radius)
{
    Darken(dc, r, radius, 2);
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
// The third person's view (ring.cpp) adds a far ring of 6 x 6 tiles of the same zoom ahead, only while it is on.
static const int kRing = 5, kRingPx = kRing * TILE;   // 1280 x 1280, 3.2 MB
static const int kFarRing = 6;                        // 1536 x 1536, 4.6 MB
struct Ring {
    Image img;
    unsigned short* bits;
    int z;                            // its zoom
    int n;                            // its slots a side (kRing, kFarRing)
    SlotShown slot[kFarRing][kFarRing];
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

// The keys less those a ring shows already as they are (exact, in the chosen detail): with the third person's two rings
// the memory cache can't hold all their tiles, and loading them again would only push others out; their count
static int Unshown(MapRequest* keys, int n)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const MapRequest& k = keys[i];
        bool shown = false;
        for (int r = 0; r < 2 && !shown; r++) {
            const Ring& g = r ? s_far : s_near;
            const SlotShown& sl = g.slot[Mod(k.y, g.n ? g.n : 1)][Mod(k.x, g.n ? g.n : 1)];
            shown = g.bits && g.z == k.z && k.src == SRC_OSM && sl.used && sl.exact && sl.x == k.x && sl.y == k.y;
        }
        if (!shown)
            keys[m++] = k;
    }
    return m;
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

static TrafficLook TrafficNow()   // how the traffic is shown now (trafshade.h)
{
    return TrafficLookOf(g_cfg.traffic != 0, g_cfg.trafficRoute != 0, g_cfg.night != 0, g_cfg.vector != 0);
}

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
    TrafficLook tl = TrafficNow();
    for (int i = 0; i < nb; i++)
        h = Mix(Mix(Mix(Mix(Mix(h, base[i]->req.z), base[i]->req.x), base[i]->req.y), base[i]->fetchTick),
                (DWORD)(size_t)base[i]->img.bmp);
    if (tl.onMap) {
        h = Mix(h, 0x7AFF + TrafficSource() + (tl.mapDim ? 16 : 0));
        for (int i = 0; i < no; i++)
            if (ov[i].left < (tx + 1) * TILE && ov[i].right > tx * TILE && ov[i].top < (ty + 1) * TILE &&
                ov[i].bottom > ty * TILE)
                h = Mix(Mix(Mix(Mix(h, ov[i].e->req.x), ov[i].e->req.y), ov[i].e->fetchTick), (DWORD)(size_t)ov[i].e->ov);
    }
    return h;
}

// A traffic image blended into the ring, clipped to `clip` (ring pixels); (left, top) its ring position, sy ring
// px per image row, 1 px per image px horizontally; dim: its colours at kTrafficNightPct (the vector map's night)
static void BlendRing(unsigned short* bits, const Overlay* ov, double left, double top, double sy, const RECT& clip,
                      bool dim)
{
    int k = dim ? kTrafficNightPct : 100;
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
                dr += (s[0] * k / 100 - dr) * a / 255;
                dg += (s[1] * k / 100 - dg) * a / 255;
                db += (s[2] * k / 100 - db) * a / 255;
                drow[x] = RGB565(dr, dg, db);
            }
        }
    }
}

// Draws slot (tx, ty): the background, the base map, the traffic, night.
static void DrawSlot(Ring& r, HDC rdc, HDC src, int tx, int ty, bool inWorld, const CacheEntry** base, int nb,
                     const OvPlace* ov, int no)
{
    int ox = Mod(tx, r.n) * TILE, oy = Mod(ty, r.n) * TILE, px = r.n * TILE;
    RECT rc = { ox, oy, ox + TILE, oy + TILE };
    HBRUSH bg = CreateSolidBrush(MapBg());
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
    TrafficLook tl = TrafficNow();
    if (inWorld && tl.onMap)
        for (int i = 0; i < no; i++)
            if (ov[i].left < (tx + 1) * TILE && ov[i].right > tx * TILE && ov[i].top < (ty + 1) * TILE &&
                ov[i].bottom > ty * TILE)
                BlendRing(r.bits, ov[i].e->ov, ox + ov[i].left - tx * TILE, oy + ov[i].top - ty * TILE, ov[i].sy, rc,
                          tl.mapDim);
    const unsigned short* lut = g_cfg.night && !g_cfg.vector ? NightLut() : NULL;   // (vector: its own night palette)
    if (lut)
        for (int y = rc.top; y < rc.bottom; y++) {
            unsigned short* p = r.bits + (px - 1 - y) * px + rc.left;
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
// of them show some map. Traffic only on the near ring (the view's).
static int RingUpdate(Ring& r, HDC dc, double vx, double vy, double ex, double ey)
{
    int i0 = (int)floor((vx - ex) / TILE), i1 = (int)floor((vx + ex) / TILE);
    int j0 = (int)floor((vy - ey) / TILE), j1 = (int)floor((vy + ey) / TILE);
    if (i1 - i0 >= r.n) i1 = i0 + r.n - 1;   // can't be with 800 x 480; never two tiles in one slot
    if (j1 - j0 >= r.n) j1 = j0 + r.n - 1;
    r.i0 = i0;
    r.j0 = j0;
    r.i1 = i1;
    r.j1 = j1;
    int tiles = 1 << r.z;
    CacheEntry* list[CACHE_MEM];
    int n = CacheList(list, CACHE_MEM);
    OvPlace ov[CACHE_MEM];
    int no = TrafficNow().onMap && &r == &s_near ? OverlayPlaces(list, n, ov) : 0;
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
            DWORD ctx = SlotSig(r.z, tx, ty, inWorld, NULL, 0, ov, no);
            bool exact = nb == 1 && base[0]->req.z == r.z && !base[0]->draft;
            SlotShown& sl = r.slot[Mod(ty, r.n)][Mod(tx, r.n)];
            if (!SlotRedraw(sl, tx, ty, sig, ctx, exact))
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
            sl.ctx = ctx;
            sl.exact = exact;
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
static RingView RingViewOf(const Ring& r)
{
    int px = r.n * TILE;
    RingView v = { r.bits + (px - 1) * px, -px, px, 0, r.i0 * TILE, r.j0 * TILE, (r.i1 + 1) * TILE, (r.j1 + 1) * TILE };
    return v;
}

// The third person's view: the frame tilted out of the near ring and the far one (ring.cpp), into s_rotated.
static void TiltFrame(const Tilt& t, double vx, double vy, double angle)
{
    const int W = s_rotated.w, H = s_rotated.h, stride = ((W * 2 + 3) & ~3) / 2;
    RingView rings[2] = { RingViewOf(s_near), RingViewOf(s_far) };
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
               Tr(kCredit[g_cfg.vector ? 1 : 0][!g_cfg.traffic ? 0 : g_cfg.trafficSrc == TRF_2GIS ? 2 : 1]));
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

// The route's alternatives (design v3 preview, the user 2026-11-02): a panel at the left - from and to, a card for
// each (the one on: the accent's border), "ПОЕХАЛИ" under them; the map fitted to the routes beside it.
enum { kAltGo = 74, kAltPanel = 75 };   // (HitOverMap's ids: the cards 70 + k)

static bool CardRect(int k, RECT* r)   // k: an alternative, kAltGo - "ПОЕХАЛИ", kAltPanel - the panel itself
{
    int n = RouteAlts(), h = n > 2 ? 78 : 96;
    if (n < 2 || k < 0 || (k >= n && k < kAltGo))
        return false;
    if (k == kAltPanel)
        SetRect(r, 16, 56, 336, 466);
    else if (k == kAltGo)
        SetRect(r, 28, 392, 324, 454);
    else
        SetRect(r, 28, 154 + k * (h + 8), 324, 154 + k * (h + 8) + h);
    return true;
}

static void FmtMin(wchar_t* s, int cap, int sec)
{
    int m = (sec + 59) / 60;
    if (m >= 60)
        _snwprintf(s, cap, Tr(L"%d ч %02d мин"), m / 60, m % 60);
    else
        _snwprintf(s, cap, Tr(L"%d мин"), m);
    s[cap - 1] = 0;
}

static void DrawEnds(HDC dc)   // the panel's top: from (А) and to (Б)
{
    const RoutePlan* p = RoutePlanGet();
    const wchar_t* from = p->from.kind == PL_GPS ? MyPlaceName() : p->from.name;
    RouteMarker(dc, 42, 86, RGB(40, 150, 70), L"А");
    RouteMarker(dc, 42, 128, RGB(30, 50, 110), L"Б");
    Text(dc, s_fontLabel, kText, 66, 70, 324, 102, DT_LEFT, from);
    Text(dc, s_fontLabel, kText, 66, 112, 324, 144, DT_LEFT, p->to.name);
    HBRUSH b = CreateSolidBrush(kTextDim);
    RECT l = { 41, 100, 43, 114 };
    FillRect(dc, &l, b);
    DeleteObject(b);
}

static bool s_altFit;      // the map fitted to the alternatives (the follow back once chosen)
static int  s_altFollow;

static void AltFit()
{
    if (RouteAlts() < 2) {
        if (s_altFit) {
            g_cfg.follow = s_altFollow;
            s_altFit = false;
            UserChanged();
        }
        return;
    }
    if (s_altFit)
        return;
    double xy[64], cx, cy;
    int n = 0;
    for (int k = 0; k < RouteAlts(); k++) {
        const double* ll;
        int np;
        double t, d;
        RouteAltGet(k, &ll, &np, &t, &d);
        for (int i = 0; i < 8 && np > 1; i++)   // (eight points of each: enough for the box)
            GeoToWorld(ll[i * (np - 1) / 7 * 2], ll[i * (np - 1) / 7 * 2 + 1], 0, &xy[n * 2], &xy[n * 2 + 1]), n++;
    }
    int z = FitZoom(xy, n, 380, 330, 5, 17, &cx, &cy);
    if (z < 0)
        return;
    s_altFollow = g_cfg.follow;
    s_altFit = true;
    double k = (double)(1 << z);   // (as the search's places: right of the panel)
    g_cfg.z = z;
    g_cfg.follow = FOLLOW_OFF;
    WorldToGeo(cx * k - 168, cy * k - 21, z, &g_cfg.lon, &g_cfg.lat);
    UserChanged();
}

static COLORREF AltRange(const AltEst& e, double timeS, wchar_t* line)   // the time by the traffic seen on it; its colour
{
    if (!g_cfg.traffic) {
        wcscpy(line, L"пробки выключены");
        return kTextDim;
    }
    if (!e.seenPct) {
        wcscpy(line, L"пробок на пути не видно");
        return kTextDim;
    }
    wchar_t a[24], b[24];
    FmtMin(a, 24, e.minS);
    FmtMin(b, 24, e.maxS);
    if (e.maxS - e.minS < 120)
        _snwprintf(line, 96, Tr(L"с пробками ~%s"), a);
    else if (e.maxS < 3600)
        _snwprintf(line, 96, Tr(L"с пробками %d–%d мин"), (e.minS + 59) / 60, (e.maxS + 59) / 60);
    else
        _snwprintf(line, 96, Tr(L"с пробками %s – %s"), a, b);
    line[95] = 0;
    return e.jamM > 300 ? kTextErr : e.maxS > timeS * 1.3 ? kOrange : RGB(120, 220, 120);
}

static bool AltJam(const AltEst& e, wchar_t* line)   // the worst jam on it (none seen: false)
{
    if (!g_cfg.traffic || !e.seenPct)
        return false;
    wchar_t d[24];
    FmtDist(d, 24, e.jamM);
    if (e.jamM > 300)
        _snwprintf(line, 96, e.seenPct < 90 ? Tr(L"пробка %s · видно %d%% пути") : Tr(L"пробка %s"), d, e.seenPct);
    else
        _snwprintf(line, 96, e.seenPct < 90 ? Tr(L"без сильных пробок · видно %d%% пути") : Tr(L"без сильных пробок"),
                   e.seenPct);
    line[95] = 0;
    return true;
}

static void DrawAltCard(HDC dc, int k, RECT r, DWORD sig)   // an alternative: its time, length, the traffic on it
{
    Estimate(k, sig);
    const AltEst& e = s_est[k];
    const double* ll;
    int np;
    double timeS, distM;
    RouteAltGet(k, &ll, &np, &timeS, &distM);
    bool cur = k == RouteAltCur(), down = s_planPressed == 70 + k;
    Tile(dc, r, down ? kOrange : kPanel2, cur || down ? kOrange : kPanel2, 16);
    wchar_t t[48], d[24], line[96];
    FmtMin(t, 48, (int)timeS);
    FmtDist(d, 24, (int)distM);
    int top = r.top + (r.bottom - r.top - 72) / 2;   // (three lines in the middle)
    HGDIOBJ of = SelectObject(dc, s_fontSpeed);
    SIZE sz;
    GetTextExtentPoint32(dc, t, (int)wcslen(t), &sz);
    SelectObject(dc, of);
    Text(dc, s_fontSpeed, down ? kInk : kText, r.left + 14, top, r.right - 8, top + 32, DT_LEFT, t);
    Text(dc, s_fontSmall, down ? kInk : kTextDim, r.left + 22 + sz.cx, top + 4, r.right - 8, top + 32, DT_LEFT, d);
    COLORREF c = AltRange(e, timeS, line);
    Text(dc, s_fontLabel, down ? kInk : c, r.left + 14, top + 34, r.right - 8, top + 56, DT_LEFT, line);
    if (AltJam(e, line))
        Text(dc, s_fontSmall, down ? kInk : kTextDim, r.left + 14, top + 56, r.right - 8, top + 74, DT_LEFT, line);
}

static void DrawCards(HDC dc)
{
    DWORD sig = Mix(Mix(Mix(s_ringStamp, g_cfg.z), g_cfg.trafficSrc), g_cfg.traffic) | 1;
    RECT r;
    if (!CardRect(kAltPanel, &r))
        return;
    Card(dc, r, 20);
    DrawEnds(dc);
    CardRect(kAltGo, &r);
    bool down = s_planPressed == 70 + kAltGo;
    Tile(dc, r, down ? kPanel2 : kOrange, down ? kPanel2 : kOrange, 24);
    Text(dc, s_fontSpeed, down ? kOrange : kInk, r.left, r.top, r.right, r.bottom, DT_CENTER, L"ПОЕХАЛИ");
    for (int k = 0; CardRect(k, &r); k++)
        DrawAltCard(dc, k, r, sig);
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

// The long press's point (design v3): the accent's pin, its tip on the point, a dark hole in its head, a shade under it.
static void DrawPin(HDC dc, int x, int y)
{
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH sh = CreateSolidBrush(RGB(40, 40, 40)), acc = CreateSolidBrush(kOrange), ink = CreateSolidBrush(kInk);
    HGDIOBJ ob = SelectObject(dc, sh);
    Ellipse(dc, x - 8, y - 3, x + 9, y + 4);
    SelectObject(dc, acc);
    POINT tip[3] = { { x - 12, y - 22 }, { x + 12, y - 22 }, { x, y } };
    Polygon(dc, tip, 3);
    Ellipse(dc, x - 14, y - 42, x + 15, y - 13);
    SelectObject(dc, ink);
    Ellipse(dc, x - 5, y - 33, x + 6, y - 22);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(sh);
    DeleteObject(acc);
    DeleteObject(ink);
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

// Where the line starts (the user 2026-11-03: a long tail stayed behind the car): the car shown - a fix late - on
// the route near the segment its tracking says (a few behind, one ahead): that segment, its point there (cut); -1:
// the whole line (no route on, choosing, the car off it)
static int RouteFrom(const double* ll, int np, double* cut)
{
    RouteInfo ri;
    RouteGet(&ri);
    if (ri.state != ROUTE_ON || !s_gps.valid || RouteAlts() >= 2)
        return -1;
    return RouteCut(ll, np, ri.seg - 8, ri.seg + 1, s_gps.lon, s_gps.lat, 50, cut);
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
        bool dim = TrafficNow().routeDim;   // (the route alone at night: dimmed as the map's would be)
        DWORD sig = Mix(Mix(Mix(Mix(s_ringStamp, g_cfg.z), g_cfg.trafficSrc), g_cfg.traffic + 2 * dim),
                        (DWORD)(size_t)ll) | 1;
        double l = g_cfg.mapX - 50, t = g_cfg.mapY - 50, r = g_cfg.mapX + g_cfg.mapW + 50, b = g_cfg.mapY + g_cfg.mapH + 50;
        const Tilt* tp = ViewTilt();
        static int runs[512];
        int nr = 0, n = 0, runStart = 0;
        bool open = false;
        double px = 0, py = 0;
        double cut[2];
        int k = RouteFrom(ll, np, cut), i0 = k < 0 ? 0 : k;   // (k >= 0: from the car's point on segment k)
        for (int i = i0; i < np; i++) {
            double gx, gy;
            bool at = i == i0 && k >= 0;
            GeoToWorld(at ? cut[0] : ll[i * 2], at ? cut[1] : ll[i * 2 + 1], g_cfg.z, &gx, &gy);
            double dx = gx - vx, dy = gy - vy;
            double x = cx + dx * rc + dy * rs, y = cy - dx * rs + dy * rc;
            if (i > i0) {
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
                            if (c != kRouteBlue)
                                c = TrafficShade(c, dim);
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
        if (!ScreenOf(gx, gy, vx, vy, cx, cy, angle, &sx, &sy))
            continue;
        if (m == 5)
            DrawPin(dc, Round(sx), Round(sy));
        else
            RouteMarker(dc, Round(sx), Round(sy), fill, t[0] ? t : NULL);
    }
}

// The favourites' signs (vfavs.h VFAV_*): a shape and up to four holes in it (the background's colour), in tenths
// of the radius.
struct FavGlyph { signed char p[12][2]; int n; signed char h[4][4][2]; int nh; };
static const FavGlyph kFavGlyph[VFAV_ICONS] = {
    { { {0,-10}, {2,-3}, {10,-3}, {4,1}, {6,8}, {0,4}, {-6,8}, {-4,1}, {-10,-3}, {-2,-3} }, 10, {}, 0 },        // star
    { { {0,-10}, {10,-1}, {6,-1}, {6,9}, {-6,9}, {-6,-1}, {-10,-1} }, 7, { { {-2,3}, {2,3}, {2,9}, {-2,9} } }, 1 }, // home
    { { {-10,-4}, {-5,-4}, {-5,-9}, {5,-9}, {5,-4}, {10,-4}, {10,8}, {-10,8} }, 8,                               // work
      { { {-3,-7}, {3,-7}, {3,-4}, {-3,-4} } }, 1 },
    { { {-7,10}, {-7,-10}, {9,-5}, {-4,0}, {-4,10} }, 5, {}, 0 },                                              // flag
    { { {0,9}, {-9,0}, {-10,-4}, {-8,-8}, {-4,-9}, {0,-6}, {4,-9}, {8,-8}, {10,-4}, {9,0} }, 10, {}, 0 },       // heart
    { { {-8,10}, {-8,-9}, {4,-9}, {4,-3}, {9,0}, {9,7}, {7,7}, {7,1}, {4,-1}, {4,10} }, 10,                     // fuel
      { { {-5,-6}, {1,-6}, {1,-2}, {-5,-2} } }, 1 },
    { { {0,-9}, {4,-5}, {4,-8}, {6,-8}, {6,-3}, {9,0}, {6,0}, {6,9}, {-6,9}, {-6,0}, {-9,0} }, 11,             // cottage
      { { {-2,2}, {2,2}, {2,6}, {-2,6} } }, 1 },
    { { {-7,-10}, {7,-10}, {7,10}, {-7,10} }, 4,                                                               // flats
      { { {-4,-7}, {-1,-7}, {-1,-4}, {-4,-4} }, { {1,-7}, {4,-7}, {4,-4}, {1,-4} },
        { {-4,-1}, {-1,-1}, {-1,2}, {-4,2} }, { {1,-1}, {4,-1}, {4,2}, {1,2} } }, 4 },
    { { {-8,-3}, {-5,-3}, {-5,-6}, {-3,-9}, {3,-9}, {5,-6}, {5,-3}, {8,-3}, {9,10}, {-9,10} }, 10,             // shop
      { { {-3,-6}, {3,-6}, {3,-3}, {-3,-3} } }, 1 },
    { { {-3,-9}, {3,-9}, {3,-3}, {9,-3}, {9,3}, {3,3}, {3,9}, {-3,9}, {-3,3}, {-9,3}, {-9,-3}, {-3,-3} }, 12,  // hospital
      {}, 0 },
    { { {-9,-7}, {0,-5}, {9,-7}, {9,8}, {0,10}, {-9,8} }, 6, { { {-1,-5}, {1,-5}, {1,10}, {-1,10} } }, 1 },    // school
    { { {-7,-10}, {3,-10}, {7,-7}, {7,-1}, {3,2}, {-2,2}, {-2,10}, {-7,10} }, 8,                               // parking
      { { {-2,-6}, {2,-6}, {2,-2}, {-2,-2} } }, 1 },
};

static void FavPoly(HDC dc, const signed char (*pt)[2], int n, int x, int y, int r, COLORREF c)
{
    POINT q[12];
    for (int i = 0; i < n; i++) {
        q[i].x = x + pt[i][0] * r / 10;
        q[i].y = y + pt[i][1] * r / 10;
    }
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, q, n);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

static void FavSign(HDC dc, int icon, int x, int y, int r, COLORREF fg, COLORREF bg)
{
    const FavGlyph& g = kFavGlyph[icon >= 0 && icon < VFAV_ICONS ? icon : VFAV_STAR];
    FavPoly(dc, g.p, g.n, x, y, r, fg);
    for (int i = 0; i < g.nh; i++)
        FavPoly(dc, g.h[i], 4, x, y, r, bg);
}

// The favourites' marks: a dark disc in a white ring, the sign in the accent, the name under it on a card (from
// kFavNamesZ: further out they pile up); none further out than kFavMarksZ.
enum { kFavMarksZ = 12, kFavNamesZ = 14 };

static void DrawFavMarks(HDC dc, double cx, double cy, double vx, double vy, double angle)
{
    for (int i = 0; i < s_favs.n; i++) {
        double gx, gy, sx, sy;
        GeoToWorld(s_favs.f[i].lon, s_favs.f[i].lat, g_cfg.z, &gx, &gy);
        s_favOn[i] = g_cfg.z >= kFavMarksZ && ScreenOf(gx, gy, vx, vy, cx, cy, angle, &sx, &sy) &&
                     (!s_dockShown || sy + 36 <= kDockY + s_dockOff);   // (not under the bottom bar)
        if (!s_favOn[i])
            continue;
        int x = Round(sx), y = Round(sy);
        s_favPt[i].x = x;
        s_favPt[i].y = y;
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HBRUSH br = CreateSolidBrush(kPanel2);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
        Ellipse(dc, x - 16, y - 16, x + 17, y + 17);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);
        FavSign(dc, s_favs.f[i].icon, x, y, 10, kOrange, kPanel2);
        if (g_cfg.z < kFavNamesZ)
            continue;
        SIZE sz;
        HGDIOBJ of = SelectObject(dc, s_fontSmall);
        GetTextExtentPoint32(dc, s_favs.f[i].name, (int)wcslen(s_favs.f[i].name), &sz);
        SelectObject(dc, of);
        RECT t = { x - sz.cx / 2 - 8, y + 19, x + sz.cx / 2 + 8, y + 21 + sz.cy };
        Card(dc, t, 8);
        Text(dc, s_fontSmall, kText, t.left, t.top, t.right, t.bottom, DT_CENTER, s_favs.f[i].name);
    }
}

static int FavAt(int x, int y)   // the mark under the finger (the nearest within 26 px), -1: none
{
    int best = -1, bd = 26 * 26;
    for (int i = 0; i < s_favs.n; i++) {
        int dx = s_favPt[i].x - x, dy = s_favPt[i].y - y;
        if (s_favOn[i] && dx * dx + dy * dy < bd) {
            bd = dx * dx + dy * dy;
            best = i;
        }
    }
    return best;
}

static void PartLap(int part)
{
    DWORD now = GetTickCount(), d = now - s_ptAt;
    s_ptSum[part] += d;
    if (d > s_ptMax[part])
        s_ptMax[part] = d;
    s_ptAt = now;
}

static void ZoomKeep(HDC dc, const RECT& mr)   // the map's frame (without the car) kept for a zoom's change
{
    if (!s_zoomBits && !(s_zoomBits = ImageCreate(mr.right - mr.left, mr.bottom - mr.top, &s_zoomImg)))
        return;
    HDC k = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(k, s_zoomImg.bmp);
    BitBlt(k, 0, 0, mr.right - mr.left, mr.bottom - mr.top, dc, mr.left, mr.top, SRCCOPY);
    SelectObject(k, old);
    DeleteDC(k);
}

static void ZoomFrame(HDC dc, const RECT& mr, double scale)   // that frame scaled around the anchor
{
    HBRUSH bg = CreateSolidBrush(MapBg());   // (farther: the ground around it)
    FillRect(dc, &mr, bg);
    DeleteObject(bg);
    RECT r;
    ZoomAnimRect(scale, s_zoomAx, s_zoomAy, mr, &r);
    HDC k = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(k, s_zoomImg.bmp);
    StretchBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, k, 0, 0, mr.right - mr.left, mr.bottom - mr.top,
               SRCCOPY);
    SelectObject(k, old);
    DeleteDC(k);
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

    double scale;
    if (s_zoomBits && ZoomAnimScale(&s_zoom, GetTickCount(), &scale)) {   // the zoom changing: the last frame scaled
        ZoomFrame(dc, mr, scale);
        double k = ZoomAnimNew(&s_zoom, scale), gx, gy;   // the car on the frame shown, not where it ends
        if (s_gps.valid) {
            GeoToWorld(s_gps.lon, s_gps.lat, g_cfg.z, &gx, &gy);
            vx = gx + k * (vx - gx);
            vy = gy + k * (vy - gy);
            cx = s_zoomAx + k * (cx - s_zoomAx);
            cy = s_zoomAy + k * (cy - s_zoomAy);
        }
        DrawGpsMarker(dc, cx, cy, vx, vy, angle);
        SelectClipRgn(dc, NULL);
        DeleteObject(rgn);
        return;
    }

    // the rotated frame (768 KB at 800 x 480) is kept only while heading-up is on
    bool rotate = g_cfg.follow == FOLLOW_HEADING;
    if (rotate && !s_rotatedBits) {
        s_rotatedBits = ImageCreate(g_cfg.mapW, g_cfg.mapH, &s_rotated);
        s_rotStamp = 0xFFFFFFFF;
    } else if (!rotate && s_rotatedBits) {
        ImageFree(&s_rotated);
        s_rotatedBits = NULL;
    }

    // the third person's view: the far ring (4.6 MB) only while it is on
    const Tilt* tilt = ViewTilt();
    if (tilt && !s_far.bits) {
        memset(s_far.slot, 0, sizeof(s_far.slot));
        s_far.n = kFarRing;
        s_far.bits = ImageCreate(kFarRing * TILE, kFarRing * TILE, &s_far.img);
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
            double ax, ay;
            RingAhead(vx, vy, angle, kFarAhead, &ax, &ay);
            s_far.z = g_cfg.z;
            RingUpdate(s_far, dc, ax, ay, kFarHalf, kFarHalf);
        }
        PartLap(PT_RING);
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
    PartLap(PT_FRAME);
    if (!drawn)   // (the vector map: no network and nothing at hand here - the user 2026-10-30: just this)
        Text(dc, s_fontSmall, g_cfg.night ? RGB(160, 160, 160) : RGB(90, 90, 90), mr.left, mr.top, mr.right,
             mr.bottom, DT_CENTER, !s_lastErr ? L"Загрузка карты..." : g_cfg.vector ? L"Нет карты этой области"
                                                                                 : NetErrorText(s_lastErr));

    if (g_cfg.vector && drawn) {   // the names over the turned frame, upright (vtile.cpp); under the car too (the
        // user 2026-11-05: kept away from it, they blinked as it went by - the car is drawn over them, neither hides
        // the other; the controls are dark enough over them, 2026-10-27)
        g_tilt = tilt;   // (the names tilted with the frame)
        VLabelsDraw(dc, mr, cx, cy, vx, vy, angle, g_cfg.z, NULL, 0);
        g_tilt = NULL;
    }
    PartLap(PT_LABELS);
    DrawAttribution(dc, mr);
    DrawRoute(dc, cx, cy, vx, vy, angle);
    DrawFavMarks(dc, cx, cy, vx, vy, angle);
    ZoomKeep(dc, mr);
    DrawGpsMarker(dc, cx, cy, vx, vy, angle);
    PartLap(PT_MARKS);

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
static void DrawExitSheet(HDC dc);
static void PowerSign(HDC dc, const RECT& r, COLORREF c);
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
        wcscpy(baud, Tr(L"как есть"));
    if (!s_gpsRunning)
        _snwprintf(s, cap, Tr(L"выключен (gps=0)"));
    else if (s_gps.status == GPS_OPEN)
        _snwprintf(s, cap, Tr(L"COM%d · %s · %d спутн.%s"), s_setPort, baud, s_gps.sats, s_gps.valid ? Tr(L" · фикс") : L"");
    else if (s_gps.status == GPS_BUSY)
        _snwprintf(s, cap, Tr(L"COM%d занят другой программой"), s_setPort);
    else if (s_gps.status == GPS_SCANNING)
        _snwprintf(s, cap, Tr(L"поиск: %s %lu…"), s_gps.port, s_gps.baud);
    else
        _snwprintf(s, cap, Tr(L"COM%d не открывается"), s_setPort);
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
        _snwprintf(s, 160, Tr(L"векторная · детализация: %s"), Tr(kDetailLow[g_cfg.detail - 1]));
    else
        _snwprintf(s, 160, Tr(L"векторная · детализация: авто (%s)"), Tr(kDetailLow[VTileDetail()]));
    SetRow(dc, kSetRow0, L"Карта", s, true);
    DrawCtl(dc, kSetCtl[S_MAPS], s_setPressed == S_MAPS, s_fontLabel, L"КАРТЫ");
    _snwprintf(s, 160, Tr(L"схема развязки: %s · платные: %s"), g_cfg.junction ? Tr(L"вкл") : Tr(L"выкл"),
               g_cfg.noToll ? Tr(L"объезжать") : Tr(L"можно"));
    SetRow(dc, kSetRow0 + kSetRowH, L"Маршрут", s, true);
    DrawCtl(dc, kSetCtl[S_ROUTE], s_setPressed == S_ROUTE, s_fontLabel, L"НАСТРОИТЬ");
    if (g_cfg.cacheKb >= 0)
        _snwprintf(s, 160, Tr(L"занято %ld МБ"), g_cfg.cacheKb / 1024);
    else
        wcscpy(s, L"занято ?");
    SetRow(dc, kSetRow0 + 2 * kSetRowH, L"Кэш карт", s, true);
    int mb = kSetCacheMb[s_setCache];
    _snwprintf(s, 160, mb >= 1000 ? Tr(L"%d ГБ") : Tr(L"%d МБ"), mb >= 1000 ? mb / 1000 : mb);
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
    Text(dc, s_fontLabel, kText, 100, 416, 436, 468, DT_LEFT, L"Язык / Language");   // (in both: found by who needs it)
    Stepper(dc, S_LANG_PREV, S_LANG_NEXT, LangName());
    DrawCtl(dc, kSetCtl[S_EXIT], s_setPressed == S_EXIT, s_fontLabel, L"");
    PowerSign(dc, kSetCtl[S_EXIT], s_setPressed == S_EXIT ? kInk : kText);
    if (s_setSheet == SHEET_GPS)
        DrawGpsSheet(dc);
    else if (s_setSheet == SHEET_INFO)
        DrawInfoSheet(dc);
    else if (s_setSheet == SHEET_ROUTE)
        DrawRouteSheet(dc);
    else if (s_setSheet == SHEET_EXIT)
        DrawExitSheet(dc);
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
        _snwprintf(s, 160, Tr(L"NMEA %d (ошибок %d) · %.5f, %.5f"), s_gps.lines, s_gps.badLines, s_gps.lat, s_gps.lon);
    else
        _snwprintf(s, 160, Tr(L"NMEA %d (ошибок %d) · координат пока нет"), s_gps.lines, s_gps.badLines);
    s[159] = 0;
    Text(dc, s_fontSmall, s_gps.valid ? kText : kTextDim, 36, 358, s_cw - 36, 380, DT_LEFT, s);
    COLORREF c = kTextDim;
    if (s_gps.found == 1) {
        _snwprintf(s, 160, Tr(L"Найден GPS: %s %lu, настройки сохранены"), s_gps.port, s_gps.baud);
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

// The program closed? (the user 2026-10-30: not at one tap)
static void DrawExitSheet(HDC dc)
{
    Sheet(dc, L"Завершить OSMapsCE?");
    Text(dc, s_fontLabel, kTextDim, 36, 270, s_cw - 36, 340, DT_LEFT,
         L"Карта и маршрут закроются, GPS освободится для других программ.");
    DrawCtl(dc, kSetCtl[S_EXIT_NO], s_setPressed == S_EXIT_NO, s_fontLabel, L"ОТМЕНА");
    const RECT& r = kSetCtl[S_EXIT_YES];
    Tile(dc, r, kOrange, kOrange, 20);
    Text(dc, s_fontLabel, kInk, r.left, r.top, r.right, r.bottom, DT_CENTER, L"ЗАВЕРШИТЬ");
}

// The day's requests by their kind, against their limits
static void DrawInfoSheet(HDC dc)
{
    Sheet(dc, L"Запросы за день");
    wchar_t s[4][96];
    _snwprintf(s[0], 96, Tr(L"Пробки Яндекса: %d из %d"), *Counter(SRC_YANDEX), Limit(SRC_YANDEX));
    _snwprintf(s[1], 96, Tr(L"Пробки 2ГИС: %d из %d"), *Counter(SRC_2GIS), Limit(SRC_2GIS));
    _snwprintf(s[2], 96, Tr(L"Плитки OSM: %d"), g_cfg.osmCount);
    _snwprintf(s[3], 96, Tr(L"Поиск и маршрут: %d из %d"), g_cfg.apiCount, g_cfg.apiLimit);
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
    int kb = m.fullKb ? m.fullKb : m.sizeKb;   // (whole from the disk: with its tiles)
    _snwprintf(sub, 64, Tr(L"%d,%d МБ · %s"), kb / 1024, kb % 1024 * 10 / 1024, kState[st]);
    sub[63] = 0;
    SetRow(dc, kSetRow0 + r * kSetRowH, t, sub, r < kMapsRows - 1);
    const RECT& b = kRegCtl[R_ROW0 + r];
    if (s_mapsConfirm == i) {
        Tile(dc, b, kOrange, kOrange, 20);
        Text(dc, s_fontLabel, kInk, b.left, b.top, b.right, b.bottom, DT_CENTER, L"ТОЧНО?");
    } else
        DrawCtl(dc, b, s_mapsPressed == R_ROW0 + r, s_fontLabel, MapsBusyRegion() == i ? L"ОТМЕНА" : kBtn[st]);
}

// A circle's arc round the button's middle, radius 11, clockwise from a0 degrees by step - 13 points (CE's GDI has no
// Arc)
static void ArcPoints(const RECT& r, double a0, double step, POINT* p)
{
    double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
    for (int k = 0; k < 13; k++) {
        double a = (a0 - k * step) * M_PI / 180;
        p[k].x = (int)(cx + 11 * cos(a));
        p[k].y = (int)(cy - 11 * sin(a));
    }
}

// The exit's sign: "power" - the arc open at the top, a stroke down into it
static void PowerSign(HDC dc, const RECT& r, COLORREF c)
{
    POINT p[13], bar[2];
    ArcPoints(r, 60, 25, p);   // (60 round to -240: the gap of 60 degrees at the top)
    bar[0].x = bar[1].x = (r.left + r.right) / 2;
    bar[0].y = (r.top + r.bottom) / 2 - 14;
    bar[1].y = (r.top + r.bottom) / 2 - 2;
    HPEN pen = CreatePen(PS_SOLID, 3, c);
    HGDIOBJ op = SelectObject(dc, pen);
    Polyline(dc, p, 13);
    Polyline(dc, bar, 2);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// The catalog's button: a "refresh" sign - a circle's arc, clockwise, its arrow's head at the end
static void RefreshSign(HDC dc, const RECT& r, COLORREF c)
{
    POINT p[13], head[3];
    ArcPoints(r, 315, 22.5, p);   // from 315 degrees back round to 45 (the gap on the right)
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
        _snwprintf(s, 192, Tr(L"%s %d,%d из %d,%d МБ"), text, got / 1024, got % 1024 * 10 / 1024, total / 1024,
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
        _snwprintf(s, 64, Tr(L"по скорости устройства: сейчас %s"), Tr(kDetailLow[VTileDetail()]));
    SetRow(dc, kSetRow0 + kSetRowH, L"Детализация", s, true);
    StepperAt(dc, kMapsCtl[M_DETAIL_PREV], kMapsCtl[M_DETAIL_NEXT], s_mapsPressed - M_DETAIL_PREV,
              kDetailNames[g_cfg.detail]);
    SetRow(dc, kSetRow0 + 2 * kSetRowH, L"Пробки", g_cfg.trafficRoute ? L"только на проложенном маршруте"
                                                                   : L"на карте и на маршруте", true);
    StepperAt(dc, kMapsCtl[M_TRF_PREV], kMapsCtl[M_TRF_NEXT], s_mapsPressed - M_TRF_PREV,
              g_cfg.trafficRoute ? L"Маршрут" : L"Везде");
    SetRow(dc, kSetRow0 + 3 * kSetRowH, L"Карты областей", L"маршрут и поиск без интернета", false);
    DrawCtl(dc, kMapsCtl[M_REGIONS], s_mapsPressed == M_REGIONS, s_fontLabel, L"СКАЧАТЬ");
}

// The regions' page: the catalog's rows, the job's line, the pages and "refresh"
static void DrawRegions(HDC dc)
{
    wchar_t s[32];
    int n = MapsCount(), pages = n ? (n + kMapsRows - 1) / kMapsRows : 1;
    if (s_mapsPage >= pages)
        s_mapsPage = pages - 1;
    MapRegion m;
    for (int r = 0; r < kMapsRows && MapsAt(s_mapsPage * kMapsRows + r, &m); r++)
        MapsRow(dc, r, s_mapsPage * kMapsRows + r, m);
    MapsLine(dc);
    DrawCtl(dc, kRegCtl[R_PREV], s_mapsPressed == R_PREV, s_fontBig, L"\x2039");
    DrawCtl(dc, kRegCtl[R_NEXT], s_mapsPressed == R_NEXT, s_fontBig, L"\x203A");
    _snwprintf(s, 32, L"%d/%d", s_mapsPage + 1, pages);
    Text(dc, s_fontLabel, kText, 348, 420, 452, 468, DT_CENTER, s);
    DrawCtl(dc, kRegCtl[R_LIST], s_mapsPressed == R_LIST, s_fontLabel, L"");
    RefreshSign(dc, kRegCtl[R_LIST], s_mapsPressed == R_LIST ? kInk : kText);
}

// Over the settings, as they look: the title, the clock, a panel with the rows; the map's page or the regions'
static void DrawMaps(HDC dc)
{
    RECT all = { 0, 0, s_cw, s_ch };
    HBRUSH ink = CreateSolidBrush(kInk);
    FillRect(dc, &all, ink);
    DeleteObject(ink);
    wchar_t s[32];
    Text(dc, s_fontBig, kText, 20, 0, 500, 56, DT_LEFT, s_regions ? L"Карты областей" : L"Карта");
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(s, 32, L"%02d:%02d", t.wHour, t.wMinute);
    Text(dc, s_fontClock, kText, s_cw - 160, 0, s_cw - 16, 56, DT_RIGHT, s);
    BackDisc(dc, s_mapsPressed == (s_regions ? R_BACK : M_BACK));
    RECT panel = { 16, 64, s_cw - 16, s_regions ? 412 : 276 };
    Tile(dc, panel, kPanel, kPanel, 32);
    if (s_regions)
        DrawRegions(dc);
    else
        MapsLook(dc);
}

// Distance as the bar shows it: metres by 10 (by 50 above 100), km with a tenth up to 10 km.
static void FmtDist(wchar_t* s, int cap, int m)
{
    if (m < 100)
        _snwprintf(s, cap, Tr(L"%d м"), m / 10 * 10);
    else if (m < 1000)
        _snwprintf(s, cap, Tr(L"%d м"), m / 50 * 50);
    else if (m < 10000)
        _snwprintf(s, cap, Tr(L"%d,%d км"), m / 1000, m % 1000 / 100);
    else
        _snwprintf(s, cap, Tr(L"%d км"), (m + 500) / 1000);
    s[cap - 1] = 0;
}

static void DrawTurn(HDC dc, const RECT& r, int turn, COLORREF col = kText)
{
    VNavDrawTurn(dc, r, turn, col);
}

// The turn (the user 2026-10-26): the big card only before it - 300 m or 20 s at the speed, whichever is more -
// narrower than the mockup's; the rest of the time a compact one, about a quarter of it: the sign and the distance.
// Off the route: the compact card says so in red. Not on (being laid, not found, arrived): a line card.
// ---- the junction card (junction.cpp; the user 2026-10-28, on in the settings): under the big turn card before a
// complex maneuver, the map at z17 round it turned so the way in points up, the route on it. Built only when what it
// shows changes (the maneuver, its tiles coming, night) - through a 512 x 512 canvas freed right after - and copied
// into each frame.
// (z16 and the route's piece twice as long: HU 2026-10-31, the user - a smaller scale at speed)
enum { kJunZ = 16, kJunCanvas = 512, kJunW = 242, kJunH = 142, kJunTiles = 9 };
static unsigned short* s_junCard;   // kJunW x kJunH, rows top-down
static DWORD s_junSig;

// The card's tiles (kJunZ round the next maneuver) while it is shown (early: from 10 s before, loaded by then), and the
// maneuver's world pixel; their count
static int JunctionKeys(MapRequest* out, double* mx, double* my, bool early)
{
    RouteInfo ri;
    RouteGet(&ri);
    const double* ll;
    int np = RoutePoints(&ll), n = 0, h = kJunCanvas / 2;
    int reach = JunctionReachM(s_gps.speedKmh) + (early ? (int)(s_gps.speedKmh / 3.6 * 10) : 0);
    if (!JunctionShown(g_cfg.junction != 0, ri.complex, ri.nextM, ri.off, reach) || ri.shape >= np)
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
    int n = JunctionSlice(ll, np, ri.shape, 220, 140, pts, 64, &at, &course);
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
    HBRUSH bg = CreateSolidBrush(MapBg());
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
    int n = JunctionKeys(keys, &mx, &my, false);
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

// The big card of the next manoeuvre at its place moved by (dx, dy): the sign, the distance, what to do, the street
static void DrawGuideCard(HDC dc, int dx, int dy, const RouteInfo& ri)
{
    wchar_t d[32], num[16];
    FmtDist(d, 32, ri.nextM);
    const wchar_t* unit = wcsrchr(d, L' ');
    int n = unit ? (int)(unit - d) : (int)wcslen(d);
    wcsncpy(num, d, n);
    num[n] = 0;
    RECT p = { 16 + dx, 56 + dy, 266 + dx, (ri.thenText ? 222 : 188) + dy };
    Card(dc, p, 22);
    RECT sq = { 28 + dx, 68 + dy, 96 + dx, 136 + dy };
    Tile(dc, sq, kOrange, kOrange, 32);
    DrawTurn(dc, sq, ri.turn, kInk);
    SelectObject(dc, s_fontHuge);
    SIZE sz;
    GetTextExtentPoint32(dc, num, n, &sz);
    Text(dc, s_fontHuge, kText, 108 + dx, 62 + dy, 266 + dx, 116 + dy, DT_LEFT, num);
    if (unit)
        Text(dc, s_fontKm, kTextDim, 108 + dx + sz.cx + 5, 76 + dy, 266 + dx, 114 + dy, DT_LEFT, unit + 1);
    wchar_t verb[112], street[112];
    VNavSplit(ri.text, verb, street, 112);
    Text(dc, s_fontSmall, kTextDim, 108 + dx, 114 + dy, 262 + dx, 138 + dy, DT_LEFT, verb);
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
        Text(dc, sf, kText, 28 + dx, 144 + dy, 258 + dx, 180 + dy, DT_LEFT, street);
    } else {
        RECT sr = { 28 + dx, 142 + dy, 258 + dx, 184 + dy };
        SelectObject(dc, s_fontSmall);
        SetTextColor(dc, kText);
        DrawText(dc, street, -1, &sr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    if (ri.thenText) {
        RECT l = { 28 + dx, 188 + dy, 254 + dx, 189 + dy };
        HBRUSH b = CreateSolidBrush(kLine);
        FillRect(dc, &l, b);
        DeleteObject(b);
        wchar_t t[128];
        _snwprintf(t, 128, Tr(L"затем — %s"), ri.thenText);
        t[127] = 0;
        Text(dc, s_fontTiny, kTextDim, 28 + dx, 192 + dy, 258 + dx, 216 + dy, DT_LEFT, t);
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
    double soonM = s_gps.speedKmh / 3.6 * 20;
    bool big = !ri.off && ri.nextM <= (soonM > 300 ? soonM : 300);
    if (!big) {   // compact: the sign in the accent's square and the distance
        wchar_t d[32];
        FmtDist(d, 32, ri.nextM);
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
    DrawJunction(dc, (ri.thenText ? 222 : 188) + 8);   // (under the card: its own pixels first, the card's GDI after)
    DrawGuideCard(dc, 0, 0, ri);
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

// The arrival by the clock: "14:41"
static void FmtArrival(wchar_t* s, int cap, int leftS)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    int at = (t.wHour * 60 + t.wMinute + (leftS + 59) / 60) % (24 * 60);
    _snwprintf(s, cap, L"%02d:%02d", at / 60, at % 60);
    s[cap - 1] = 0;
}

// The trip's three columns: the arrival, the minutes (with the range by the traffic seen), the km; the first one's
// middle at cx0, the next `step` on; from `top` down.
static void DrawTripColumns(HDC dc, const RouteInfo& ri, int cx0, int step, int top)
{
    wchar_t big[3][24], under[3][40];
    FmtArrival(big[0], 24, ri.leftS);
    wcscpy(under[0], L"прибытие");
    int mins = (ri.leftS + 59) / 60;
    if (mins >= 60)
        _snwprintf(big[1], 24, Tr(L"%d ч %02d"), mins / 60, mins % 60);
    else
        _snwprintf(big[1], 24, Tr(L"%d мин"), mins);
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
            _snwprintf(under[1], 40, Tr(L"с пробками %d–%d"), (s_estCur.minS + 59) / 60, (s_estCur.maxS + 59) / 60);
    }
    FmtDist(big[2], 24, ri.leftM);
    wcscpy(under[2], L"осталось");
    for (int i = 0; i < 3; i++) {
        int cx = cx0 + i * step;
        Text(dc, s_fontKm, kText, cx - 62, top, cx + 62, top + 26, DT_CENTER, big[i]);
        Text(dc, s_fontCap, kTextDim, cx - 62, top + 26, cx + 62, top + 46, DT_CENTER, under[i]);
    }
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
    DrawTripColumns(dc, ri, 262, 120, 406);
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
enum { kHitRows = 5 };   // (the panel beside the map: design v3)

static int ListCount()   // the list's rows: the favourites or the search's hits, kHitRows at most
{
    int n = s_showFavs ? s_favs.n : SearchCount();
    return n < kHitRows ? n : kHitRows;
}

static bool FavHit(int i, SearchHit* h)   // a favourite as a search's hit
{
    if (i < 0 || i >= s_favs.n)
        return false;
    wcscpy(h->name, s_favs.f[i].name);
    wcscpy(h->sub, L"Избранное");
    h->lon = s_favs.f[i].lon;
    h->lat = s_favs.f[i].lat;
    return true;
}

static bool ListHit(int i, SearchHit* h)   // a row of the list, as a search's hit
{
    if (!s_showFavs) {
        const SearchHit* s = SearchGet(i);
        if (s)
            *h = *s;
        return s != NULL;
    }
    return FavHit(i, h);
}

static void FavsSave()
{
    wchar_t path[MAX_PATH];
    PathInDir(path, L"favorites.txt");
    if (!VFavsSave(&s_favs, path))
        Log("favs: not saved");
}

// The found places' panel over the map (design v3, the user 2026-11-02): the query's field (a tap - the keyboard
// again), the rows (a favourite's "x" at its right), the panel itself (a tap beside it closes the search)
static bool HitRowRect(int id, RECT* r)   // a row (K_HIT + i) or a favourite's "x" (K_FAVDEL + i)
{
    bool del = id >= K_FAVDEL;
    int i = id - (del ? K_FAVDEL : K_HIT), y = 124 + i * 66;
    if (i >= ListCount() || (del && !s_showFavs))
        return false;
    if (del)
        SetRect(r, 278, y + 10, 320, y + 52);
    else
        SetRect(r, 28, y, s_showFavs ? 272 : 324, y + 62);
    return true;
}

static bool HitsRect(int id, RECT* r)
{
    if (id == K_PANEL)
        SetRect(r, 16, 56, 336, 466);
    else if (id == K_FIELD)
        SetRect(r, 28, 68, 324, 112);
    else
        return id >= K_HIT && HitRowRect(id, r);
    return true;
}

static bool SearchRect(int id, RECT* r)
{
    if (s_showHits)
        return HitsRect(id, r);
    if (id == K_BACK) {
        SetRect(r, 8, 8, 64, 48);
        return true;
    }
    if (id == K_FIELD) {
        SetRect(r, 76, 6, s_cw - 12, 50);
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

static int HitSearch(int x, int y)   // (the panel last: under its rows)
{
    RECT r;
    POINT pt = { x, y };
    for (int id = 0; id < K_FAVDEL + kHitRows; id++)
        if (id != K_PANEL && SearchRect(id, &r) && PtInRect(&r, pt))
            return id;
    return SearchRect(K_PANEL, &r) && PtInRect(&r, pt) ? K_PANEL : -1;
}

// The map fitted to the found places beside the panel (junction.cpp FitZoom), the view before it kept: back when the
// search closes with no place chosen
static bool   s_fitDone, s_fitSaved;
static double s_fitLon, s_fitLat;
static int    s_fitZ, s_fitFollow;

static void FitHits()
{
    int n = ListCount();
    if (s_fitDone || !n || (!s_showFavs && SearchState() != SEARCH_DONE))
        return;
    double xy[kHitRows * 2], cx, cy;
    for (int i = 0; i < n; i++) {
        SearchHit h;
        ListHit(i, &h);
        GeoToWorld(h.lon, h.lat, 0, &xy[i * 2], &xy[i * 2 + 1]);
    }
    int z = FitZoom(xy, n, 380, 330, 5, 17, &cx, &cy);
    if (!s_fitSaved) {
        s_fitLon = g_cfg.lon;
        s_fitLat = g_cfg.lat;
        s_fitZ = g_cfg.z;
        s_fitFollow = g_cfg.follow;
        s_fitSaved = true;
    }
    double k = (double)(1 << z);   // the box's centre at the free part's (568, 261): the view's 168 px left, 21 up
    g_cfg.z = z;
    g_cfg.follow = FOLLOW_OFF;
    WorldToGeo(cx * k - 168, cy * k - 21, z, &g_cfg.lon, &g_cfg.lat);
    s_fitDone = true;
    UserChanged();
}

static void FitBack(bool restore)   // the search closed: the view as it was (restore), or as the place chosen left it
{
    if (restore && s_fitSaved) {
        g_cfg.lon = s_fitLon;
        g_cfg.lat = s_fitLat;
        g_cfg.z = s_fitZ;
        g_cfg.follow = s_fitFollow;
        UserChanged();
    }
    s_fitSaved = s_fitDone = false;
}

static void SearchClose()   // a tap beside the found places' panel
{
    s_search = false;
    s_showHits = s_showFavs = false;
    if (s_searchSlot >= 0) {   // back to the route editor
        s_searchSlot = -1;
        s_plan = true;
    }
    FitBack(true);
}

// The point the search is near: the car, or the map's center without a fix.
static void SearchFrom(double* lon, double* lat)
{
    *lon = s_gps.valid ? s_gps.lon : g_cfg.lon;
    *lat = s_gps.valid ? s_gps.lat : g_cfg.lat;
}

// The route to a place: from StartNow() (without a fix the map shows where it goes)
static void FitBack(bool restore);

static void RouteToHit(const SearchHit* h)
{
    FitBack(false);
    StartPoint st = StartNow();
    RouteTo(h, st.lon, st.lat, st.source == START_CENTER ? -2 : st.source == START_CAR && s_gps.speedKmh > 10 ? s_gps.course : -1);
    s_search = false;
    if (!s_gps.valid) {
        g_cfg.follow = FOLLOW_OFF;
        g_cfg.lon = h->lon;
        g_cfg.lat = h->lat;
        UserChanged();
    }
}

static void FavNamed()   // the keyboard's name for the new favourite (or the card's one): kept
{
    char u[VFAV_NAME * 3];
    WideCharToMultiByte(CP_UTF8, 0, s_kb.text, -1, u, sizeof(u), NULL, NULL);
    if (s_favRename >= 0) {
        int i = VFavsRename(&s_favs, s_favRename, s_kb.text);
        s_favEdit = i >= 0 ? i : s_favRename;   // (its card again)
        s_favRename = -1;
        if (i >= 0) {
            FavsSave();
            Log("favs: renamed \"%s\"", u);
        }
    } else if (s_kb.text[0] && (s_favEdit = VFavsAdd(&s_favs, s_kb.text, s_lpLat, s_lpLon)) >= 0) {
        FavsSave();   // (its card at once: the sign chosen there - the user 2026-11-03: not by the name)
        Log("favs: \"%s\" %.5f, %.5f", u, s_lpLat, s_lpLon);
    }
    s_favName = false;
    s_search = false;
    VKeysOpen(&s_kb, L"", kQueryCap);
}

static void OnSearchKey(int id)
{
    int n = (int)wcslen(s_kb.text), c = s_kb.cur < n ? s_kb.cur : n;
    if (id != K_FAVDEL + s_favDel)
        s_favDel = -1;
    if (id == K_BACK && s_favName) {   // no name: no favourite (renaming: the name kept, its card again)
        s_favEdit = s_favRename;
        s_favRename = -1;
        s_favName = false;
        s_search = false;
        VKeysOpen(&s_kb, L"", kQueryCap);
    } else if (id == K_BACK) {
        if (s_showHits)
            s_showHits = s_showFavs = false;
        else {
            s_search = false;
            if (s_searchSlot >= 0) {   // back to the route editor
                s_searchSlot = -1;
                s_plan = true;
            }
        }
    } else if (id == K_FIELD) {
        s_showHits = s_showFavs = false;
        s_fitDone = false;
    } else if (id == K_SHIFT) {
        s_kb.shift = !s_kb.shift;
    } else if (id == K_LANG) {
        s_kb.en = !s_kb.en;
    } else if (id == K_SPACE) {
        if (c > 0 && s_kb.text[c - 1] != L' ' && s_kb.text[c] != L' ')   // (no doubled spaces, none first)
            VKeysType(&s_kb, L' ');
    } else if (id == K_BS) {
        VKeysBack(&s_kb);
    } else if (id == K_FIND && s_favName) {
        FavNamed();
    } else if (id == K_FIND) {
        double lon, lat;
        SearchFrom(&lon, &lat);
        s_showFavs = !n;   // the query empty: the favourites
        s_showHits = true;
        s_fitDone = false;
        if (n)
            SearchStart(s_kb.text, lon, lat);   // (the query is in the log as the request's URL)
    } else if (id >= K_FAVDEL) {   // a favourite's "x": "ТОЧНО?", then gone
        if (s_favDel == id - K_FAVDEL) {
            VFavsDel(&s_favs, s_favDel);
            FavsSave();
            s_favDel = -1;
        } else {
            s_favDel = id - K_FAVDEL;
        }
    } else if (id >= K_HIT) {
        SearchHit hit;
        const SearchHit* h = ListHit(id - K_HIT, &hit) ? &hit : NULL;
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
            RouteToHit(h);
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
             s_showHits ? L"" : s_favName ? L"Короткое имя: Дом, Работа…" : L"Адрес или место");
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

static bool GeoScreen(double lon, double lat, int* x, int* y)   // a place on the screen now; false: out of the view
{
    double vx, vy, gx, gy, sx, sy;
    ViewWorld(&vx, &vy);
    GeoToWorld(lon, lat, g_cfg.z, &gx, &gy);
    bool on = ScreenOf(gx, gy, vx, vy, g_cfg.mapX + g_cfg.mapW / 2.0, g_cfg.mapY + g_cfg.mapH / 2.0, ViewAngle(), &sx,
                       &sy);
    *x = Round(sx);
    *y = Round(sy);
    return on;
}

static bool HitsMessage(HDC dc)   // nothing to list: why, in the panel; true: shown
{
    int st = SearchState(), n = ListCount();
    const wchar_t* msg = s_showFavs ? (n ? NULL : L"Избранного нет") : st == SEARCH_BUSY ? L"Поиск…"
                       : st == SEARCH_FAIL ? L"Нет связи с сервером поиска" : !n ? L"Ничего не найдено" : NULL;
    if (!msg)
        return false;
    Text(dc, s_fontLabel, st == SEARCH_FAIL ? kTextErr : kTextDim, 28, 130, 324, 160, DT_CENTER, msg);
    if (s_showFavs)
        Text(dc, s_fontSmall, kTextDim, 28, 160, 324, 186, DT_CENTER, L"долгое нажатие на карте → «В избранное»");
    return true;
}

static void HitSign(HDC dc, int i, int x, int y)   // a row's number (its mark on the map's) or a favourite's sign
{
    wchar_t num[4];
    _snwprintf(num, 4, L"%d", i + 1);
    if (s_showFavs)
        FavSign(dc, s_favs.f[i].icon, x, y, 11, kOrange, kInk);
    else
        RouteMarker(dc, x, y, i == 0 || s_kbPressed == K_HIT + i ? kOrange : kInk, num);
}

static void HitDel(HDC dc, int i, bool sure)   // a favourite's "x" ("ТОЧНО?" - red)
{
    RECT r;
    if (!HitsRect(K_FAVDEL + i, &r))
        return;
    Tile(dc, r, sure ? kTextErr : kPanel2, sure ? kTextErr : kPanel2, 16);
    Text(dc, s_fontLabel, sure ? kInk : kText, r.left, r.top, r.right, r.bottom, DT_CENTER, L"✕");
}

static void DrawHitRow(HDC dc, int i, const SearchHit& h, double lon, double lat)
{
    RECT r;
    HitsRect(K_HIT + i, &r);
    bool sure = s_showFavs && s_favDel == i;
    if (s_kbPressed == K_HIT + i)
        Tile(dc, r, kPanel2, kPanel2, 16);
    HitSign(dc, i, r.left + 20, (r.top + r.bottom) / 2);
    double dx = (h.lon - lon) * 111320 * cos(lat * M_PI / 180), dy = (h.lat - lat) * 110540;
    wchar_t d[32], name[100];
    FmtDist(d, 32, (int)sqrt(dx * dx + dy * dy));
    _snwprintf(name, 100, sure ? Tr(L"Удалить «%s»?") : L"%s", h.name);
    name[99] = 0;
    int right = sure ? r.right - 4 : r.right - 64;   // (asked "delete?": the whole row, no distance)
    Text(dc, s_fontLabel, sure ? kTextErr : kText, r.left + 44, r.top + 6, right, r.top + 34, DT_LEFT, name);
    Text(dc, s_fontSmall, kTextDim, r.left + 44, r.top + 34, right, r.bottom - 6, DT_LEFT,
         sure ? L"ещё раз ✕ — удалить" : h.sub);
    if (!sure)
        Text(dc, s_fontSmall, kTextDim, r.right - 70, r.top, r.right - 8, r.bottom, DT_RIGHT, d);
    HitDel(dc, i, sure);
}

static void DrawStatus(HDC dc);

static void DrawHitMarks(HDC dc, int left)   // the found places' numbers on the map right of left (the first on top;
{                                            // the favourites have their marks)
    for (int i = ListCount() - 1; i >= 0 && !s_showFavs; i--) {
        SearchHit h;
        int x, y;
        if (ListHit(i, &h) && GeoScreen(h.lon, h.lat, &x, &y) && x > left + 14)
            HitSign(dc, i, x, y);
    }
}

static void DrawHits(HDC dc)   // the found places beside the map (design v3), numbered on it too
{
    FitHits();
    DrawStatus(dc);
    RECT box, r;
    HitsRect(K_PANEL, &box);
    Card(dc, box, 20);
    HitsRect(K_FIELD, &r);
    Tile(dc, r, kPanel2, kOrange, 22);
    Text(dc, s_fontBig, kText, r.left + 16, r.top, r.right - 12, r.bottom, DT_LEFT, s_showFavs ? L"Избранное" : s_kb.text);
    if (HitsMessage(dc))
        return;
    double lon, lat;
    SearchFrom(&lon, &lat);
    DrawHitMarks(dc, box.right);
    for (int i = 0; i < ListCount(); i++) {
        SearchHit h;
        if (ListHit(i, &h))
            DrawHitRow(dc, i, h, lon, lat);
    }
}

static void DrawSearch(HDC dc)
{
    if (s_showHits) {
        DrawHits(dc);
        return;
    }
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
        int st = SearchState(), n = ListCount();
        const wchar_t* msg = s_showFavs ? (n ? NULL : L"Избранного нет: долгое нажатие на карте → В ИЗБРАННОЕ")
                           : st == SEARCH_BUSY ? L"Поиск…" : st == SEARCH_FAIL ? L"Нет связи с сервером поиска"
                           : !n ? L"Ничего не найдено" : NULL;
        if (msg) {
            Text(dc, s_fontLabel, st == SEARCH_FAIL ? kTextErr : kTextDim, 0, 120, s_cw, 200, DT_CENTER, msg);
            return;
        }
        double lon, lat;
        SearchFrom(&lon, &lat);
        for (int i = 0; i < n; i++) {
            SearchHit hit;
            if (!ListHit(i, &hit))
                continue;
            const SearchHit* h = &hit;
            if (SearchRect(K_FAVDEL + i, &r)) {
                bool sure = s_favDel == i;
                Tile(dc, r, sure ? kTextErr : kPanel2, sure ? kTextErr : kPanel2, 16);
                Text(dc, s_fontLabel, sure ? kInk : kText, r.left, r.top, r.right, r.bottom, DT_CENTER,
                     sure ? L"ТОЧНО?" : L"✕");
            }
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
    const wchar_t* find = s_favName ? L"СОХРАНИТЬ" : s_kb.text[0] ? L"НАЙТИ" : L"ИЗБРАННОЕ";
    for (int id = 0; id <= K_FIND; id++) {
        if (!SearchRect(id, &r))
            continue;
        bool lit = s_kbPressed == id || (id == K_FIND && (s_kb.text[0] || (!s_favName && s_favs.n))) ||
                   (id == K_SHIFT && s_kb.shift);
        wchar_t cap[8] = { 0 };
        if (id < K_SHIFT)
            cap[0] = VKeysChar(&s_kb, id / 16, id % 16);
        else
            wcsncpy(cap, id == K_LANG ? (s_kb.en ? L"РУС" : L"ENG") : id == K_FIND ? find : kBottom[id - K_SHIFT], 15);
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

// The editor as design v3 (the user 2026-11-02: back to it): a panel at the left over the map - the title, a field a
// row (its sign on the line at the left: А, the stops' numbers, Б; a stop's "x"), "+ Заехать", "ПОЕХАЛИ"; a field's
// choices in a card beside it; a tap beside them closes them (P_BACK), beside the panel - the editor.
static const RECT kPlanPanel = { 16, 56, 336, 466 };

static int PlanRowH()   // the rows share 228 px, 74 at most
{
    int h = 228 / (RoutePlanGet()->nvia + 2);
    return h > 74 ? 74 : h;
}

static bool ChoiceRect(int id, RECT* r)
{
    for (int i = 0; i < ChoiceCount(); i++)
        if (ChoiceId(i) == id) {
            SetRect(r, 360, 120 + i * 62, 680, 120 + i * 62 + 52);
            return true;
        }
    return false;
}

static bool RowRect(int id, RECT* r)   // a field (P_ROW + slot) or a stop's "x" (P_DEL + slot)
{
    bool del = id >= P_DEL;
    int slot = id - (del ? P_DEL : P_ROW), h = PlanRowH(), y = 112 + SlotRow(slot) * h;
    bool stop = slot >= 1 && slot <= 3;
    if ((stop && slot > RoutePlanGet()->nvia) || (del && !stop))
        return false;
    if (del)
        SetRect(r, 286, y, 324, y + h - 8);
    else
        SetRect(r, 66, y, stop ? 278 : 324, y + h - 8);
    return true;
}

static bool PlanRect(int id, RECT* r)
{
    if (s_planChoose >= 0)   // the choices over the editor: only they answer
        return ChoiceRect(id, r);
    if (id >= P_ROW && id <= P_DEL + 4)
        return RowRect(id, r);
    if (id == P_ADDVIA)
        SetRect(r, 66, 344, 324, 382);
    else if (id == P_GO)
        SetRect(r, 28, 392, 324, 454);
    else
        return false;
    return id != P_ADDVIA || RoutePlanGet()->nvia < kMaxVia;
}

static int HitPlan(int x, int y)   // beside the choices or the panel: P_BACK
{
    static const int ids[] = { P_ROW, P_ROW + 1, P_ROW + 2, P_ROW + 3, P_ROW + 4, P_DEL + 1, P_DEL + 2, P_DEL + 3,
                               P_ADDVIA, P_GO, P_CH_GPS, P_CH_SEARCH, P_CH_MAP, P_CH_CANCEL };
    RECT r;
    POINT pt = { x, y };
    for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++)
        if (PlanRect(ids[i], &r) && PtInRect(&r, pt))
            return ids[i];
    return s_planChoose < 0 && PtInRect(&kPlanPanel, pt) ? -1 : P_BACK;
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
    SearchHit h;
    p.kind = PL_PLACE;
    if (PlaceNamed(lon, lat, &h))
        wcscpy(p.name, h.name);
    else
        _snwprintf(p.name, 80, Tr(L"Точка на карте (%.5f, %.5f)"), lat, lon);
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
    if (s_lpStop < 0) {   // (no route: the route there or a favourite - the user 2026-11-02)
        s_lpMenu = true;
        s_lpRoute = on;
        s_lpLon = lon;
        s_lpLat = lat;
        PlaceLookup(MapPlace(lon, lat));   // (its address on the card)
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
// The plan's start when it is "my location": lon, lat, course of StartNow(); with nothing known of the car the start
// becomes the map's centre, a place of its own.
static void PlanStart(RoutePlan* p, double* lon, double* lat, double* course)
{
    StartPoint st = StartNow();
    if (p->from.kind == PL_GPS && st.source == START_CENTER) {
        p->from.kind = PL_PLACE;
        p->from.lon = st.lon;
        p->from.lat = st.lat;
        wcscpy(p->from.name, Tr(L"Центр карты"));
    }
    *lon = st.lon;
    *lat = st.lat;
    *course = st.source == START_CAR && s_gps.speedKmh > 10 ? s_gps.course : -1;
}

static void PlanGo()
{
    RoutePlan* p = RoutePlanGet();
    double lon, lat, course;
    PlanStart(p, &lon, &lat, &course);
    if (p->to.kind != PL_PLACE)
        s_planMsg = L"Выберите, куда ехать";
    else if (RouteStart(lon, lat, course, true)) {
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

static void SlotLabel(int slot, wchar_t* label)   // "Откуда", "Заехать (n)", "Куда"
{
    if (slot == 0 || slot == 4)
        wcscpy(label, slot ? L"Куда" : L"Откуда");
    else if (RoutePlanGet()->nvia > 1)   // (the user's word 2026-10-26)
        _snwprintf(label, 16, Tr(L"Заехать %d"), slot);
    else
        wcscpy(label, L"Заехать");
}

static const wchar_t* SlotValue(const RoutePlace* pl)
{
    if (pl->kind == PL_GPS)
        return MyPlaceName();
    return pl->kind == PL_PLACE ? pl->name : L"не выбрано";
}

static void PlanField(HDC dc, int slot, bool chosen)   // a field: its label, its place
{
    RECT r;
    RowRect(P_ROW + slot, &r);
    bool pr = s_planPressed == P_ROW + slot;
    Tile(dc, r, pr ? kOrange : kPanel2, chosen || pr ? kOrange : kPanel2, 16);
    wchar_t label[16];
    SlotLabel(slot, label);
    const RoutePlace* pl = PlanSlot(slot);
    const wchar_t* v = SlotValue(pl);
    COLORREF vc = pr ? kInk : pl->kind == PL_NONE ? kTextDim : kText;
    if (r.bottom - r.top < 50) {   // (five rows: the place only)
        Text(dc, s_fontLabel, vc, r.left + 12, r.top, r.right - 8, r.bottom, DT_LEFT, v);
        return;
    }
    Text(dc, s_fontSmall, pr ? kInk : kTextDim, r.left + 12, r.top + 4, r.right - 8, r.top + 24, DT_LEFT, label);
    Text(dc, s_fontLabel, vc, r.left + 12, r.top + 24, r.right - 8, r.bottom - 4, DT_LEFT, v);
}

static void PlanSigns(HDC dc)   // the line at the left: А, the stops' numbers, Б
{
    int n = RoutePlanGet()->nvia, h = PlanRowH(), y0 = 112 + (h - 8) / 2;
    RECT l = { 41, y0, 43, y0 + (n + 1) * h };
    HBRUSH b = CreateSolidBrush(kTextDim);
    FillRect(dc, &l, b);
    DeleteObject(b);
    for (int row = 0; row < n + 2; row++) {
        wchar_t t[4];
        _snwprintf(t, 4, L"%d", row);
        COLORREF c = row == 0 ? RGB(40, 150, 70) : row == n + 1 ? RGB(30, 50, 110) : kOrange;
        RouteMarker(dc, 42, y0 + row * h, c, row == 0 ? L"А" : row == n + 1 ? L"Б" : t);
    }
}

static void DrawChoices(HDC dc)   // a field's choices beside the panel
{
    RECT r, box = { 344, 104, 696, 120 + ChoiceCount() * 62 + 6 };
    Card(dc, box, 20);
    static const wchar_t* kChoice[4] = { L"Моё местоположение", L"Найти адрес", L"Точка на карте", L"Отмена" };
    for (int i = 0; i < ChoiceCount(); i++) {
        int id = ChoiceId(i);
        ChoiceRect(id, &r);
        DrawCtl(dc, r, s_planPressed == id, s_fontLabel, kChoice[id - P_CH_GPS]);
    }
}

static void PlanDels(HDC dc)   // the stops' "x"
{
    RECT r;
    for (int slot = 1; slot <= RoutePlanGet()->nvia; slot++) {
        RowRect(P_DEL + slot, &r);
        bool dp = s_planPressed == P_DEL + slot;
        Tile(dc, r, dp ? kOrange : kPanel2, dp ? kOrange : kPanel2, 16);
        Text(dc, s_fontBig, dp ? kInk : kText, r.left, r.top, r.right, r.bottom, DT_CENTER, L"×");
    }
}

static bool PlanReady(const RoutePlan* p)   // where to and where from
{
    return p->to.kind == PL_PLACE && p->from.kind != PL_NONE;
}

static void PlanBottom(HDC dc)   // why it cannot go (or "+ Заехать"), "ПОЕХАЛИ" - in the accent when it can
{
    const RoutePlan* p = RoutePlanGet();
    RECT r = { 66, 344, 324, 382 }, go = { 28, 392, 324, 454 };
    if (s_planMsg)
        Text(dc, s_fontSmall, kTextErr, 28, r.top, 324, r.bottom, DT_CENTER, s_planMsg);
    else if (p->nvia < kMaxVia)
        Text(dc, s_fontLabel, s_planPressed == P_ADDVIA ? kText : kOrange, r.left, r.top, r.right, r.bottom, DT_CENTER,
             L"+ Заехать");
    bool ready = PlanReady(p), lit = ready && s_planPressed != P_GO;
    Tile(dc, go, lit ? kOrange : kPanel2, lit ? kOrange : kPanel2, 24);
    Text(dc, s_fontSpeed, lit ? kInk : ready ? kOrange : kTextDim, go.left, go.top, go.right, go.bottom, DT_CENTER,
         L"ПОЕХАЛИ");
}

static void DrawPlan(HDC dc)
{
    RoutePlan* p = RoutePlanGet();
    DrawStatus(dc);
    Card(dc, kPlanPanel, 20);
    Text(dc, s_fontBig, kText, 32, 64, 320, 104, DT_LEFT, L"Маршрут");
    PlanSigns(dc);
    for (int row = 0; row < p->nvia + 2; row++)
        PlanField(dc, RowSlot(row), s_planChoose == RowSlot(row));
    PlanDels(dc);
    PlanBottom(dc);
    if (s_planChoose >= 0)
        DrawChoices(dc);
}

// The picker's bar over the map and the long press menu.
static bool PickRect(RECT* r)
{
    SetRect(r, s_cw - 168, kStatusH + 12, s_cw - 16, kStatusH + 56);
    return s_pickSlot >= 0;
}

// The long press's card (design v3, the user 2026-11-02: back to it): the address at the bottom, "Маршрут сюда" in the
// accent, "Заехать сюда" with a route on (till three stops), the favourite (with a route: its star only); a tap beside
// it closes it.
static bool LpRect(int id, RECT* r)
{
    if (id == M_VIA && (!s_lpRoute || RoutePlanGet()->nvia >= kMaxVia))
        return false;
    if (id == M_CARD)
        SetRect(r, 160, 300, 640, 466);
    else if (id == M_NEW)
        SetRect(r, 180, 384, s_lpRoute ? 360 : 400, 446);
    else if (id == M_VIA)
        SetRect(r, 372, 384, 552, 446);
    else
        SetRect(r, s_lpRoute ? 564 : 412, 384, 620, 446);
    return true;
}

static bool StopRect(int id, RECT* r)   // a stop's menu: out, earlier (not the first), cancel
{
    if (s_lpStop < 0 || (id == M_STOP_UP && s_lpStop == 0))
        return false;
    int i = id - M_STOP_OUT - (id > M_STOP_UP && s_lpStop == 0);   // (no "earlier": closed up)
    SetRect(r, 220, 300 + i * 58, 580, 300 + i * 58 + 50);
    return true;
}

static bool MenuRect(int id, RECT* r)
{
    if (id >= M_VIA && id <= M_CARD)
        return s_lpMenu && LpRect(id, r);
    return id >= M_STOP_OUT && id <= M_STOP_CANCEL && StopRect(id, r);
}

// The favourite's card: the signs' two rows of six; the name, delete; done.
static void FavCardRect(int id, RECT* r)
{
    int x = 188 + (id - E_ICON) % 6 * 72, y = 196 + (id - E_ICON) / 6 * 62;
    if (id < E_NAME)
        SetRect(r, x, y, x + 64, y + 56);
    else if (id == E_DONE)
        SetRect(r, 220, 392, 580, 442);
    else
        SetRect(r, id == E_NAME ? 220 : 405, 330, id == E_NAME ? 395 : 580, 380);
}

static bool FavBox(RECT* r)   // the mark's menu's card or the favourite's; false: none shown
{
    if (s_favMenu >= 0)   // (as the long press's card: design v3)
        SetRect(r, 160, 300, 640, 466);
    else
        SetRect(r, 172, 130, 628, 456);
    return s_favMenu >= 0 || s_favEdit >= 0;
}

// The mark's menu (three rows) and the favourite's card (E_BOX: the card itself - a tap between its controls).
static bool FavRect(int id, RECT* r)
{
    bool menu = s_favMenu >= 0 && id >= F_GO && id <= F_EDIT, card = s_favEdit >= 0 && id >= E_ICON && id < E_BOX;
    if (id == E_BOX)
        return FavBox(r);
    if (menu)   // "Маршрут сюда" in the accent, "Изменить" (no "Отмена": a tap beside)
        SetRect(r, id == F_GO ? 180 : 412, 384, id == F_GO ? 400 : 620, 446);
    else if (card)
        FavCardRect(id, r);
    return menu || card;
}

static int HitIn(int lo, int hi, bool (*rect)(int, RECT*), int x, int y)   // the control lo..hi under the finger
{
    RECT r;
    for (int id = lo; id <= hi; id++)
        if (rect(id, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return id;
    return -1;
}

// "Куда едем?" (design v3, the map with the bottom bar up and no route): the search's field, the first three
// favourites under it (a tap - the route there).
static bool WhereShown()
{
    RouteInfo ri;
    RouteGet(&ri);
    return DockUp() && ri.state == ROUTE_NONE && !s_lpMenu && s_lpStop < 0 && s_favMenu < 0 && s_favEdit < 0 &&
           s_pickSlot < 0;
}

static bool WhereRect(int id, RECT* r)
{
    if (!WhereShown() || (id >= W_FAV && id < W_BOX && id - W_FAV >= s_favs.n))
        return false;
    if (id == W_BOX)
        SetRect(r, 16, 56, 380, s_favs.n ? 190 : 124);
    else if (id == W_FIELD)
        SetRect(r, 28, 68, 368, 112);
    else
        SetRect(r, 28 + (id - W_FAV) * 116, 122, 28 + (id - W_FAV) * 116 + 108, 178);
    return true;
}

static bool TripShown();

static int HitOverMap(int x, int y)
{
    RECT r;
    if (TripShown() && x >= 548 && x < 596 && y >= 409 && y < 457)
        return P_TRIP_X;
    POINT pt = { x, y };
    for (int k = 0; k <= kAltPanel; k++)
        if (CardRect(k, &r) && PtInRect(&r, pt))
            return 70 + k;
    if (PickRect(&r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
        return P_PICK_CANCEL;
    int id = HitIn(M_VIA, M_STOP_CANCEL, MenuRect, x, y);
    if (id < 0)
        id = HitIn(F_GO, E_BOX, FavRect, x, y);
    if (id < 0)
        id = HitIn(W_FIELD, W_BOX, WhereRect, x, y);
    if (id >= 0)
        return id;
    if (TripShown() && x >= 196 && x < 604 && y >= 400 && y < 466)   // the trip's card: the route's editor
        return P_TRIP;
    return -1;
}

static void OnFavMenu(int id)   // the mark's menu
{
    SearchHit h;
    if (id == F_GO && FavHit(s_favMenu, &h))
        RouteToHit(&h);
    else if (id == F_EDIT)
        s_favEdit = s_favMenu;
    s_favMenu = -1;
}

static void OnFavCtl(int id)   // the mark's menu, the favourite's card
{
    if (id <= F_CANCEL)
        OnFavMenu(id);
    else if (id < E_NAME) {
        s_favs.f[s_favEdit].icon = id - E_ICON;
        FavsSave();
    } else if (id == E_NAME) {   // the keyboard, its name in it
        s_favRename = s_favEdit;
        s_favEdit = -1;
        s_favName = true;
        s_showHits = s_showFavs = false;
        VKeysOpen(&s_kb, s_favs.f[s_favRename].name, VFAV_NAME - 1);
        s_search = true;
    } else if (id == E_DEL && s_favSure) {
        VFavsDel(&s_favs, s_favEdit);
        FavsSave();
        s_favEdit = -1;
    } else if (id == E_DONE) {
        s_favEdit = -1;
    }
    s_favSure = id == E_DEL && !s_favSure;
}

static void OnWhere(int id)   // "Куда едем?": the field - the search; a favourite - the route there
{
    SearchHit h;
    if (id == W_FIELD) {
        OnButton(B_SEARCH);
    } else if (id < W_BOX && FavHit(id - W_FAV, &h)) {
        RouteToHit(&h);
        DockShow(false);
    }
}

static void OnOverMap(int id)
{
    if (id == P_TRIP_X) {   // the trip's "x": the route off
        RouteClear();
        Log("ui: route off");
    } else if (id >= 70 && id < 73) {   // an alternative card: that route on
        RouteAltChoose(id - 70);
    } else if (id == 70 + kAltGo) {   // "ПОЕХАЛИ": the one on it is
        RouteAltsDone();
    } else if (id == P_PICK_CANCEL) {
        s_pickSlot = -1;
        s_plan = true;
    } else if (id == M_VIA) {
        RoutePlace p = MapPlace(s_lpLon, s_lpLat);
        PlaceLookup(p);
        double lon, lat, course;
        PlanStart(RoutePlanGet(), &lon, &lat, &course);
        RouteAddVia(p, lon, lat, course, true);
        s_lpMenu = false;
    } else if (id == M_NEW) {
        s_lpMenu = false;
        PlanNew(s_lpLon, s_lpLat);
    } else if (id == M_FAV) {   // the keyboard names the point
        s_lpMenu = false;
        s_favName = true;
        s_showHits = s_showFavs = false;
        VKeysOpen(&s_kb, L"", VFAV_NAME - 1);
        s_search = true;
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
    } else if (id >= W_FIELD) {
        OnWhere(id);
    } else if (id >= F_GO) {
        OnFavCtl(id);
    }
    InvalidateRect(s_wnd, NULL, FALSE);
}

// The main action of a card (design v3): the accent's fill, dark letters; pressed - the other way round.
static void DrawMainCtl(HDC dc, const RECT& r, bool pressed, const wchar_t* caption)
{
    Tile(dc, r, pressed ? kPanel2 : kOrange, pressed ? kPanel2 : kOrange, 20);
    Text(dc, s_fontLabel, pressed ? kOrange : kInk, r.left, r.top, r.right, r.bottom, DT_CENTER, caption);
}

static void DrawFavMenu(HDC dc)
{
    RECT box, r;
    FavBox(&box);
    Card(dc, box, 20);
    const VFav& f = s_favs.f[s_favMenu];
    FavSign(dc, f.icon, box.left + 34, box.top + 30, 13, kOrange, kInk);
    Text(dc, s_fontBig, kText, box.left + 60, box.top + 14, box.right - 20, box.top + 46, DT_LEFT, f.name);
    wchar_t sub[64] = L"Избранное";
    if (s_gps.valid) {
        double dx = (f.lon - s_gps.lon) * 111320 * cos(f.lat * M_PI / 180), dy = (f.lat - s_gps.lat) * 110540;
        wchar_t d[32];
        FmtDist(d, 32, (int)sqrt(dx * dx + dy * dy));
        _snwprintf(sub, 64, Tr(L"Избранное · %s от вас"), d);
    }
    Text(dc, s_fontSmall, kTextDim, box.left + 20, box.top + 48, box.right - 20, box.top + 72, DT_LEFT, sub);
    FavRect(F_GO, &r);
    DrawMainCtl(dc, r, s_planPressed == F_GO, L"Маршрут сюда");
    FavRect(F_EDIT, &r);
    DrawCtl(dc, r, s_planPressed == F_EDIT, s_fontLabel, L"Изменить");
}

static void DrawFavSigns(HDC dc)   // the card's row of signs: its own in the accent's frame
{
    RECT r;
    for (int k = 0; k < VFAV_ICONS; k++) {
        FavRect(E_ICON + k, &r);
        bool on = s_favs.f[s_favEdit].icon == k, down = s_planPressed == E_ICON + k;
        Tile(dc, r, down ? kOrange : kPanel2, on || down ? kOrange : kPanel2, 16);
        FavSign(dc, k, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 16, down ? kInk : on ? kOrange : kText,
                down ? kOrange : kPanel2);
    }
}

static void DrawFavCards(HDC dc)
{
    RECT r;
    if (s_favMenu >= 0)
        DrawFavMenu(dc);
    if (s_favEdit < 0)
        return;
    RECT box;
    FavBox(&box);
    Card(dc, box, 20);
    Text(dc, s_fontLabel, kText, box.left + 16, box.top + 6, box.right - 16, 186, DT_CENTER, s_favs.f[s_favEdit].name);
    DrawFavSigns(dc);
    FavRect(E_NAME, &r);
    DrawCtl(dc, r, s_planPressed == E_NAME, s_fontLabel, L"Имя");
    FavRect(E_DEL, &r);
    if (s_favSure) {
        Tile(dc, r, kTextErr, kTextErr, 20);
        Text(dc, s_fontLabel, kInk, r.left, r.top, r.right, r.bottom, DT_CENTER, L"Точно?");
    } else {
        DrawCtl(dc, r, s_planPressed == E_DEL, s_fontLabel, L"Удалить");
    }
    FavRect(E_DONE, &r);
    DrawMainCtl(dc, r, s_planPressed == E_DONE, L"Готово");
}

static void LpAddress(wchar_t* title, wchar_t* sub)   // the long press's point: its address (or "Точка на карте"), how far
{
    SearchHit h;
    bool named = PlaceNamed(s_lpLon, s_lpLat, &h);
    wcscpy(title, named ? h.name : L"Точка на карте");
    if (named)
        wcscpy(sub, h.sub);
    else
        _snwprintf(sub, 96, L"%.5f, %.5f", s_lpLat, s_lpLon);
    if (s_gps.valid) {
        double dx = (s_lpLon - s_gps.lon) * 111320 * cos(s_lpLat * M_PI / 180), dy = (s_lpLat - s_gps.lat) * 110540;
        wchar_t d[32];
        FmtDist(d, 32, (int)sqrt(dx * dx + dy * dy));
        int n = (int)wcslen(sub);
        _snwprintf(sub + n, 140 - n, Tr(L"%s%s от вас"), n ? L" · " : L"", d);
    }
    sub[139] = 0;
}

static void Magnifier(HDC dc, int x, int y, COLORREF c)   // the search's sign, centred on x, y
{
    HPEN pen = CreatePen(PS_SOLID, 3, c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x - 9, y - 9, x + 6, y + 6);
    MoveToEx(dc, x + 4, y + 4, NULL);
    LineTo(dc, x + 10, y + 10);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void DrawWhere(HDC dc)
{
    RECT box, r;
    if (!WhereRect(W_BOX, &box))
        return;
    Card(dc, box, 20);
    WhereRect(W_FIELD, &r);
    Tile(dc, r, kPanel2, s_planPressed == W_FIELD ? kOrange : kPanel2, 22);
    Magnifier(dc, r.left + 26, (r.top + r.bottom) / 2, kTextDim);
    Text(dc, s_fontBig, kTextDim, r.left + 48, r.top, r.right - 12, r.bottom, DT_LEFT, L"Куда едем?");
    double lon, lat;
    SearchFrom(&lon, &lat);
    for (int id = W_FAV; id < W_BOX && WhereRect(id, &r); id++) {
        const VFav& f = s_favs.f[id - W_FAV];
        bool down = s_planPressed == id;
        Tile(dc, r, down ? kOrange : kPanel2, down ? kOrange : kPanel2, 16);
        Text(dc, s_fontLabel, down ? kInk : kText, r.left + 12, r.top + 4, r.right - 6, r.top + 30, DT_LEFT, f.name);
        double dx = (f.lon - lon) * 111320 * cos(lat * M_PI / 180), dy = (f.lat - lat) * 110540;
        wchar_t d[32];
        FmtDist(d, 32, (int)sqrt(dx * dx + dy * dy));
        Text(dc, s_fontSmall, down ? kInk : kTextDim, r.left + 12, r.top + 30, r.right - 6, r.bottom - 4, DT_LEFT, d);
    }
}

static void DrawLpCard(HDC dc)
{
    RECT box, r;
    LpRect(M_CARD, &box);
    Card(dc, box, 20);
    wchar_t title[80], sub[140];
    LpAddress(title, sub);
    Text(dc, s_fontBig, kText, box.left + 20, box.top + 14, box.right - 20, box.top + 46, DT_LEFT, title);
    Text(dc, s_fontSmall, kTextDim, box.left + 20, box.top + 48, box.right - 20, box.top + 72, DT_LEFT, sub);
    LpRect(M_NEW, &r);
    DrawMainCtl(dc, r, s_planPressed == M_NEW, L"Маршрут сюда");
    if (LpRect(M_VIA, &r))
        DrawCtl(dc, r, s_planPressed == M_VIA, s_fontLabel, L"Заехать сюда");
    else if (s_lpRoute)
        Text(dc, s_fontSmall, kTextDim, 372, 384, 552, 446, DT_CENTER, L"уже три «Заехать»");
    LpRect(M_FAV, &r);
    bool down = s_planPressed == M_FAV;
    if (s_lpRoute) {
        Tile(dc, r, down ? kOrange : kPanel2, down ? kOrange : kPanel2, 20);
        FavSign(dc, VFAV_STAR, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 13, down ? kInk : kText, kPanel2);
    } else {
        DrawCtl(dc, r, down, s_fontLabel, L"В избранное");
    }
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
        _snwprintf(s, 96, Tr(L"Нажмите на карту: точка для %s"), Tr(kWhat[s_pickSlot]));
        s[95] = 0;
        Text(dc, s_fontLabel, kText, bar.left + 16, bar.top, r.left - 8, bar.bottom, DT_LEFT, s);
        DrawCtl(dc, r, s_planPressed == P_PICK_CANCEL, s_fontLabel, L"ОТМЕНА");
    }
    if (s_lpStop >= 0) {
        int n = s_lpStop > 0 ? 3 : 2;
        RECT box = { 204, 288 - 40, 596, 300 + n * 58 + 4 };
        Card(dc, box, 20);
        wchar_t t[32];
        _snwprintf(t, 32, Tr(L"«Заехать %d»"), s_lpStop + 1);
        Text(dc, s_fontLabel, kText, box.left, box.top + 4, box.right, box.top + 48, DT_CENTER, t);
        static const wchar_t* kStop[3] = { L"УБРАТЬ ТОЧКУ", L"РАНЬШЕ", L"ОТМЕНА" };
        for (int id = M_STOP_OUT; id <= M_STOP_CANCEL; id++)
            if (MenuRect(id, &r))
                DrawCtl(dc, r, s_planPressed == id, s_fontLabel, kStop[id - M_STOP_OUT]);
    }
    DrawFavCards(dc);
    if (s_lpMenu)
        DrawLpCard(dc);
    DrawWhere(dc);
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
        _snwprintf(s, 96, Tr(L"GPS: порт %s занят"), s_gps.port);
        fg = kTextErr;
        bg = errBg;
    } else if (s_gps.status != GPS_OPEN) {
        _snwprintf(s, 96, Tr(L"GPS: нет порта %s"), s_gps.port);
        fg = kTextErr;
        bg = errBg;
    } else if (!s_gps.lines) {
        wcscpy(s, L"GPS: нет данных");
        fg = kTextErr;
        bg = errBg;
    } else if (!s_gps.valid) {
        _snwprintf(s, 96, Tr(L"GPS: поиск · %d"), s_gps.sats);
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
            _snwprintf(s, 96, Tr(L"Ошибка HTTP %d"), s_lastHttp);
        else
            _snwprintf(s, 96, Tr(L"Ошибка: %s"), Tr(NetErrorText(s_lastErr)));
        fg = kTextErr;
        bg = errBg;
    } else if (s_inflight) {
        wcscpy(s, L"Загрузка…");
    } else if (!g_cfg.traffic) {
        wcscpy(s, L"Пробки выкл.");
    } else if (!TrafficWanted()) {   // (on the route alone, no route)
        wcscpy(s, L"Пробки · на маршруте");
    } else {
        MapRequest keys[kMaxKeys];
        CacheEntry* e = NeededKeys(TrafficSource(), keys, kMaxKeys) ? CacheFind(keys[0], false) : NULL;
        if (!e)
            _snwprintf(s, 96, Tr(L"Пробки · %s · нет данных"), Tr(kTrafficTitle[g_cfg.trafficSrc]));
        else
            _snwprintf(s, 96, Tr(L"Пробки · %s · %d мин"), Tr(kTrafficTitle[g_cfg.trafficSrc]),
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
        // see-through again (the user 2026-10-28, as the design had it; 2026-11-02: darker than the cards - an eighth
        // of the map's brightness); the orange part of the line shrinks to the moment the bar goes down
        DWORD el = GetTickCount() - s_dockTick;
        RECT left = { 0, y, el >= (DWORD)kDockMs ? 0 : (int)(s_cw * (kDockMs - el) / kDockMs), y + 2 };
        Darken(dc, bar, 0, 3);
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
    s_ptAt = GetTickCount();
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
    PartLap(PT_BG);
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
        if (s_pickSlot < 0 && RouteAlts() < 2)   // (not over the alternatives' panel)
            DrawGuide(dc);
        AltFit();
        DrawTrip(dc);
        DrawButtons(dc);
        DrawOverMap(dc);
    }
    PartLap(PT_UI);

    BitBlt(hdc, 0, 0, s_cw, s_ch, dc, 0, 0, SRCCOPY);
    PartLap(PT_OUT);
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

static HFONT MakeFont(int height, int weight)   // (not smoothed: VestaCore vfont.h)
{
    return VFontMake(s_face, height, weight);
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
    if (s_statPaints) {   // the frame's parts: the average and the longest, ms
        DWORD n = (DWORD)s_statPaints;
        Log("paint: avg/max ms - bg %lu/%lu, tiles %lu/%lu, frame %lu/%lu, names %lu/%lu, marks %lu/%lu, "
            "panels %lu/%lu, out %lu/%lu", s_ptSum[PT_BG] / n, s_ptMax[PT_BG], s_ptSum[PT_RING] / n,
            s_ptMax[PT_RING], s_ptSum[PT_FRAME] / n, s_ptMax[PT_FRAME], s_ptSum[PT_LABELS] / n, s_ptMax[PT_LABELS],
            s_ptSum[PT_MARKS] / n, s_ptMax[PT_MARKS], s_ptSum[PT_UI] / n, s_ptMax[PT_UI], s_ptSum[PT_OUT] / n,
            s_ptMax[PT_OUT]);
    }
    memset(s_ptSum, 0, sizeof(s_ptSum));
    memset(s_ptMax, 0, sizeof(s_ptMax));
    s_statTick = GetTickCount();
    s_statPaints = 0;
    s_statPaintMax = 0;
    s_statSlots = 0;
    s_statGpsBytes = s_gps.bytes;
    s_statGpsLines = s_gps.lines;
    s_statGpsBad = s_gps.badLines;
}

// A route asked for from outside: once the car has a fix (or after 15 s - then from the map's centre)
static void RouteAsked()
{
    if (!s_toTick || (!s_gps.valid && GetTickCount() - s_toTick < 15000))
        return;
    SearchHit h;
    wcscpy(h.name, s_toName);
    h.sub[0] = 0;
    h.lon = s_toLon;
    h.lat = s_toLat;
    s_toTick = 0;
    RouteToHit(&h);
    InvalidateRect(s_wnd, NULL, FALSE);
}

static void OnCreate(HWND hwnd)
{
    VKeysOpen(&s_kb, L"", kQueryCap);
    {
        wchar_t path[MAX_PATH];
        PathInDir(path, L"favorites.txt");
        VFavsLoad(&s_favs, path);
    }
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
    s_near.n = kRing;
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
    SetTimer(hwnd, TIMER_GLIDE, kGlideMs, NULL);
    s_lastSaveTick = GetTickCount();
    g_today = Today();
    Ensure(REQ_AUTO);
}

static void OnDestroy(HWND hwnd)
{
    KillTimer(hwnd, TIMER_TICK);
    KillTimer(hwnd, TIMER_GLIDE);
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
static HWND           s_vh;         // VestaHome's window
static HANDLE         s_vhMap;
static unsigned char* s_vhView;     // 16 bytes ('VHIM', w, h, frame number), then the rows
static int            s_vhW, s_vhH;
static bool           s_vhShown;
static DWORD          s_vhSeq, s_vhHelloTick;
static wchar_t        s_vhNavSent[256];   // the last "nav" told (only a change is sent)

static void VhSend(const wchar_t* fmt, ...)
{
    if (!s_vh || !IsWindow(s_vh)) {
        s_vh = NULL;
        return;
    }
    va_list a;
    va_start(a, fmt);
    VhapSendV(s_vh, s_wnd, fmt, a);
    va_end(a);
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
        if (VhapField(cmd, L"accent", v, 16) && v[0]) {
            unsigned long c = wcstoul(v, NULL, 16);
            AccentSet(RGB(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF));
        }
    }
    if (!wcsncmp(cmd, L"route\t", 6)) {   // VestaHome's widget (a favourite): the route there, from the car
        s_toLat = VhapField(cmd, L"lat", v, 32) ? wcstod(v, NULL) : 0;
        s_toLon = VhapField(cmd, L"lon", v, 32) ? wcstod(v, NULL) : 0;
        if (!VhapField(cmd, L"name", s_toName, 80))
            wcscpy(s_toName, L"Избранное");
        s_toTick = s_toLat || s_toLon ? GetTickCount() | 1 : 0;
        Log("vh: route to %.5f, %.5f", s_toLat, s_toLon);
        return 1;
    }
    if (!wcsncmp(cmd, L"key", 3) && cmd[3] == L'\t') {
        // The wheel's buttons (the user 2026-10-26, 2026-11-02): held - the zoom (WheelZoom), while the map is in front
        // (VestaHome asks only then); short presses stay VestaHome's (the music, the phone).
        int code = VhapField(cmd, L"code", v, 8) ? _wtoi(v) : 0;
        int st = VhapField(cmd, L"st", v, 8) ? _wtoi(v) : 0;
        int kind = VhapField(cmd, L"kind", v, 8) ? _wtoi(v) : 0;
        int dz = WheelZoom(code, kind);
        if (dz) {
            if (WheelNow(code, st)) {
                Zoom(dz);
                UserChanged();
                InvalidateRect(s_wnd, NULL, FALSE);
            }
            return 2;
        }
        return 1;
    }
    if (!wcsncmp(cmd, L"welcome", 7)) {
        s_vhNavSent[0] = 0;
        VhSend(L"widget\tid=map\ttitle=%s\ticon=map\tsize=2x2\timage=1", Tr(L"Карта OSM"));
        VhSend(L"subscribe\twhat=keys");
    } else if (!wcsncmp(cmd, L"widget.size", 11)) {
        VhFree();
        VhapField(cmd, L"w", v, 8);
        s_vhW = _wtoi(v);
        VhapField(cmd, L"h", v, 8);
        s_vhH = _wtoi(v);
        VhapField(cmd, L"shm", v, 64);
        DWORD size = 16 + s_vhW * s_vhH * 2;
        if (s_vhW > 0 && s_vhH > 0 && s_vhW <= s_cw && s_vhH <= s_ch) {   // the same memory: VestaHome made it
            s_vhMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, v);
            s_vhView = s_vhMap ? (unsigned char*)MapViewOfFile(s_vhMap, FILE_MAP_ALL_ACCESS, 0, 0, size) : NULL;
        }
        Log("vestahome: map widget %d x %d %s", s_vhW, s_vhH, s_vhView ? "ok" : "NO MEMORY");
    } else if (!wcsncmp(cmd, L"widget.show", 11)) {
        VhapField(cmd, L"value", v, 8);
        s_vhShown = v[0] == L'1';
    } else if (!wcsncmp(cmd, L"widget.tap", 10) || !wcsncmp(cmd, L"nav.tap", 7)) {   // to the front, the whole screen
        ShowWindow(s_wnd, SW_SHOW);
        SetForegroundWindow(s_wnd);
        InvalidateRect(s_wnd, NULL, FALSE);
    } else if (!wcsncmp(cmd, L"bye", 3) || !wcsncmp(cmd, L"quit", 4)) {   // VestaHome gone: OSMapsCE stays
        VhFree();
        s_vh = NULL;
    }
    return 1;
}

// The route's next manoeuvre to VestaHome (its top bar's chip), when it changes
static void VhNavTick(const RouteInfo& ri)
{
    VNav n;
    memset(&n, 0, sizeof(n));
    n.on = ri.state == ROUTE_ON && !ri.off;
    if (n.on) {
        wchar_t verb[112], street[112];
        VNavSplit(ri.text, verb, street, 112);
        n.turn = ri.turn;
        FmtDist(n.dist, 16, ri.nextM);
        wcsncpy(n.street, street, 63);
        FmtArrival(n.eta, 8, ri.leftS);
    }
    wchar_t cmd[256];
    VNavFormat(n, cmd, 256);
    if (wcscmp(cmd, s_vhNavSent)) {
        wcscpy(s_vhNavSent, cmd);
        VhSend(L"%s", cmd);
    }
}

// Each second: find VestaHome (again), tell it the manoeuvre, and a frame for the widget while it is in sight.
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
    RouteInfo ri;
    RouteGet(&ri);
    VhNavTick(ri);
    if (!s_vhShown || !s_vhView || !s_backBits || !s_back)
        return;
    // On a route the widget is the way ahead (ideas 0.7): by the course, whatever the map's own mode (given back below),
    // the car low in the picture, over the trip's line; the card of the manoeuvre and the trip on it.
    bool guide = ri.state == ROUTE_ON && !ri.off;
    int follow = g_cfg.follow;
    if (guide) {
        g_cfg.follow = FOLLOW_HEADING;
        FollowGps();
    }
    HDC wdc = GetDC(s_wnd);
    HDC dc = CreateCompatibleDC(wdc);
    ReleaseDC(s_wnd, wdc);
    HGDIOBJ ob = SelectObject(dc, s_back);
    SetBkMode(dc, TRANSPARENT);
    DrawMap(dc);   // (the next paint of our own window draws its frame anew)
    int x0 = g_cfg.mapX + (g_cfg.mapW - s_vhW) / 2, y0 = g_cfg.mapY + (g_cfg.mapH - s_vhH) / 2, stride = BackStride();
    if (guide) {
        y0 += (int)(g_cfg.mapH * kCarAhead) + 90 - s_vhH / 2;   // the car 90 px above the picture's bottom, over the trip
        RouteInfo card = ri;
        card.thenText = NULL;   // (no room for the next one here)
        DrawGuideCard(dc, x0 + 12 - 16, y0 + 12 - 56, card);
        RECT trip = { x0 + 12, y0 + s_vhH - 60, x0 + s_vhW - 12, y0 + s_vhH - 8 };
        Card(dc, trip, 26);
        DrawTripColumns(dc, ri, x0 + s_vhW / 2 - 150, 150, trip.top + 3);
    }
#ifndef UNDER_CE
    GdiFlush();
#endif
    SelectObject(dc, ob);
    DeleteDC(dc);
    if (guide) {
        g_cfg.follow = follow;
        FollowGps();
    }
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
        s_paintMs = s_lastPaintTick - t0;
        s_statPaints++;
        if (s_lastPaintTick - t0 > s_statPaintMax)
            s_statPaintMax = s_lastPaintTick - t0;
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_GLIDE) {   // a frame between the fixes: the map on screen (following or not - the car moves on
            // it then), the CPU not taken over
            if (!MapHeld() && !s_settings && !s_search && !s_plan &&
                GetForegroundWindow() == hwnd && GetTickCount() - s_lastPaintTick >= s_paintMs && GlideApply()) {
                FollowGps();
                if (FrameSig() != s_paintSig)
                    InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (wp == TIMER_ZOOM) {   // the zoom's change: a frame (the last one draws the new zoom's own)
            if (!s_zoom.on)
                KillTimer(hwnd, TIMER_ZOOM);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wp == TIMER_DOCK) {
            DockStep();
            return 0;
        }
        if (wp == TIMER_FLING) {
            DWORD now = GetTickCount();
            double dx, dy, wx, wy;
            if (s_flinging && FlingStep(&s_fling, now - s_flingTick, &dx, &dy)) {
                ViewWorld(&wx, &wy);
                WorldToGeo(wx - dx, wy - dy, g_cfg.z, &g_cfg.lon, &g_cfg.lat);
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                FlingStop();
                UserChanged();
            }
            s_flingTick = now;
            return 0;
        }
        if (wp == TIMER_DISK) {
            bool view;
            if (!DiskStep(&view) && !MapHeld())   // (moving: on, for the new place's drafts)
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
            GpsTake();
            GlideApply();
            g_today = Today();
            if (s_settings && s_maps)
                InvalidateRect(hwnd, NULL, FALSE);   // the download's progress
            VhTick();   // VestaHome's map widget
            RouteAsked();
            int sharedNight = NightShared();   // switched in VestaHome: follow it
            if (sharedNight >= 0 && (sharedNight != 0) != (g_cfg.night != 0))
                NightSet(sharedNight != 0);
            if (!MapHeld())
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
        GpsTake();
        HttpSetGpsDate(s_gps.date);   // a reliable date for HTTPS certificate checks
        if (wp == 1)
            OnGpsScanDone();
        if (s_gps.valid) {
            if (RouteAlts() >= 2 && s_gps.speedKmh > 15)
                RouteAltsDone();   // driving: the route chosen is the one on
            RouteGps(s_gps.lon, s_gps.lat, s_gps.speedKmh, s_gps.course);
        }
        if (!MapHeld()) {
            GlideApply();
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
        if (s_flinging) {   // a touch stops the map where it is - and loads nothing yet (another swipe may
            FlingStop();   // follow: the user 2026-11-05); a tap: in a while, a drag: when it ends
            s_userPending = true;
            SetTimer(hwnd, TIMER_USER, kUserDelayMs, NULL);
        }
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
            if (s_kbPressed < 0 && s_showHits)   // beside the found places' panel: the search closed
                SearchClose();
        } else if (s_plan) {
            s_planPressed = HitPlan(x, y);
        } else if ((s_planPressed = HitOverMap(x, y)) >= 0) {
            // the picker's "cancel", the long press menu
        } else if (s_lpMenu || s_lpStop >= 0 || s_favMenu >= 0 || s_favEdit >= 0) {
            s_lpMenu = false;   // a tap beside the menu closes it
            s_lpStop = -1;
            s_favMenu = s_favEdit = -1;
            s_favSure = false;
        } else if (InMap(x, y)) {
            SetTimer(hwnd, TIMER_LONG, kLongMs, NULL);
            s_longDone = false;
            s_downTick = GetTickCount();
            s_dragging = true;
            DiskSteps();   // (the drafts of where it goes)
            s_dragMoved = false;
            s_dragStart.x = x;
            s_dragStart.y = y;
            FlingReset(&s_fling);
            FlingAdd(&s_fling, s_downTick, x, y);
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
                FlingAdd(&s_fling, GetTickCount(), x, y);
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
                DWORD now = GetTickCount();
                FlingAdd(&s_fling, now, x, y);
                if (!s_dragTilt && FlingStart(&s_fling, now)) {   // (the tilted view: not - its drag is the flat
                    s_flinging = true;                               // point's, the way would not match the finger)
                    s_flingTick = now;
                    SetTimer(hwnd, TIMER_FLING, kFlingMs, NULL);
                } else {
                    UserChanged();
                }
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
            } else if ((s_favMenu = FavAt(x, y)) >= 0) {   // a favourite's mark: its menu
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
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int)
#else
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int)
#endif
{
    // "route<TAB>lat=..<TAB>lon=..<TAB>name=.." (VestaHome's map widget, a favourite): VhReceive's command - to the
    // copy running, or taken once the window is up
    if (cmdLine && cmdLine[0] == L'"') {   // (quoted: the PC's argument with its tabs)
        cmdLine++;
        wchar_t* q = wcsrchr(cmdLine, L'"');
        if (q)
            *q = 0;
    }
    bool routeArg = cmdLine && !wcsncmp(cmdLine, L"route\t", 6);
    GetModuleFileName(NULL, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash)
        slash[1] = 0;

    // single instance: bring the running copy to front
    HWND prev = FindWindow(APP_CLASS, NULL);
    if (prev && routeArg) {
        VhapSend(prev, NULL, L"%s", cmdLine);
    }
    if (prev) {   // it may be behind another window or at the bottom (OSMapsCE's "home"): to the top
        ShowWindow(prev, SW_SHOW);
        SetWindowPos(prev, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(prev);
        return 0;
    }

    bool cfg = ConfigLoad();
    LangInit(g_cfg.lang);
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
    if (routeArg)
        VhReceive(cmdLine);

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
