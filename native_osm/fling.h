// fling.h - the map's inertia (the user 2026-11-04: "rubber" scrolling): the finger's speed at its release, then the
// map going on that way and slowing down (exponentially, ~325 ms), whatever the frames. Pure: no window, no clock.
#pragma once
#include <windows.h>

enum { kFlingSamples = 8 };
struct Fling {
    DWORD  t[kFlingSamples];   // the finger's last points (a ring)
    int    x[kFlingSamples], y[kFlingSamples];
    int    n;                  // points kept (the newest at (n - 1) % kFlingSamples)
    double vx, vy;             // px/ms once started
};

void FlingReset(Fling* f);
void FlingAdd(Fling* f, DWORD t, int x, int y);   // the finger's point at t (ms)
// Released at t: true - the map goes on (the speed of the finger's last 100 ms; none when it stood still the last
// 80 ms, or slower than 0.3 px/ms; at most 4 px/ms)
bool FlingStart(Fling* f, DWORD t);
// The map's way (with the finger's sign) in the next dt ms; false - it has stopped (nothing to move)
bool FlingStep(Fling* f, DWORD dt, double* dx, double* dy);
