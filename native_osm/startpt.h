// startpt.h - where a route starts when the plan says "my location" (the user 2026-11-06): the car; without a fix the
// last place the GPS told, kept over restarts; if the GPS never told one, the map's centre. Another start is always one
// choice away in the route's editor. Pure.
#pragma once

enum { START_CAR, START_LAST, START_CENTER };
struct StartPoint { int source; double lon, lat; };

// lastLon, lastLat: 0, 0 - none yet
inline StartPoint StartFrom(bool fix, double fixLon, double fixLat, double lastLon, double lastLat, double mapLon, double mapLat)
{
    StartPoint s = { START_CENTER, mapLon, mapLat };
    if (fix) {
        s.source = START_CAR;
        s.lon = fixLon;
        s.lat = fixLat;
    } else if (lastLon != 0 || lastLat != 0) {
        s.source = START_LAST;
        s.lon = lastLon;
        s.lat = lastLat;
    }
    return s;
}
