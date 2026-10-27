#include "yamaps.h"

// The third person's view (the user 2026-10-27): a camera behind and over the car looking ahead, its angle from
// straight down by the zoom (z16 less, z17 more, ~70 degrees). A flat point v px ahead of the car (v < 0: behind) goes
// to the screen row r = rc * f / (v + f) below the horizon (the car's row: rc), sideways scaled by r / rc - so the car
// keeps its size, the far is higher and smaller; at the car the depth is shortened by rc / f = cot(pitch). The
// visibility: the fog, a circle of radius fog around the car ("the fog of war": the map's ring of tiles in memory is
// enough, nothing more is downloaded); f = 1.25 fog keeps v + f > 0 inside it. A screen row is one flat row, its x a
// scale of the screen's - the turned frame (main.cpp RotateRing) stays a walk along a line per row, in integers.

const Tilt* g_tilt;

static const double kDepth = 1.25;   // f / fog
static const double kFade = 0.8;     // the fog starts fading in at this part of its radius

void TiltMake(Tilt* t, double carFlat, double carScreen, double fogPx, double pitchDeg)
{
    t->cf = carFlat;
    t->cs = carScreen;
    t->fog = fogPx;
    t->f = kDepth * fogPx;
    t->rc = t->f / tan(pitchDeg * M_PI / 180);
}

bool TiltPoint(const Tilt& t, double fx, double fy, double* sx, double* sy)
{
    double v = t.cf - fy;
    if (fx * fx + v * v >= t.fog * t.fog)
        return false;
    double r = t.rc * t.f / (v + t.f);
    *sx = fx * r / t.rc;
    *sy = t.cs - t.rc + r;
    return true;
}

static double HalfAt(double radius, double v, double k)   // the circle's half-width at depth v, on the screen
{
    return radius > fabs(v) ? sqrt(radius * radius - v * v) / k : 0;
}

bool TiltRow(const Tilt& t, double sy, double* fy, double* k, double* half, double* inner)
{
    double r = sy - (t.cs - t.rc);
    if (r <= 0)
        return false;
    double v = t.f * (t.rc / r - 1);
    *fy = t.cf - v;
    *k = t.rc / r;
    *half = HalfAt(t.fog, v, *k);
    *inner = HalfAt(t.fog * kFade, v, *k);
    return *half > 0;
}

bool TiltClip(const Tilt& t, double* x0, double* y0, double* x1, double* y1)
{
    // the line's points p0 + s (p1 - p0), s in 0..1, inside the circle: a s^2 + b s + c < 0
    double ax = *x0, ay = *y0 - t.cf, dx = *x1 - *x0, dy = *y1 - *y0;
    double a = dx * dx + dy * dy, b = 2 * (ax * dx + ay * dy), c = ax * ax + ay * ay - t.fog * t.fog;
    double disc = b * b - 4 * a * c;
    if (a == 0 || disc <= 0)
        return false;
    double q = sqrt(disc), s0 = (-b - q) / (2 * a), s1 = (-b + q) / (2 * a);
    s0 = s0 < 0 ? 0 : s0;
    s1 = s1 > 1 ? 1 : s1;
    if (s0 >= s1)
        return false;
    *x0 = ax + s0 * dx;
    *y0 = ay + s0 * dy + t.cf;
    *x1 = ax + s1 * dx;
    *y1 = ay + s1 * dy + t.cf;
    return true;
}
