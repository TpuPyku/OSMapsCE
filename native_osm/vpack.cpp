#include "yamaps.h"

// A region's vector tiles without the internet (2026-10-27, the user: a full offline mode): OSMapsData\<region>\
// tiles.vtp, cut out of Protomaps' planet build by offline/pmextract.py (z0-15) or made by offline/make_tiles.py from
// planetiler's MBTiles (z0-14) - vtile.cpp draws both schemas. The index (sorted keys) stays in memory, a tile is read from the file when asked. The
// region: the one whose pack's bounds hold the tile's middle. Asked by the download thread only (net.cpp).

#pragma pack(push, 1)
struct PHead {
    char  magic[4];
    DWORD version, count;
    int   minLat, minLon, maxLat, maxLon;   // 1e-6 degrees
    DWORD offIndex;
};
struct PEntry { unsigned __int64 key; DWORD off, len; };   // key = z << 32 | x << 16 | y (version 2: z15's x, y)
#pragma pack(pop)

struct Pack {
    PHead   h;
    PEntry* index;
    OffFile f;
};
static Pack s_p;    // the region's pack open (one at a time)
static Pack s_ov;   // the overview pack (OSMapsData\overview\overview.vtp: the CIS's small zooms whole, the PC app's)
static int  s_ovZ = -1;      // its deepest zoom, -1 none
static DWORD s_ovTick;       // looked for then (a card may come: again after kOvMs)
enum { kOvMs = 5000 };

struct LatLon { int lat, lon; };

static bool PackHolds(const wchar_t* path, void* ctx)   // OffRegion's test: a pack whose bounds hold the point
{
    OffFile f;
    PHead h;
    bool ok = OffOpen(&f, path, &h, sizeof(h)) && !memcmp(h.magic, "OSMT", 4) && h.version == 2 &&
              OffInside(&h.minLat, ((const LatLon*)ctx)->lat, ((const LatLon*)ctx)->lon);
    OffClose(&f);
    return ok;
}

static bool OpenPath(Pack& k, const wchar_t* dir, const wchar_t* name)   // dir\name into k (closed before)
{
    wchar_t path[MAX_PATH];
    OffClose(&k.f);
    memset(&k, 0, sizeof(k));
    _snwprintf(path, MAX_PATH, L"%s\\%s", dir, name);
    path[MAX_PATH - 1] = 0;
    if (!OffOpen(&k.f, path, &k.h, sizeof(k.h)) || memcmp(k.h.magic, "OSMT", 4) || k.h.version != 2 || !k.h.count) {
        OffClose(&k.f);
        return false;
    }
    k.index = (PEntry*)OffPart(&k.f, k.h.offIndex, k.h.count * sizeof(PEntry));
    if (!k.index) {
        Log("vpack: cannot load %S (memory?)", path);
        OffClose(&k.f);
        memset(&k, 0, sizeof(k));
        return false;
    }
    Log("vpack: %S: %lu tiles", path, k.h.count);
    return true;
}

static bool Open(const LatLon& p)
{
    if (s_p.index && OffInside(&s_p.h.minLat, p.lat, p.lon))
        return true;
    wchar_t dir[MAX_PATH];
    return OffRegion(L"tiles.vtp", PackHolds, (void*)&p, dir) && OpenPath(s_p, dir, L"tiles.vtp");
}

static const PEntry* Find(const Pack& k, unsigned __int64 key)
{
    DWORD lo = 0, hi = k.h.count;
    while (lo < hi) {
        DWORD m = (lo + hi) / 2;
        if (k.index[m].key < key)
            lo = m + 1;
        else
            hi = m;
    }
    return lo < k.h.count && k.index[lo].key == key ? &k.index[lo] : NULL;
}

static bool Read(const Pack& k, const MapRequest& d, unsigned char** data, int* len)   // d's tile from k
{
    const PEntry* e = Find(k, (unsigned __int64)(d.z - VEC_ZBASE) << 32 | (unsigned __int64)d.x << 16 | (DWORD)d.y);
    if (!e)
        return false;
    *data = (unsigned char*)malloc(e->len ? e->len : 1);
    if (*data && OffRead(k.f.file, e->off, *data, e->len)) {
        *len = (int)e->len;
        return true;
    }
    free(*data);
    *data = NULL;
    return false;
}

static bool Exists(const wchar_t* path, void*)   // OffRegion's test: the file is there
{
    return GetFileAttributes(path) != 0xFFFFFFFF;
}

int VOverviewZ()   // (its index sorted by key: the last one's zoom the deepest)
{
    if (s_ovZ >= 0 || (s_ovTick && GetTickCount() - s_ovTick < kOvMs))
        return s_ovZ;
    s_ovTick = GetTickCount() | 1;
    wchar_t dir[MAX_PATH];
    if (OffRegion(L"overview.vtp", Exists, NULL, dir) && OpenPath(s_ov, dir, L"overview.vtp"))
        s_ovZ = (int)(s_ov.index[s_ov.h.count - 1].key >> 32);
    return s_ovZ;
}

bool VOverviewTile(const MapRequest& d, unsigned char** data, int* len)
{
    return d.z - VEC_ZBASE <= VOverviewZ() && Read(s_ov, d, data, len);
}

static LatLon Middle(int z, int x, int y)   // the tile's middle, 1e-6 degrees
{
    double n = (double)(1 << z), m = M_PI * (1 - 2 * (y + 0.5) / n);
    LatLon p = { (int)(atan(0.5 * (exp(m) - exp(-m))) * 180 / M_PI * 1e6), (int)(((x + 0.5) / n * 360 - 180) * 1e6) };
    return p;
}

bool VPackTile(const MapRequest& d, unsigned char** data, int* len)
{
    int z = d.z - VEC_ZBASE;
    return (unsigned)z <= VEC_PACKZ && Open(Middle(z, d.x, d.y)) && Read(s_p, d, data, len);
}
