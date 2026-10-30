#include "yamaps.h"

// The offline data's files and answers, shared by the router (offline.cpp, route.grf) and the search (offsearch.cpp,
// search.idx): a file with its header and the parts kept in memory; the JSON text the answers are written into.

bool OffRead(HANDLE f, DWORD off, void* to, DWORD bytes)
{
    DWORD got = 0;
    return SetFilePointer(f, off, NULL, FILE_BEGIN) != 0xFFFFFFFF && ReadFile(f, to, bytes, &got, NULL) && got == bytes;
}

// The file mapped read-only (the user 2026-10-29: on a Shturmann NSL 500 no route and no search in a region -
// Moscow's oblast's graph is 99 MB, 77 MB of it were read into memory; mapped, its pages come as they are read and
// go when memory is short). Not mapped: its parts are read into memory as before.
static void Map(OffFile* f, const wchar_t* path)
{
#ifdef UNDER_CE
    f->mfile = CreateFileForMapping(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    NULL);
#else
    f->mfile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
#endif
    if (f->mfile == INVALID_HANDLE_VALUE) {
        f->mfile = NULL;
        return;
    }
    f->map = CreateFileMapping(f->mfile, NULL, PAGE_READONLY, 0, 0, NULL);
    f->view = f->map ? (const BYTE*)MapViewOfFile(f->map, FILE_MAP_READ, 0, 0, 0) : NULL;
    f->size = f->view ? GetFileSize(f->file, NULL) : 0;
}

bool OffOpen(OffFile* f, const wchar_t* path, void* head, DWORD bytes)
{
    memset(f, 0, sizeof(*f));
    f->file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f->file == INVALID_HANDLE_VALUE) {
        f->file = NULL;
        return false;
    }
    if (!OffRead(f->file, 0, head, bytes)) {
        OffClose(f);
        return false;
    }
    Map(f, path);
    return true;
}

static bool InView(const OffFile* f, DWORD off, DWORD bytes)   // mapped there and 4-byte aligned (ARM's loads)
{
    return f->view && !(off & 3) && off + bytes <= f->size && off + bytes >= off;
}

// A part of the file: in its mapped view, else read into memory
void* OffPart(OffFile* f, DWORD off, DWORD bytes)
{
    if (InView(f, off, bytes))
        return (void*)(f->view + off);
    void* p = f->nparts < kOffParts ? malloc(bytes ? bytes : 1) : NULL;
    if (p && !OffRead(f->file, off, p, bytes)) {
        free(p);
        p = NULL;
    }
    if (p)
        f->part[f->nparts++] = p;
    else
        f->bad = true;
    return p;
}

// The routes, the search and the download writing a region over their files take turns (the user 2026-10-30: the
// Moscow oblast's zip not unpacked - its route.grf held by the route just made)
static struct UseLock {
    CRITICAL_SECTION cs;
    UseLock() { InitializeCriticalSection(&cs); }
} s_use;

void OffUse(bool on)
{
    if (on)
        EnterCriticalSection(&s_use.cs);
    else
        LeaveCriticalSection(&s_use.cs);
}

void OffClose(OffFile* f)
{
    for (int i = 0; i < f->nparts; i++)
        free(f->part[i]);
    if (f->view)
        UnmapViewOfFile((LPVOID)f->view);
    if (f->map)
        CloseHandle(f->map);
#ifndef UNDER_CE
    if (f->mfile)   // (CE: CreateFileForMapping's handle closes with its mapping)
        CloseHandle(f->mfile);
#endif
    if (f->file)
        CloseHandle(f->file);
    memset(f, 0, sizeof(*f));
}

// ---------------------------------------------------------------- the regions' folders

// The data folders on any device (the user 2026-10-29: Shturmann NSL 500 saw no vector maps - its card is not the
// Vesta's \SDMemory): every volume's \OSMapsData - CE: the root's folders (the storage cards, the flash; their names
// are the device's), the PC: the drives - then the program's own folder's (the PC: the tools' output beside it too).
// Listed again at most every 5 s (asked for every pack and route; a card may come meanwhile).
enum { kMaxVols = 12, kMaxDirs = kMaxVols + 2, kDirsMs = 5000 };

struct Volume { wchar_t root[MAX_PATH]; bool card; };   // card: removable (CE: a storage card's folder)

static int Volumes(Volume* v, int cap)
{
    int n = 0;
#ifdef UNDER_CE
    WIN32_FIND_DATA fd;
    HANDLE ff = FindFirstFile(L"\\*", &fd);
    if (ff == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < cap) {
            _snwprintf(v[n].root, MAX_PATH, L"\\%s", fd.cFileName);
            v[n++].card = (fd.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0;
        }
    } while (FindNextFile(ff, &fd));
    FindClose(ff);
#else
    UINT was = SetErrorMode(SEM_FAILCRITICALERRORS);   // (a drive without its disk: no window)
    DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26 && n < cap; d++)   // (from C:: A: and B: - floppies)
        if (drives & (1 << d)) {
            _snwprintf(v[n].root, MAX_PATH, L"%c:", L'A' + d);
            v[n].card = GetDriveType(v[n].root) == DRIVE_REMOVABLE;
            n++;
        }
    SetErrorMode(was);
#endif
    return n;
}

static bool HasData(const wchar_t* root, wchar_t* dir)   // root's \OSMapsData, when there
{
    _snwprintf(dir, MAX_PATH, L"%s\\OSMapsData", root);
    dir[MAX_PATH - 1] = 0;
    return GetFileAttributes(dir) != 0xFFFFFFFF;
}

static int  s_nDirs;          // the data folders listed (0: to list)
static bool s_volumes = true;   // false: the program's folder only (the tests: not the PC's own disks' maps)

void OffVolumes(bool on)
{
    s_volumes = on;
    s_nDirs = 0;   // (listed again)
}

static int ListDirs(wchar_t dirs[][MAX_PATH])
{
    Volume v[kMaxVols];
    int nv = s_volumes ? Volumes(v, kMaxVols) : 0, nd = 0;
    for (int i = 0; i < nv; i++)
        nd += HasData(v[i].root, dirs[nd]) ? 1 : 0;
    PathInDir(dirs[nd++], L"OSMapsData");
#ifndef UNDER_CE
    PathInDir(dirs[nd++], L"..\\OSMapsData");
#endif
    return nd;
}

static CRITICAL_SECTION s_dirsCs;
static volatile LONG    s_dirsCsDone;
static wchar_t          s_dirs[kMaxDirs][MAX_PATH];
static DWORD            s_dirsTick;

// The folders the regions' folders may be in (the volumes' first, the program's last); returns their count.
static int DataDirs(wchar_t dirs[][MAX_PATH])
{
    if (!InterlockedExchange((LONG*)&s_dirsCsDone, 1))
        InitializeCriticalSection(&s_dirsCs);
    EnterCriticalSection(&s_dirsCs);
    if (!s_nDirs || GetTickCount() - s_dirsTick >= kDirsMs) {
        s_nDirs = ListDirs(s_dirs);
        s_dirsTick = GetTickCount();
    }
    int nd = s_nDirs;
    memcpy(dirs, s_dirs, sizeof(s_dirs));
    LeaveCriticalSection(&s_dirsCs);
    return nd;
}

static ULONGLONG FreeOn(const wchar_t* root)
{
    ULARGE_INTEGER avail, total, all;
    wchar_t r[MAX_PATH];
    _snwprintf(r, MAX_PATH, L"%s\\", root);
    return GetDiskFreeSpaceEx(r, &avail, &total, &all) ? avail.QuadPart : 0;
}

// Where the downloaded regions go: the volume that has \OSMapsData already, else the removable one with the most
// room, else the program's folder
void OffWriteDir(wchar_t* out)
{
    Volume v[kMaxVols];
    int nv = Volumes(v, kMaxVols), best = -1;
    ULONGLONG most = 0;
    for (int i = 0; i < nv; i++) {
        if (HasData(v[i].root, out))
            return;
        ULONGLONG f = v[i].card ? FreeOn(v[i].root) : 0;
        if (f > most) {
            most = f;
            best = i;
        }
    }
    if (best >= 0)
        HasData(v[best].root, out);
    else
        PathInDir(out, L"OSMapsData");
}

// A found entry of a data folder: a region's folder whose file passes the test (dir = the folder)?
static bool Holds(const wchar_t* data, const WIN32_FIND_DATA& fd, const wchar_t* file, OffHolds holds, void* ctx,
                  wchar_t* dir)
{
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
        return false;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\%s\\%s", data, fd.cFileName, file);
    path[MAX_PATH - 1] = 0;
    if (!holds(path, ctx))
        return false;
    _snwprintf(dir, MAX_PATH, L"%s\\%s", data, fd.cFileName);
    dir[MAX_PATH - 1] = 0;
    return true;
}

static bool RegionIn(const wchar_t* data, const wchar_t* file, OffHolds holds, void* ctx, wchar_t* dir)
{
    wchar_t mask[MAX_PATH];
    _snwprintf(mask, MAX_PATH, L"%s\\*", data);
    WIN32_FIND_DATA fd;
    HANDLE ff = FindFirstFile(mask, &fd);
    if (ff == INVALID_HANDLE_VALUE)
        return false;
    bool found = Holds(data, fd, file, holds, ctx, dir);
    while (!found && FindNextFile(ff, &fd))
        found = Holds(data, fd, file, holds, ctx, dir);
    FindClose(ff);
    return found;
}

bool OffInside(const int* box, int lat, int lon)
{
    return lat >= box[0] && lat <= box[2] && lon >= box[1] && lon <= box[3];
}

bool OffRegion(const wchar_t* file, OffHolds holds, void* ctx, wchar_t* dir)
{
    wchar_t dirs[kMaxDirs][MAX_PATH];
    int nd = DataDirs(dirs);
    for (int d = 0; d < nd; d++)
        if (RegionIn(dirs[d], file, holds, ctx, dir))
            return true;
    return false;
}

// ---------------------------------------------------------------- JSON

void JPut(JBuf& b, const char* s, int n)
{
    if (n < 0)
        n = (int)strlen(s);
    if (b.n + n + 1 > b.cap) {
        int c = (b.cap ? b.cap * 2 : 4096);
        while (c < b.n + n + 1)
            c *= 2;
        char* p = (char*)realloc(b.p, c);
        if (!p)
            return;
        b.p = p;
        b.cap = c;
    }
    memcpy(b.p + b.n, s, n);
    b.n += n;
    b.p[b.n] = 0;
}

void JPutf(JBuf& b, const char* fmt, ...)
{
    char t[256];
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf(t, sizeof(t) - 1, fmt, a);
    va_end(a);
    t[sizeof(t) - 1] = 0;
    JPut(b, t, n < 0 ? (int)strlen(t) : n);
}

void JPutStr(JBuf& b, const char* s, int n)
{
    JPut(b, "\"");
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', c };
            JPut(b, e, 2);
        } else if ((unsigned char)c >= 0x20)
            JPut(b, &c, 1);
    }
    JPut(b, "\"");
}
