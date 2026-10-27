// The offline router and search (offline.cpp, offsearch.cpp) on the test map (offline/test/tiny.osm, built by
// test_offline.bat into obj\test\OSMapsData\tiny): asked as OSMapsCE's download thread asks them, checked as the
// driver would see the answer. Exit code: the number of failed checks.
#include "yamaps.h"
#include <stdio.h>

Config  g_cfg;
wchar_t g_dir[MAX_PATH];

void PathInDir(wchar_t* out, const wchar_t* name)
{
    _snwprintf(out, MAX_PATH, L"%s%s", g_dir, name);
    out[MAX_PATH - 1] = 0;
}

void Log(const char*, ...)
{
}

static int s_failed, s_checks;

static void Check(bool ok, const char* what, const char* body)
{
    s_checks++;
    if (ok)
        return;
    s_failed++;
    printf("FAIL: %s\n  %.400s\n", what, body ? body : "(no answer)");
}

static char* Route(const char* lonLat)
{
    char path[300], *body = NULL;
    int len = 0;
    _snprintf(path, sizeof(path), "/routed-car/route/v1/driving/%s?overview=full&geometries=polyline6&steps=true", lonLat);
    return OfflineRoute(path, &body, &len) ? body : NULL;
}

static char* Ask(const char* photonPath)
{
    char* body = NULL;
    int len = 0;
    return OfflineSearch(photonPath, &body, &len) ? body : NULL;
}

static double Num(const char* body, const char* key)
{
    const char* p = body ? strstr(body, key) : NULL;
    return p ? atof(p + strlen(key)) : -1;
}

// The answer (UTF-8) holds the text (the source's Cyrillic: wide literals - VS2005 recodes narrow ones)
static bool Has(const char* body, const wchar_t* text)
{
    char u[300];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u, sizeof(u), NULL, NULL);
    return body && strstr(body, u) != NULL;
}

static int Count(const char* body, const char* s)
{
    int n = 0;
    for (const char* p = body; p && (p = strstr(p, s)) != NULL; p++)
        n++;
    return n;
}

int main()
{
    GetModuleFileName(NULL, g_dir, MAX_PATH);
    wchar_t* s = wcsrchr(g_dir, L'\\');
    if (s)   // as from the program's folder obj\test\app\: its ..\OSMapsData is obj\test\OSMapsData
        wcscpy(s + 1, L"app\\");

    // along the main street, west to east: ~400 m, no turn on the way
    char* b = Route("50.000,53.000;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 380 && Num(b, "\"distance\":") < 420, "main street: ~400 m", b);
    Check(b && Count(b, "\"type\":\"turn\"") == 0 && Count(b, "\"type\":\"depart\"") == 1 &&
          Count(b, "\"type\":\"arrive\"") == 1, "main street: depart, arrive, no turns", b);
    free(b);

    // from the east end to the upper corner: right onto the one-way street (east -> west, then north)
    b = Route("50.006,53.000;50.004,53.002");
    Check(b && Num(b, "\"distance\":") > 330 && Num(b, "\"distance\":") < 380, "to the one-way: ~356 m", b);
    Check(Has(b, L"\"name\":\"Односторонняя улица\",\"ref\":\"\",\"maneuver\":{\"type\":\"turn\",\"modifier\":\"right\""),
          "to the one-way: turn right onto it", b);
    free(b);

    // back: not against the one-way - round by the upper and the side streets (~624 m)
    b = Route("50.004,53.002;50.006,53.000");
    Check(b && Num(b, "\"distance\":") > 590 && Num(b, "\"distance\":") < 660, "back: round the one-way, ~624 m", b);
    free(b);

    // a point outside every region: not answered (OSRM is asked)
    b = Route("37.6,55.7;37.7,55.8");
    Check(!b, "outside the regions: no answer", b);
    free(b);

    // the search: the town first, a house by street and number, the station by its kind
    b = Ask("/api/?q=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D1%81%D0%BA&limit=7&lat=53.0&lon=50.0");   // Тестовск
    Check(Has(b, L"{\"features\":[{\"geometry\":{\"coordinates\":[50.003000,53.001000]},\"properties\":{\"name\":\"Тестовск\""),
          "search: the town first", b);
    free(b);
    b = Ask("/api/?q=%D1%83%D0%BB+%D0%B3%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F+5&limit=7&lat=53.0&lon=50.0");   // ул главная 5
    Check(Has(b, L"\"street\":\"Главная улица\",\"housenumber\":\"5\""), "search: the house", b);
    free(b);
    b = Ask("/api/?q=%D0%B0%D0%B7%D1%81&limit=7&lat=53.0&lon=50.0");   // азс
    Check(Has(b, L"Тест-АЗС (АЗС)"), "search: the station by its kind", b);
    free(b);
    b = Ask("/api/?q=%D0%BD%D0%B5%D1%82%D1%82%D0%B0%D0%BA%D0%BE%D0%B9&limit=7&lat=53.0&lon=50.0");   // неттакой
    Check(!b, "search: nothing found - no answer (Photon is asked)", b);
    free(b);

    // the address of a point: the house within 80 m
    b = Ask("/reverse?lon=50.00212&lat=53.00012&limit=1");
    Check(Has(b, L"\"street\":\"Главная улица\",\"housenumber\":\"5\""), "reverse: the house", b);
    free(b);

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
