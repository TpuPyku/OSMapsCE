#include "yamaps.h"

// Two-level cache:
//  - up to CACHE_MEM decoded images in memory (least recently used is evicted):
//    OSM tiles and traffic overlays;
//  - OSM tiles on disk in <exe>\Cache\<z>\<bx>_<by>\<x>_<y>.img (see CachePath), kept across
//    reboots. Traffic is never stored: it is stale in minutes.
// The disk size is kept in g_cfg.cacheKb (saved in the ini), so the card is scanned only
// when the limit is exceeded or the size is unknown.

static CacheEntry s_mem[CACHE_MEM];
static int        s_count;

bool SameKey(const MapRequest& a, const MapRequest& b)
{
    return a.src == b.src && a.z == b.z && a.x == b.x && a.y == b.y;
}

static LONG FileKb(DWORD bytes)
{
    return (LONG)((bytes + 1023) / 1024);
}

// Called by the download thread after a file was written.
void CacheCountSaved(int newBytes, int oldBytes)
{
    if (g_cfg.cacheKb >= 0)
        InterlockedExchangeAdd((LONG*)&g_cfg.cacheKb, FileKb(newBytes) - FileKb(oldBytes));
}

// ---------------------------------------------------------------- disk scan & prune

struct FileRec {
    MapRequest req;
    LONG       kb;
    FILETIME   time;
};

static FileRec* s_recs;
static int      s_recCount, s_recCap;

// "<x>_<y>.img"
static bool ParseName(const wchar_t* s, int* x, int* y)
{
    wchar_t* end;
    *x = (int)wcstol(s, &end, 10);
    if (end == s || *end != L'_')
        return false;
    s = end + 1;
    *y = (int)wcstol(s, &end, 10);
    return end != s && !wcscmp(end, L".img");
}

static void AddRec(int z, const WIN32_FIND_DATA& fd)
{
    int x, y;
    if (!ParseName(fd.cFileName, &x, &y))
        return;
    if (s_recCount == s_recCap) {
        int cap = s_recCap ? s_recCap * 2 : 1024;
        FileRec* r = (FileRec*)realloc(s_recs, cap * sizeof(FileRec));
        if (!r)
            return;
        s_recs = r;
        s_recCap = cap;
    }
    FileRec& r = s_recs[s_recCount++];
    r.req.src = SRC_OSM;
    r.req.z = z;
    r.req.x = x;
    r.req.y = y;
    r.kb = FileKb(fd.nFileSizeLow);
    r.time = fd.ftLastWriteTime;
}

// The oldest first. The unit's clock is unreliable, so the order is approximate.
static int CompareRec(const void* a, const void* b)
{
    return CompareFileTime(&((const FileRec*)a)->time, &((const FileRec*)b)->time);
}

static bool IsSubdir(const WIN32_FIND_DATA& fd)
{
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.';
}

static void Scan()
{
    wchar_t name[MAX_PATH], mask[MAX_PATH];
    WIN32_FIND_DATA zd, bd, fd;
    PathInDir(mask, L"Cache\\*");
    HANDLE hz = FindFirstFile(mask, &zd);
    if (hz == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!IsSubdir(zd))
            continue;
        int z = (int)wcstol(zd.cFileName, NULL, 10);
        _snwprintf(name, MAX_PATH, L"Cache\\%s\\*", zd.cFileName);
        name[MAX_PATH - 1] = 0;
        PathInDir(mask, name);
        HANDLE hb = FindFirstFile(mask, &bd);
        if (hb == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!IsSubdir(bd))
                continue;
            _snwprintf(name, MAX_PATH, L"Cache\\%s\\%s\\*.img", zd.cFileName, bd.cFileName);
            name[MAX_PATH - 1] = 0;
            PathInDir(mask, name);
            HANDLE hf = FindFirstFile(mask, &fd);
            if (hf == INVALID_HANDLE_VALUE)
                continue;
            do AddRec(z, fd); while (FindNextFile(hf, &fd));
            FindClose(hf);
        } while (FindNextFile(hb, &bd));
        FindClose(hb);
    } while (FindNextFile(hz, &zd));
    FindClose(hz);
}

void CachePrune()
{
    DWORD t0 = GetTickCount();
    s_recCount = 0;
    Scan();
    LONG total = 0;
    for (int i = 0; i < s_recCount; i++)
        total += s_recs[i].kb;

    LONG limit = (LONG)g_cfg.cacheMb * 1024;
    int deleted = 0;
    if (total > limit) {
        qsort(s_recs, s_recCount, sizeof(FileRec), CompareRec);
        LONG target = limit / 10 * 9;
        for (int i = 0; i < s_recCount && total > target; i++) {
            wchar_t path[MAX_PATH];
            CachePath(path, s_recs[i].req);
            if (DeleteFile(path)) {
                total -= s_recs[i].kb;
                deleted++;
            }
        }
    }
    g_cfg.cacheKb = total;
    Log("cache: %d files, %ld MB of %d MB, %d deleted, %lu ms",
        s_recCount - deleted, total / 1024, g_cfg.cacheMb, deleted, GetTickCount() - t0);
    free(s_recs);
    s_recs = NULL;
    s_recCap = 0;
    ConfigSave();
}

void CacheInit()
{
    wchar_t dir[MAX_PATH];
    PathInDir(dir, L"Cache");
    CreateDirectory(dir, NULL);
    if (g_cfg.cacheKb < 0 || g_cfg.cacheKb > (LONG)g_cfg.cacheMb * 1024)
        CachePrune();
}

// ---------------------------------------------------------------- memory

static int s_bytes;   // decoded images in s_mem, held under CACHE_BYTES

static int EntryBytes(const CacheEntry* e)
{
    return e->img.w * e->img.h * 2 + (e->ov ? e->ov->bytes : 0);
}

static void FreeEntry(CacheEntry* e)
{
    s_bytes -= EntryBytes(e);
    ImageFree(&e->img);
    free(e->ov);
    e->ov = NULL;
}

// Drops the least recently used entries (never keep) while over the memory cap.
// The last entry moves into a freed slot; returns where keep is now.
static CacheEntry* Trim(CacheEntry* keep)
{
    while (s_bytes > CACHE_BYTES && s_count > 1) {
        CacheEntry* e = NULL;
        for (int i = 0; i < s_count; i++)
            if (&s_mem[i] != keep && (!e || s_mem[i].useTick < e->useTick))
                e = &s_mem[i];
        FreeEntry(e);
        CacheEntry* last = &s_mem[--s_count];
        if (e != last) {
            *e = *last;
            if (keep == last)
                keep = e;
        }
        memset(last, 0, sizeof(*last));   // its images now belong to e: never free them twice
    }
    return keep;
}

static CacheEntry* Slot(const MapRequest& r)
{
    CacheEntry* e = NULL;
    for (int i = 0; i < s_count && !e; i++)
        if (SameKey(s_mem[i].req, r))
            e = &s_mem[i];
    if (!e && s_count < CACHE_MEM)
        e = &s_mem[s_count++];
    if (!e) {
        e = &s_mem[0];
        for (int i = 1; i < s_count; i++)
            if (s_mem[i].useTick < e->useTick)
                e = &s_mem[i];
    }
    FreeEntry(e);
    e->req = r;
    e->useTick = GetTickCount();
    return e;
}

CacheEntry* CacheAdd(MapResult* res, DWORD fetchTick)
{
    CacheEntry* e = Slot(res->req);
    e->img = res->img;
    e->ov = res->ov;
    e->fetchTick = fetchTick;
    e->fileDay = 0;
    res->img.bmp = NULL;
    res->ov = NULL;
    s_bytes += EntryBytes(e);
    return Trim(e);
}

CacheEntry* CacheFind(const MapRequest& r, bool loadFromDisk)
{
    for (int i = 0; i < s_count; i++) {
        if (SameKey(s_mem[i].req, r)) {
            s_mem[i].useTick = GetTickCount();
            return &s_mem[i];
        }
    }
    if (!loadFromDisk || r.src != SRC_OSM)
        return NULL;
    wchar_t path[MAX_PATH];
    CachePath(path, r);
    MapResult res;
    memset(&res, 0, sizeof(res));
    res.req = r;
    if (!ImageFromFile(path, &res.img))
        return NULL;
    CacheEntry* e = CacheAdd(&res, 0);
    e->fileDay = FileDay(path);
    return e;
}

int CacheBytes()
{
    return s_bytes;
}

int CacheList(CacheEntry** out, int max)
{
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++)
        out[n++] = &s_mem[i];
    return n;
}
