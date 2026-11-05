// trafshade.h - how the traffic is shown (the user 2026-11-05): on the map (and the route), or on the route alone;
// at night dimmed - on the map always (the vector map: the raster's night LUT dims it itself), on the route only when
// it is shown there alone (with the map's: the route's kept bright, the way stands out). Pure.
#pragma once
#include <windows.h>

enum { kTrafficNightPct = 55 };   // a dimmed colour's channels, % (BlendRing: the map's overlays the same)
struct TrafficLook {
    bool onMap;      // the overlays blended into the map
    bool mapDim;     // ... dimmed
    bool routeDim;   // the route's traffic colours dimmed (TrafficShade)
};

TrafficLook TrafficLookOf(bool traffic, bool routeOnly, bool night, bool vector);
COLORREF TrafficShade(COLORREF c, bool dim);
