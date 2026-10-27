#include "yamaps.h"

// Offline search (2026-10-27, the user: a full offline mode): a region's index (OSMapsData\<region>\search.idx, made
// by offline/make_search.py) answers Photon's requests - the search and the address of a point - as Photon's GeoJSON,
// so route.cpp reads them unchanged. The region: the one whose graph holds the point (OfflineDir). Names are matched by
// their normalized form (lower case, ё = е, only letters and digits; make_search.py's norm()): every word of the
// query begins a word of the name. A query's first word starting with a digit and the rest after it: the house.

#pragma pack(push, 1)
struct SHead {
    char  magic[4];
    DWORD version, places, streets, houses, pois, strBytes, gw, gh;
    int   cell, minLat, minLon;
    DWORD items;
    DWORD off[6];   // strings, places, streets, houses, pois, grid
};
struct SPlace  { int lat, lon; DWORD name, norm, rank; };
struct SStreet { int lat, lon; DWORD name, norm, city, first, count; };
struct SHouse  { int lat, lon; DWORD street, number; };
struct SPoi    { int lat, lon; DWORD name, norm, city, what, street, number; };
#pragma pack(pop)

struct Index {
    wchar_t  dir[MAX_PATH];
    SHead    h;
    char*    str;
    SPlace*  places;
    SStreet* streets;
    SPoi*    pois;
    OffFile  d;      // the parts above; the houses and their grid are read from its file
};
static Index s_x;

static void Unload()
{
    OffClose(&s_x.d);
    memset(&s_x, 0, sizeof(s_x));
}

static bool ReadAt(DWORD off, void* to, DWORD bytes)
{
    return OffRead(s_x.d.file, off, to, bytes);
}

// The index of the region at a point: loaded; false = none.
static bool Load(int lat, int lon)
{
    int pt[2] = { lat, lon };
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!OfflineDir(pt, 1, dir))
        return false;
    if (s_x.str && !wcscmp(s_x.dir, dir))
        return true;
    Unload();
    _snwprintf(path, MAX_PATH, L"%s\\search.idx", dir);
    path[MAX_PATH - 1] = 0;
    if (!OffOpen(&s_x.d, path, &s_x.h, sizeof(s_x.h)) || memcmp(s_x.h.magic, "OSMS", 4) || s_x.h.version != 1) {
        Unload();
        return false;
    }
    const SHead& h = s_x.h;
    s_x.str = (char*)OffPart(&s_x.d, h.off[0], h.strBytes);
    s_x.places = (SPlace*)OffPart(&s_x.d, h.off[1], h.places * sizeof(SPlace));
    s_x.streets = (SStreet*)OffPart(&s_x.d, h.off[2], h.streets * sizeof(SStreet));
    s_x.pois = (SPoi*)OffPart(&s_x.d, h.off[4], h.pois * sizeof(SPoi));
    if (s_x.d.bad) {
        Log("offsearch: cannot load %S", path);
        Unload();
        return false;
    }
    wcscpy(s_x.dir, dir);
    Log("offsearch: %S: %lu places, %lu streets, %lu houses, %lu pois", path, h.places, h.streets, h.houses, h.pois);
    return true;
}

// ---------------------------------------------------------------- the query

static wchar_t Lower(wchar_t c)   // Latin and Russian lower case, ё -> е
{
    if (c == 0x401 || c == 0x451)
        return 0x435;
    bool upper = (c >= L'A' && c <= L'Z') || (c >= 0x410 && c <= 0x42F);
    return upper ? (wchar_t)(c + 32) : c;
}

static bool IsWordChar(wchar_t c)   // a lower case letter (Latin, Russian) or a digit
{
    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || (c >= 0x430 && c <= 0x44F);
}

// make_search.py's norm(): lower case, ё -> е, letters (Latin, Russian) and digits, the rest one space
static void Norm(const wchar_t* s, wchar_t* out, int cap)
{
    int n = 0;
    bool space = true;
    for (; *s && n < cap - 1; s++) {
        wchar_t c = Lower(*s);
        if (IsWordChar(c)) {
            out[n++] = c;
            space = false;
        } else if (!space) {
            out[n++] = L' ';
            space = true;
        }
    }
    while (n && out[n - 1] == L' ')
        n--;
    out[n] = 0;
}

// The street's kind words: not needed to find it ("ул ленина" = "ленина")
static const wchar_t* kStop[] = { L"ул", L"улица", L"пр", L"пр-т", L"проспект", L"пер", L"переулок", L"ш", L"шоссе",
                                  L"б-р", L"бульвар", L"пл", L"площадь", L"проезд", L"д", L"дом", L"г", L"город",
                                  L"с", L"село", L"п", L"пос", L"поселок", L"наб", L"набережная", L"мкр" };

enum { kTok = 8 };
struct Query {
    char tok[kTok][64];   // the name's words, normalized, UTF-8
    int  nt;
    char house[48];       // the house, normalized ("15 к2"), "" none
    char all[200];        // the name's words joined (exact matches)
};

static bool IsStop(const wchar_t* t)
{
    for (int i = 0; i < (int)(sizeof(kStop) / sizeof(kStop[0])); i++)
        if (!wcscmp(t, kStop[i]))
            return true;
    return false;
}

static bool HouseStarts(const wchar_t* t, const Query& q, bool last)   // a number after the name, or alone at the end
{
    return *t >= L'0' && *t <= L'9' && (q.nt || last);
}

static void Join(char* to, size_t cap, const char* word)   // the words joined by a space
{
    if (to[0])
        strncat(to, " ", cap - strlen(to) - 1);
    strncat(to, word, cap - strlen(to) - 1);
}

static void AddWord(Query* q, const wchar_t* t, bool inHouse)   // a word of the house, else of the name (not a kind)
{
    char u[64];
    WideCharToMultiByte(CP_UTF8, 0, t, -1, u, 64, NULL, NULL);
    u[63] = 0;
    if (inHouse)
        Join(q->house, sizeof(q->house), u);
    else if (!IsStop(t) && q->nt < kTok) {
        strcpy(q->tok[q->nt++], u);
        Join(q->all, sizeof(q->all), u);
    }
}

static void ParseQuery(const char* utf8, Query* q)
{
    wchar_t w[200], n[200];
    memset(q, 0, sizeof(*q));
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 200))
        return;
    w[199] = 0;
    Norm(w, n, 200);
    bool inHouse = false;
    for (wchar_t* t = n; *t;) {
        wchar_t* e = wcschr(t, L' ');
        if (e)
            *e = 0;
        inHouse = inHouse || HouseStarts(t, *q, !e);
        AddWord(q, t, inHouse);
        if (!e)
            break;
        t = e + 1;
    }
}

// Every word of the query begins a word of the name.
static bool Match(const char* name, const Query& q)
{
    for (int i = 0; i < q.nt; i++) {
        int len = (int)strlen(q.tok[i]);
        bool found = false;
        for (const char* p = name; *p && !found;) {
            if (!strncmp(p, q.tok[i], len))
                found = true;
            const char* sp = strchr(p, ' ');
            p = sp ? sp + 1 : p + strlen(p);
        }
        if (!found)
            return false;
    }
    return true;
}

static void NormUtf8(const char* s, char* out, int cap)   // a house number from the index, as the query's
{
    wchar_t w[64], n[64];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 64);
    w[63] = 0;
    Norm(w, n, 64);
    WideCharToMultiByte(CP_UTF8, 0, n, -1, out, cap, NULL, NULL);
    out[cap - 1] = 0;
}

// ---------------------------------------------------------------- the answer (Photon's GeoJSON)

enum { H_HOUSE, H_PLACE, H_STREET, H_POI };
struct Hit { int kind, lat, lon; DWORD name, street, number, city, what; double score; };

static double Km(int lat0, int lon0, int lat1, int lon1)
{
    double dy = (lat1 - lat0) * 0.000111195, dx = (lon1 - lon0) * 0.000111195 * cos(lat0 / 1e6 * M_PI / 180);
    return sqrt(dx * dx + dy * dy);
}

static void AddHit(Hit* hits, int* n, int cap, const Hit& h)
{
    if (*n < cap) {
        hits[(*n)++] = h;
        return;
    }
    int worst = 0;
    for (int i = 1; i < cap; i++)
        if (hits[i].score > hits[worst].score)
            worst = i;
    if (h.score < hits[worst].score)
        hits[worst] = h;
}

static int CmpHit(const void* a, const void* b)
{
    double d = ((const Hit*)a)->score - ((const Hit*)b)->score;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void PutField(JBuf& o, const char* key, const char* s, bool comma)
{
    JPut(o, comma ? ",\"" : "\"");
    JPut(o, key);
    JPut(o, "\":");
    JPutStr(o, s, (int)strlen(s));
}

static const char* S(DWORD off)
{
    return off < s_x.h.strBytes ? s_x.str + off : "";
}

static void PutHits(JBuf& o, const Hit* hits, int n)
{
    JPut(o, "{\"features\":[");
    for (int i = 0; i < n; i++) {
        const Hit& h = hits[i];
        char t[96];
        _snprintf(t, sizeof(t), "%s{\"geometry\":{\"coordinates\":[%.6f,%.6f]},\"properties\":{", i ? "," : "",
                  h.lon / 1e6, h.lat / 1e6);
        JPut(o, t);
        char name[200];
        if (h.kind == H_POI && h.what) {   // "Лукойл (АЗС)"
            _snprintf(name, sizeof(name), "%s (%s)", S(h.name), S(h.what));
            name[sizeof(name) - 1] = 0;
        } else
            strncpy(name, S(h.name), sizeof(name) - 1), name[sizeof(name) - 1] = 0;
        PutField(o, "name", name, false);
        PutField(o, "street", h.street != 0xFFFFFFFF ? S(s_x.streets[h.street].name) : "", true);
        PutField(o, "housenumber", S(h.number), true);
        PutField(o, "city", S(h.city), true);
        JPut(o, "}}");
    }
    JPut(o, "]}");
}

static bool ReadHouse(DWORD i, SHouse* hs)
{
    return ReadAt(s_x.h.off[3] + i * sizeof(SHouse), hs, sizeof(SHouse));
}

static bool Search(const char* q8, int lat, int lon, char** body, int* len)
{
    Query q;
    ParseQuery(q8, &q);
    if (!q.nt)
        return false;
    static Hit hits[32];
    int n = 0;
    const SHead& h = s_x.h;
    // places: an exact name first (by size), then the words' matches
    for (DWORD i = 0; i < h.places; i++) {
        const SPlace& p = s_x.places[i];
        if (!Match(S(p.norm), q))
            continue;
        Hit x = { H_PLACE, p.lat, p.lon, p.name, 0xFFFFFFFF, 0, 0, 0, 0 };
        bool exact = !strcmp(S(p.norm), q.all);
        x.score = (exact ? -500 : 50) + (int)p.rank * 20 + Km(lat, lon, p.lat, p.lon) / 10;
        if (!q.house[0])
            AddHit(hits, &n, 32, x);
    }
    // streets, and their house when one is asked
    for (DWORD i = 0; i < h.streets; i++) {
        const SStreet& s = s_x.streets[i];
        if (!Match(S(s.norm), q))
            continue;
        double km = Km(lat, lon, s.lat, s.lon);
        bool houseFound = false;
        if (q.house[0] && s.count) {
            for (DWORD k = 0; k < s.count && k < 4000; k++) {
                SHouse hs;
                char num[48];
                if (!ReadHouse(s.first + k, &hs))
                    break;
                NormUtf8(S(hs.number), num, sizeof(num));
                if (!strcmp(num, q.house)) {
                    Hit x = { H_HOUSE, hs.lat, hs.lon, 0, i, hs.number, s.city, 0, -1000 + km };
                    AddHit(hits, &n, 32, x);
                    houseFound = true;
                    break;
                }
            }
        }
        if (!houseFound) {
            Hit x = { H_STREET, s.lat, s.lon, s.name, 0xFFFFFFFF, 0, s.city, 0, 100 + km };
            AddHit(hits, &n, 32, x);
        }
    }
    // places to go: by their name or their kind ("азс")
    for (DWORD i = 0; i < h.pois; i++) {
        const SPoi& p = s_x.pois[i];
        char what[64];
        NormUtf8(S(p.what), what, sizeof(what));
        if (!Match(S(p.norm), q) && !Match(what, q))
            continue;
        Hit x = { H_POI, p.lat, p.lon, p.name, p.street, p.number, p.city, p.what, 150 + Km(lat, lon, p.lat, p.lon) };
        AddHit(hits, &n, 32, x);
    }
    if (!n)
        return false;
    qsort(hits, n, sizeof(Hit), CmpHit);
    JBuf o = { NULL, 0, 0 };
    PutHits(o, hits, n < 7 ? n : 7);
    *body = o.p;
    *len = o.n;
    return o.p != NULL;
}

// The address of a point: the nearest house within 80 m, else the nearest street (its middle) within 400 m, else
// the settlement.
static bool Reverse(int lat, int lon, char** body, int* len)
{
    const SHead& h = s_x.h;
    Hit best = { -1 };
    best.score = 1e18;
    int cy = (lat - h.minLat) / h.cell, cx = (lon - h.minLon) / h.cell;
    for (int y = cy - 1; y <= cy + 1; y++) {
        if (y < 0 || y >= (int)h.gh)
            continue;
        int x0 = cx - 1 < 0 ? 0 : cx - 1, x1 = cx + 1 >= (int)h.gw ? h.gw - 1 : cx + 1;
        if (x0 > x1)
            continue;
        DWORD cs[4];
        if (!ReadAt(h.off[5] + (y * h.gw + x0) * 4, cs, (x1 - x0 + 2) * 4))
            continue;
        DWORD from = cs[0], to = cs[x1 - x0 + 1];
        for (DWORD k = from; k < to && k < from + 2000; k++) {
            DWORD hi;
            SHouse hs;
            if (!ReadAt(h.off[5] + (h.gw * h.gh + 1 + k) * 4, &hi, 4) || !ReadHouse(hi, &hs))
                break;
            double km = Km(lat, lon, hs.lat, hs.lon);
            if (km < 0.08 && km < best.score) {
                Hit x = { H_HOUSE, hs.lat, hs.lon, 0, hs.street, hs.number, s_x.streets[hs.street].city, 0, km };
                best = x;
            }
        }
    }
    char road[200];   // no house: the road under the point (the graph's, as a route's), its settlement from here
    if (best.kind < 0 && OfflineRoadAt(lat, lon, 400, road, sizeof(road)))
        for (DWORD i = 0; i < h.streets; i++) {
            const SStreet& s = s_x.streets[i];
            double km = Km(lat, lon, s.lat, s.lon);
            if (!strcmp(S(s.name), road) && km < best.score) {   // of that name, the nearest one
                Hit x = { H_STREET, lat, lon, s.name, 0xFFFFFFFF, 0, s.city, 0, km };
                best = x;
            }
        }
    if (best.kind < 0)
        for (DWORD i = 0; i < h.places; i++) {
            const SPlace& p = s_x.places[i];
            double km = Km(lat, lon, p.lat, p.lon);
            if (p.rank <= 3 && km < 5 && km < best.score) {
                Hit x = { H_PLACE, lat, lon, p.name, 0xFFFFFFFF, 0, 0, 0, km };
                best = x;
            }
        }
    if (best.kind < 0)
        return false;
    JBuf o = { NULL, 0, 0 };
    PutHits(o, &best, 1);
    *body = o.p;
    *len = o.n;
    return o.p != NULL;
}

// ---------------------------------------------------------------- Photon's requests

static void UrlDecode(const char* s, char* out, int cap)
{
    int n = 0;
    for (; *s && *s != '&' && n < cap - 1; s++) {
        if (*s == '+')
            out[n++] = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            out[n++] = (char)strtol(hex, NULL, 16);
            s += 2;
        } else
            out[n++] = *s;
    }
    out[n] = 0;
}

static double Arg(const char* path, const char* key, double def)
{
    const char* p = strstr(path, key);
    return p ? atof(p + strlen(key)) : def;
}

// "/api/?q=...&limit=7&lat=..&lon=.." or "/reverse?lon=..&lat=..&limit=1": answered from the region's index, as
// Photon's; false: no region there, or nothing found - Photon is asked.
bool OfflineSearch(const char* path, char** body, int* len)
{
    DWORD t0 = GetTickCount();
    bool rev = !strncmp(path, "/reverse", 8);
    double lat = Arg(path, rev ? "?lon=" : "&lat=", 0), lon = 0;
    if (rev) {
        lon = lat;
        lat = Arg(path, "&lat=", 0);
    } else
        lon = Arg(path, "&lon=", 0);
    int la = (int)(lat * 1e6), lo = (int)(lon * 1e6);
    if (!Load(la, lo))
        return false;
    bool ok;
    if (rev)
        ok = Reverse(la, lo, body, len);
    else {
        const char* q = strstr(path, "q=");
        char text[300];
        UrlDecode(q ? q + 2 : "", text, sizeof(text));
        ok = Search(text, la, lo, body, len);
    }
    Log("offsearch: %s -> %s (%lu ms)", rev ? "reverse" : "search", ok ? "found" : "nothing", GetTickCount() - t0);
    return ok;
}
