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
static Pack s_p;   // the region's pack open (one at a time)

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

static bool Open(const LatLon& p)
{
    if (s_p.index && OffInside(&s_p.h.minLat, p.lat, p.lon))
        return true;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!OffRegion(L"tiles.vtp", PackHolds, (void*)&p, dir))
        return false;
    OffClose(&s_p.f);
    memset(&s_p, 0, sizeof(s_p));
    _snwprintf(path, MAX_PATH, L"%s\\tiles.vtp", dir);
    path[MAX_PATH - 1] = 0;
    if (!OffOpen(&s_p.f, path, &s_p.h, sizeof(s_p.h)))
        return false;
    s_p.index = (PEntry*)OffPart(&s_p.f, s_p.h.offIndex, s_p.h.count * sizeof(PEntry));
    if (!s_p.index) {
        Log("vpack: cannot load %S (memory?)", path);
        OffClose(&s_p.f);
        memset(&s_p, 0, sizeof(s_p));
        return false;
    }
    Log("vpack: %S: %lu tiles", path, s_p.h.count);
    return true;
}

static const PEntry* Find(unsigned __int64 key)
{
    DWORD lo = 0, hi = s_p.h.count;
    while (lo < hi) {
        DWORD m = (lo + hi) / 2;
        if (s_p.index[m].key < key)
            lo = m + 1;
        else
            hi = m;
    }
    return lo < s_p.h.count && s_p.index[lo].key == key ? &s_p.index[lo] : NULL;
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
    if ((unsigned)z > VEC_PACKZ || !Open(Middle(z, d.x, d.y)))
        return false;
    const PEntry* e = Find((unsigned __int64)z << 32 | (unsigned __int64)d.x << 16 | (DWORD)d.y);
    if (!e)
        return false;
    *data = (unsigned char*)malloc(e->len ? e->len : 1);
    if (*data && OffRead(s_p.f.file, e->off, *data, e->len)) {
        *len = (int)e->len;
        return true;
    }
    free(*data);
    *data = NULL;
    return false;
}
