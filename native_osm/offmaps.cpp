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

static bool WriteAll(const wchar_t* path, const char* data, DWORD n)
{
    HANDLE f = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, n, &put, NULL) && put == n;
    CloseHandle(f);
    return ok;
}

static bool ZipFile(HANDLE f, const wchar_t* dir, const char* name)   // one entry into dir\name
{
    ZipEntry e;
    char* data = ZipFind(f, name, &e) ? ZipData(f, e) : NULL;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\%S", dir, name);
    path[MAX_PATH - 1] = 0;
    bool ok = data && WriteAll(path, data, e.size);
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
