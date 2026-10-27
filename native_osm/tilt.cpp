#include "yamaps.h"

// The third person's view (the user 2026-10-27): a camera behind and over the car looking ahead, its angle from
// straight down by the zoom (z16 less, z17 more). A flat point v px ahead of the car (v < 0: behind) goes to the
// screen row r = rc * f / (v + f) below the horizon (the car's row: rc), sideways scaled by r / rc - so the car keeps
// its size, the far is higher and smaller; at the car the depth is shortened by rc / f = cot(pitch). The visibility:
// a fog by the depth (a straight horizon - the user's choice), fog px ahead and behind, fading in from 0.8 of it;
// f = 1.25 fog keeps v + f > 0 within it. A screen row is one flat row, its x a scale of the screen's - the turned
// frame (main.cpp) stays a walk along a line per row, in integers.

const Tilt* g_tilt;

static const double kDepth = 1.25;   // f / fog
static const double kFade = 0.8;     // the fog starts fading in at this part of its depth

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
    if (fabs(v) >= t.fog)
        return false;
    double r = t.rc * t.f / (v + t.f);
    *sx = fx * r / t.rc;
    *sy = t.cs - t.rc + r;
    return true;
}

bool TiltRow(const Tilt& t, double sy, double* fy, double* k, double* fade)
{
    double r = sy - (t.cs - t.rc);
    if (r <= 0)
        return false;
    double v = t.f * (t.rc / r - 1), a = (v - kFade * t.fog) / ((1 - kFade) * t.fog);
    *fy = t.cf - v;
    *k = t.rc / r;
    *fade = a < 0 ? 0 : a;
    return v < t.fog;
}

// The part s0..s1 of a line where the depth v = v0 + s dv is within the fog's (-fog, fog)
static bool DepthSpan(double v0, double dv, double fog, double* s0, double* s1)
{
    *s0 = 0;
    *s1 = 1;
    if (dv == 0)
        return fabs(v0) < fog;
    double a = (-fog - v0) / dv, b = (fog - v0) / dv;
    if (a > b) {
        double x = a;
        a = b;
        b = x;
    }
    *s0 = a > 0 ? a : 0;
    *s1 = b < 1 ? b : 1;
    return *s0 < *s1;
}

bool TiltClip(const Tilt& t, double* x0, double* y0, double* x1, double* y1)
{
    double dx = *x1 - *x0, dy = *y1 - *y0, s0, s1;
    if (!DepthSpan(t.cf - *y0, -dy, t.fog, &s0, &s1))
        return false;
    double ax = *x0, ay = *y0;
    *x0 = ax + s0 * dx;
    *y0 = ay + s0 * dy;
    *x1 = ax + s1 * dx;
    *y1 = ay + s1 * dy;
    return true;
}
