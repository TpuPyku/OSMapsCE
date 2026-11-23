// lanes.h - the lanes before a maneuver (the user 2026-11-22), from an OSRM route's steps (route.cpp): the lanes of
// the road coming to the maneuver's intersection, left to right, each with its arrows and whether the route may take it.
#ifndef LANES_H
#define LANES_H

// A lane's arrows (OSRM's "indications"), bit i = kLaneNames[i] in lanes.cpp
enum {
    LANE_NONE = 1, LANE_UTURN = 2, LANE_SHARP_RIGHT = 4, LANE_RIGHT = 8, LANE_SLIGHT_RIGHT = 16, LANE_STRAIGHT = 32,
    LANE_SLIGHT_LEFT = 64, LANE_LEFT = 128, LANE_SHARP_LEFT = 256, LANE_KINDS = 9
};
enum { kMaxLanes = 8 };   // a road with more (a toll plaza): none shown

struct LaneSet {
    int            n;                  // 0: none
    unsigned       valid;              // bit i: lane i is one for the route
    unsigned short ind[kMaxLanes];     // LANE_* bits of each lane, left to right
};

// The lanes for a step's maneuver: its own intersection's (the step's first); none there - the last intersection
// with lanes of the step before (its first one is the maneuver before, left out) no farther than kLanesBackM from it.
// step, prev: the JSON step objects (prev NULL: the first step).
enum { kLanesBackM = 300 };
void LanesOfStep(const char* step, const char* prev, LaneSet* o);

// A lane's arrows as turns (degrees, + right, VNavDrawTurn's), into out[LANE_KINDS]; their count. No arrow
// ("none" only or nothing): straight.
int LaneTurns(unsigned ind, int* out);

#endif
