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

// The map moved by the finger or the fling (the user 2026-11-23: "the map jumps under the finger" - every move drew it
// anew, 130-215 ms on the unit): the last full frame (the one kept above) shifted instead. The kept frame's view and
// the view now (world px, the same zoom, north up) -> its shift on the screen (rounded); false - more than a quarter of
// the map's w or h would be uncovered: draw it anew
bool FrameShift(double keepX, double keepY, double vx, double vy, int w, int h, int* ox, int* oy);
// The map's parts the frame shifted by ox, oy leaves uncovered (the ground's colour there): their count (0..2)
int FrameUncovered(const RECT& map, int ox, int oy, RECT* out);
