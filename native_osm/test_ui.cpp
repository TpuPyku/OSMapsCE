// test_ui.cpp - the route's start (startpt.h) and the languages (lang.cpp) on the PC. Exit code: the failed checks.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lang.h"
#include "startpt.h"

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

int main()
{
    TestStart();
    TestLang();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
