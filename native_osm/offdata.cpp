#include "yamaps.h"

// The offline data's files and answers, shared by the router (offline.cpp, route.grf) and the search (offsearch.cpp,
// search.idx): a file with its header and the parts kept in memory; the JSON text the answers are written into.

bool OffRead(HANDLE f, DWORD off, void* to, DWORD bytes)
{
    DWORD got = 0;
    return SetFilePointer(f, off, NULL, FILE_BEGIN) != 0xFFFFFFFF && ReadFile(f, to, bytes, &got, NULL) && got == bytes;
}

bool OffOpen(OffFile* f, const wchar_t* path, void* head, DWORD bytes)
{
    memset(f, 0, sizeof(*f));
    f->file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f->file == INVALID_HANDLE_VALUE) {
        f->file = NULL;
        return false;
    }
    if (OffRead(f->file, 0, head, bytes))
        return true;
    OffClose(f);
    return false;
}

void* OffPart(OffFile* f, DWORD off, DWORD bytes)
{
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

void OffClose(OffFile* f)
{
    for (int i = 0; i < f->nparts; i++)
        free(f->part[i]);
    if (f->file)
        CloseHandle(f->file);
    memset(f, 0, sizeof(*f));
}

// ---------------------------------------------------------------- the regions' folders

static const wchar_t* kDataDirs[] = { L"\\SDMemory\\OSMapsData", L"\\USBDevice\\OSMapsData" };

// The folders the regions' folders may be in; returns their count.
static int DataDirs(wchar_t dirs[][MAX_PATH])
{
    int nd = 0;
    for (int i = 0; i < 2; i++)
        wcscpy(dirs[nd++], kDataDirs[i]);
    PathInDir(dirs[nd++], L"OSMapsData");
#ifndef UNDER_CE
    PathInDir(dirs[nd++], L"..\\OSMapsData");   // the PC: the tool's output beside the program's folder
#endif
    return nd;
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
    wchar_t dirs[4][MAX_PATH];
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
