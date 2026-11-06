// fling.cpp - the map's inertia. See fling.h.
#include "fling.h"
#include <math.h>
#include <string.h>

enum { kWindowMs = 100, kStillMs = 80 };
static const double kTauMs = 325, kMinV = 0.3, kMaxV = 4.0, kStopV = 0.02;

void FlingReset(Fling* f)
{
    memset(f, 0, sizeof(*f));
}

void FlingAdd(Fling* f, DWORD t, int x, int y)
{
    int i = f->n % kFlingSamples;
    f->t[i] = t;
    f->x[i] = x;
    f->y[i] = y;
    f->n++;
}

bool FlingStart(Fling* f, DWORD t)
{
    f->vx = f->vy = 0;
    if (f->n < 2)
        return false;
    int last = (f->n - 1) % kFlingSamples, first = last;
    if (t - f->t[last] > kStillMs)
        return false;
    int kept = f->n < kFlingSamples ? f->n : kFlingSamples;
    for (int k = 1; k < kept; k++) {   // back to the oldest point within the window
        int i = (f->n - 1 - k) % kFlingSamples;
        if (f->t[last] - f->t[i] > kWindowMs)
            break;
        first = i;
    }
    double ms = (double)(f->t[last] - f->t[first]);
    if (ms <= 0)
        return false;
    double vx = (f->x[last] - f->x[first]) / ms, vy = (f->y[last] - f->y[first]) / ms, v = sqrt(vx * vx + vy * vy);
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
