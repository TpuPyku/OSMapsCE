#include "yamaps.h"
#include "stb_image.h"   // (its zlib: the zips' deflated files; the implementation is image.cpp's)

// The regions' maps downloaded on the unit (the user 2026-10-29: "пусть хотя бы графы будут", from OSMapsCE itself,
// not only the PC app): the release "maps"'s catalog, the regions the card has (their region.txt's dates), a
// region's zip unpacked (route.grf, search.idx, region.txt last - a region half written never looks whole), URLs.

static void Copy(char* out, int cap, const char* p)   // a JSON string into out ("" when not one)
{
    char s[256];
    JStr(p, s, sizeof(s));
    strncpy(out, s, cap - 1);
    out[cap - 1] = 0;
}

// catalog.json's regions into out (at most cap); their count, 0 from a bad one
int MapsCatalog(const char* json, MapRegion* out, int cap)
{
    int n = 0;
    for (const char* r = JFirst(JKey(json, "regions")); r && n < cap; r = JNext(r)) {
        MapRegion& m = out[n];
        memset(&m, 0, sizeof(m));
        Copy(m.name, sizeof(m.name), JKey(r, "name"));
        Copy(m.title, sizeof(m.title), JKey(r, "title"));
        Copy(m.date, sizeof(m.date), JKey(r, "date"));
        Copy(m.zip, sizeof(m.zip), JKey(r, "zip"));
        m.sizeKb = (int)(JNum(JKey(r, "size"), 0) / 1024);
        m.fullKb = (int)(JNum(JKey(JKey(r, "full"), "size"), 0) / 1024);
        if (m.name[0] && m.zip[0])
            n++;
    }
    return n;
}

static bool RegionDate(const wchar_t* path, char* date, int cap)   // region.txt's "date=" line
{
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    char buf[512];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    const char* d = strstr(buf, "date=");
    if (!d)
        return false;
    int k = 0;
    for (d += 5; *d && *d != '\r' && *d != '\n' && k < cap - 1; d++)
        date[k++] = *d;
    date[k] = 0;
    return true;
}

struct HaveCtx { const char* name; char* date; };

static bool HaveName(const wchar_t* path, void* ctx)   // OffRegion's test: the region of that name, its date
{
    const HaveCtx* h = (const HaveCtx*)ctx;
    wchar_t want[64];
    MultiByteToWideChar(CP_ACP, 0, h->name, -1, want, 64);
    const wchar_t* dir = wcsrchr(path, L'\\');   // ...\<name>\region.txt
    int n = (int)wcslen(want);
    return dir && dir - path > n && !_wcsnicmp(dir - n, want, n) && dir[-n - 1] == L'\\' &&
           RegionDate(path, h->date, 12);
}

void MapsHave(MapRegion* r, int n)   // each one's region.txt date on the card (any data folder), "" none
{
    for (int i = 0; i < n; i++) {
        HaveCtx h = { r[i].name, r[i].have };
        wchar_t dir[MAX_PATH];
        r[i].have[0] = 0;
        OffRegion(L"region.txt", HaveName, &h, dir);
    }
}

int MapState(const MapRegion& r)
{
    return !r.have[0] ? MAP_NEW : strcmp(r.have, r.date) ? MAP_UPDATE : MAP_OK;
}

// "https://host/path?..." into host and path (in url); false - not http(s), no path
bool UrlSplit(const char* url, char* host, int cap, bool* https, const char** path)
{
    int skip = !strncmp(url, "https://", 8) ? 8 : !strncmp(url, "http://", 7) ? 7 : 0;
    const char* slash = skip ? strchr(url + skip, '/') : NULL;
    if (!slash || slash - url - skip >= cap)
        return false;
    memcpy(host, url + skip, slash - url - skip);
    host[slash - url - skip] = 0;
    *https = skip == 8;
    *path = slash;
    return true;
}

// ---------------------------------------------------------------- the zip

static DWORD U32(const BYTE* p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24;
}

static WORD U16(const BYTE* p)
{
    return (WORD)(p[0] | p[1] << 8);
}

struct ZipEntry { DWORD method, packed, size, local; };

// The central directory: where it is and how long (the end record: in the last 64 KB + 22 bytes, no zip64)
static bool ZipDir(HANDLE f, DWORD* off, DWORD* size)
{
    static BYTE tail[65536 + 22];
    DWORD fsize = GetFileSize(f, NULL), n = fsize < sizeof(tail) ? fsize : sizeof(tail);
    if (!OffRead(f, fsize - n, tail, n))
        return false;
    int end = (int)n - 22;
    while (end >= 0 && U32(tail + end) != 0x06054b50)
        end--;
    if (end < 0)
        return false;
    *size = U32(tail + end + 12);
    *off = U32(tail + end + 16);
    return true;
}

// The central directory's entry of that name
static bool ZipFind(HANDLE f, const char* name, ZipEntry* e)
{
    DWORD off, size;
    BYTE* cd = ZipDir(f, &off, &size) ? (BYTE*)malloc(size + 1) : NULL;
    bool found = false;
    if (cd && OffRead(f, off, cd, size))
        for (DWORD p = 0; p + 46 <= size && U32(cd + p) == 0x02014b50 && !found;
             p += 46 + U16(cd + p + 28) + U16(cd + p + 30) + U16(cd + p + 32)) {
            WORD nl = U16(cd + p + 28);
            found = nl == strlen(name) && !memcmp(cd + p + 46, name, nl);
            e->method = U16(cd + p + 10);
            e->packed = U32(cd + p + 20);
            e->size = U32(cd + p + 24);
            e->local = U32(cd + p + 42);
        }
    free(cd);
    return found;
}

static char* ZipPacked(HANDLE f, const ZipEntry& e)   // the entry's bytes as stored (malloc; NULL - cannot)
{
    BYTE lh[30];
    char* packed = OffRead(f, e.local, lh, 30) && U32(lh) == 0x04034b50 ? (char*)malloc(e.packed + 1) : NULL;
    if (packed && !OffRead(f, e.local + 30 + U16(lh + 26) + U16(lh + 28), packed, e.packed)) {
        free(packed);
        packed = NULL;
    }
    return packed;
}

static char* ZipData(HANDLE f, const ZipEntry& e)   // the entry's bytes, unpacked (stored or deflated; malloc)
{
    char* packed = e.method == 0 || e.method == 8 ? ZipPacked(f, e) : NULL;
    if (!packed || e.method == 0)
        return packed;
    int out = 0;
    char* data = stbi_zlib_decode_malloc_guesssize_headerflag(packed, (int)e.packed, (int)e.size + 1, &out, 0);
    free(packed);
    if (data && (DWORD)out != e.size) {
        free(data);
        data = NULL;
    }
    return data;
}

static bool Put(const wchar_t* path, const void* data, DWORD n, bool append)   // a new file, or onto its end
{
    HANDLE f = CreateFile(path, GENERIC_WRITE, 0, NULL, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    SetFilePointer(f, 0, NULL, FILE_END);
    bool ok = WriteFile(f, data, n, &put, NULL) && put == n;
    CloseHandle(f);
    return ok;
}

// A stored entry into path piece by piece (the region whole: tiles.vtp's hundreds of MB - no memory for them whole)
static bool CopyStored(HANDLE f, const ZipEntry& e, const wchar_t* path)
{
    enum { kPiece = 64 * 1024 };
    BYTE lh[30];
    if (!OffRead(f, e.local, lh, 30) || U32(lh) != 0x04034b50)
        return false;
    DWORD off = e.local + 30 + U16(lh + 26) + U16(lh + 28), left = e.size;
    char* buf = (char*)malloc(kPiece);
    bool ok = buf && Put(path, "", 0, false);
    while (ok && left) {
        DWORD n = left < kPiece ? left : kPiece;
        ok = OffRead(f, off, buf, n) && Put(path, buf, n, true);
        off += n;
        left -= n;
    }
    free(buf);
    return ok;
}

static bool ZipFile(HANDLE f, const wchar_t* dir, const char* name)   // one entry into dir\name
{
    ZipEntry e;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\%S", dir, name);
    path[MAX_PATH - 1] = 0;
    if (!ZipFind(f, name, &e))
        return false;
    if (e.method == 0)
        return CopyStored(f, e, path);
    char* data = ZipData(f, e);
    bool ok = data && Put(path, data, e.size, false);
    free(data);
    return ok;
}

// The zip's files (in that order) into dir (made when missing); false - the zip, a file not there, the card full
bool ZipExtract(const wchar_t* zip, const wchar_t* dir, const char* const* files, int n)
{
    HANDLE f = CreateFile(zip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    CreateDirectory(dir, NULL);
    bool ok = true;
    for (int i = 0; i < n && ok; i++)
        ok = ZipFile(f, dir, files[i]);
    CloseHandle(f);
    return ok;
}

// ---------------------------------------------------------------- the download

// In ranges of kChunk (HttpGet keeps a body in memory, at most 2 MB) appended to a .part file: a lost link resumes
// where it stopped (the car's Wi-Fi, the modem); each range asked through github.com again - its storage's signed
// URL lives minutes, a region's zip may take longer
enum { kChunk = 1024 * 1024, kFails = 3 };

static int RealGet(const char* host, bool https, const char* path, unsigned char** body, int* len, int* status,
                   const HttpExtra* x)
{
    return HttpGet(host, https, path, body, len, status, NULL, NULL, 0, x);
}

static MapsGetFn     s_get = RealGet;
static int           s_retryMs = 3000;
static volatile LONG s_got;   // the bytes of the file being downloaded (for the screen)
static volatile LONG s_cancel;   // "ОТМЕНА" pressed: the download stops before its next piece

void MapsSetGet(MapsGetFn fn, int retryMs)   // tests: a made-up server; NULL - the real one
{
    s_get = fn ? fn : RealGet;
    s_retryMs = fn ? retryMs : 3000;
}

// GET url following the redirects (GitHub's release files: github.com -> its storage); range "a-b" or NULL
static int GetUrl(const char* url, const char* range, unsigned char** body, int* len, int* status)
{
    char cur[2048], loc[2048], hdr[64];
    _snprintf(hdr, sizeof(hdr), "Range: bytes=%s\r\n", range ? range : "");
    strncpy(cur, url, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;
    for (int hop = 0; hop < 4; hop++) {
        char host[128];
        bool https;
        const char* path;
        if (!UrlSplit(cur, host, sizeof(host), &https, &path))
            return NET_HTTP;
        HttpExtra x = { range ? hdr : NULL, loc, sizeof(loc) };
        loc[0] = 0;
        *body = NULL;
        *len = 0;
        int err = s_get(host, https, path, body, len, status, &x);
        if (err != NET_OK || *status < 300 || *status >= 400 || !loc[0])
            return err;
        free(*body);
        *body = NULL;
        strcpy(cur, loc);
    }
    return NET_HTTP;
}

static DWORD SizeOf(const wchar_t* path)   // a file's size, 0 none
{
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    DWORD n = GetFileSize(f, NULL);
    CloseHandle(f);
    return n;
}

// The next range onto the file: 1 a piece, 0 the end (past it: 416), -1 failed
static int Piece(const char* url, const wchar_t* file, DWORD have, int chunk, int* len)
{
    char range[48];
    _snprintf(range, sizeof(range), "%lu-%lu", have, have + chunk - 1);
    unsigned char* body = NULL;
    int status = 0;
    int err = GetUrl(url, range, &body, len, &status);
    int r = err == NET_OK && status == 416 ? 0 :
            err == NET_OK && status == 206 && *len > 0 && Put(file, body, *len, true) ? 1 : -1;
    free(body);
    return r;
}

// url into file (a .part), from what it has on, in ranges of chunk; false - the link lost kFails times running
bool MapsFetch(const char* url, const wchar_t* file, int chunk)
{
    DWORD have = SizeOf(file);
    for (int fails = 0; fails <= kFails && !s_cancel;) {
        int len = 0, r = Piece(url, file, have, chunk, &len);
        if (r == 0)
            return have > 0;
        if (r < 0) {
            Sleep(s_retryMs * ++fails);
            continue;
        }
        have += len;
        InterlockedExchange((LONG*)&s_got, (LONG)have);
        fails = 0;
        if (len < chunk)
            return true;
    }
    return false;
}

// A zip (url) into data\<name>\ - kind MAPS_*: a region's graph and search, the trunk roads, a region whole (its
// tiles too), the overview - through data\<name>.zip, removed after; region.txt last
bool MapsInstall(const char* url, const wchar_t* data, const char* name, int kind)
{
    static const char* const kFiles[4][4] = { { "route.grf", "search.idx", "region.txt" }, { "trunk.grf", "region.txt" },
                                              { "route.grf", "search.idx", "tiles.vtp", "region.txt" },
                                              { "overview.vtp", "region.txt" } };
    static const int kCount[4] = { 3, 2, 4, 2 };
    wchar_t zip[MAX_PATH], part[MAX_PATH], dir[MAX_PATH];
    CreateDirectory(data, NULL);
    _snwprintf(zip, MAX_PATH, L"%s\\%S.zip", data, name);
    _snwprintf(part, MAX_PATH, L"%s\\%S.zip.part", data, name);
    _snwprintf(dir, MAX_PATH, L"%s\\%S", data, name);
    zip[MAX_PATH - 1] = part[MAX_PATH - 1] = dir[MAX_PATH - 1] = 0;
    InterlockedExchange((LONG*)&s_got, (LONG)SizeOf(part));
    DeleteFile(zip);
    bool ok = MapsFetch(url, part, kChunk) && !s_cancel && MoveFile(part, zip);
    if (ok) {   // (a route, the search let go of the files written over - and wait meanwhile: CE, Windows refuse them)
        OffUseScope use;
        OfflineDrop();
        ok = ZipExtract(zip, dir, kFiles[kind], kCount[kind]);
    }
    if (ok)
        DeleteFile(zip);
    return ok;
}

// ---------------------------------------------------------------- the job: one at a time, on its own thread

// The settings' "Карты" screen asks: the catalog (MapsList), a region (MapsGetRegion) - the trunk roads after it when
// the card has none. The regions go where OffWriteDir says (the volume with \OSMapsData already, else the
// removable one with the most room, else the program's folder).
static const char kAssets[] = "https://github.com/TpuPyku/OSMapsCE/releases/download/maps/";
enum { kMaxRegions = 200 };

static MapRegion        s_reg[kMaxRegions];
static int              s_nReg;
static CRITICAL_SECTION s_cs;
static volatile LONG    s_csDone, s_busy;
static volatile LONG    s_busyRegion = -1;   // the region GetThread downloads
static wchar_t          s_text[128];   // what goes on / the last result
static int              s_totalKb;     // the file being downloaded
static char             s_disk[256];   // the catalog's Yandex Disk folder (public link): the regions whole; "" none
static int              s_ovKb;        // ... its overview's size

static void Lock()
{
    if (!InterlockedExchange((LONG*)&s_csDone, 1))
        InitializeCriticalSection(&s_cs);
    EnterCriticalSection(&s_cs);
}

static void Say(const wchar_t* fmt, const char* utf8)   // the job's line (a region's title in it)
{
    wchar_t w[96];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, w, 96))
        w[0] = 0;
    w[95] = 0;
    Lock();
    _snwprintf(s_text, 128, fmt, w);
    s_text[127] = 0;
    LeaveCriticalSection(&s_cs);
}

static bool Exists(const wchar_t* path, void*)   // OffRegion's test: the file is there
{
    return GetFileAttributes(path) != 0xFFFFFFFF;
}

static int ByTitle(const void* a, const void* b)   // (UTF-8's bytes: the Cyrillic alphabet's order, ё aside)
{
    return strcmp(((const MapRegion*)a)->title, ((const MapRegion*)b)->title);
}

static DWORD WINAPI ListThread(LPVOID)
{
    char url[256];
    _snprintf(url, sizeof(url), "%scatalog.json", kAssets);
    unsigned char* body = NULL;
    int len = 0, status = 0;
    int err = GetUrl(url, NULL, &body, &len, &status);
    bool ok = err == NET_OK && status == 200 && body;
    static MapRegion got[kMaxRegions];
    int n = ok ? MapsCatalog((const char*)body, got, kMaxRegions) : 0;
    char disk[256] = "";
    if (n)
        Copy(disk, sizeof(disk), JKey((const char*)body, "disk"));
    int ovKb = n ? (int)(JNum(JKey(JKey((const char*)body, "overview"), "size"), 0) / 1024) : 0;
    free(body);
    qsort(got, n, sizeof(MapRegion), ByTitle);
    MapsHave(got, n);
    Lock();
    memcpy(s_reg, got, n * sizeof(MapRegion));
    s_nReg = n;
    strcpy(s_disk, disk);
    s_ovKb = ovKb;
    _snwprintf(s_text, 128, n ? L"Областей: %d" : L"Каталог не получен (нет связи с GitHub?)", n);
    LeaveCriticalSection(&s_cs);
    Log("maps: catalog %d regions (HTTP %d, error %d)", n, status, err);
    InterlockedExchange((LONG*)&s_busy, 0);
    return 0;
}

static bool TrunkThere()
{
    wchar_t dir[MAX_PATH];
    return OffRegion(L"trunk.grf", Exists, NULL, dir);
}

// A public file of the catalog's Yandex Disk folder: its download link, asked of the disk's API (no token); false -
// none (no link)
static bool DiskHref(const char* name, char* href, int cap)
{
    char key[512], path[96], file[128], url[1024];
    UrlEncode(s_disk, key, sizeof(key));
    _snprintf(file, sizeof(file), "/%s", name);
    file[sizeof(file) - 1] = 0;
    UrlEncode(file, path, sizeof(path));
    _snprintf(url, sizeof(url), "https://cloud-api.yandex.net/v1/disk/public/resources/download?public_key=%s&path=%s",
              key, path);
    url[sizeof(url) - 1] = 0;
    unsigned char* body = NULL;
    int len = 0, status = 0;
    bool ok = GetUrl(url, NULL, &body, &len, &status) == NET_OK && status == 200 && body;
    href[0] = 0;
    if (ok)
        JStr(JKey((const char*)body, "href"), href, cap);
    free(body);
    return href[0] != 0;
}

// The region: whole from the disk when the catalog has it there (the user 2026-10-30: GitHub's downloads slow), else
// its graph and search from the release
static bool GetRegion(const MapRegion& r, const wchar_t* data)
{
    char url[2048];
    if (s_disk[0] && r.fullKb) {
        s_totalKb = r.fullKb;
        return DiskHref(r.zip, url, sizeof(url)) && MapsInstall(url, data, r.name, MAPS_FULL);
    }
    _snprintf(url, sizeof(url), "%s%s", kAssets, r.zip);
    return MapsInstall(url, data, r.name, MAPS_GRAPH);
}

// After a region: the overview (the disk's; none where the region went) and the trunk roads (~10 MB, none on a card)
static void GetExtras(const wchar_t* data)
{
    char url[2048];
    wchar_t ov[MAX_PATH];
    _snwprintf(ov, MAX_PATH, L"%s\\overview\\overview.vtp", data);
    ov[MAX_PATH - 1] = 0;
    if (s_disk[0] && s_ovKb && !Exists(ov, NULL) && !s_cancel) {
        Say(L"Обзорная карта СНГ…", NULL);
        s_totalKb = s_ovKb;
        if (DiskHref("overview.zip", url, sizeof(url)))
            MapsInstall(url, data, "overview", MAPS_OVERVIEW);
    }
    if (!TrunkThere() && !s_cancel) {
        Say(L"Магистрали России и СНГ…", NULL);
        _snprintf(url, sizeof(url), "%strunk.zip", kAssets);
        s_totalKb = 10 * 1024;
        MapsInstall(url, data, "trunk", MAPS_TRUNK);
    }
}

static DWORD WINAPI GetThread(LPVOID p)
{
    MapRegion r;
    Lock();
    r = s_reg[(int)p];
    s_totalKb = r.sizeKb;
    LeaveCriticalSection(&s_cs);
    wchar_t data[MAX_PATH];
    if (s_get == RealGet)
        OffWriteDir(data);
    else
        PathInDir(data, L"OSMapsData");   // (the tests' made-up server: never onto a disk of the PC's)
    bool ok = GetRegion(r, data);
    if (ok)
        GetExtras(data);
    Lock();
    MapsHave(s_reg, s_nReg);
    LeaveCriticalSection(&s_cs);
    Say(ok ? L"«%s» записана" : s_cancel ? L"«%s»: загрузка остановлена" : L"«%s» не скачана: нет связи или нет места",
        r.title);
    Log("maps: %s %s", r.name, ok ? "downloaded" : s_cancel ? "cancelled" : "failed");
    InterlockedExchange((LONG*)&s_busyRegion, -1);
    InterlockedExchange((LONG*)&s_busy, 0);
    return 0;
}

static bool Start(LPTHREAD_START_ROUTINE fn, LPVOID arg)
{
    if (InterlockedExchange((LONG*)&s_busy, 1))
        return false;
    InterlockedExchange((LONG*)&s_cancel, 0);
    HANDLE t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (!t) {
        InterlockedExchange((LONG*)&s_busy, 0);
        return false;
    }
    CloseHandle(t);
    return true;
}

bool MapsList()   // the catalog, on its thread; false - busy
{
    Say(L"Каталог областей…", NULL);
    return Start(ListThread, NULL);
}

bool MapsGetRegion(int i)   // region i of the catalog, on its thread; false - none, busy
{
    Lock();
    bool there = i >= 0 && i < s_nReg;
    LeaveCriticalSection(&s_cs);
    if (!there || s_busy)
        return false;
    InterlockedExchange((LONG*)&s_busyRegion, i);   // (before the thread: it may be done at once)
    if (!Start(GetThread, (LPVOID)i)) {
        InterlockedExchange((LONG*)&s_busyRegion, -1);
        return false;
    }
    InterlockedExchange((LONG*)&s_got, 0);
    Say(L"«%s»…", s_reg[i].title);
    return true;
}

int MapsBusyRegion()
{
    return s_busyRegion;
}

void MapsCancel()
{
    InterlockedExchange((LONG*)&s_cancel, 1);
}

int MapsCount()
{
    return s_nReg;
}

bool MapsAt(int i, MapRegion* out)
{
    Lock();
    bool ok = i >= 0 && i < s_nReg;
    if (ok)
        *out = s_reg[i];
    LeaveCriticalSection(&s_cs);
    return ok;
}

// MAPS_BUSY / MAPS_IDLE, its line, the file's KB so far and in all
int MapsJob(wchar_t* text, int cap, int* gotKb, int* totalKb)
{
    Lock();
    wcsncpy(text, s_text, cap - 1);
    text[cap - 1] = 0;
    *totalKb = s_totalKb;
    LeaveCriticalSection(&s_cs);
    *gotKb = (int)(s_got / 1024);
    return s_busy ? MAPS_BUSY : MAPS_IDLE;
}
