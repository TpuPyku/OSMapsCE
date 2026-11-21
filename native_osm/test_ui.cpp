// test_ui.cpp - the route's start (startpt.h) and the languages (lang.cpp) on the PC. Exit code: the failed checks.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lang.h"
#include "startpt.h"
#include "segpick.h"
#include "recent.h"

static int s_failed, s_checks;

static void Check(bool ok, const char* what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAILED: %s\n", what);
    }
}

static void TestStart()
{
    StartPoint s = StartFrom(true, 37.5, 55.5, 50.1, 53.2, 10, 20);
    Check(s.source == START_CAR && s.lon == 37.5 && s.lat == 55.5, "a fix: the car");
    s = StartFrom(false, 37.5, 55.5, 50.1, 53.2, 10, 20);
    Check(s.source == START_LAST && s.lon == 50.1 && s.lat == 53.2, "no fix: the last place the GPS told");
    s = StartFrom(false, 0, 0, 0, 0, 10, 20);
    Check(s.source == START_CENTER && s.lon == 10 && s.lat == 20, "the GPS never told one: the map's centre");
    s = StartFrom(true, 37.5, 55.5, 0, 0, 10, 20);
    Check(s.source == START_CAR, "a first fix with nothing kept: the car");
    s = StartFrom(false, 0, 0, 0.5, 0, 10, 20);
    Check(s.source == START_LAST, "a place on the equator is a place");
}

static void TestSegPick()
{
    double one[4] = { 30, 20, 2, 25 };
    Check(SegPick(one, 4, 0, 8) == 2, "seg: the nearest when no other is near it");
    double jog[4] = { 3.5, 20, 3.4, 40 };   // before a jog of two turns: the piece after it as near as ours
    Check(SegPick(jog, 4, 0, 8) == 0, "seg: of two as near, the earlier one");
    Check(SegPick(jog, 4, 1, 8) == 2, "seg: not back before where it was");
    double past[3] = { 21, 15, 0.5 };   // gone on past the jog: the old piece far behind
    Check(SegPick(past, 3, 0, 8) == 2, "seg: past it - the piece it is on");
    double edge[3] = { 8.6, 50, 0.5 };
    Check(SegPick(edge, 3, 0, 8) == 2 && SegPick(edge, 3, 0, 8.1) == 0, "seg: within the tie only (its edge in)");
    Check(SegPick(one, 0, 0, 8) == 0, "seg: no pieces - 0");
}

static int Load(const char* text)
{
    return LangFromBytes(text, (int)strlen(text));
}

static void TestLang()
{
    const wchar_t* key = L"Карта";
    Check(Tr(key) == key, "Russian on: the same pointer back");
    int n = Load("\xEF\xBB\xBF#name English\r\n"
                 "# a comment\r\n"
                 "\xD0\x9A\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0\tMap\r\n"
                 "%d \xD0\xBC\xD0\xB8\xD0\xBD\t%d min\n"
                 "\n"
                 "no tab here\n");
    Check(n == 2, "the two lines kept, the BOM, the comment, the blank and the broken one passed over");
    Check(!wcscmp(Tr(L"Карта"), L"Map"), "a translated string");
    Check(!wcscmp(Tr(L"%d мин"), L"%d min"), "a format, the last line without its CR");
    const wchar_t* other = L"Нет такой";
    Check(Tr(other) == other, "not in the file: stays Russian");
    Check(!wcscmp(LangName(), L"English"), "its name");

    // a wrong format in someone's file would crash the program: such a line is dropped
    n = Load("#name X\n"
             "%d a\t%s a\n"
             "%d b\t%d %d b\n"
             "%s c\tc\n"
             "%02d d\t%d d\n"
             "e\t%s e\n"
             "%d%% f\t%d%% f ok\n"
             "%s %d g\t%d %s g\n"
             "%.5f h\t%.5f h ok\n"
             "%lu i\t%lu i ok\n");
    Check(n == 3, "only the lines whose formats are the same kept");
    const wchar_t* a = L"%d a";
    Check(Tr(a) == a, "%d into %s: dropped, the original stays");
    const wchar_t* g = L"%s %d g";
    Check(Tr(g) == g, "the order changed: dropped");
    Check(!wcscmp(Tr(L"%d%% f"), L"%d%% f ok"), "%% is no argument");

    // many lines: a table that holds them all, or stops without harm
    enum { kLines = 3000 };
    static char big[kLines * 24];
    int at = 0;
    for (int i = 0; i < kLines; i++)
        at += sprintf(big + at, "k%d\tv%d\n", i, i);
    n = LangFromBytes(big, at);
    Check(n > 0 && n <= kLines, "too many lines: no harm");
    Check(!wcscmp(Tr(L"k1"), L"v1"), "the first ones found");
    Check(n == 0 || Load("#name Y\nk\tv\n") == 1, "loaded again");
    LangOff();
    Check(Tr(key) == key, "off: Russian again");
    Check(LangFromBytes("", 0) == 0, "an empty file");
    LangOff();
}

// The recent addresses of "Куда едем?" (the user 2026-11-21): the newest first, at most kRecentMax, the same place
// (150 m) or the same name once - moved up; the file's lines "name<TAB>lat<TAB>lon".
static void TestRecent()
{
    Recents r;
    RecentClear(&r);
    RecentAdd(&r, L"Ленина, 1", 55.7500, 37.6000);
    RecentAdd(&r, L"Мира, 5", 55.8000, 37.6500);
    Check(r.n == 2 && !wcscmp(r.it[0].name, L"Мира, 5") && !wcscmp(r.it[1].name, L"Ленина, 1"), "recent: the newest first");
    RecentAdd(&r, L"Ленина, 1 (подъезд)", 55.7505, 37.6005);
    Check(r.n == 2 && !wcscmp(r.it[0].name, L"Ленина, 1 (подъезд)") && !wcscmp(r.it[1].name, L"Мира, 5"),
          "recent: the same place (70 m) - moved up with its new name, not twice");
    RecentAdd(&r, L"Мира, 5", 55.9000, 37.9000);
    Check(r.n == 2 && !wcscmp(r.it[0].name, L"Мира, 5") && r.it[0].lat == 55.9, "recent: the same name - moved up, its new place");
    RecentAdd(&r, L"Вокзал", 55.7600, 37.6000);
    Check(r.n == 3 && !wcscmp(r.it[0].name, L"Вокзал"), "recent: 1.1 km away - another place");
    for (int i = 0; i < 10; i++) {
        wchar_t n[16];
        swprintf(n, 16, L"Место %d", i);
        RecentAdd(&r, n, 50 + i, 30);
    }
    Check(r.n == kRecentMax && !wcscmp(r.it[0].name, L"Место 9") && !wcscmp(r.it[kRecentMax - 1].name, L"Место 5"),
          "recent: no more than kRecentMax, the oldest out");
    RecentAdd(&r, L"", 1, 1);
    Check(r.n == kRecentMax && !wcscmp(r.it[0].name, L"Место 9"), "recent: no name - not kept");

    wchar_t text[1024];
    RecentClear(&r);
    RecentAdd(&r, L"Ленина, 1", 55.75, 37.6);
    RecentAdd(&r, L"Мира, 5", 55.8, 37.65);
    RecentFormat(&r, text, 1024);
    Check(!wcscmp(text, L"Мира, 5\t55.800000\t37.650000\nЛенина, 1\t55.750000\t37.600000\n"), "recent: the file's text");
    Recents g;
    Check(RecentParse(text, &g) == 2 && !wcscmp(g.it[0].name, L"Мира, 5") && g.it[1].lat == 55.75 && g.it[1].lon == 37.6,
          "recent: read back as written");
    Check(RecentParse(L"bad line\nA\t1\t2\r\n\t3\t4\nB\t5\n", &g) == 1 && !wcscmp(g.it[0].name, L"A") &&
          g.it[0].lat == 1 && g.it[0].lon == 2, "recent: lines without both numbers or a name left out, CRLF too");
    wchar_t room[30];
    RecentFormat(&r, room, 30);
    Check(!wcscmp(room, L"Мира, 5\t55.800000\t37.650000\n"), "recent: whole lines only - the room for one");
    RecentFormat(&r, room, 20);
    Check(!room[0], "recent: no room for a line - nothing");
}

int main()
{
    TestRecent();
    TestStart();
    TestSegPick();
    TestLang();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
