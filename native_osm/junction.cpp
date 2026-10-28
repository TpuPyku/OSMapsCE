#include "yamaps.h"

// The junction card (the user 2026-10-28, on in the settings; the mock-up: PLAN.md): before a complex maneuver - a
// fork, a ramp, a roundabout, a merge - a picture of the place under the turn card: the map round the maneuver at z17
// turned so the way in points up, the route on it from 110 m before the maneuver to 70 m after. Here: which maneuvers,
// when, the route's piece and the way in's course, the turn of the map's canvas into the card.

static const char* const kComplex[] = { "fork", "off ramp", "on ramp", "roundabout", "rotary", "roundabout turn",
                                        "exit roundabout", "exit rotary", "merge" };
static const int kShownM = 300;   // the card from this far before the maneuver (as the big turn card)

bool JunctionComplex(const char* osrmType)
{
    for (int i = 0; i < (int)(sizeof(kComplex) / sizeof(kComplex[0])); i++)
        if (!strcmp(osrmType, kComplex[i]))
            return true;
    return false;
}

bool JunctionShown(bool on, bool complex, int nextM, bool off)
{
    return on && complex && !off && nextM <= kShownM;
}

// Metres east and north from point a to b (a small piece: the plane at a's latitude)
static void Metres(const double* a, const double* b, double* e, double* n)
{
    *e = (b[0] - a[0]) * 111320.0 * cos(a[1] * M_PI / 180);
    *n = (b[1] - a[1]) * 110540.0;
}

static double Dist(const double* a, const double* b)
{
    double e, n;
    Metres(a, b, &e, &n);
    return sqrt(e * e + n * n);
}

static void Put(double* out, int& n, int cap, double lon, double lat)
{
    if (n < cap) {
        out[n * 2] = lon;
        out[n * 2 + 1] = lat;
        n++;
    }
}

static void PutPart(double* out, int& n, int cap, const double* a, const double* b, double t)   // a + t (b - a)
{
    Put(out, n, cap, a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t);
}

// The point `metres` along the line from point `at` in the direction step (-1 back, +1 on): its piece's first point
// (the one nearer to at) and the part of the piece; at the line's end: that end
static int Walk(const double* ll, int np, int at, int step, double metres, double* part)
{
    int i = at;
    for (; i + step >= 0 && i + step < np; i += step) {
        double d = Dist(ll + i * 2, ll + (i + step) * 2);
        if (d >= metres) {
            *part = d > 0 ? metres / d : 0;
            return i;
        }
        metres -= d;
    }
    *part = 0;
    return i;
}

int JunctionSlice(const double* ll, int np, int at, double beforeM, double afterM, double* out, int cap, int* outAt,
                  double* course)
{
    double pb, pa;
    int b = Walk(ll, np, at, -1, beforeM, &pb), a = Walk(ll, np, at, 1, afterM, &pa), n = 0;
    if (pb > 0)   // (the start inside a piece; else the line's own point)
        PutPart(out, n, cap, ll + b * 2, ll + (b - 1) * 2, pb);
    for (int i = b; i <= a; i++)
        Put(out, n, cap, ll[i * 2], ll[i * 2 + 1]);
    if (pa > 0)
        PutPart(out, n, cap, ll + a * 2, ll + (a + 1) * 2, pa);
    *outAt = at - b + (pb > 0 ? 1 : 0);
    int from = at > 0 ? at - 1 : 0;   // the way in: the piece before the maneuver (at the start: the first one)
    double e, nn;
    Metres(ll + from * 2, ll + (from + 1 < np ? from + 1 : from) * 2, &e, &nn);
    *course = atan2(e, nn) * 180 / M_PI;
    if (*course < 0)
        *course += 360;
    return n;
}

void JunctionTurn(const unsigned short* src, int sw, int sh, int sstride, double scx, double scy, unsigned short* dst,
                  int dw, int dh, int dstride, double dcx, double dcy, double courseDeg, unsigned short bg)
{
    double a = courseDeg * M_PI / 180, ca = cos(a), sa = sin(a);
    for (int y = 0; y < dh; y++) {
        double sy = y - dcy;
        for (int x = 0; x < dw; x++) {
            double sx = x - dcx;   // the card's offset -> the map's: turned by the course (as main.cpp RotateRing)
            int px = (int)floor(sx * ca - sy * sa + scx + 0.5), py = (int)floor(sx * sa + sy * ca + scy + 0.5);
            dst[y * dstride + x] = px >= 0 && py >= 0 && px < sw && py < sh ? src[py * sstride + px] : bg;
        }
    }
}
