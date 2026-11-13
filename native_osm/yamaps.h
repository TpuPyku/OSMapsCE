// OSMapsCE - OpenStreetMap viewer with traffic overlays (Yandex, 2GIS)
// for Windows CE 6.0 (Lada Vesta MMC). Also builds as a desktop Win32 app for testing on a PC.
#pragma once

#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define APP_NAME   L"OSMapsCE"
#define APP_CLASS  L"OSMapsCE_Wnd"
// OSM tile policy: a clear, unique User-Agent with a contact URL
#define HTTP_USER_AGENT "OSMapsCE/1.0 (+https://github.com/TpuPyku/OSMapsCE)"

#define WM_APP_MAP (WM_APP + 1)   // lParam = MapResult*, receiver frees it
#define WM_APP_GPS (WM_APP + 2)   // GPS state changed
#define WM_APP_API (WM_APP + 4)   // lParam = ApiResult*, receiver frees it (ApiFree)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Image sources. OSM is the base map, the others are transparent traffic overlays.
enum { SRC_OSM = 0, SRC_YANDEX, SRC_2GIS, SRC_COUNT };
// Traffic providers selectable by the button, in button order
enum { TRF_YANDEX = 0, TRF_2GIS, TRF_COUNT };

struct Config {
    double  lon, lat;       // view center
    double  lastLon, lastLat;   // the last place the GPS told (0, 0: none yet)
    char    lang[8];        // the interface: Lang\<lang>.txt ("": Russian)
    int     z;
    int     traffic;        // traffic overlay on/off
    int     trafficSrc;     // TRF_*
    int     trafficRoute;   // traffic on the route alone, not on the map (trafshade.h)
    int     parking;        // car parks on the map, in turn with the traffic on one button (layers.h)
    int     layerSignals;   // traffic lights on the map
    int     layerBumps;     // speed bumps on the map, and the warning on the way
    int     follow;         // GPS mode: 0 off, 1 north-up, 2 heading-up
    int     night;          // dark map: lightness inverted, dimmed
    int     vector;         // the base map: 0 OSM raster tiles, 1 vector tiles (OpenFreeMap, vtile.cpp)
    int     junction;       // the junction card before complex maneuvers (junction.cpp; the user 2026-10-28: off by default)
    int     noToll;         // routes keep off toll roads (the user 2026-10-29; the unit's graphs only - OSRM cannot)
    int     voice;          // the voice prompts on the route (voice.cpp, say.cpp; users 2026-11-07)
    int     voiceVol;       // ... their volume: 1 quiet, 2 medium, 3 loud (voice.h VoiceGain; the user 2026-11-12)
    int     detail;         // the vector map's detail: 0 auto, 1 full, 2 medium, 3 fast (VTileSetDetail(detail - 1))
    int     log;            // OSMapsCE.log kept (the user 2026-10-29: off by default)
    int     tilt;           // the third person's view (its button: heading-up from z15; tilt.cpp, ring.cpp)
    int     trafficTtlMin;  // traffic image is considered fresh this long
    int     yandexLimit;    // requests per day per traffic provider
    int     twogisLimit;
    int     autoReserve;    // requests kept for manual actions (auto stops earlier)
    int     cacheMb;        // disk cache size limit
    volatile LONG cacheKb;  // disk cache size now (-1 = unknown, scan), kept in ini
    int     cacheDays;      // map tiles older than this are downloaded again (0 = never)
    int     reqDay;         // yyyymmdd the counters belong to
    int     yandexCount;    // traffic requests made on reqDay
    int     twogisCount;
    int     osmCount;       // OSM tiles downloaded on reqDay (no limit, for the log)
    int     apiLimit;       // search + route requests per day (public servers: fair use)
    int     apiCount;       // made on reqDay
    int     gpsEnabled;
    wchar_t gpsPort[16];
    int     gpsBaud;
    wchar_t killProcess[32]; // closed when it holds the GPS port (Navitel)
    int     hideTaskbar;
    int     mapX, mapY, mapW, mapH;
};

extern Config  g_cfg;
extern wchar_t g_dir[MAX_PATH];   // exe directory, with trailing backslash
extern volatile int g_today;      // yyyymmdd from GPS (or a sane clock), 0 = unknown

// util.cpp
void PathInDir(wchar_t* out, const wchar_t* name);
bool ReadWholeFile(const wchar_t* path, unsigned char** data, int* len);
void LogInit();
void LogEnable(bool on);   // the log opened / closed (Settings: "ЖУРНАЛ")
bool LogFile(wchar_t* out);   // the open log's path (MAX_PATH); false: no log open
void Log(const char* fmt, ...);
void ConfigDefaults();
bool ConfigLoad();
void ConfigSave();
// World pixels at zoom z, 256 px tiles. OSM and 2GIS: spherical Mercator (EPSG:3857).
void GeoToWorld(double lon, double lat, int z, double* x, double* y);
void WorldToGeo(double x, double y, int z, double* lon, double* lat);
// Yandex: elliptical Mercator (EPSG:3395)
void GeoToWorldYa(double lon, double lat, int z, double* x, double* y);
void WorldToGeoYa(double x, double y, int z, double* lon, double* lat);
void StampFile(HANDLE h, int yyyymmdd);   // write time := that day (0: leave)
int  FileDay(const wchar_t* path);        // yyyymmdd of the write time, 0 = unknown
int  DaysBetween(int from, int to);       // yyyymmdd dates
bool KillProcess(const wchar_t* exeName);

// image.cpp
struct Image { HBITMAP bmp; int w, h; };
#define RGB565(r, g, b) ((unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
unsigned short* ImageCreate(int w, int h, Image* out);   // 16bpp RGB565, bottom-up
bool ImageFromMemory(const unsigned char* data, int len, Image* out);
bool ImageFromFile(const wchar_t* path, Image* out);
void ImageFree(Image* im);
// n RGB565 pixels to 0x00RRGGBB (each channel widened with its top bits: 31 -> 255, 0 -> 0) - the frame for a
// 32-bit screen (sheet 29 p. 25-26: GDI's own conversion of the frame took 42 ms on the unit)
void Rgb565To32(const unsigned short* s, unsigned long* d, int n);
// Text as ready pixels (stamp.cpp; the Shturmann's frame 2026-11: names and panels, GDI's text, over half of it): a text
// drawn once on black and on white gives its runs; the next frames lay those straight into the frame.
struct StampKey { const wchar_t* s; int n, a, b, c, d; };   // the text and what else makes its look (font, colours, box)
typedef void (*StampPaint)(HDC dc, int x, int y, void* arg);   // draws it with its origin at (x, y)
int StampBuild(const WORD* onBlack, const WORD* onWhite, int stride, int w, int h, WORD* out, int cap);   // words, -1 no room
void StampBlit(const WORD* runs, int words, WORD* frame, int stride, int cw, int ch, int x, int y);   // frame: top-down
void StampReset(int budgetWords);
const WORD* StampFind(const StampKey& key, int* words);
bool StampPut(const StampKey& key, const WORD* runs, int words);
int StampWords();
void StampTarget(HDC dc, WORD* frame, int stride, int cw, int ch);   // frame with stride: dc's pixels, rows as they lie
// Draws the box (x, y, w, h) on the target's dc, paint's origin at (x + ox, y + oy); other dc, or too big: paint itself.
void StampDraw(HDC dc, const StampKey& key, int x, int y, int w, int h, StampPaint paint, void* arg, int ox, int oy);
void StampStats(int* hits, int* misses, int* direct);   // since the last call
// Traffic overlay in one malloc'ed block, transparent pixels left out (most of a traffic
// image is empty). Row y (top-down) at rowOfs[y] from the block start:
// u16 run count, u16 padding, then runs of { u16 skip, u16 count, count * RGBA }.
struct Overlay {
    int          w, h;
    int          bytes;      // whole block, for the memory budget
    unsigned int rowOfs[1];  // h entries
};
Overlay* OverlayFromMemory(const unsigned char* data, int len);   // free() it

// Tiles. OSM and 2GIS: 256 px slippy tiles x, y at zoom z.
// Yandex traffic: Static API images 600x450 laid edge to edge in Yandex world pixels,
// image (x, y) is centered at (x * YA_W, y * YA_H).
#define TILE 256
#define YA_W 600
#define YA_H 450
#define CACHE_BLOCK 64      // tiles per cache subfolder side: FAT is slow on big folders

// HTTP: VestaCore vhttp.cpp (HttpInit in NetStart: HTTP_USER_AGENT, the log)
#include "vhttp.h"

// net.cpp
struct MapRequest { int src, z, x, y; };
struct MapResult {
    MapRequest     req;
    DWORD          seq;
    int            err;
    int            httpStatus;
    int            bytes;
    Image          img;     // base map
    Overlay*       ov;      // traffic overlay
    int            look;    // a vector tile: drawn for this g_cfg.vector + 2 * night (MapLook); else -1
};
int   MapLook();            // the base map's kind and palette now: a tile drawn for another one is not kept
void  NetStart(HWND notify);
DWORD NetRequest(const MapRequest& r);   // returns sequence number
void  NetStop();
const wchar_t* NetErrorText(int err);
void  CachePath(wchar_t* out, const MapRequest& r);
// One search / route request besides the tiles (GET, the answer's body as text); a newer one replaces a
// waiting one, it goes before the tiles.
enum { API_SEARCH = 1, API_ROUTE, API_REVERSE };
struct ApiResult {
    int    kind;
    DWORD  seq;
    int    err, httpStatus;
    char*  body;     // 0-terminated, may be NULL
    int    len;
};
DWORD NetApi(int kind, const char* host, bool https, const char* path);   // returns sequence number
void  ApiFree(ApiResult* r);
// JSON and URL encoding: VestaCore vjson.cpp
#include "vjson.h"

// offmaps.cpp - the regions' maps downloaded on the unit: the release's catalog, the card's regions, a zip unpacked
enum { MAP_NEW, MAP_UPDATE, MAP_OK };
struct MapRegion {
    char name[48], title[96], date[12], zip[64], have[12];   // title: UTF-8; have: the card's date, "" none
    int  sizeKb;
    int  fullKb;   // the region whole (its tiles too) on the catalog's Yandex Disk; 0 none
    bool files;    // ... on the catalog's own server instead ("files" in its entry, 2026-11-09)
    int  diskKb;   // its copy on the catalog's Yandex Disk (2026-11-12: two sources - "disk" {size, date}); 0 none
    char diskDate[12];   // ... uploaded that day (the disk: only the schedule's runs - older than date maybe)
    bool foreign;  // outside the former USSR: the list's end, a globe by its title
};
int  MapsOrder(const void* a, const void* b);   // the list's order: the foreign ones (far in the catalog) last, by the title
bool MapsDiskOld(const MapRegion& r);           // its disk copy older than the region's build
int  MapsCatalog(const char* json, MapRegion* out, int cap);
void MapsHave(MapRegion* r, int n);
int  MapState(const MapRegion& r);
bool UrlSplit(const char* url, char* host, int cap, bool* https, const char** path);
bool ZipExtract(const wchar_t* zip, const wchar_t* dir, const char* const* files, int n);
typedef int (*MapsGetFn)(const char* host, bool https, const char* path, unsigned char** body, int* len, int* status,
                         const HttpExtra* extra);
void MapsSetGet(MapsGetFn fn, int retryMs);   // tests: a made-up server; NULL - HttpGet
bool MapsFetch(const char* url, const wchar_t* file, int chunk);   // resumable, in ranges, through redirects
enum { MAPS_GRAPH, MAPS_FULL, MAPS_OVERVIEW };   // what a zip holds (its files unpacked)
bool MapsInstall(const char* url, const wchar_t* data, const char* name, int kind);   // a zip downloaded, unpacked
enum { MAPS_IDLE, MAPS_BUSY };
bool MapsList();                 // the release's catalog, on a thread (false: busy)
bool MapsGetRegion(int i);       // the catalog's region i (and the trunk roads when none), on a thread
enum { MAPS_SRC_SERVER, MAPS_SRC_DISK };   // where from: the maps' own server (else GitHub), Yandex Disk
bool MapsGetRegionFrom(int i, int src);   // ... from there (its overview's pack too)
int  MapsBusySource();            // ... the one being downloaded's
bool MapsGetRegions(const int* list, int n, int src);   // several (ticked: the user 2026-11-12), in turn; false - busy,
                                                         // none or one not in the catalog
int  MapsCount();
bool MapsAt(int i, MapRegion* out);
int  MapsJob(wchar_t* text, int cap, int* gotKb, int* totalKb);   // MAPS_BUSY / MAPS_IDLE, its line, progress
int  MapsBusyRegion();            // the catalog's region being downloaded, -1 none
void MapsCancel();                // the download stopped (its part kept: goes on from there next time)

// offline.cpp - routes from a region's road graph (OSMapsData\<region>\route.grf), answered as OSRM's JSON
int   RoadTier(int cls);   // a road class's level: 0 main (motorway..primary, links), 1 secondary / tertiary, 2 minor
bool  HierAllows(int tier, int nearM, int hier);   // its edge searched nearM from the nearer end (hier: 2, 1, 0 none)
int   TurnCostDs(int x1, int y1, int x2, int y2);   // a turn from move 1 to move 2 (metres): its time, 0.1 s
int   TurnCostB(int in, int out);   // the same by bearings (256 = a full circle: the graph's own, make_graph.py)
bool  OfflineRoute(const char* osrmPath, char** body, int* len);   // false: no region / no way - ask OSRM
bool  OfflineTrunkRoute(const char* osrmPath, char** body, int* len);   // over the trunk roads (4b): after OSRM failed
bool  OfflineHasTrunk();   // the trunk roads' graph is on a card
bool  OfflineDir(const int* latLon, int n, wchar_t* dir);          // the region's folder holding the points
bool  OfflineRoadAt(int lat, int lon, double maxM, char* name, int cap);   // the nearest road's name (1e-6 degrees)
// The posted speed limit (km/h) of the road the car is on: the nearest within maxM going the course's way (either
// direction; course < 0: any road); 0 = not known (no road, a graph made before the limits, none posted); -1 = the
// data busy (a route being made): try again later
int   OfflineLimitAt(int lat, int lon, double course, double maxM);
// The magnet with no route (2026-11-11): the fix (1e-6 degrees) onto the nearest road within maxM going the course's way
// within tolDeg (either direction; tolDeg < 0: any road), its course then the road's way nearest the car's (tolDeg < 0:
// kept). 1 - moved, 0 - no road there (left as it was), -1 - the data busy (a route being made)
int   OfflineSnapFix(int* lat, int* lon, double* course, double maxM, double tolDeg);
void  OfflineDrop();   // the graph and the search's index let go (a region written over them); loaded again when asked
// offsearch.cpp - the search and the address of a point from a region's index (search.idx), answered as Photon's
bool  OfflineSearch(const char* photonPath, char** body, int* len);   // false: no region / nothing - ask Photon
void  OfflineSearchDrop();   // its index let go (OfflineDrop)
// vpack.cpp - a region's vector tiles (tiles.vtp, offline/make_tiles.py): the data tile d (VEC_ZBASE + z), malloc'd
bool  VPackTile(const MapRequest& d, unsigned char** data, int* len);   // false: no region's pack has it
int   VOverviewZ();   // the overview pack's deepest zoom (OSMapsData\overview\overview.vtp: the CIS's small zooms whole), -1 none
bool  VOverviewTile(const MapRequest& d, unsigned char** data, int* len);   // its data tile d (malloc'd); false: none
// offdata.cpp - the offline data's files (a header, parts kept in memory) and the JSON text of the answers
enum { kOffParts = 7 };
struct OffFile {
    HANDLE      file, mfile, map;   // mfile, map: the file mapped (its parts paged in as read, out when memory is short)
    const BYTE* view;
    DWORD       size;
    void*       part[kOffParts];    // the parts read into memory (not mapped)
    int         nparts;
    bool        bad;                // a part not there
    wchar_t     path[MAX_PATH];     // (mapped later when a part gets no memory)
};
extern void* (*g_offAlloc)(size_t);   // (tests) the parts' malloc
bool  OffOpen(OffFile* f, const wchar_t* path, void* head, DWORD bytes);   // the file and its header; false: closed
bool  OffMapWanted(DWORD fileBytes, DWORD freeBytes);   // mapped: unless three times its size is free
extern DWORD g_offFreeMem;   // (tests) the free memory as if, 0 - the system's
void* OffPart(OffFile* f, DWORD off, DWORD bytes);   // a part read into memory, freed by OffClose; NULL: no memory
bool  OffRead(HANDLE f, DWORD off, void* to, DWORD bytes);
void  OffClose(OffFile* f);                          // the parts freed, the file closed, all zero
void  OffUse(bool on);   // the data in use (a route, a search) / written over (offmaps.cpp): one at a time
bool  OffTryUse();       // OffUse(true) if free now (the window's quick looks: never wait for a route); false - busy
struct OffUseScope { OffUseScope() { OffUse(true); } ~OffUseScope() { OffUse(false); } };
// The region (a folder of OSMapsData on the SD card / USB disk / beside the program) whose file passes the test:
// dir = its folder; false = none. holds: reads the file's header, says whether it serves the caller (ctx).
typedef bool (*OffHolds)(const wchar_t* path, void* ctx);
bool  OffInside(const int* box, int lat, int lon);   // box: minLat, minLon, maxLat, maxLon (1e-6 degrees, a header's)
bool  OffRegion(const wchar_t* file, OffHolds holds, void* ctx, wchar_t* dir);
void  OffWriteDir(wchar_t* out);
void  OffVolumes(bool on);   // false: the program's folder only (tests)   // where downloaded regions go: a volume's \OSMapsData (made by the caller)
struct JBuf { char* p; int n, cap; };                // a growing 0-terminated text (no memory: the rest left out)
void  JPut(JBuf& b, const char* s, int n = -1);
void  JPutf(JBuf& b, const char* fmt, ...);
void  JPutStr(JBuf& b, const char* s, int n);       // a JSON string: quoted, escaped, control characters left out

// route.cpp - search (Photon) and the route (OSRM) with its guidance; OSM data, public servers
struct SearchHit { wchar_t name[80], sub[96]; double lon, lat; };
enum { SEARCH_IDLE, SEARCH_BUSY, SEARCH_DONE, SEARCH_FAIL };
enum { ROUTE_NONE, ROUTE_BUSY, ROUTE_ON, ROUTE_FAIL, ROUTE_ARRIVED };
struct RouteInfo {
    int            state;       // ROUTE_*
    const wchar_t* text;        // the next maneuver's instruction (or the state's text)
    int            nextM;       // metres to it
    int            turn;        // its turn, degrees (-180..180, + right), 999 = destination, 998 = roundabout,
                                // 997 = a stop on the way
    int            leftM, leftS;   // to the destination
    bool           off;         // off the route, a new one is asked for
    const wchar_t* thenText;    // the turn after the next one (NULL: none)
    int            seg;         // the line's segment the car is on (the rest of the line: from it)
    bool           complex;     // the next maneuver a fork, a ramp, a roundabout, a merge (the junction card)
    int            shape;       // its point on the line (RoutePoints)
    int            man;         // its number on the route (the voice prompts: voice.h)
    int            act;         // what it asks (VA_*), thenAct - the one after it when close behind (kThenM)
    int            thenAct;
    int            serial;      // the route's number: a new one for each answer of the router
    int            vias;        // the stops left on the way
};
void SearchStart(const wchar_t* q, double lon, double lat);
int  SearchState();
int  SearchCount();
const SearchHit* SearchGet(int i);
// The route editor's plan: from the car or a place, stops on the way, the destination.
enum { PL_NONE, PL_GPS, PL_PLACE };
enum { kMaxVia = 3 };
struct RoutePlace { int kind; wchar_t name[80]; double lon, lat; };
struct RoutePlan { RoutePlace from, via[kMaxVia], to; int nvia; };
RoutePlan* RoutePlanGet();
bool RouteStart(double gpsLon, double gpsLat, double course, bool gpsValid);   // false: the plan is not complete
// From the search: the car (course >= 0 or -1: unknown) or, course == -2, the map's center as a fixed start.
void RouteTo(const SearchHit* to, double fromLon, double fromLat, double course);
void RouteAddVia(const RoutePlace& p, double gpsLon, double gpsLat, double course, bool gpsValid);
void PlaceLookup(const RoutePlace& p);   // its address into the plan when Photon answers
bool PlaceNamed(double lon, double lat, SearchHit* h);   // the address found for that very point (the last asked)
// Alternatives of a new route without stops: >= 2 while the driver chooses (the first one is on meanwhile).
int  RouteAlts();
int  RouteAltCur();
bool RouteAltGet(int k, const double** lonLat, int* np, double* timeS, double* distM);
void RouteAltChoose(int k);
void RouteAltsDone();                    // chosen: the others and the kept answer go
void RouteClear();
bool RouteMagnet(double* lon, double* lat, double kmh, double* course);   // the fix shown: on the line when near it
void RouteGps(double lon, double lat, double kmh, double course);   // each fix: progress, off -> a new route
void RouteGet(RouteInfo* out);
int  RoutePoints(const double** lonLat);   // the line: lon, lat pairs
bool RouteDest(double* lon, double* lat);
DWORD RouteSig();                   // changes when what is drawn of the route changes

// say.cpp - the voice prompts spoken (voice.h decides what and when)
enum { TIMER_VOICE = 9 };            // (main.cpp's timers: 1..8)
void SayInit(HWND wnd);
void SayTick(int kmh);               // each GPS fix: the route's prompt due, said (g_cfg.voice)
void SayDone();                      // MM_WOM_DONE
void SayTimer();                     // TIMER_VOICE
void SayStop();                      // the program ends
int VoiceChannel(bool on);           // main.cpp: the radio down for a prompt - 1 by the shell, 2 by VestaHome, 0 no one
bool OnApiResult(ApiResult* r);     // frees it; true = redraw

// vtile.cpp - vector tiles drawn into TILE x TILE images; their .pbf on disk as Cache\<VEC_ZBASE + z>\...
#define VEC_ZBASE 100
#define VEC_MAXZ  14        // OpenFreeMap's deepest: z15 and up are parts of the z14 tile drawn larger
#define VEC_PACKZ 15        // a region's pack's deepest (Protomaps' z15: the house numbers, the car parks)
void       VTileInit();     // before the download thread starts
MapRequest VTileData(const MapRequest& r);   // the data tile (VEC_ZBASE + z) a shown tile is drawn from
MapRequest VTileDataAt(const MapRequest& r, int maxz);   // ... with data tiles up to zoom maxz (a pack: VEC_PACKZ)
bool       VTileRender(const MapRequest& r, const unsigned char* data, int len, Image* out);   // data NULL: parsed already
bool       VTileRenderFrom(const MapRequest& r, const MapRequest& d, const unsigned char* data, int len, Image* out,
                           bool draft = false);   // from data tile d; draft: in the fast style (redrawn when idle)
bool       VTileLocal(const MapRequest& r, Image* out, bool draft = false);   // from the data at hand (parsed, a pack, the
                                                                             // cache); false: none
void       VTileSetDetail(int d);   // the map's detail: -1 auto (by the tiles' time), 0 full, 1 medium, 2 fast
int        VTileDetail();           // the detail drawn now (auto: medium till kAutoTiles are timed)
int        VTileDetailFor(DWORD avgMs);   // auto's choice by a tile's average time
void       VTileNoteMs(DWORD ms, int z);   // a tile's time (auto: z10 and up, timed once a run)
void       VTileAutoReset();        // (tests) auto timed anew
void       VLabelsDraw(HDC dc, const RECT& mr, double cx, double cy, double vx, double vy, double angle, int z,
                       const RECT* avoid, int navoid);   // avoid: the controls over the map, no names under them
int        VLabelPasses();          // the names' passes: places, signs, streets; the last one the house numbers
bool       VLabelPassOn(int i, int z, int detail, bool tilted);   // pass i drawn (tilted: no house numbers)
const wchar_t* MapFace();
extern bool g_noYo;          // the font has no yo (not our Golos Text): drawn as ye (vtile.cpp NoYo)
void NoYo(wchar_t* s);   // main.cpp: the text's face (Golos Text, else Tahoma)

// tilt.cpp: the third person's view - the flat map (offsets from the map's centre, px, y down) tilted, the car at
// (0, cf) to the screen's (0, cs) keeping its size, nothing past the fog (fog px ahead of the car or behind it)
struct Tilt { double cf, cs, rc, f, fog, fr; };   // fr: the fog's row (the ground's top) on the screen
extern const Tilt* g_tilt;   // the view's tilt while it is on (VLabelsDraw), else NULL
void TiltMake(Tilt* t, double carFlat, double carScreen, double fogPx, double pitchDeg);   // pitch: from straight down
// The pitch shown going to the zoom's (target) at kTiltDegPerS, ms after the last step (the user 2026-11-11: the zoom
// smooth, the pitch jumped 10 degrees at its end); the first (shown < 0): the target at once
enum { kTiltDegPerS = 25 };
double TiltEase(double shown, double target, DWORD ms);
// The mark under the finger (x, y): the nearest of those on (pts[i] where drawn) within r px, -1: none. (A Shturmann
// 2026-11-11: a mark 48000 px off in the tilted view - its squared distance past int, any tap opened it.)
int MarkAt(const POINT* pts, const bool* on, int n, int x, int y, int r);
bool TiltPoint(const Tilt& t, double fx, double fy, double* sx, double* sy);   // false: in the fog
void TiltProject(const Tilt& t, double fx, double fy, double* sx, double* sy);   // the same, no fog
bool TiltRow(const Tilt& t, double sy, double* fy, double* k, double* fade);   // a screen row: the flat row fy, its
                                // x = sx * k, the fog over it 0..1; false: all fog
void TiltFog(const Tilt& t, double sy, bool night, unsigned char rgb[3]);   // the fog's colour at a screen row
bool TiltClip(const Tilt& t, double* x0, double* y0, double* x1, double* y1);   // a flat line cut to the seen part

// ring.cpp: the third person's frame out of the map's rings. A ring: a square canvas that wraps (a world px of the
// view's zoom at ((wx >> shift) mod size, (wy >> shift) mod size)), top: its top row (a DIB's: stride negative), the part
// loaded [x0, x1) x [y0, y1) in the view zoom's world px
struct RingView { const unsigned short* top; int stride, size, shift; int x0, y0, x1, y1; };
unsigned short Blend565(unsigned short a, unsigned short b, int alpha32);   // a to b by alpha / 32
void TiltWarp(const RingView* rings, int n, const Tilt& t, int w, int h, double vx, double vy, double angleDeg, bool night,
              unsigned short* dst, int dstride);   // rings: the first that has a pixel gives it; dst: the top row
void RingAhead(double vx, double vy, double angleDeg, double d, double* x, double* y);   // d px ahead by the course
// A ring's slot (main.cpp): the tile it shows, what it was drawn from (sig) and with (ctx: night, traffic), exact: the
// tile of its own zoom (not a coarser one scaled)
struct SlotShown { int x, y; DWORD sig, ctx; bool used, exact; };
bool SlotRedraw(const SlotShown& s, int x, int y, DWORD sig, DWORD ctx, bool exact);   // to show (x, y) as sig says
struct CacheEntry;
// The entry the memory's cap drops (cache.cpp's Trim): the least recently used of the base tiles and the traffic not
// used for 30 s, none of them - of all; never keep; -1: none
int  CacheVictim(const CacheEntry* e, int n, int keep, DWORD now);
int  CacheCapFor(DWORD freeKb);   // the images' memory cap, bytes, by the memory free at the start

// junction.cpp: the junction card (the user 2026-10-28, on in the settings) - the route round a complex maneuver on the
// map turned so the way in points up
bool JunctionComplex(const char* osrmType);   // a fork, a ramp, a roundabout, a merge
bool JunctionShown(bool on, bool complex, int nextM, bool off, int reachM);
// The places (x, y pairs in the world's px at z0) in a w x h area: the closest zoom zMin..zMax showing them all, the
// centre of their box; -1: no places
int  FitZoom(const double* xy, int n, double w, double h, int zMin, int zMax, double* cx, double* cy);
int  JunctionReachM(double kmh);   // the card shown this far before the maneuver: 15 s of the way, 300 m at least
int  JunctionZoom(int userZ, bool heading, bool complex, int nextM, bool off, int reachM);   // z17 before it
int  WheelZoom(int code, int kind);   // a wheel button from VestaHome: +1 / -1 the zoom (up / down), 0 - not ours
bool WheelNow(int code, int st);   // its zoom done at this event (up / down: when let go - the MCU sends only that)
double GlideCourse(double from, double to, DWORD ms);   // the turn from -> to after ms of its second (0..360)
// The mark a fix late (the user 2026-11-02): its speed from where it is shown to the fix in periodMs, metres a ms east
// and north; false: more than 50 m off or no time - none (put there)
bool GlideAim(double lon, double lat, double tLon, double tLat, DWORD periodMs, double* ve, double* vn);
// The car on the route's line, for the eye only (the user 2026-11-03: it jumps about on a poor signal): the line's nearest
// point around segment seg of pts[np] (lon, lat pairs) when the fix is near it and goes its way. "was": it was on the line.
bool MagnetSnap(const double* pts, int np, int seg, double* lon, double* lat, double kmh, double* course, bool was);
// The route (lon, lat pairs) behind the car cut off: of its segments from..to (clamped) the nearest to (lon, lat) within
// maxM; cut: the car's point on it. -1: none that near
int  RouteCut(const double* ll, int np, int from, int to, double lon, double lat, double maxM, double* cut);
void GlideMove(double* lon, double* lat, double ve, double vn, DWORD ms);
int  JunctionSlice(const double* ll, int np, int at, double beforeM, double afterM, double* out, int cap, int* outAt,
                   double* course);   // lon, lat pairs; outAt: the maneuver's point in out; course: the way in's, degrees
void JunctionTurn(const unsigned short* src, int sw, int sh, int sstride, double scx, double scy, unsigned short* dst,
                  int dw, int dh, int dstride, double dcx, double dcy, double courseDeg, unsigned short bg, int radius);
                                // src, dst: their top rows; strides in pixels (a DIB's bottom-up rows: negative);
                                // radius: the card's round corners (their pixels left as they are)
int  CornerInset(int radius, int y, int top, int bottom);   // a rounded rectangle's row y: px in from each side

// cache.cpp - decoded images in memory + OSM tiles on disk in <exe>\Cache
struct CacheEntry {
    MapRequest     req;
    Image          img;         // base map tile
    Overlay*       ov;          // traffic overlay (not stored on disk)
    DWORD          fetchTick;   // GetTickCount() of download; 0 = loaded from disk
    int            fileDay;     // loaded from disk: yyyymmdd the file was saved, 0 = unknown
    DWORD          useTick;
    bool           draft;       // drawn in the fast style first (CacheRefine: in the chosen detail when idle)
};
enum { CACHE_MEM = 96 };       // (3D: the two rings' 61 tiles and the traffic)        // a rotated view needs ~16 base tiles + overlays + coarser levels
// Memory cap for the decoded images: a view needs ~2 MB of tiles (128 KB each)
// + up to ~2 MB of overlays (~360 KB each); the least recently used are dropped above it.
#define CACHE_BYTES CacheCap()   // (ring.cpp CacheCapFor: 12 MB with 96 MB free, else 6)
int CacheCap();
void        CacheInit();
void        CachePrune();                 // scan the disk, delete over g_cfg.cacheMb
void        CacheCountSaved(int newBytes, int oldBytes);
CacheEntry* CacheFind(const MapRequest& r, bool loadFromDisk, bool draft = false);   // draft: the vector one drawn fast
CacheEntry* CacheAdd(MapResult* res, DWORD fetchTick);   // takes the images from res
int         CacheList(CacheEntry** out, int max);
int         CacheBytes();                 // decoded images in memory now, for the log
bool        SameKey(const MapRequest& a, const MapRequest& b);
void        CacheDropBase();
bool        CacheRefine(CacheEntry* e);   // a draft drawn again in the chosen detail; false: not a draft / no data              // the base map's tiles out of memory (raster <-> vector)

// GPS: VestaCore (core/vgps.h) - the port shared with VestaHome through shared memory
#include "vgps.h"
#include "vlogdir.h"   // VestaCore: logs only on the SD card / USB disk
#include "vnight.h"    // VestaCore: the night mode shared with VestaHome
