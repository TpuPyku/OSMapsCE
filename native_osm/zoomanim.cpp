// zoomanim.cpp - the zoom's change shown smoothly. See zoomanim.h.
#include "zoomanim.h"
#include <math.h>

static double Ease(double t)   // 0..1, slow at both ends
{
    return t * t * (3 - 2 * t);
}

static double Power(const ZoomAnim* a, DWORD now)   // the scale's power of 2 at now
{
    double t = (double)(now - a->t0) / kZoomMs;
    return a->from + (a->to - a->from) * Ease(t < 1 ? t : 1);
}

void ZoomAnimStart(ZoomAnim* a, DWORD now, int dz)
{
    bool going = a->on && now - a->t0 < (DWORD)kZoomMs;   // (over by the clock though not asked yet: anew)
    double at = going ? Power(a, now) : 0;
    a->to = (going ? a->to : 0) + dz;
    a->from = at;
    a->t0 = now;
    a->on = true;
}

bool ZoomAnimScale(ZoomAnim* a, DWORD now, double* scale)
{
    if (!a->on) {
        *scale = 1;
        return false;
    }
    *scale = pow(2.0, Power(a, now));
    if (now - a->t0 >= (DWORD)kZoomMs)
        a->on = false;
    return a->on;
}

double ZoomAnimNew(const ZoomAnim* a, double scale)
{
    return scale / pow(2.0, a->to);
}

static int Round(double v)
{
    return (int)floor(v + 0.5);
}

void ZoomAnimRect(double scale, double ax, double ay, const RECT& map, RECT* dst)
{
    dst->left = Round(ax - (ax - map.left) * scale);
    dst->top = Round(ay - (ay - map.top) * scale);
    dst->right = Round(ax + (map.right - ax) * scale);
    dst->bottom = Round(ay + (map.bottom - ay) * scale);
}
