// layers.h - the map's layers from a region's layers.lay (offline/make_layers.py; the user 2026-11-06): traffic lights,
// speed bumps, car parks; and its danger.lay (the PC loader, from openspeedcam.net; the user 2026-11-22): speed bumps and
// dangers, in the same format. Boxes: lat min, lon min, lat max, lon max, 1e-6 degrees.
#ifndef LAYERS_H
#define LAYERS_H

// LAY_CAMERA: a speed camera of the region's cams.lay (the PC loader, openspeedcam.net; the user 2026-11-28): its
// Navitel type in fee (1, 4 static, 3 red light, 5 mobile), its limit in places (0 - not known)
enum { LAY_SIGNAL = 1, LAY_BUMP = 2, LAY_PARKING = 3, LAY_DANGER = 4, LAY_CAMERA = 5, LAY_KINDS };
enum { FEE_UNKNOWN, FEE_FREE, FEE_PAID };
// A danger's type, in its fee (Navitel's codes; 1, 2 not used)
enum DangerType { DANGER_CROSSING = 0, DANGER_BAD_ROAD = 3, DANGER_TURN = 4, DANGER_CROSSROADS = 5, DANGER_OTHER = 6 };
// The dangers' setting: none; the main ones (bad road, dangerous turn and crossroads); all (+ crossings, other)
enum { DANGERS_OFF, DANGERS_MAIN, DANGERS_ALL, DANGERS_COUNT };
struct LayerPoint { int lat, lon; unsigned char kind, fee; unsigned short places; };   // (12 bytes, as in the file)
// The most points read of a region at once: of layers.lay, of danger.lay - each its own room (crossings, many, cannot
// push out the lights and bumps)
enum { LAYERS_MAX_POINTS = 3000, LAYERS_MAX_DANGERS = 2000, LAYERS_MAX_CAMS = 2000,
       LAYERS_MAX = LAYERS_MAX_POINTS + LAYERS_MAX_DANGERS + LAYERS_MAX_CAMS };

// Which points are wanted (shown, warned of): a bit a kind, a danger's type its own bit. dangers: DANGERS_*
unsigned LayersWanted(int signals, int bumps, int dangers, int cams = 0);
bool LayerWanted(const LayerPoint& p, unsigned wanted);

// The file's points in the box's cells; -1: no such file, or not a whole one
int LayersRead(const wchar_t* path, const int* box, LayerPoint* out, int cap);
// The points of the region under the box (layers.lay's, then danger.lay's; either may be missing), merged, kept until
// the box's cells or the region change: nothing is held open between the calls, so a download can write over the files.
// No region, neither file: none.
const LayerPoint* LayersIn(const int* box, int* n);
void LayersPause(bool on);   // a region being written: the points in hand stay, none read; after it - read anew
void LayersDrop();

// One point for one thing (the user 2026-11-12): traffic lights near one another (a crossing's approaches, 40 m),
// bumps (one each way of a divided road; the user 2026-11-16: 40 m), dangers of one type (40 m) - one at their middle;
// car parks as they are; a danger never with a bump nor with a danger of another type. In place; the points left
int LayersMerge(LayerPoint* p, int n);

// Signs over one another on the screen (the user 2026-11-16): car parks, bumps, dangers of one type whose signs overlap
// (centres at most px apart across and along) - one at the middle of them all; a car park's fee: free when all are, paid
// when one is, else not told. lat, lon hold the screen's y, x. Lights never merged here. In place; the signs left
int LayersMergeOnScreen(LayerPoint* p, int n, int px);

// The nearest wanted point (LayersWanted) on the way ahead: along the course, at most maxM, at most kLayerWideM off the
// line; its index, *dist - its metres. -1: none
int LayerAhead(const LayerPoint* p, int n, unsigned wanted, int lat, int lon, double course, int maxM, int* dist);

// Moscow's paid street parking as lines (parking.lay of the region, offline/make_parklines.py; the user 2026-11-07): a piece
// of a line, its price in tens of rubles an hour, weekdays and the weekend x before 8, from 8 to 21, from 21. Read, kept,
// paused and dropped with the points.
struct ParkLine { int lat, lon; short dLat, dLon; unsigned char price[6]; };   // (20 bytes, as in the file)
int ParkLinesRead(const wchar_t* path, const int* box, ParkLine* out, int cap);
const ParkLine* ParkLinesIn(const int* box, int* n);
int ParkPrice(const ParkLine& l, int weekday, int hour);   // a car's rubles an hour then; weekday 0: Sunday. Holidays not known

// The layers' window (the user 2026-11-28: a window of switches instead of the traffic button's turns): its rows.
// Traffic and car parks never together (the user 2026-11-07: Moscow's car parks are lines); the dangers switched on are
// the main ones (all of them: the settings' stepper).
enum { LYR_TRAFFIC, LYR_CAMS, LYR_BUMPS, LYR_SIGNALS, LYR_DANGERS, LYR_PARKING, LYR_COUNT };
struct LayerSet { int traffic, parking, signals, bumps, dangers, cams; };
void LayersToggle(LayerSet* s, int row);
bool LayersOn(const LayerSet* s, int row);

#endif
