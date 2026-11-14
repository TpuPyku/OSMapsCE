#include "yamaps.h"

// The junction card (the user 2026-10-28, on in the settings; the mock-up: PLAN.md): before a complex maneuver - a
// fork, a ramp, a roundabout, a merge - a picture of the place under the turn card: the map round the maneuver at z17
// turned so the way in points up, the route on it from 110 m before the maneuver to 70 m after. Here: which maneuvers,
// when, the route's piece and the way in's course, the turn of the map's canvas into the card.

static const char* const kComplex[] = { "fork", "off ramp", "on ramp", "roundabout", "rotary", "roundabout turn",
                                        "exit roundabout", "exit rotary", "merge" };
static const int kShownM = 300;   // the card from this far before the maneuver at least (as the big turn card)
static const double kLeadS = 15;  // ... and this many seconds of the way (HU 2026-10-31: at 110 km/h it came ~50 m before)
static const int kCloseZ = 17;    // the zoom before it

bool JunctionComplex(const char* osrmType)
{
    for (int i = 0; i < (int)(sizeof(kComplex) / sizeof(kComplex[0])); i++)
        if (!strcmp(osrmType, kComplex[i]))
            return true;
    return false;
}

int JunctionReachM(double kmh)
{
    int m = (int)(kmh / 3.6 * kLeadS);
    return m > kShownM ? m : kShownM;
}

bool JunctionShown(bool on, bool complex, int nextM, bool off, int reachM)
{
    return on && complex && !off && nextM <= reachM;
}

// The zoom before a complex maneuver (the user 2026-10-28): z17 - the third person's view at its lowest camera -
// within the card's reach, heading-up; else the user's own (closer already: kept)
int JunctionZoom(int userZ, bool heading, bool complex, int nextM, bool off, int reachM)
{
    return JunctionShown(heading, complex, nextM, off, reachM) && userZ < kCloseZ ? kCloseZ : userZ;
}

// The wheel's buttons, from VestaHome while the map is in front (kind: 1 short, 2 long, 3 repeat): up / down - 9 / 8
// (HU 2026-10-31: the tracks they stepped were swapped) and phone / hang up (0x12 / 0x14) held, the MCU's own held up /
// down 0x0A / 0x0B - zoom in / out; short presses stay VestaHome's (the music, the phone - the user 2026-11-02)
int WheelZoom(int code, int kind)
{
    if (code == 0x0A || code == 0x0B)
        return code == 0x0A ? 1 : -1;
    if (kind < 2)
        return 0;
    return code == 9 || code == 0x12 ? 1 : code == 8 || code == 0x14 ? -1 : 0;
}

bool WheelNow(int code, int st)
{
    return code == 8 || code == 9 ? st == 1 : st == 0;
}

// The view's turn eased in over the second (the user 2026-10-31: the map jerked, a frame a second)
double GlideCourse(double from, double to, DWORD ms)
{
    double delta = fmod(to - from + 540, 360) - 180, c = from + delta * (ms < 1000 ? ms : 1000) / 1000;
    return c < 0 ? c + 360 : c >= 360 ? c - 360 : c;
}

// The mark a fix late (the user 2026-11-02: the prediction slowed towards the second's end and jumped at the next
// fix - the point a second ahead is known, no guess needed): at an even speed to the fix's point in the fixes' period
bool GlideAim(double lon, double lat, double tLon, double tLat, DWORD periodMs, double* ve, double* vn)
{
    double e = (tLon - lon) * 111320.0 * cos(lat * M_PI / 180), n = (tLat - lat) * 110540.0;
    bool on = periodMs > 0 && e * e + n * n <= 50.0 * 50.0;
    *ve = on ? e / periodMs : 0;
    *vn = on ? n / periodMs : 0;
    return on;
}

void GlideMove(double* lon, double* lat, double ve, double vn, DWORD ms)
{
    *lon += ve * ms / (111320.0 * cos(*lat * M_PI / 180));
    *lat += vn * ms / 110540.0;
}

// Near: 30 m to catch the line, 40 m to leave it (no flicker at the edge). Going its way: within 60 degrees, above 10 km/h
// (slower the course is noise). A side road the car really took fails one of the two, and the mark follows the fix.
bool MagnetSnap(const double* pts, int np, int seg, double* lon, double* lat, double kmh, double* course, bool was)
{
    const double kx = 111320.0 * cos(*lat * M_PI / 180), ky = 110540.0;
    int b = seg + 200 < np - 1 ? seg + 200 : np - 1;
    double best = 1e18, bx = 0, by = 0, lineCourse = 0;
    for (int i = seg > 10 ? seg - 10 : 0; i < b; i++) {   // (the same window as RouteGps: the line may pass here twice)
        double ax = (pts[i * 2] - *lon) * kx, ay = (pts[i * 2 + 1] - *lat) * ky;
        double dx = (pts[i * 2 + 2] - pts[i * 2]) * kx, dy = (pts[i * 2 + 3] - pts[i * 2 + 1]) * ky, l2 = dx * dx + dy * dy;
        if (l2 <= 0)
            continue;
        double k = -(ax * dx + ay * dy) / l2;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        double ex = ax + k * dx, ey = ay + k * dy, d2 = ex * ex + ey * ey;
        if (d2 < best) {
            best = d2;
            bx = ex;
            by = ey;
            lineCourse = atan2(dx, dy) * 180 / M_PI;
        }
    }
    double reach = was ? 40 : 30;
    if (best > reach * reach)
        return false;
    if (lineCourse < 0)
        lineCourse += 360;
    if (kmh > 10 && fabs(fmod(*course - lineCourse + 540, 360) - 180) > 60)
        return false;
    *lon += bx / kx;
    *lat += by / ky;
    *course = lineCourse;
    return true;
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

static double OnSegment(const double* a, const double* b, const double* p, double* q)   // p's nearest point on a-b: q;
{                                                                                      // its distance, metres
    double se, sn, pe, pn;
    Metres(a, b, &se, &sn);
    Metres(a, p, &pe, &pn);
    double len2 = se * se + sn * sn, t = len2 > 0 ? (pe * se + pn * sn) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    q[0] = a[0] + (b[0] - a[0]) * t;
    q[1] = a[1] + (b[1] - a[1]) * t;
    return Dist(q, p);
}

// The route behind the car cut off (the user 2026-11-03: a long tail stayed behind it): the car shown (a fix late) is
// looked for near the segment the route's own tracking says
int RouteCut(const double* ll, int np, int from, int to, double lon, double lat, double maxM, double* cut)
{
    double p[2] = { lon, lat }, q[2];
    int best = -1;
    from = from < 0 ? 0 : from;
    to = to > np - 2 ? np - 2 : to;
    for (int i = from; i <= to; i++) {
        double d = OnSegment(ll + i * 2, ll + i * 2 + 2, p, q);
        if (d <= maxM) {
            maxM = d;
            best = i;
            cut[0] = q[0];
            cut[1] = q[1];
        }
    }
    return best;
}

// The car led on along the route's line with no fix (the user 2026-11-14: in a tunnel the mark left the route)
static double Toward(double* p, const double* b, double m)   // p moved up to m metres to b: the metres left past b
{                                                            // (<= 0: it stopped short)
    double d = Dist(p, b), t = m <= 0 ? 0 : d > m ? m / d : 1;
    p[0] += (b[0] - p[0]) * t;
    p[1] += (b[1] - p[1]) * t;
    return m - d;
}

static double CourseOf(const double* a, const double* b, double was)   // a -> b, degrees; no length: the one it was
{
    double e, n;
    Metres(a, b, &e, &n);
    return e || n ? fmod(atan2(e, n) * 180 / M_PI + 360, 360) : was;
}

int RouteAhead(const double* ll, int np, int seg, const double* at, double m, double* out)
{
    if (np < 2)
        return -1;
    seg = seg < 0 ? 0 : seg > np - 2 ? np - 2 : seg;
    out[0] = at[0];
    out[1] = at[1];
    out[2] = 0;
    for (;; seg++) {
        const double* b = ll + seg * 2 + 2;
        out[2] = CourseOf(b - 2, b, out[2]);
        m = Toward(out, b, m);
        if (m <= 0 || seg == np - 2)
            return seg;
    }
}

// A fix farther from the last trusted one than its speed allows (the drift before a tunnel): 1.5 times the way at the
// faster of the two speeds, and 30 m of noise
bool FixJumps(double lon0, double lat0, double kmh0, DWORD ms, double lon, double lat, double kmh)
{
    double a[2] = { lon0, lat0 }, b[2] = { lon, lat }, v = kmh0 > kmh ? kmh0 : kmh;
    return Dist(a, b) > v / 3.6 * ms / 1000 * 1.5 + 30;
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
    // the way in: from the piece's start to the maneuver - the approach, not its last bend (a roundabout's entry);
    // at the line's start: its first piece
    double e, nn;
    if (*outAt > 0 && *outAt < n)
        Metres(out, out + *outAt * 2, &e, &nn);
    else
        Metres(ll, ll + (np > 1 ? 2 : 0), &e, &nn);
    *course = atan2(e, nn) * 180 / M_PI;
    if (*course < 0)
        *course += 360;
    return n;
}

// The search's places beside its panel (design v3): their box, the closest zoom it fits at
static void Grow(double v, double* lo, double* hi)
{
    *lo = v < *lo ? v : *lo;
    *hi = v > *hi ? v : *hi;
}

int FitZoom(const double* xy, int n, double w, double h, int zMin, int zMax, double* cx, double* cy)
{
    if (n < 1)
        return -1;
    double x0 = xy[0], y0 = xy[1], x1 = x0, y1 = y0;
    for (int i = 1; i < n; i++) {
        Grow(xy[i * 2], &x0, &x1);
        Grow(xy[i * 2 + 1], &y0, &y1);
    }
    *cx = (x0 + x1) / 2;
    *cy = (y0 + y1) / 2;
    int z = zMax;
    while (z > zMin && ((x1 - x0) * (1 << z) > w || (y1 - y0) * (1 << z) > h))
        z--;
    return z;
}

// A rounded rectangle's (rows top..bottom - 1) row y: how far in from each side it starts (the circle's, by the pixel
// centres); also main.cpp's Darken
int CornerInset(int radius, int y, int top, int bottom)
{
    int dy = y < top + radius ? top + radius - y : y >= bottom - radius ? y - (bottom - radius - 1) : 0;
    if (dy <= 0)
        return 0;
    double d = dy - 0.5;
    return (int)(radius - sqrt(radius * radius - d * d) + 0.5);
}

void JunctionTurn(const unsigned short* src, int sw, int sh, int sstride, double scx, double scy, unsigned short* dst,
                  int dw, int dh, int dstride, double dcx, double dcy, double courseDeg, unsigned short bg, int radius)
{
    double a = courseDeg * M_PI / 180, ca = cos(a), sa = sin(a);
    for (int y = 0; y < dh; y++) {
        double sy = y - dcy;
        int in = CornerInset(radius, y, 0, dh);
        for (int x = in; x < dw - in; x++) {
            double sx = x - dcx;   // the card's offset -> the map's: turned by the course (as main.cpp RotateRing)
            int px = (int)floor(sx * ca - sy * sa + scx + 0.5), py = (int)floor(sx * sa + sy * ca + scy + 0.5);
            dst[y * dstride + x] = px >= 0 && py >= 0 && px < sw && py < sh ? src[py * sstride + px] : bg;
        }
    }
}
