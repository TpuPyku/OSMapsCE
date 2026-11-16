#include "yamaps.h"

// The route kept for the next start (the user 2026-11-14: OSMapsCE closed or the power gone with a route on - the next
// start asks to go on): the plan's stops and its end, from the car again. Written when a route starts or changes, gone
// when it ends (arrived, cancelled) - a few writes a trip (the flash).

static int PlaceLine(const RoutePlace& pl, const char* kind, char* line, int cap)   // its line; 0: not a place
{
    char name[80 * 3];
    if (pl.kind != PL_PLACE || !WideCharToMultiByte(CP_UTF8, 0, pl.name, -1, name, sizeof(name), NULL, NULL))
        return 0;
    return _snprintf(line, cap, "%s\t%.7f\t%.7f\t%s\r\n", kind, pl.lat, pl.lon, name);
}

bool ResumeSave(const RoutePlan* p, const wchar_t* path)
{
    if (p->to.kind != PL_PLACE) {
        ResumeDrop(path);
        return false;
    }
    char text[(kMaxVia + 1) * 300];
    int n = 0;
    for (int i = 0; i < p->nvia && i < kMaxVia; i++)
        n += PlaceLine(p->via[i], "via", text + n, sizeof(text) - n);
    n += PlaceLine(p->to, "to", text + n, sizeof(text) - n);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = WriteFile(h, text, n, &put, NULL) && put == (DWORD)n;
    CloseHandle(h);
    return ok;
}

static void TakeLine(RoutePlan* p, char* s)   // "via|to<TAB>lat<TAB>lon<TAB>name"; anything else left out
{
    char* t1 = strchr(s, '\t');
    char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    char* t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
    if (!t3)
        return;
    *t1 = 0;
    RoutePlace* pl = !strcmp(s, "to") ? &p->to : !strcmp(s, "via") && p->nvia < kMaxVia ? &p->via[p->nvia++] : NULL;
    if (!pl)
        return;
    pl->kind = PL_PLACE;
    pl->lat = atof(t1 + 1);
    pl->lon = atof(t2 + 1);
    MultiByteToWideChar(CP_UTF8, 0, t3 + 1, -1, pl->name, 80);
    pl->name[79] = 0;
}

bool ResumeLoad(RoutePlan* p, const wchar_t* path)
{
    memset(p, 0, sizeof(*p));
    p->from.kind = PL_GPS;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char buf[2048];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    for (char* s = buf; s; ) {
        char* nl = strchr(s, '\n');
        if (nl)
            *nl = 0;
        if (nl && nl > s && nl[-1] == '\r')
            nl[-1] = 0;
        TakeLine(p, s);
        s = nl ? nl + 1 : NULL;
    }
    return p->to.kind == PL_PLACE;
}

void ResumeDrop(const wchar_t* path)
{
    DeleteFileW(path);
}
