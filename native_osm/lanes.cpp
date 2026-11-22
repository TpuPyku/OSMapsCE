// lanes.cpp - the lanes before a maneuver from an OSRM route's step (see lanes.h). Pure: tested by test_ui.
#include <windows.h>
#include <string.h>
#include <math.h>
#include "vjson.h"
#include "lanes.h"

// OSRM's indications, in the order of the LANE_* bits, and each one's turn (U-turn: to the left, right-hand traffic)
static const char* const kLaneNames[LANE_KINDS] = { "none", "uturn", "sharp right", "right", "slight right",
                                                    "straight", "slight left", "left", "sharp left" };
static const int kLaneTurn[LANE_KINDS] = { 0, -180, 135, 90, 45, 0, -45, -90, -135 };

static unsigned LaneBits(const char* inds)
{
    unsigned bits = 0;
    for (const char* p = JFirst(inds); p; p = JNext(p)) {
        char s[16];
        JStr(p, s, sizeof(s));
        for (int i = 0; i < LANE_KINDS; i++)
            if (!strcmp(s, kLaneNames[i]))
                bits |= 1u << i;
    }
    return bits;
}

// An intersection's lanes; false: none (or more than kMaxLanes)
static bool LanesAt(const char* in, LaneSet* o)
{
    o->n = 0;
    o->valid = 0;
    int n = 0;
    for (const char* l = JFirst(JKey(in, "lanes")); l; l = JNext(l), n++) {
        if (n == kMaxLanes) {
            o->n = 0;
            return false;
        }
        o->ind[n] = (unsigned short)LaneBits(JKey(l, "indications"));
        const char* v = JKey(l, "valid");
        if (v && *v == 't')
            o->valid |= 1u << n;
        o->n = n + 1;
    }
    return o->n > 0;
}

static void Loc(const char* obj, double* lon, double* lat)
{
    const char* p = JFirst(JKey(obj, "location"));
    *lon = JNum(p, 0);
    *lat = JNum(p ? JNext(p) : NULL, 0);
}

void LanesOfStep(const char* step, const char* prev, LaneSet* o)
{
    const char* own = JFirst(JKey(step, "intersections"));
    if (LanesAt(own, o) || !prev)
        return;
    double mx, my;
    Loc(JKey(step, "maneuver"), &mx, &my);
    double kx = 111320 * cos(my * 3.14159265358979 / 180);
    LaneSet at;
    const char* first = JFirst(JKey(prev, "intersections"));
    for (const char* in = first ? JNext(first) : NULL; in; in = JNext(in)) {
        double x, y;
        Loc(in, &x, &y);
        double dx = (x - mx) * kx, dy = (y - my) * 110540;
        if (LanesAt(in, &at) && dx * dx + dy * dy <= (double)kLanesBackM * kLanesBackM)
            *o = at;
    }
}

int LaneTurns(unsigned ind, int* out)
{
    int n = 0;
    for (int i = 0; i < LANE_KINDS; i++) {
        if (!(ind & 1u << i))
            continue;
        bool dup = false;
        for (int k = 0; k < n; k++)
            dup = dup || out[k] == kLaneTurn[i];
        if (!dup)
            out[n++] = kLaneTurn[i];
    }
    if (!n)
        out[n++] = 0;
    return n;
}
