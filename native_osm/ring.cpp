#include "yamaps.h"

// The third person's frame (the user 2026-10-27..28; the projection: tilt.cpp) out of the map's rings: the near ring
// (the view's zoom, main.cpp's) where it is loaded, the far one (two zooms out) past it - the fog's ~1150 px ahead are
// more than the near ring holds. Each screen row is one row of the flat map turned by the course: a walk along a line in
// 16.16 fixed point (offsets from the view's centre: small numbers), the first ring having the pixel gives it, none:
// the fog's colour; the fog laid over the far rows by its fade.

static inline unsigned short Blend(unsigned short a, unsigned short b, int alpha32)   // (inlined in the walks)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (unsigned short)((ar + (br - ar) * alpha32 / 32) << 11 | (ag + (bg - ag) * alpha32 / 32) << 5 |
                            (ab + (bb - ab) * alpha32 / 32));
}

unsigned short Blend565(unsigned short a, unsigned short b, int alpha32)
{
    return Blend(a, b, alpha32);
}

static const double kFix = 1 << 16;   // 16.16 fixed point

// A ring as the walk reads it: its loaded part's first column and row (its px) less their remainder by the ring's size
// - a pixel's place in the ring then by subtraction, no division per pixel (HU sheet 30: the 3D frame 60-85 ms; ARMv4
// has no divide, two "%" a pixel of ~320 thousand)
struct RingFast { RingView v; int bx, by; };

static RingFast Fast(const RingView& r)
{
    RingFast f = { r, (r.x0 >> r.shift) - (r.x0 >> r.shift) % r.size, (r.y0 >> r.shift) - (r.y0 >> r.shift) % r.size };
    return f;
}

static int Wrapped(int v, int size)   // v >= 0; the loaded part is no wider than the ring: once at most there
{
    while (v >= size)
        v -= size;
    return v;
}

// The pixel of world px (x, y) of the view's zoom from the first ring loaded there; else the fog's colour
static unsigned short RingPixel(const RingFast* rings, int n, int x, int y, unsigned short fog)
{
    for (int i = 0; i < n; i++) {
        const RingView& r = rings[i].v;
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
            return r.top[Wrapped((y >> r.shift) - rings[i].by, r.size) * r.stride +
                         Wrapped((x >> r.shift) - rings[i].bx, r.size)];
    }
    return fog;
}

struct RowWalk { int bx, by; long ox, oy, dx, dy; };   // the view's centre (whole px), the walk in 16.16 from it

// Narrows [*lo, *hi] to the steps x whose (o + x * d) / 2^16 lies in [a, b]
static void Keep(long o, long d, double a, double b, double* lo, double* hi)
{
    double p = o / kFix, q = d / kFix;
    if (q == 0) {
        if (p < a || p > b)
            *hi = *lo - 1;
        return;
    }
    double t0 = (a - p) / q, t1 = (b - p) / q;
    if (q < 0) {   // (walking back: b reached first)
        double t = t0;
        t0 = t1;
        t1 = t;
    }
    *lo = t0 > *lo ? t0 : *lo;
    *hi = t1 < *hi ? t1 : *hi;
}

// The row's steps surely inside the first ring's loaded part (a pixel inside its edges): [*lo, *hi); Shturmann
// 2026-11-12 - the 3D frame 80-158 ms: those need no ring's check a pixel
static void Inside(const RingFast& f, const RowWalk& w, int width, int* lo, int* hi)
{
    const RingView& r = f.v;
    double l = 0, h = width - 1;
    Keep(w.ox, w.dx, r.x0 - w.bx + 1, r.x1 - w.bx - 2, &l, &h);
    Keep(w.oy, w.dy, r.y0 - w.by + 1, r.y1 - w.by - 2, &l, &h);
    *lo = (int)ceil(l);
    *hi = h < l ? *lo : (int)floor(h) + 1;
}

// Steps from..to - 1 through each ring's check
static void WalkAny(const RingFast* rings, int n, RowWalk* w, int from, int to, unsigned short fog, int alpha, unsigned short* d)
{
    long ox = w->ox, oy = w->oy;   // (locals: see WalkNear)
    const long dx = w->dx, dy = w->dy;
    const int bx = w->bx, by = w->by;
    for (int x = from; x < to; x++, ox += dx, oy += dy) {
        unsigned short p = RingPixel(rings, n, bx + (int)(ox >> 16), by + (int)(oy >> 16), fog);
        d[x] = alpha ? Blend(p, fog, alpha) : p;
    }
    w->ox = ox;
    w->oy = oy;
}

// ... all in the first ring
static void WalkNear(const RingFast& f, RowWalk* w, int from, int to, unsigned short fog, int alpha, unsigned short* d)
{
    // (all in locals: VS2005 keeps no field of a struct in a register past a store through a pointer - each pixel
    // read and wrote them, the emulator's 3D frame 62 ms)
    const unsigned short* top = f.v.top;
    const int stride = f.v.stride, size = f.v.size, shift = f.v.shift, bx = w->bx, by = w->by, fbx = f.bx, fby = f.by;
    long ox = w->ox, oy = w->oy;
    const long dx = w->dx, dy = w->dy;
    if (!alpha && !shift) {   // (the rows by the car: the ring of the view's zoom, no fog - the most of the frame;
        // the loaded part no wider than the ring: one wrap at most)
        const int ub = bx - fbx, vb = by - fby;
        for (int x = from; x < to; x++, ox += dx, oy += dy) {
            int u = ub + (int)(ox >> 16), v = vb + (int)(oy >> 16);
            if (u >= size)
                u -= size;
            if (v >= size)
                v -= size;
            d[x] = top[v * stride + u];
        }
        w->ox = ox;
        w->oy = oy;
        return;
    }
    for (int x = from; x < to; x++, ox += dx, oy += dy) {
        int u = Wrapped(((bx + (int)(ox >> 16)) >> shift) - fbx, size), v = Wrapped(((by + (int)(oy >> 16)) >> shift) - fby, size);
        unsigned short p = top[v * stride + u];
        d[x] = alpha ? Blend(p, fog, alpha) : p;
    }
    w->ox = ox;
    w->oy = oy;
}

static void WarpRow(const RingFast* rings, int n, const RowWalk& w0, int width, unsigned short fog, int alpha,
                    unsigned short* d)
{
    RowWalk w = w0;
    int lo, hi;
    Inside(rings[0], w, width, &lo, &hi);
    lo = lo < 0 ? 0 : lo > width ? width : lo;
    hi = hi < lo ? lo : hi > width ? width : hi;
    WalkAny(rings, n, &w, 0, lo, fog, alpha, d);
    WalkNear(rings[0], &w, lo, hi, fog, alpha, d);
    WalkAny(rings, n, &w, hi, width, fog, alpha, d);
}

void TiltWarp(const RingView* rings, int n, const Tilt& t, int w, int h, double vx, double vy, double angleDeg, bool night,
              unsigned short* dst, int dstride)
{
    double a = angleDeg * M_PI / 180, ca = cos(a), sa = sin(a);
    RingFast fast[4];
    n = n < 4 ? n : 4;
    for (int i = 0; i < n; i++)
        fast[i] = Fast(rings[i]);
    RowWalk walk;
    walk.bx = (int)floor(vx);
    walk.by = (int)floor(vy);
    for (int y = 0; y < h; y++) {
        double sy = y - h / 2.0, fy, k, fade;
        unsigned char c[3];
        TiltFog(t, sy, night, c);
        unsigned short fog = RGB565(c[0], c[1], c[2]);
        unsigned short* d = dst + y * dstride;
        if (!TiltRow(t, sy, &fy, &k, &fade)) {
            for (int x = 0; x < w; x++)
                d[x] = fog;
            continue;
        }
        double fx = -w / 2.0 * k;   // the row's first pixel on the flat map, turned: the world's offset
        walk.ox = (long)floor((fx * ca - fy * sa + vx - walk.bx) * kFix);
        walk.oy = (long)floor((fx * sa + fy * ca + vy - walk.by) * kFix);
        walk.dx = (long)(k * ca * kFix);
        walk.dy = (long)(k * sa * kFix);
        WarpRow(fast, n, walk, w, fog, (int)(fade * 32 + 0.5), d);
    }
}

// The far ring's centre (the user 2026-10-31: the view's own zoom - two zooms out was blurred and its roads too wide):
// d px ahead of the view's centre by the course, the screen's up turned as TiltWarp turns it
void RingAhead(double vx, double vy, double angleDeg, double d, double* x, double* y)
{
    double a = angleDeg * M_PI / 180;
    *x = vx + d * sin(a);
    *y = vy - d * cos(a);
}

// A slot showing its exact tile keeps it while only the memory let the tile go (the same night and traffic): a coarser
// one would be worse, and the two rings' tiles don't all fit the memory cache at once
bool SlotRedraw(const SlotShown& s, int x, int y, DWORD sig, DWORD ctx, bool exact)
{
    if (!s.used || s.x != x || s.y != y)
        return true;
    if (s.exact && !exact && s.ctx == ctx)
        return false;
    return s.sig != sig;
}

// The memory's cap (the user 2026-11-02: in 3D the traffic was asked again and again - the rings' tiles, more than the
// cap, pushed it out; it came, the slots were drawn again, their tiles came from the disk and pushed it out again): a
// base tile comes back from the disk, the traffic only from the network and its budget - it stays while used lately.
enum { kTrafficKeepMs = 30000 };

// The images' cap (the user 2026-11-03: black squares in 3D at night - the two rings' 61 tiles never fit 6 MB): the
// unit (275 MB free) twice that, a small device (Shturmann) as before
int CacheCapFor(DWORD freeKb)
{
    return (freeKb >= 96 * 1024 ? 12 : 6) * 1024 * 1024;
}

static int Older(const CacheEntry* e, int best, int i)   // i, when used less recently than best (-1: none yet)
{
    return best < 0 || e[i].useTick < e[best].useTick ? i : best;
}

int CacheVictim(const CacheEntry* e, int n, int keep, DWORD now)
{
    int spare = -1, any = -1;
    for (int i = 0; i < n; i++) {
        if (i == keep)
            continue;
        any = Older(e, any, i);
        if (e[i].req.src == SRC_OSM || now - e[i].useTick >= kTrafficKeepMs)
            spare = Older(e, spare, i);
    }
    return spare >= 0 ? spare : any;
}
