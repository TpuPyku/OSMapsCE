// recent.h - the recent addresses of "Куда едем?" (the user 2026-11-21, design v3): the places routed to from the
// search, the newest first, at most kRecentMax; the same place (within 150 m) or the same name kept once - moved up.
// The file recent.txt beside the exe: UTF-16 text, lines "name<TAB>lat<TAB>lon". Pure, header-only (test_ui).
#ifndef RECENT_H
#define RECENT_H

#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

enum { kRecentMax = 5, kRecentName = 80 };
struct RecentItem { wchar_t name[kRecentName]; double lat, lon; };
struct Recents { RecentItem it[kRecentMax]; int n; };

static void RecentClear(Recents* r)
{
    r->n = 0;
}

static bool RecentSame(const RecentItem& a, const wchar_t* name, double lat, double lon)
{
    double dy = (lat - a.lat) * 111.2, dx = (lon - a.lon) * 111.2 * cos(a.lat * 3.14159265 / 180);
    return !wcscmp(a.name, name) || dx * dx + dy * dy < 0.15 * 0.15;
}

static void RecentAdd(Recents* r, const wchar_t* name, double lat, double lon)
{
    if (!name[0])
        return;
    int k = 0;
    while (k < r->n && !RecentSame(r->it[k], name, lat, lon))
        k++;
    if (k == r->n)   // a new one: the oldest out when full
        k = r->n < kRecentMax ? r->n++ : kRecentMax - 1;
    for (; k > 0; k--)
        r->it[k] = r->it[k - 1];
    wcsncpy(r->it[0].name, name, kRecentName - 1);
    r->it[0].name[kRecentName - 1] = 0;
    r->it[0].lat = lat;
    r->it[0].lon = lon;
}

static void RecentFormat(const Recents* r, wchar_t* out, int cap)   // whole lines only, as many as fit
{
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < r->n; i++) {
        wchar_t line[kRecentName + 48];
        int len = _snwprintf(line, kRecentName + 48, L"%s\t%.6f\t%.6f\n", r->it[i].name, r->it[i].lat, r->it[i].lon);
        if (len < 0 || n + len >= cap)
            break;
        wcscpy(out + n, line);
        n += len;
    }
}

static int RecentParse(const wchar_t* text, Recents* r)   // the items read
{
    RecentClear(r);
    for (const wchar_t* p = text; *p && r->n < kRecentMax;) {
        const wchar_t* e = wcschr(p, L'\n');
        int len = e ? (int)(e - p) : (int)wcslen(p);
        const wchar_t* t1 = wcschr(p, L'\t');
        const wchar_t* t2 = t1 && t1 < p + len ? wcschr(t1 + 1, L'\t') : NULL;
        if (t1 && t2 && t2 < p + len && t1 > p && t1 - p < kRecentName) {
            RecentItem& it = r->it[r->n];
            wcsncpy(it.name, p, t1 - p);
            it.name[t1 - p] = 0;
            it.lat = wcstod(t1 + 1, NULL);
            it.lon = wcstod(t2 + 1, NULL);
            r->n++;
        }
        p += len + (e ? 1 : 0);
    }
    return r->n;
}

#endif
