#include "yamaps.h"

// Search and route over OSM data on public servers (2026-10-26): Photon (komoot, HTTPS) finds places by text,
// OSRM of FOSSGIS (routing.openstreetmap.de, plain HTTP) lays the car route. The user's choice was Valhalla, but
// FOSSGIS's Valhalla and both OSRM servers speak TLS 1.3 only, BearSSL has 1.2 at most; FOSSGIS's OSRM answers
// plain HTTP too. OSRM has no Russian text: the instructions are made here from the maneuver and the street.
// One request at a time through the download thread (NetApi), counted in g_cfg.apiCount against apiLimit a day.
// The guidance: each GPS fix is put on the route line - the next maneuver and the distance to it; off the line
// for a while - a new route from here (not more often than kRerouteMs).

static const char* kPhoton   = "photon.komoot.io";
static const char* kOsrm     = "routing.openstreetmap.de";   // its car profile: /routed-car
static const int    kMaxHits      = 7;
static const double kOffM         = 50;     // farther from the line than this ...
static const int    kOffFixes     = 4;      // ... this many fixes in a row: off the route
static const DWORD  kRerouteMs    = 15000;  // a new route not more often
static const double kArriveM      = 30;     // the destination this close: arrived

// ---------------------------------------------------------------- JSON (read in place, no tree)

static const char* Ws(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static const char* JSkip(const char* p)
{
    p = Ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            if (*p == '\\' && p[1])
                p++;
        return *p ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = JSkip(p);
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0)
                return p + 1;
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

// The value of key in the object at p, NULL if none.
static const char* JKey(const char* p, const char* key)
{
    if (!p)
        return NULL;
    p = Ws(p);
    if (*p != '{')
        return NULL;
    p = Ws(p + 1);
    int kl = (int)strlen(key);
    while (*p == '"') {
        const char* k = p + 1;
        p = JSkip(p);
        bool match = p - k - 1 == kl && !strncmp(k, key, kl);
        p = Ws(p);
        if (*p != ':')
            return NULL;
        p = Ws(p + 1);
        if (match)
            return p;
        p = Ws(JSkip(p));
        if (*p != ',')
            return NULL;
        p = Ws(p + 1);
    }
    return NULL;
}

static const char* JFirst(const char* p)   // the first item of the array at p
{
    if (!p)
        return NULL;
    p = Ws(p);
    if (*p != '[')
        return NULL;
    p = Ws(p + 1);
    return *p == ']' || !*p ? NULL : p;
}

static const char* JNext(const char* p)
{
    p = Ws(JSkip(p));
    return *p == ',' ? Ws(p + 1) : NULL;
}

static double JNum(const char* p, double def)
{
    if (!p || !((*p >= '0' && *p <= '9') || *p == '-'))
        return def;
    return atof(p);
}

// A string value, unescaped, UTF-8 into out (cap bytes); "" if p is not a string.
static int JStr(const char* p, char* out, int cap)
{
    int n = 0;
    if (p && *p == '"') {
        for (p++; *p && *p != '"' && n < cap - 4; p++) {
            if (*p != '\\') {
                out[n++] = *p;
                continue;
            }
            p++;
            switch (*p) {
            case 'n': out[n++] = ' '; break;
            case 't': out[n++] = ' '; break;
            case 'r': case 'b': case 'f': break;
            case 'u': {
                unsigned u = 0;
                for (int i = 1; i <= 4 && p[i]; i++) {
                    char c = p[i];
                    u = u * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
                }
                p += 4;
                if (u < 0x80) {
                    out[n++] = (char)u;
                } else if (u < 0x800) {
                    out[n++] = (char)(0xC0 | (u >> 6));
                    out[n++] = (char)(0x80 | (u & 63));
                } else {   // (surrogate pairs: not in Russian place names)
                    out[n++] = (char)(0xE0 | (u >> 12));
                    out[n++] = (char)(0x80 | ((u >> 6) & 63));
                    out[n++] = (char)(0x80 | (u & 63));
                }
                break;
            }
            case 0: p--; break;
            default: out[n++] = *p; break;   // \" \\ \/
            }
        }
    }
    out[n] = 0;
    return n;
}

static void JWide(const char* p, wchar_t* out, int cap)
{
    char u[512];
    JStr(p, u, sizeof(u));
    if (!MultiByteToWideChar(CP_UTF8, 0, u, -1, out, cap))
        out[0] = 0;
    out[cap - 1] = 0;
}

// ---------------------------------------------------------------- requests

static void UrlEncode(const char* s, char* out, int cap)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *s && n < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}

static bool Budget()
{
    if (g_cfg.apiCount >= g_cfg.apiLimit) {
        Log("route: the day's limit of search / route requests (%d) is reached", g_cfg.apiLimit);
        return false;
    }
    g_cfg.apiCount++;
    return true;
}

// ---------------------------------------------------------------- search

static int       s_searchState;
static DWORD     s_searchSeq;
static SearchHit s_hits[kMaxHits];
static int       s_hitCount;

void SearchStart(const wchar_t* q, double lon, double lat)
{
    char u[256], e[600], path[900];
    if (!WideCharToMultiByte(CP_UTF8, 0, q, -1, u, sizeof(u), NULL, NULL) || !u[0])
        return;
    if (!Budget()) {
        s_searchState = SEARCH_FAIL;
        return;
    }
    UrlEncode(u, e, sizeof(e));
    // near the car first (the bias), Russian names as they are in OSM (no lang: the local name)
    _snprintf(path, sizeof(path), "/api/?q=%s&limit=%d&lat=%.5f&lon=%.5f", e, kMaxHits, lat, lon);
    path[sizeof(path) - 1] = 0;
    s_searchSeq = NetApi(API_SEARCH, kPhoton, true, path);
    s_searchState = SEARCH_BUSY;
    s_hitCount = 0;
}

int SearchState() { return s_searchState; }
int SearchCount() { return s_hitCount; }
const SearchHit* SearchGet(int i) { return i >= 0 && i < s_hitCount ? &s_hits[i] : NULL; }

static void Join(wchar_t* out, int cap, const wchar_t* s)
{
    if (!s[0])
        return;
    int n = (int)wcslen(out);
    if (n)
        _snwprintf(out + n, cap - n, L", %s", s);
    else
        _snwprintf(out, cap, L"%s", s);
    out[cap - 1] = 0;
}

static void OnSearch(const char* body)
{
    s_hitCount = 0;
    for (const char* f = JFirst(JKey(body, "features")); f && s_hitCount < kMaxHits; f = JNext(f)) {
        const char* pr = JKey(f, "properties");
        const char* c = JFirst(JKey(JKey(f, "geometry"), "coordinates"));
        if (!pr || !c)
            continue;
        SearchHit& h = s_hits[s_hitCount];
        h.lon = JNum(c, 0);
        h.lat = JNum(JNext(c), 0);
        wchar_t name[80], street[80], house[24], city[64], state[64];
        JWide(JKey(pr, "name"), name, 80);
        JWide(JKey(pr, "street"), street, 80);
        JWide(JKey(pr, "housenumber"), house, 24);
        JWide(JKey(pr, "city"), city, 64);
        JWide(JKey(pr, "state"), state, 64);
        wchar_t addr[112] = L"";
        if (street[0])
            _snwprintf(addr, 112, house[0] ? L"%s, %s" : L"%s", street, house);
        else if (house[0])
            _snwprintf(addr, 112, L"%s", house);
        addr[111] = 0;
        // the line: the place's name, or its address; under it the rest of the address
        h.sub[0] = 0;
        if (name[0]) {
            wcscpy(h.name, name);
            Join(h.sub, 96, addr);
        } else {
            wcsncpy(h.name, addr[0] ? addr : city, 79);
            h.name[79] = 0;
        }
        if (wcscmp(h.name, city))
            Join(h.sub, 96, city);
        if (!city[0] || wcscmp(city, state))
            Join(h.sub, 96, state);
        if (h.name[0])
            s_hitCount++;
    }
    s_searchState = SEARCH_DONE;
}

// ---------------------------------------------------------------- route

struct Maneuver {
    int     shape;          // its point on the line
    int     turn;           // RouteInfo::turn
    wchar_t text[112];
};

static int       s_state;
static DWORD     s_routeSeq;
static double*   s_pts;          // lon, lat pairs
static float*    s_cum;          // metres from the start to each point
static int       s_np;
static Maneuver* s_man;
static int       s_nm;
static double    s_timeS;        // the route's time, s (the rest: in proportion to the distance)
static SearchHit s_dest;
static int       s_seg;          // the line's segment the car is on
static double    s_along;        // metres from the start to the car
static int       s_offFixes;
static bool      s_off;
static DWORD     s_reqTick;
static DWORD     s_sig;
static wchar_t   s_stateText[64];

static void FreeRoute()
{
    free(s_pts);
    free(s_cum);
    free(s_man);
    s_pts = NULL;
    s_cum = NULL;
    s_man = NULL;
    s_np = s_nm = 0;
}

static void Ask(double fromLon, double fromLat, double course)
{
    if (!Budget()) {
        s_state = s_np ? s_state : ROUTE_FAIL;
        wcscpy(s_stateText, L"Лимит запросов на сегодня");
        s_sig++;
        return;
    }
    char head[32] = "", path[400];
    if (course >= 0)   // the way the car goes: no route starting with a U-turn
        _snprintf(head, sizeof(head), "&bearings=%d,60;", (int)course);
    _snprintf(path, sizeof(path), "/routed-car/route/v1/driving/%.6f,%.6f;%.6f,%.6f?overview=full&geometries=polyline6"
              "&steps=true%s", fromLon, fromLat, s_dest.lon, s_dest.lat, head);
    path[sizeof(path) - 1] = 0;
    s_routeSeq = NetApi(API_ROUTE, kOsrm, false, path);
    s_reqTick = GetTickCount();
    if (!s_np)
        s_state = ROUTE_BUSY;
    s_sig++;
}

void RouteTo(const SearchHit* to, double fromLon, double fromLat, double course)
{
    FreeRoute();
    s_dest = *to;
    s_off = false;
    s_offFixes = 0;
    s_stateText[0] = 0;
    Log("route: to %.5f,%.5f from %.5f,%.5f course %d", to->lat, to->lon, fromLat, fromLon, (int)course);
    Ask(fromLon, fromLat, course);
}

void RouteClear()
{
    FreeRoute();
    s_state = ROUTE_NONE;
    s_routeSeq = 0;
    s_sig++;
}

// Local metres around a latitude (the route's scale: an equirectangular plane is enough).
static double s_kx = 111320, s_ky = 110540;

static void Plane(double lat)
{
    s_kx = 111320 * cos(lat * M_PI / 180);
}

// OSRM's line (geometries=polyline6): Google's polyline encoding with 6 digits.
static int Decode(const char* s, double* out, int max)
{
    int n = 0;
    long lat = 0, lon = 0;
    while (*s && n < max) {
        long v[2];
        for (int k = 0; k < 2; k++) {
            long r = 0;
            int shift = 0, b;
            do {
                b = *s ? *s++ - 63 : 0;
                r |= (long)(b & 0x1f) << shift;
                shift += 5;
            } while (b >= 0x20);
            v[k] = r & 1 ? ~(r >> 1) : r >> 1;
        }
        lat += v[0];
        lon += v[1];
        out[n * 2] = lon / 1e6;
        out[n * 2 + 1] = lat / 1e6;
        n++;
    }
    return n;
}

// OSRM's maneuver (step.maneuver) -> RouteInfo::turn
static int Turn(const char* m, const char* type, const char* mod)
{
    if (!strcmp(type, "arrive"))
        return 999;
    if (!strcmp(type, "roundabout") || !strcmp(type, "rotary") || !strcmp(type, "roundabout turn"))
        return 998;
    if (!strcmp(mod, "uturn"))
        return strcmp(type, "turn") ? 180 : -180;
    double a = JNum(JKey(m, "bearing_before"), -1), b = JNum(JKey(m, "bearing_after"), -1);
    if (a < 0 || b < 0)
        return 0;
    int t = (int)(b - a);
    while (t > 180)
        t -= 360;
    while (t < -180)
        t += 360;
    return t;
}

static const wchar_t* Side(const char* mod)   // "направо" / "налево" ... by OSRM's modifier
{
    if (!strcmp(mod, "right"))        return L"направо";
    if (!strcmp(mod, "left"))         return L"налево";
    if (!strcmp(mod, "slight right")) return L"плавно направо";
    if (!strcmp(mod, "slight left"))  return L"плавно налево";
    if (!strcmp(mod, "sharp right"))  return L"резко направо";
    if (!strcmp(mod, "sharp left"))   return L"резко налево";
    if (!strcmp(mod, "uturn"))        return L"на разворот";
    return L"прямо";
}

static const wchar_t* Keep(const char* mod)   // "правее" / "левее" for forks and lanes
{
    return strstr(mod, "right") ? L"правее" : strstr(mod, "left") ? L"левее" : L"прямо";
}

// The instruction of a step, as a driver says it: what to do, then onto which road.
static void Instruction(const char* step, const char* m, const char* type, const char* mod, wchar_t* out, int cap)
{
    wchar_t name[80], ref[32], road[120] = L"";
    JWide(JKey(step, "name"), name, 80);
    JWide(JKey(step, "ref"), ref, 32);
    if (name[0] && ref[0])
        _snwprintf(road, 120, L"%s (%s)", name, ref);
    else
        wcscpy(road, name[0] ? name : ref);
    road[119] = 0;
    wchar_t act[64];
    if (!strcmp(type, "depart"))
        wcscpy(act, L"Начало маршрута");
    else if (!strcmp(type, "arrive"))
        wcscpy(act, L"Вы у цели");
    else if (!strcmp(type, "roundabout") || !strcmp(type, "rotary")) {
        int exit = (int)JNum(JKey(m, "exit"), 0);
        if (exit > 0)
            _snwprintf(act, 64, L"На кольце %d-й съезд", exit);
        else
            wcscpy(act, L"Въезд на кольцо");
    } else if (!strcmp(type, "exit roundabout") || !strcmp(type, "exit rotary"))
        wcscpy(act, L"Съезд с кольца");
    else if (!strcmp(type, "fork") || !strcmp(type, "use lane"))
        _snwprintf(act, 64, L"Держитесь %s", Keep(mod));
    else if (!strcmp(type, "merge"))
        _snwprintf(act, 64, L"Перестройтесь %s", Keep(mod));
    else if (!strcmp(type, "on ramp"))
        _snwprintf(act, 64, L"Въезд %s", Side(mod));
    else if (!strcmp(type, "off ramp"))
        _snwprintf(act, 64, L"Съезд %s", Side(mod));
    else if (!strcmp(type, "end of road"))
        _snwprintf(act, 64, L"В конце дороги %s", Side(mod));
    else if (!strcmp(mod, "uturn"))
        wcscpy(act, L"Развернитесь");
    else if (!strcmp(mod, "straight") || !mod[0] || !strcmp(type, "new name"))
        wcscpy(act, !strcmp(type, "new name") ? L"Продолжайте" : L"Прямо");
    else
        _snwprintf(act, 64, L"Поверните %s", Side(mod));
    act[63] = 0;
    if (road[0] && strcmp(type, "arrive"))
        _snwprintf(out, cap, L"%s: %s", act, road);
    else
        _snwprintf(out, cap, L"%s", act);
    out[cap - 1] = 0;
}

static void OnRoute(const char* body)
{
    char code[32];
    JStr(JKey(body, "code"), code, sizeof(code));
    const char* route = JFirst(JKey(body, "routes"));
    const char* geom = JKey(route, "geometry");
    if (strcmp(code, "Ok") || !geom || *geom != '"') {
        Log("route: no route: %s", code);
        if (!s_np)
            s_state = ROUTE_FAIL;
        wcscpy(s_stateText, L"Маршрут не найден");
        return;
    }
    // the line: its encoded length is a bound of the point count (each point >= 2 characters)
    const char* end = JSkip(geom);
    int cap = (int)(end - geom) / 2 + 2;
    char* enc = (char*)malloc(end - geom + 1);
    double* pts = (double*)malloc(cap * 2 * sizeof(double));
    if (!enc || !pts) {
        free(enc);
        free(pts);
        return;
    }
    JStr(geom, enc, (int)(end - geom) + 1);
    int np = Decode(enc, pts, cap);
    free(enc);
    const char* steps = JKey(JFirst(JKey(route, "legs")), "steps");
    int nm = 0;
    for (const char* st = JFirst(steps); st; st = JNext(st))
        nm++;
    float* cum = (float*)malloc((np > 0 ? np : 1) * sizeof(float));
    Maneuver* man = (Maneuver*)malloc((nm ? nm : 1) * sizeof(Maneuver));
    if (np < 2 || !cum || !man) {
        free(pts);
        free(cum);
        free(man);
        return;
    }
    FreeRoute();
    s_pts = pts;
    s_np = np;
    s_cum = cum;
    s_man = man;
    Plane(pts[1]);
    cum[0] = 0;
    for (int i = 1; i < np; i++) {
        double dx = (pts[i * 2] - pts[i * 2 - 2]) * s_kx, dy = (pts[i * 2 + 1] - pts[i * 2 - 1]) * s_ky;
        cum[i] = cum[i - 1] + (float)sqrt(dx * dx + dy * dy);
    }
    // each step's maneuver point on the line: the nearest point from the previous step's on
    int from = 0;
    for (const char* st = JFirst(steps); st && s_nm < nm; st = JNext(st)) {
        const char* m = JKey(st, "maneuver");
        char type[24], mod[24];
        JStr(JKey(m, "type"), type, sizeof(type));
        JStr(JKey(m, "modifier"), mod, sizeof(mod));
        if (!strcmp(type, "notification"))
            continue;
        const char* loc = JFirst(JKey(m, "location"));
        double mx = JNum(loc, 0) * s_kx, my = JNum(loc ? JNext(loc) : NULL, 0) * s_ky, best = 1e18;
        int at = from;
        for (int i = from; i < np; i++) {
            double dx = pts[i * 2] * s_kx - mx, dy = pts[i * 2 + 1] * s_ky - my, d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                at = i;
            }
            if (best < 1 && d > 1e6)   // found it and gone 1 km past: the line may come back later
                break;
        }
        from = at;
        Maneuver& d = man[s_nm++];
        d.shape = at;
        d.turn = Turn(m, type, mod);
        Instruction(st, m, type, mod, d.text, 112);
    }
    s_timeS = JNum(JKey(route, "duration"), 0);
    s_seg = 0;
    s_along = 0;
    s_off = false;
    s_offFixes = 0;
    s_state = ROUTE_ON;
    s_stateText[0] = 0;
    Log("route: %d points, %d maneuvers, %.1f km, %d min", np, s_nm, cum[np - 1] / 1000, (int)(s_timeS / 60));
}

bool OnApiResult(ApiResult* r)
{
    bool redraw = false;
    bool ok = r->err == NET_OK && r->httpStatus == 200 && r->body;
    if (r->kind == API_SEARCH && r->seq == s_searchSeq) {
        if (ok)
            OnSearch(r->body);
        else
            s_searchState = SEARCH_FAIL;
        redraw = true;
    } else if (r->kind == API_ROUTE && r->seq == s_routeSeq && s_state != ROUTE_NONE) {
        if (r->body && (r->httpStatus == 200 || r->httpStatus == 400))   // 400: OSRM's "NoRoute" with its code
            OnRoute(r->body);
        else {
            if (!s_np)
                s_state = ROUTE_FAIL;
            _snwprintf(s_stateText, 64, r->err == NET_OK ? L"Ошибка сервера %d" : L"Нет связи с сервером",
                       r->httpStatus);
            s_stateText[63] = 0;
        }
        s_sig++;
        redraw = true;
    }
    ApiFree(r);
    return redraw;
}

// Distance from (x, y) to segment a-b in the plane, t = where on it (0..1).
static double SegDist(double x, double y, int a, double* t)
{
    double ax = s_pts[a * 2] * s_kx, ay = s_pts[a * 2 + 1] * s_ky;
    double bx = s_pts[a * 2 + 2] * s_kx, by = s_pts[a * 2 + 3] * s_ky;
    double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double k = l2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    *t = k;
    double ex = ax + k * dx - x, ey = ay + k * dy - y;
    return sqrt(ex * ex + ey * ey);
}

void RouteGps(double lon, double lat, double kmh, double course)
{
    if (s_state != ROUTE_ON || !s_np)
        return;
    double x = lon * s_kx, y = lat * s_ky;
    // near where the car was first (the line may pass the same place twice), the whole line if far from it
    double best = 1e18, bestT = 0;
    int seg = s_seg;
    for (int pass = 0; pass < 2 && best > kOffM; pass++) {
        int a = pass ? 0 : (s_seg > 10 ? s_seg - 10 : 0), b = pass ? s_np - 1 : s_seg + 200;
        if (b > s_np - 1)
            b = s_np - 1;
        for (int i = a; i < b; i++) {
            double t, d = SegDist(x, y, i, &t);
            if (d < best) {
                best = d;
                bestT = t;
                seg = i;
            }
        }
    }
    s_seg = seg;
    s_along = s_cum[seg] + bestT * (s_cum[seg + 1] - s_cum[seg]);
    double dx = (s_dest.lon - lon) * s_kx, dy = (s_dest.lat - lat) * s_ky;
    if (sqrt(dx * dx + dy * dy) < kArriveM || (s_cum[s_np - 1] - s_along < kArriveM && best < kOffM)) {
        FreeRoute();
        s_state = ROUTE_ARRIVED;
        Log("route: arrived");
        s_sig++;
        return;
    }
    s_offFixes = best > kOffM && kmh > 5 ? s_offFixes + 1 : 0;
    bool off = s_offFixes >= kOffFixes;
    if (off && GetTickCount() - s_reqTick > kRerouteMs) {
        Log("route: off by %d m, a new route", (int)best);
        Ask(lon, lat, kmh > 10 ? course : -1);
    }
    if (off != s_off)
        s_sig++;
    s_off = off;
}

void RouteGet(RouteInfo* o)
{
    memset(o, 0, sizeof(*o));
    o->state = s_state;
    o->text = s_stateText;
    if (s_state == ROUTE_BUSY && !s_stateText[0])
        o->text = L"Расчёт маршрута…";
    else if (s_state == ROUTE_ARRIVED)
        o->text = L"Вы приехали";
    else if (s_state == ROUTE_FAIL && !s_stateText[0])
        o->text = L"Маршрут не построен";
    if (s_state != ROUTE_ON || !s_np)
        return;
    o->off = s_off;
    double total = s_cum[s_np - 1];
    o->leftM = (int)(total - s_along);
    o->leftS = total > 0 ? (int)(s_timeS * (total - s_along) / total) : 0;
    for (int i = 0; i < s_nm; i++)
        if (s_cum[s_man[i].shape] > s_along + 5 || i == s_nm - 1) {
            o->text = s_man[i].text;
            o->turn = s_man[i].turn;
            o->nextM = (int)(s_cum[s_man[i].shape] - s_along);
            if (o->nextM < 0)
                o->nextM = 0;
            break;
        }
    if (s_off)
        o->text = L"Вне маршрута, перестраиваю…";
}

int RoutePoints(const double** lonLat)
{
    *lonLat = s_pts;
    return s_state == ROUTE_ON ? s_np : 0;
}

bool RouteDest(double* lon, double* lat)
{
    if (s_state != ROUTE_ON && s_state != ROUTE_BUSY)
        return false;
    *lon = s_dest.lon;
    *lat = s_dest.lat;
    return true;
}

DWORD RouteSig()
{
    RouteInfo i;
    RouteGet(&i);
    return s_sig * 7919 + s_state * 31 + i.nextM / 10 + (DWORD)i.leftM / 100 * 131;
}
