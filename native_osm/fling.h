// fling.h - the map's inertia (the user 2026-11-04: "rubber" scrolling): the finger's speed at its release (VestaCore
// vtouch: its clean points' last 100 ms), then the map going on that way and slowing down (exponentially, ~325 ms),
// whatever the frames. Pure: no window, no clock.
#pragma once
#include <windows.h>

struct Fling {
    double vx, vy;   // px/ms once started
};

// The finger's speed at its release (px/ms): true - the map goes on (none slower than 0.3 px/ms; at most 4)
bool FlingStart(Fling* f, double vx, double vy);
// The map's way (with the finger's sign) in the next dt ms; false - it has stopped (nothing to move)
bool FlingStep(Fling* f, DWORD dt, double* dx, double* dy);
