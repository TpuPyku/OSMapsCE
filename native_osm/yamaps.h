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
int  HttpGet(const char* host, bool https, const char* path, unsigned char** body, int* bodyLen, int* status,
             const char* ifNoneMatch = NULL, char* etag = NULL, int etagCap = 0);
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
void       VTileInit();     // before the download thread starts
MapRequest VTileData(const MapRequest& r);   // the data tile (VEC_ZBASE + z) a shown tile is drawn from
bool       VTileRender(const MapRequest& r, const unsigned char* data, int len, Image* out);   // data NULL: parsed already
void       VLabelsDraw(HDC dc, const RECT& mr, double cx, double cy, double vx, double vy, double angle, int z,
                       const RECT* avoid, int navoid);   // avoid: the controls over the map, no names under them
const wchar_t* MapFace();   // main.cpp: the text's face (Golos Text, else Tahoma)

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
