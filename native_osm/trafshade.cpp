// trafshade.cpp - how the traffic is shown. See trafshade.h.
#include "trafshade.h"

TrafficLook TrafficLookOf(bool traffic, bool routeOnly, bool night, bool vector)
{
    TrafficLook t;
    t.onMap = traffic && !routeOnly;
    t.mapDim = t.onMap && night && vector;
    t.routeDim = traffic && routeOnly && night;
    return t;
}

COLORREF TrafficShade(COLORREF c, bool dim)
{
    if (!dim)
        return c;
    return RGB(GetRValue(c) * kTrafficNightPct / 100, GetGValue(c) * kTrafficNightPct / 100,
               GetBValue(c) * kTrafficNightPct / 100);
}
