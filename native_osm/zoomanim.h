// zoomanim.h - the zoom's change shown smoothly (the user 2026-11-05): the last map frame scaled around the point the
// zoom keeps in place (the car while following, else the map's center) from what it showed to 2^dz in kZoomMs, eased
// at both ends; another zoom meanwhile goes on from the scale shown. Pure: no window, no clock.
#pragma once
#include <windows.h>

enum { kZoomMs = 250 };
struct ZoomAnim {
    bool   on;
    DWORD  t0;
    double from, to;   // the kept frame's scale as powers of 2, at t0 and at the end
};

void ZoomAnimStart(ZoomAnim* a, DWORD now, int dz);   // dz levels (+ closer); while on: from the scale shown now
// The kept frame's scale at now; false - over (a->on cleared): the new zoom's own frame then
bool ZoomAnimScale(ZoomAnim* a, DWORD now, double* scale);
// The new zoom's frame against the one shown at that scale (a point of it - the car - is anchor + (p - anchor) * this)
double ZoomAnimNew(const ZoomAnim* a, double scale);
// Where the kept frame (the map's rect) goes at that scale around the anchor (ax, ay)
void ZoomAnimRect(double scale, double ax, double ay, const RECT& map, RECT* dst);
