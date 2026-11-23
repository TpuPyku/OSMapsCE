// fling.cpp - the map's inertia. See fling.h.
#include "fling.h"
#include <math.h>

static const double kTauMs = 325, kMinV = 0.3, kMaxV = 4.0, kStopV = 0.02;

bool FlingStart(Fling* f, double vx, double vy)
{
    f->vx = f->vy = 0;
    double v = sqrt(vx * vx + vy * vy);
    if (v < kMinV)
        return false;
    double k = v > kMaxV ? kMaxV / v : 1;
    f->vx = vx * k;
    f->vy = vy * k;
    return true;
}

bool FlingStep(Fling* f, DWORD dt, double* dx, double* dy)
{
    *dx = *dy = 0;
    if (sqrt(f->vx * f->vx + f->vy * f->vy) < kStopV)
        return false;
    double decay = exp(-(double)dt / kTauMs), way = kTauMs * (1 - decay);   // the integral of v e^(-t/tau)
    *dx = f->vx * way;
    *dy = f->vy * way;
    f->vx *= decay;
    f->vy *= decay;
    return true;
}
