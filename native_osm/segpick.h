// segpick.h - which piece of the route's line the car is on (route.cpp RouteGps), from its distance to each.
#ifndef SEGPICK_H
#define SEGPICK_H

// Of n pieces (d: the car's distance from each, metres) the one it is on: the nearest - or an earlier one, not before
// `from` (where it was), as near within `tie` metres: at a jog of two turns metres apart (the user 2026-11-08: the
// second one was told, the first skipped) the car is not taken past the first while it is still before it.
int SegPick(const double* d, int n, int from, double tie);

#endif
