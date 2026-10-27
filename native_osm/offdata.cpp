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
