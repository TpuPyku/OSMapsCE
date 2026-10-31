#include "yamaps.h"

// The third person's frame (the user 2026-10-27..28; the projection: tilt.cpp) out of the map's rings: the near ring
// (the view's zoom, main.cpp's) where it is loaded, the far one (two zooms out) past it - the fog's ~1150 px ahead are
// more than the near ring holds. Each screen row is one row of the flat map turned by the course: a walk along a line in
// 16.16 fixed point (offsets from the view's centre: small numbers), the first ring having the pixel gives it, none:
// the fog's colour; the fog laid over the far rows by its fade.

unsigned short Blend565(unsigned short a, unsigned short b, int alpha32)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (unsigned short)((ar + (br - ar) * alpha32 / 32) << 11 | (ag + (bg - ag) * alpha32 / 32) << 5 |
                            (ab + (bb - ab) * alpha32 / 32));
}

static const double kFix = 1 << 16;   // 16.16 fixed point

// The pixel of world px (x, y) of the view's zoom from the first ring loaded there; else the fog's colour
static unsigned short RingPixel(const RingView* rings, int n, int x, int y, unsigned short fog)
{
    for (int i = 0; i < n; i++) {
        const RingView& r = rings[i];
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
            return r.top[(y >> r.shift) % r.size * r.stride + (x >> r.shift) % r.size];   // (world px: >= 0)
    }
    return fog;
}

struct RowWalk { int bx, by; long ox, oy, dx, dy; };   // the view's centre (whole px), the walk in 16.16 from it

static void WarpRow(const RingView* rings, int n, const RowWalk& w0, int width, unsigned short fog, int alpha,
                    unsigned short* d)
{
    RowWalk w = w0;
    for (int x = 0; x < width; x++, w.ox += w.dx, w.oy += w.dy) {
        unsigned short p = RingPixel(rings, n, w.bx + (int)(w.ox >> 16), w.by + (int)(w.oy >> 16), fog);
        d[x] = alpha ? Blend565(p, fog, alpha) : p;
    }
}

void TiltWarp(const RingView* rings, int n, const Tilt& t, int w, int h, double vx, double vy, double angleDeg, bool night,
              unsigned short* dst, int dstride)
{
    double a = angleDeg * M_PI / 180, ca = cos(a), sa = sin(a);
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
        WarpRow(rings, n, walk, w, fog, (int)(fade * 32 + 0.5), d);
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
