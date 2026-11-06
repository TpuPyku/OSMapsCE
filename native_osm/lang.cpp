// lang.cpp - see lang.h. A language is read once, when first chosen, and stays in memory (one file, some 30 KB): other
// threads may hold its strings, so it is never freed. A line whose formats differ from the Russian is dropped: someone's
// "%s" for a "%d" would crash the program.
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kSlots = 1024, kMaxKept = 900, kLangs = 8, kNameCap = 24, kCodeCap = 8 };
struct Entry { const wchar_t* ru; const wchar_t* tr; };
struct Lang { char code[kCodeCap]; wchar_t name[kNameCap]; Entry* slots; wchar_t* text; };

static Lang s_langs[kLangs];
static int s_count;
static Lang s_loose;              // LangFromBytes'
static Lang* volatile s_on;       // NULL: Russian

static unsigned Hash(const wchar_t* s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ *s) * 16777619u;
    return h;
}

const wchar_t* Tr(const wchar_t* ru)
{
    Lang* l = s_on;
    if (!l || !l->slots)
        return ru;
    for (unsigned i = Hash(ru) % kSlots; l->slots[i].ru; i = (i + 1) % kSlots)
        if (!wcscmp(l->slots[i].ru, ru))
            return l->slots[i].tr;
    return ru;
}

// The next format of s into out (flags, width, length, its letter), "%%" passed over; false: no more.
static bool NextSpec(const wchar_t** s, wchar_t* out, int cap)
{
    const wchar_t* p = *s;
    for (; *p; p++) {
        if (*p != L'%')
            continue;
        if (p[1] == L'%') {
            p++;
            continue;
        }
        int n = 0;
        out[n++] = *p++;
        while (*p && n < cap - 1) {
            wchar_t c = *p++;
            out[n++] = c;
            if ((c >= L'a' && c <= L'z' && c != L'l' && c != L'h') || (c >= L'A' && c <= L'Z'))
                break;
        }
        out[n] = 0;
        *s = p;
        return true;
    }
    return false;
}

static bool SameFormats(const wchar_t* a, const wchar_t* b)
{
    wchar_t x[16], y[16];
    for (;;) {
        bool hasX = NextSpec(&a, x, 16), hasY = NextSpec(&b, y, 16);
        if (hasX != hasY)
            return false;
        if (!hasX)
            return true;
        if (wcscmp(x, y))
            return false;
    }
}

static void Put(Lang& l, const wchar_t* ru, const wchar_t* tr)
{
    unsigned i = Hash(ru) % kSlots;
    while (l.slots[i].ru)
        i = (i + 1) % kSlots;
    l.slots[i].ru = ru;
    l.slots[i].tr = tr;
}

// A line, without its end: the name, a comment, a pair; true: a pair kept
static bool TakeLine(Lang& l, wchar_t* line)
{
    if (!wcsncmp(line, L"#name ", 6)) {
        wcsncpy(l.name, line + 6, kNameCap - 1);
        l.name[kNameCap - 1] = 0;
        return false;
    }
    wchar_t* tab = line[0] == L'#' ? NULL : wcschr(line, L'\t');
    if (!tab)
        return false;
    *tab = 0;
    if (!SameFormats(line, tab + 1))
        return false;
    Put(l, line, tab + 1);
    return true;
}

static int Build(Lang& l, const char* utf8, int len)
{
    int n = len > 0 ? MultiByteToWideChar(CP_UTF8, 0, utf8, len, NULL, 0) : 0;
    if (n <= 0)
        return 0;
    l.text = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    l.slots = (Entry*)calloc(kSlots, sizeof(Entry));
    if (!l.text || !l.slots) {
        free(l.text);
        free(l.slots);
        l.text = NULL;
        l.slots = NULL;
        return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, l.text, n);
    l.text[n] = 0;
    int kept = 0;
    wchar_t* p = l.text[0] == 0xFEFF ? l.text + 1 : l.text;
    while (*p && kept < kMaxKept) {
        wchar_t* end = wcschr(p, L'\n');
        if (end)
            *end = 0;
        int k = (int)wcslen(p);
        if (k && p[k - 1] == L'\r')
            p[k - 1] = 0;
        if (TakeLine(l, p))
            kept++;
        if (!end)
            break;
        p = end + 1;
    }
    return kept;
}

int LangFromBytes(const char* utf8, int len)
{
    s_on = NULL;
    free(s_loose.text);
    free(s_loose.slots);
    memset(&s_loose, 0, sizeof(s_loose));
    int kept = Build(s_loose, utf8, len);
    s_on = s_loose.slots ? &s_loose : NULL;
    return kept;
}

void LangOff()
{
    s_on = NULL;
}

// ---- the files beside the exe

static void LangPath(const wchar_t* file, wchar_t* out)
{
    GetModuleFileName(NULL, out, MAX_PATH);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash)
        slash[1] = 0;
    wcsncat(out, L"Lang\\", MAX_PATH - wcslen(out) - 1);
    wcsncat(out, file, MAX_PATH - wcslen(out) - 1);
}

static char* FileBytes(const wchar_t* path, int cap, int* len)
{
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    if (cap && size > (DWORD)cap)
        size = cap;
    char* b = size != 0xFFFFFFFF ? (char*)malloc(size + 1) : NULL;
    if (b && !ReadFile(f, b, size, &got, NULL))
        got = 0;
    CloseHandle(f);
    *len = (int)got;
    return b;
}

static void Pick(Lang* l)
{
    if (l && !l->slots) {
        wchar_t file[MAX_PATH], path[MAX_PATH];
        _snwprintf(file, MAX_PATH, L"%S.txt", l->code);
        file[MAX_PATH - 1] = 0;
        LangPath(file, path);
        int len = 0;
        char* b = FileBytes(path, 0, &len);
        if (b)
            Build(*l, b, len);
        free(b);
        if (!l->slots)
            l = NULL;
    }
    s_on = l;
}

void LangInit(const char* code)
{
    wchar_t pat[MAX_PATH];
    LangPath(L"*.txt", pat);
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
            int n = dot ? (int)(dot - fd.cFileName) : 0;
            if (n <= 0 || n >= kCodeCap || s_count == kLangs)
                continue;
            Lang& l = s_langs[s_count];
            memset(&l, 0, sizeof(l));
            WideCharToMultiByte(CP_ACP, 0, fd.cFileName, n, l.code, kCodeCap - 1, NULL, NULL);
            wcsncpy(l.name, fd.cFileName, n);   // (till the file says its name: read when chosen)
            l.name[n] = 0;
            wchar_t path[MAX_PATH];
            LangPath(fd.cFileName, path);
            int len = 0;
            char* b = FileBytes(path, 256, &len);   // the name: the first line
            if (b) {
                wchar_t head[64];
                int w = MultiByteToWideChar(CP_UTF8, 0, b, len, head, 63);
                head[w > 0 ? w : 0] = 0;
                free(b);
                const wchar_t* at = wcsstr(head, L"#name ");
                if (at) {
                    wcsncpy(l.name, at + 6, kNameCap - 1);
                    l.name[kNameCap - 1] = 0;
                    wchar_t* cut = wcspbrk(l.name, L"\r\n");
                    if (cut)
                        *cut = 0;
                }
            }
            s_count++;
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    for (int i = 0; i < s_count; i++)
        if (!strcmp(s_langs[i].code, code))
            Pick(&s_langs[i]);
}

void LangStep(int dir)
{
    int cur = -1;   // Russian
    for (int i = 0; i < s_count; i++)
        if (s_on == &s_langs[i])
            cur = i;
    int next = (cur + 1 + dir + s_count + 1) % (s_count + 1) - 1;
    Pick(next < 0 ? NULL : &s_langs[next]);
}

const char* LangCode()
{
    Lang* l = s_on;
    return l ? l->code : "";
}

const wchar_t* LangName()
{
    Lang* l = s_on;
    return l ? l->name : L"Русский";
}
