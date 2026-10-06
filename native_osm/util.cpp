#include "yamaps.h"
#include <tlhelp32.h>

Config  g_cfg;
wchar_t g_dir[MAX_PATH];

static const wchar_t* kConfigName = L"OSMapsCE.ini";
static const wchar_t* kLogName    = L"OSMapsCE.log";
static const wchar_t* kLogOldName = L"OSMapsCE.old.log";
static const DWORD    kLogMaxSize = 256 * 1024;   // two files at most: ~512 KB on disk

void PathInDir(wchar_t* out, const wchar_t* name)
{
    wcscpy(out, g_dir);
    wcscat(out, name);
}

bool ReadWholeFile(const wchar_t* path, unsigned char** data, int* len)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD size = GetFileSize(h, NULL);
    unsigned char* buf = (unsigned char*)malloc(size + 1);
    DWORD got = 0;
    bool ok = buf && ReadFile(h, buf, size, &got, NULL) && got == size;
    CloseHandle(h);
    if (!ok) {
        free(buf);
        return false;
    }
    buf[size] = 0;
    *data = buf;
    *len = (int)size;
    return true;
}

// ---------------------------------------------------------------- log

static HANDLE           s_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION s_logCs;

void LogInit()
{
    InitializeCriticalSection(&s_logCs);
    wchar_t path[MAX_PATH];
    PathInDir(path, kLogName);
    // append: after a power loss the previous session is still there to look at
    s_log = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (s_log != INVALID_HANDLE_VALUE)
        SetFilePointer(s_log, 0, NULL, FILE_END);
}

// Full log becomes OSMapsCE.old.log (replacing the previous one), a new log starts.
static void RotateLog()
{
    wchar_t path[MAX_PATH], old[MAX_PATH];
    PathInDir(path, kLogName);
    PathInDir(old, kLogOldName);
    CloseHandle(s_log);
    DeleteFile(old);
    MoveFile(path, old);
    s_log = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
}

void Log(const char* fmt, ...)
{
    if (s_log == INVALID_HANDLE_VALUE)
        return;
    char line[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf(line, sizeof(line) - 2, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf(line + n, sizeof(line) - 2 - n, fmt, ap);
    va_end(ap);
    n = (m < 0) ? (int)sizeof(line) - 2 : n + m;
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&s_logCs);
    if (GetFileSize(s_log, NULL) > kLogMaxSize)
        RotateLog();
    if (s_log != INVALID_HANDLE_VALUE) {
        SetFilePointer(s_log, 0, NULL, FILE_END);
        DWORD w;
        WriteFile(s_log, line, n, &w, NULL);
        FlushFileBuffers(s_log);
    }
    LeaveCriticalSection(&s_logCs);
}

// ---------------------------------------------------------------- config

static const char* kTrafficNames[TRF_COUNT] = { "yandex", "2gis" };

void ConfigDefaults()
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.lon = 37.880098;
    g_cfg.lat = 55.823220;
    g_cfg.z = 13;
    g_cfg.traffic = 1;
    g_cfg.trafficSrc = TRF_YANDEX;
    g_cfg.follow = 1;
    g_cfg.trafficTtlMin = 10;
    g_cfg.yandexLimit = 900;    // keyless Static API: about 1000 a day per address
    g_cfg.twogisLimit = 2000;
    g_cfg.autoReserve = 100;
    g_cfg.cacheMb = 500;
    g_cfg.cacheKb = -1;
    g_cfg.cacheDays = 7;
    g_cfg.gpsEnabled = 1;
    wcscpy(g_cfg.gpsPort, L"COM6:");       // GPS receiver of Lada Vesta MMC
    g_cfg.gpsBaud = 115200;
    wcscpy(g_cfg.killProcess, L"CityGuideCE.exe");
    g_cfg.hideTaskbar = 1;
    g_cfg.mapX = 2;
    g_cfg.mapY = 28;
    g_cfg.mapW = 600;
    g_cfg.mapH = 450;
}

static char* Trim(char* s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void ConfigSet(const char* key, const char* val)
{
    if (!strcmp(key, "lon"))               g_cfg.lon = atof(val);
    else if (!strcmp(key, "lat"))          g_cfg.lat = atof(val);
    else if (!strcmp(key, "zoom"))         g_cfg.z = Clamp(atoi(val), 2, 19);
    else if (!strcmp(key, "traffic"))      g_cfg.traffic = atoi(val) != 0;
    else if (!strcmp(key, "follow"))       g_cfg.follow = Clamp(atoi(val), 0, 2);
    else if (!strcmp(key, "traffic_ttl"))  g_cfg.trafficTtlMin = Clamp(atoi(val), 2, 600);
    else if (!strcmp(key, "yandex_limit")) g_cfg.yandexLimit = Clamp(atoi(val), 0, 100000);
    else if (!strcmp(key, "2gis_limit"))   g_cfg.twogisLimit = Clamp(atoi(val), 0, 100000);
    else if (!strcmp(key, "2gis_count"))   g_cfg.twogisCount = atoi(val);
    else if (!strcmp(key, "auto_reserve")) g_cfg.autoReserve = Clamp(atoi(val), 0, 100000);
    else if (!strcmp(key, "cache_mb"))     g_cfg.cacheMb = Clamp(atoi(val), 50, 30000);
    else if (!strcmp(key, "cache_kb"))     g_cfg.cacheKb = atoi(val);
    else if (!strcmp(key, "cache_days"))   g_cfg.cacheDays = Clamp(atoi(val), 0, 3650);
    else if (!strcmp(key, "req_day"))      g_cfg.reqDay = atoi(val);
    else if (!strcmp(key, "yandex_count")) g_cfg.yandexCount = atoi(val);
    else if (!strcmp(key, "osm_count"))    g_cfg.osmCount = atoi(val);
    else if (!strcmp(key, "gps"))          g_cfg.gpsEnabled = atoi(val) != 0;
    else if (!strcmp(key, "gps_baud"))     g_cfg.gpsBaud = atoi(val);
    else if (!strcmp(key, "hide_taskbar")) g_cfg.hideTaskbar = atoi(val) != 0;
    else if (!strcmp(key, "night"))        g_cfg.night = atoi(val) != 0;
    else if (!strcmp(key, "map_x"))        g_cfg.mapX = atoi(val);
    else if (!strcmp(key, "map_y"))        g_cfg.mapY = atoi(val);
    else if (!strcmp(key, "map_w"))        g_cfg.mapW = Clamp(atoi(val), 100, 650);
    else if (!strcmp(key, "map_h"))        g_cfg.mapH = Clamp(atoi(val), 100, 450);
    else if (!strcmp(key, "gps_port")) {
        MultiByteToWideChar(CP_ACP, 0, val, -1, g_cfg.gpsPort, 15);
        g_cfg.gpsPort[15] = 0;
    }
    else if (!strcmp(key, "kill")) {
        MultiByteToWideChar(CP_ACP, 0, val, -1, g_cfg.killProcess, 31);
        g_cfg.killProcess[31] = 0;
    }
    else if (!strcmp(key, "traffic_src")) {
        for (int i = 0; i < TRF_COUNT; i++)
            if (!strcmp(val, kTrafficNames[i]))
                g_cfg.trafficSrc = i;
    }
}

bool ConfigLoad()
{
    ConfigDefaults();
    wchar_t path[MAX_PATH];
    PathInDir(path, kConfigName);
    unsigned char* data;
    int len;
    if (!ReadWholeFile(path, &data, &len))
        return false;
    char* p = (char*)data;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        line = Trim(line);
        if (!*line || *line == ';' || *line == '#' || *line == '[')
            continue;
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        ConfigSet(Trim(line), Trim(eq + 1));
    }
    free(data);
    return true;
}

void ConfigSave()
{
    char port[16], kill[32];
    WideCharToMultiByte(CP_ACP, 0, g_cfg.gpsPort, -1, port, sizeof(port), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_cfg.killProcess, -1, kill, sizeof(kill), NULL, NULL);
    char buf[2048];
    int n = _snprintf(buf, sizeof(buf),
        "; OSMapsCE settings. The file is rewritten by the program.\r\n"
        "lon=%.6f\r\n"
        "lat=%.6f\r\n"
        "zoom=%d\r\n"
        "traffic=%d\r\n"
        "; yandex | 2gis\r\n"
        "traffic_src=%s\r\n"
        "; GPS mode: 0 off, 1 north-up, 2 heading-up\r\n"
        "follow=%d\r\n"
        "; dark map (button with the moon)\r\n"
        "night=%d\r\n"
        "; minutes a traffic overlay stays fresh (auto refresh period)\r\n"
        "traffic_ttl=%d\r\n"
        "; max traffic requests per day per provider\r\n"
        "yandex_limit=%d\r\n"
        "2gis_limit=%d\r\n"
        "; automatic requests stop this many requests before the limit\r\n"
        "auto_reserve=%d\r\n"
        "; Cache folder size limit, MB (250, 500, 1000, 2000 in settings)\r\n"
        "cache_mb=%d\r\n"
        "; Cache folder size now, KB (-1 = recount)\r\n"
        "cache_kb=%ld\r\n"
        "; map tiles older than this are checked with the server again, days (0 = never)\r\n"
        "cache_days=%d\r\n"
        "; request counters (date from GPS)\r\n"
        "req_day=%d\r\n"
        "yandex_count=%d\r\n"
        "2gis_count=%d\r\n"
        "osm_count=%d\r\n"
        "gps=%d\r\n"
        "gps_port=%s\r\n"
        "gps_baud=%d\r\n"
        "; program to close when it holds the GPS port (empty = never)\r\n"
        "kill=%s\r\n"
        "hide_taskbar=%d\r\n"
        "map_x=%d\r\n"
        "map_y=%d\r\n"
        "map_w=%d\r\n"
        "map_h=%d\r\n",
        g_cfg.lon, g_cfg.lat, g_cfg.z, g_cfg.traffic, kTrafficNames[g_cfg.trafficSrc],
        g_cfg.follow, g_cfg.night, g_cfg.trafficTtlMin, g_cfg.yandexLimit, g_cfg.twogisLimit,
        g_cfg.autoReserve, g_cfg.cacheMb, g_cfg.cacheKb, g_cfg.cacheDays, g_cfg.reqDay,
        g_cfg.yandexCount, g_cfg.twogisCount, g_cfg.osmCount,
        g_cfg.gpsEnabled, port, g_cfg.gpsBaud, kill, g_cfg.hideTaskbar,
        g_cfg.mapX, g_cfg.mapY, g_cfg.mapW, g_cfg.mapH);
    if (n < 0)
        return;

    wchar_t path[MAX_PATH];
    PathInDir(path, kConfigName);
    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Log("config: cannot write (%lu)", GetLastError());
        return;
    }
    DWORD w;
    WriteFile(h, buf, n, &w, NULL);
    CloseHandle(h);
}

// ---------------------------------------------------------------- projection
// OSM and 2GIS: spherical Mercator (EPSG:3857). Yandex: elliptical Mercator (EPSG:3395).
// Both with 256 px tiles; x is the same, y differs.

void GeoToWorld(double lon, double lat, int z, double* x, double* y)
{
    double size = 256.0 * pow(2.0, z);
    if (lat > 85.05)  lat = 85.05;
    if (lat < -85.05) lat = -85.05;
    double phi = lat * M_PI / 180.0;
    *x = (lon + 180.0) / 360.0 * size;
    *y = (0.5 - log(tan(M_PI / 4 + phi / 2)) / (2 * M_PI)) * size;
}

void WorldToGeo(double x, double y, int z, double* lon, double* lat)
{
    double size = 256.0 * pow(2.0, z);
    double ln = x / size * 360.0 - 180.0;
    while (ln < -180.0) ln += 360.0;
    while (ln >= 180.0) ln -= 360.0;
    *lon = ln;
    *lat = (M_PI / 2 - 2 * atan(exp(-(0.5 - y / size) * 2 * M_PI))) * 180.0 / M_PI;
}

static const double kEcc = 0.0818191908426;

void GeoToWorldYa(double lon, double lat, int z, double* x, double* y)
{
    double size = 256.0 * pow(2.0, z);
    if (lat > 85.0)  lat = 85.0;
    if (lat < -85.0) lat = -85.0;
    double phi = lat * M_PI / 180.0;
    double es = kEcc * sin(phi);
    double m = log(tan(M_PI / 4 + phi / 2) * pow((1 - es) / (1 + es), kEcc / 2));
    *x = (lon + 180.0) / 360.0 * size;
    *y = (0.5 - m / (2 * M_PI)) * size;
}

void WorldToGeoYa(double x, double y, int z, double* lon, double* lat)
{
    double size = 256.0 * pow(2.0, z);
    double ln = x / size * 360.0 - 180.0;
    while (ln < -180.0) ln += 360.0;
    while (ln >= 180.0) ln -= 360.0;
    double t = exp(-(0.5 - y / size) * 2 * M_PI);
    double phi = M_PI / 2 - 2 * atan(t);
    for (int i = 0; i < 6; i++) {
        double es = kEcc * sin(phi);
        phi = M_PI / 2 - 2 * atan(t * pow((1 - es) / (1 + es), kEcc / 2));
    }
    *lon = ln;
    *lat = phi * 180.0 / M_PI;
}

// ---------------------------------------------------------------- dates
// The unit's clock is unreliable, so cache files get the GPS date as their write time
// (StampFile) and their age is counted from it.

static bool DayToFileTime(int yyyymmdd, FILETIME* ft)
{
    SYSTEMTIME t;
    memset(&t, 0, sizeof(t));
    t.wYear = (WORD)(yyyymmdd / 10000);
    t.wMonth = (WORD)(yyyymmdd / 100 % 100);
    t.wDay = (WORD)(yyyymmdd % 100);
    t.wHour = 12;
    return SystemTimeToFileTime(&t, ft) != 0;
}

void StampFile(HANDLE h, int yyyymmdd)
{
    FILETIME ft;
    if (yyyymmdd && DayToFileTime(yyyymmdd, &ft))
        SetFileTime(h, NULL, NULL, &ft);
}

int FileDay(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    SYSTEMTIME t;
    if (!GetFileAttributesEx(path, GetFileExInfoStandard, &fa) || !FileTimeToSystemTime(&fa.ftLastWriteTime, &t))
        return 0;
    return t.wYear * 10000 + t.wMonth * 100 + t.wDay;
}

int DaysBetween(int from, int to)
{
    FILETIME a, b;
    if (!DayToFileTime(from, &a) || !DayToFileTime(to, &b))
        return 0;
    ULARGE_INTEGER ua, ub;
    ua.LowPart = a.dwLowDateTime;
    ua.HighPart = a.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime;
    ub.HighPart = b.dwHighDateTime;
    return (int)(((LONGLONG)ub.QuadPart - (LONGLONG)ua.QuadPart) / 864000000000LL);
}

// ---------------------------------------------------------------- processes
// toolhelp is loaded dynamically: without toolhelp.dll in the image the program still starts.

typedef HANDLE (WINAPI *SnapshotFn)(DWORD, DWORD);
typedef BOOL   (WINAPI *ProcessFn)(HANDLE, PROCESSENTRY32*);
typedef BOOL   (WINAPI *CloseFn)(HANDLE);

bool KillProcess(const wchar_t* exeName)
{
#ifdef UNDER_CE
    wchar_t local[MAX_PATH];
    PathInDir(local, L"toolhelp.dll");
    HMODULE lib = LoadLibrary(local);
    if (!lib)
        lib = LoadLibrary(L"toolhelp.dll");
    if (!lib) {
        Log("kill: no toolhelp.dll");
        return false;
    }
    SnapshotFn snapshot = (SnapshotFn)GetProcAddress(lib, L"CreateToolhelp32Snapshot");
    ProcessFn  first    = (ProcessFn)GetProcAddress(lib, L"Process32First");
    ProcessFn  next     = (ProcessFn)GetProcAddress(lib, L"Process32Next");
    CloseFn    close    = (CloseFn)GetProcAddress(lib, L"CloseToolhelp32Snapshot");
#else
    HMODULE lib = LoadLibrary(L"kernel32.dll");
    SnapshotFn snapshot = (SnapshotFn)GetProcAddress(lib, "CreateToolhelp32Snapshot");
    ProcessFn  first    = (ProcessFn)GetProcAddress(lib, "Process32FirstW");
    ProcessFn  next     = (ProcessFn)GetProcAddress(lib, "Process32NextW");
    CloseFn    close    = CloseHandle;
#endif
    bool killed = false;
    if (snapshot && first && next && close) {
        HANDLE snap = snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 pe;
            memset(&pe, 0, sizeof(pe));
            pe.dwSize = sizeof(pe);
            for (BOOL ok = first(snap, &pe); ok; ok = next(snap, &pe)) {
                if (_wcsicmp(pe.szExeFile, exeName) != 0)
                    continue;
                HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (proc) {
                    killed = TerminateProcess(proc, 0) != 0;
                    CloseHandle(proc);
                }
                Log("kill: %S %s", exeName, killed ? "closed" : "failed");
            }
            close(snap);
        }
    }
    FreeLibrary(lib);
    return killed;
}
