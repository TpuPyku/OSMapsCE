// The offline router on the PC: test_offline.bat builds it with offline.cpp; it asks for routes between points of
// the region in ..\OSMapsData and prints the answer's summary (and the whole JSON with /v).
//    test_offline.exe lon,lat;lon,lat[;...] [course] [/v]
#include "yamaps.h"
#include <stdio.h>

Config  g_cfg;
wchar_t g_dir[MAX_PATH];

void PathInDir(wchar_t* out, const wchar_t* name)
{
    _snwprintf(out, MAX_PATH, L"%s%s", g_dir, name);
    out[MAX_PATH - 1] = 0;
}

void Log(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}

int main(int argc, char** argv)
{
    GetModuleFileName(NULL, g_dir, MAX_PATH);
    wchar_t* s = wcsrchr(g_dir, L'\\');
    if (s)   // as if it ran from the program's folder: native_osm\obj\test -> OSMapsCE\ (its ..\OSMapsData)
        wcscpy(s + 1, L"..\\..\\..\\OSMapsCE\\");
    const char* pts = argc > 1 ? argv[1] : "50.1006,53.1959;50.2210,53.2415";   // Samara: centre -> north-east
    char path[600];
    _snprintf(path, sizeof(path), "/routed-car/route/v1/driving/%s?overview=full&geometries=polyline6&steps=true%s%s",
              pts, argc > 2 && argv[2][0] != '/' ? "&bearings=" : "", argc > 2 && argv[2][0] != '/' ? argv[2] : "");
    bool verbose = argc > 1 && !strcmp(argv[argc - 1], "/v");
    char* body;
    int len;
    for (int run = 0; run < 2; run++) {   // the second: the graph is loaded already
        DWORD t0 = GetTickCount();
        if (!OfflineRoute(path, &body, &len)) {
            printf("no route\n");
            return 1;
        }
        printf("run %d: %d bytes, %lu ms\n", run, len, GetTickCount() - t0);
        if (run == 0) {
            // the steps: type, modifier, name
            for (const char* p = strstr(body, "\"name\":"); p; p = strstr(p + 1, "\"name\":")) {
                const char* t = strstr(p, "\"type\":\"");
                const char* m = strstr(p, "\"modifier\":\"");
                const char* x = strstr(p, "\"exit\":");
                const char* e = strstr(p + 1, "\"name\":");
                char name[96] = "";
                sscanf(p + 8, "%95[^\"]", name);
                char type[24] = "", mod[24] = "";
                if (t) sscanf(t + 8, "%23[^\"]", type);
                if (m) sscanf(m + 12, "%23[^\"]", mod);
                printf("  %-12s %-13s %s%s\n", type, mod, name, x && (!e || x < e) ? " (exit)" : "");
            }
            const char* d = strstr(body, "\"duration\":");
            if (d)
                printf("%.60s\n", d);
            if (verbose)
                printf("%s\n", body);
        }
        free(body);
    }
    return 0;
}
