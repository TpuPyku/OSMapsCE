// segpick.cpp - which piece of the route's line the car is on (segpick.h).
#include "segpick.h"

int SegPick(const double* d, int n, int from, double tie)
{
    int best = 0;
    for (int i = 1; i < n; i++)
        if (d[i] < d[best])
            best = i;
    for (int i = from > 0 ? from : 0; i < best; i++)
        if (d[i] <= d[best] + tie)
            return i;
    return best;
}
