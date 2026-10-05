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
int  HttpGet(const char* host, bool https, const char* path, unsigned char** body, int* bodyLen, int* status);
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
};
void  NetStart(HWND notify);
DWORD NetRequest(const MapRequest& r);   // returns sequence number
void  NetStop();
const wchar_t* NetErrorText(int err);
void  CachePath(wchar_t* out, const MapRequest& r);

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

// gps.cpp
enum { GPS_NO_PORT = 0, GPS_BUSY, GPS_OPEN, GPS_SCANNING };
struct GpsState {
    int    status;          // GPS_*
    wchar_t port[16];       // port being used / probed
    DWORD  baud;
    int    lines, badLines; // NMEA sentences with good / bad checksum since (re)configure
    char   last[84];        // last good sentence, for the settings screen
    int    found;           // scan result: 1 found, -1 nothing, 0 no scan
    int    valid;           // last RMC had status 'A'
    double lon, lat;
    double speedKmh, course;
    int    sats;
    DWORD  fixTick;         // GetTickCount() of last valid fix
    DWORD  dataTick;        // GetTickCount() of last NMEA sentence
    int    date;            // yyyymmdd from RMC, 0 = unknown
    DWORD  bytes;           // read from the port since start, for the log
};
void GpsStart(HWND notify);
void GpsStop();
void GpsGet(GpsState* s);
void GpsConfigure(const wchar_t* port, DWORD baud);   // reopen with new settings
void GpsScan();                                       // look for NMEA on COM1..COM9
