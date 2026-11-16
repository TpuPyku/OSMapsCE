// layers.h - the map's layers from a region's layers.lay (offline/make_layers.py; the user 2026-11-06): traffic lights,
// speed bumps, car parks. Boxes: lat min, lon min, lat max, lon max, 1e-6 degrees.
#ifndef LAYERS_H
#define LAYERS_H

enum { LAY_SIGNAL = 1, LAY_BUMP = 2, LAY_PARKING = 3 };
enum { FEE_UNKNOWN, FEE_FREE, FEE_PAID };
struct LayerPoint { int lat, lon; unsigned char kind, fee; unsigned short places; };   // (12 bytes, as in the file)

// The file's points in the box's cells; -1: no such file, or not a whole one
int LayersRead(const wchar_t* path, const int* box, LayerPoint* out, int cap);
// The points of the region under the box, kept until the box's cells or the region change: nothing is held open between
// the calls, so a download can write over the file. No region, no layers.lay (a region not rebuilt): none.
const LayerPoint* LayersIn(const int* box, int* n);
void LayersPause(bool on);   // a region being written: the points in hand stay, none read; after it - read anew
void LayersDrop();

// One point for one thing (the user 2026-11-12): traffic lights near one another (a crossing's approaches, 40 m) and
// bumps (one each way of a divided road, 20 m) - one at their middle; car parks as they are. In place; the points left
int LayersMerge(LayerPoint* p, int n);

// The nearest point of that kind on the way ahead: along the course, at most maxM, at most kLayerWideM off the line;
// *dist - its metres
bool LayerAhead(const LayerPoint* p, int n, int kind, int lat, int lon, double course, int maxM, int* dist);

// Moscow's paid street parking as lines (parking.lay of the region, offline/make_parklines.py; the user 2026-11-07): a piece
// of a line, its price in tens of rubles an hour, weekdays and the weekend x before 8, from 8 to 21, from 21. Read, kept,
// paused and dropped with the points.
struct ParkLine { int lat, lon; short dLat, dLon; unsigned char price[6]; };   // (20 bytes, as in the file)
int ParkLinesRead(const wchar_t* path, const int* box, ParkLine* out, int cap);
const ParkLine* ParkLinesIn(const int* box, int* n);
int ParkPrice(const ParkLine& l, int weekday, int hour);   // a car's rubles an hour then; weekday 0: Sunday. Holidays not known

// One button: traffic, car parks, neither (the user 2026-11-07: Moscow's car parks are lines, they do not go with traffic)
void OverlayNext(int* traffic, int* parking);

#endif
