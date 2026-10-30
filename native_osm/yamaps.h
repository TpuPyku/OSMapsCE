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
    int     z;
    int     traffic;        // traffic overlay on/off
    int     trafficSrc;     // TRF_*
    int     follow;         // GPS mode: 0 off, 1 north-up, 2 heading-up
    int     night;          // dark map: lightness inverted, dimmed
    int     vector;         // the base map: 0 OSM raster tiles, 1 vector tiles (OpenFreeMap, vtile.cpp)
    int     junction;       // the junction card before complex maneuvers (junction.cpp; the user 2026-10-28: off by default)
    int     noToll;         // routes keep off toll roads (the user 2026-10-29; the unit's graphs only - OSRM cannot)
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

// http.cpp
// With ifNoneMatch the server may answer 304 (no body); the ETag of the answer goes to etag.
struct HttpExtra {             // a download's (offmaps.cpp): its request's headers, the answer's Location
    const char* headers;       // e.g. "Range: bytes=0-1048575\r\n" (each line with CR LF), NULL none
    char*       location;      // the answer's Location (a redirect: GitHub's release files), NULL not wanted
    int         locCap;
};
int  HttpGet(const char* host, bool https, const char* path, unsigned char** body, int* bodyLen, int* status,
             const char* ifNoneMatch = NULL, char* etag = NULL, int etagCap = 0, const HttpExtra* extra = NULL);
void HttpSetGpsDate(int yyyymmdd);
void HttpCloseAll();

// net.cpp
enum { NET_OK = 0, NET_DNS, NET_CONNECT, NET_TIMEOUT, NET_HTTP, NET_DECODE, NET_MEMORY, NET_TLS };
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
// json.cpp - JSON read in place, no tree: p points at a value
const char* Ws(const char* p);                       // past the white space
const char* JSkip(const char* p);                    // past the value
const char* JKey(const char* p, const char* key);    // the object's value of key, NULL none
const char* JFirst(const char* p);                   // the array's first item, NULL none
const char* JNext(const char* p);                    // the next item, NULL none
double      JNum(const char* p, double def);
int         JStr(const char* p, char* out, int cap);   // a string, unescaped, UTF-8
void        JWide(const char* p, wchar_t* out, int cap);

// offmaps.cpp - the regions' maps downloaded on the unit: the release's catalog, the card's regions, a zip unpacked
enum { MAP_NEW, MAP_UPDATE, MAP_OK };
struct MapRegion {
    char name[48], title[96], date[12], zip[64], have[12];   // title: UTF-8; have: the card's date, "" none
    int  sizeKb;
};
int  MapsCatalog(const char* json, MapRegion* out, int cap);
void MapsHave(MapRegion* r, int n);
int  MapState(const MapRegion& r);
bool UrlSplit(const char* url, char* host, int cap, bool* https, const char** path);
bool ZipExtract(const wchar_t* zip, const wchar_t* dir, const char* const* files, int n);
typedef int (*MapsGetFn)(const char* host, bool https, const char* path, unsigned char** body, int* len, int* status,
                         const HttpExtra* extra);
void MapsSetGet(MapsGetFn fn, int retryMs);   // tests: a made-up server; NULL - HttpGet
bool MapsFetch(const char* url, const wchar_t* file, int chunk);   // resumable, in ranges, through redirects
bool MapsInstall(const char* url, const wchar_t* data, const char* name, bool trunk);   // a release zip unpacked
enum { MAPS_IDLE, MAPS_BUSY };
bool MapsList();                 // the release's catalog, on a thread (false: busy)
bool MapsGetRegion(int i);       // the catalog's region i (and the trunk roads when none), on a thread
int  MapsCount();
bool MapsAt(int i, MapRegion* out);
int  MapsJob(wchar_t* text, int cap, int* gotKb, int* totalKb);   // MAPS_BUSY / MAPS_IDLE, its line, progress

// offline.cpp - routes from a region's road graph (OSMapsData\<region>\route.grf), answered as OSRM's JSON
bool  OfflineRoute(const char* osrmPath, char** body, int* len);   // false: no region / no way - ask OSRM
bool  OfflineTrunkRoute(const char* osrmPath, char** body, int* len);   // over the trunk roads (4b): after OSRM failed
bool  OfflineHasTrunk();   // the trunk roads' graph is on a card
bool  OfflineDir(const int* latLon, int n, wchar_t* dir);          // the region's folder holding the points
bool  OfflineRoadAt(int lat, int lon, double maxM, char* name, int cap);   // the nearest road's name (1e-6 degrees)
// offsearch.cpp - the search and the address of a point from a region's index (search.idx), answered as Photon's
bool  OfflineSearch(const char* photonPath, char** body, int* len);   // false: no region / nothing - ask Photon
// vpack.cpp - a region's vector tiles (tiles.vtp, offline/make_tiles.py): the data tile d (VEC_ZBASE + z), malloc'd
bool  VPackTile(const MapRequest& d, unsigned char** data, int* len);   // false: no region's pack has it
// offdata.cpp - the offline data's files (a header, parts kept in memory) and the JSON text of the answers
enum { kOffParts = 6 };
struct OffFile {
    HANDLE      file, mfile, map;   // mfile, map: the file mapped (its parts paged in as read, out when memory is short)
    const BYTE* view;
    DWORD       size;
    void*       part[kOffParts];    // the parts read into memory (not mapped)
    int         nparts;
    bool        bad;                // a part not there
};
bool  OffOpen(OffFile* f, const wchar_t* path, void* head, DWORD bytes);   // the file and its header; false: closed
void* OffPart(OffFile* f, DWORD off, DWORD bytes);   // a part read into memory, freed by OffClose; NULL: no memory
bool  OffRead(HANDLE f, DWORD off, void* to, DWORD bytes);
void  OffClose(OffFile* f);                          // the parts freed, the file closed, all zero
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
// Alternatives of a new route without stops: >= 2 while the driver chooses (the first one is on meanwhile).
int  RouteAlts();
int  RouteAltCur();
bool RouteAltGet(int k, const double** lonLat, int* np, double* timeS, double* distM);
void RouteAltChoose(int k);
void RouteAltsDone();                    // chosen: the others and the kept answer go
void RouteClear();
void RouteGps(double lon, double lat, double kmh, double course);   // each fix: progress, off -> a new route
void RouteGet(RouteInfo* out);
int  RoutePoints(const double** lonLat);   // the line: lon, lat pairs
bool RouteDest(double* lon, double* lat);
DWORD RouteSig();                   // changes when what is drawn of the route changes
bool OnApiResult(ApiResult* r);     // frees it; true = redraw

// vtile.cpp - vector tiles drawn into TILE x TILE images; their .pbf on disk as Cache\<VEC_ZBASE + z>\...
#define VEC_ZBASE 100
#define VEC_MAXZ  14        // OpenFreeMap's deepest: z15 and up are parts of the z14 tile drawn larger
#define VEC_PACKZ 15        // a region's pack's deepest (Protomaps' z15: the house numbers, the car parks)
void       VTileInit();     // before the download thread starts
MapRequest VTileData(const MapRequest& r);   // the data tile (VEC_ZBASE + z) a shown tile is drawn from
MapRequest VTileDataAt(const MapRequest& r, int maxz);   // ... with data tiles up to zoom maxz (a pack: VEC_PACKZ)
bool       VTileRender(const MapRequest& r, const unsigned char* data, int len, Image* out);   // data NULL: parsed already
bool       VTileRenderFrom(const MapRequest& r, const MapRequest& d, const unsigned char* data, int len, Image* out);   // from data tile d
bool       VTileLocal(const MapRequest& r, Image* out);   // from the data at hand (parsed, a pack, the cache); false: none
void       VTileSetDetail(int d);   // the map's detail: -1 auto (by the tiles' time), 0 full, 1 medium, 2 fast
int        VTileDetail();           // the detail drawn now (auto: full till kAutoTiles are timed)
int        VTileDetailFor(DWORD avgMs);   // auto's choice by a tile's average time
void       VTileNoteMs(DWORD ms);   // a tile's time (auto)
void       VLabelsDraw(HDC dc, const RECT& mr, double cx, double cy, double vx, double vy, double angle, int z,
                       const RECT* avoid, int navoid);   // avoid: the controls over the map, no names under them
const wchar_t* MapFace();
extern bool g_noYo;          // the font has no yo (not our Golos Text): drawn as ye (vtile.cpp NoYo)
void NoYo(wchar_t* s);   // main.cpp: the text's face (Golos Text, else Tahoma)

// tilt.cpp: the third person's view - the flat map (offsets from the map's centre, px, y down) tilted, the car at
// (0, cf) to the screen's (0, cs) keeping its size, nothing past the fog (fog px ahead of the car or behind it)
struct Tilt { double cf, cs, rc, f, fog, fr; };   // fr: the fog's row (the ground's top) on the screen
extern const Tilt* g_tilt;   // the view's tilt while it is on (VLabelsDraw), else NULL
void TiltMake(Tilt* t, double carFlat, double carScreen, double fogPx, double pitchDeg);   // pitch: from straight down
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

// junction.cpp: the junction card (the user 2026-10-28, on in the settings) - the route round a complex maneuver on the
// map turned so the way in points up
bool JunctionComplex(const char* osrmType);   // a fork, a ramp, a roundabout, a merge
bool JunctionShown(bool on, bool complex, int nextM, bool off);
int  JunctionZoom(int userZ, bool heading, bool complex, int nextM, bool off);   // z17 before a complex maneuver
int  WheelZoom(int code, int kind);   // a wheel button from VestaHome: +1 / -1 the zoom (held up / down), 0 - not ours
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
};
enum { CACHE_MEM = 64 };        // a rotated view needs ~16 base tiles + overlays + coarser levels
// Memory cap for the decoded images: a view needs ~2 MB of tiles (128 KB each)
// + up to ~2 MB of overlays (~360 KB each); the least recently used are dropped above it.
#define CACHE_BYTES (6 * 1024 * 1024)
void        CacheInit();
void        CachePrune();                 // scan the disk, delete over g_cfg.cacheMb
void        CacheCountSaved(int newBytes, int oldBytes);
CacheEntry* CacheFind(const MapRequest& r, bool loadFromDisk);
CacheEntry* CacheAdd(MapResult* res, DWORD fetchTick);   // takes the images from res
int         CacheList(CacheEntry** out, int max);
int         CacheBytes();                 // decoded images in memory now, for the log
bool        SameKey(const MapRequest& a, const MapRequest& b);
void        CacheDropBase();              // the base map's tiles out of memory (raster <-> vector)

// GPS: VestaCore (core/vgps.h) - the port shared with VestaHome through shared memory
#include "vgps.h"
#include "vlogdir.h"   // VestaCore: logs only on the SD card / USB disk
#include "vnight.h"    // VestaCore: the night mode shared with VestaHome
