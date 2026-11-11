#include "yamaps.h"
#include "layers.h"
#include <math.h>

// See layers.h. The file: offline/make_layers.py.

static const DWORD kMagicPoints = 0x4C4D534F, kMagicLines = 0x504D534F;   // 'OSML', 'OSMP'
enum { kVersion = 1, kHeadBytes = 16, kCellDeg = 10000, kMaxPoints = 3000, kLayerWideM = 25 };
enum { kMaxLines = 5000 };   // (Moscow's centre, the widest box of the map at z15: 4542)

struct Cell { DWORD key, first; };
struct Table { Cell* cells; int nCells; DWORD nItems; int size; };

static int LatCell(int lat) { return (lat + 90000000) / kCellDeg; }
static int LonCell(int lon) { return (lon + 180000000) / kCellDeg; }

static void TableFree(Table* t)
{
    free(t->cells);
    memset(t, 0, sizeof(*t));
}

static bool TableRead(HANDLE f, DWORD magic, int size, Table* t)
{
    DWORD head[4];
    memset(t, 0, sizeof(*t));
    if (!OffRead(f, 0, head, kHeadBytes) || head[0] != magic || head[1] != kVersion)
        return false;
    if (GetFileSize(f, NULL) != kHeadBytes + head[2] * 8 + head[3] * size)
        return false;
    t->nCells = (int)head[2];
    t->nItems = head[3];
    t->size = size;
    t->cells = (Cell*)malloc(t->nCells * 8 + 1);
    if (t->cells && OffRead(f, kHeadBytes, t->cells, t->nCells * 8))
        return true;
    TableFree(t);
    return false;
}

static int FirstAtLeast(const Table& t, DWORD key)   // the first cell of that key or later
{
    int lo = 0, hi = t.nCells;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (t.cells[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static DWORD FirstPoint(const Table& t, int cell)
{
    return cell < t.nCells ? t.cells[cell].first : t.nItems;
}

// The box's cells of one row of latitude (next to each other in the file): one read
static int ReadRow(HANDLE f, const Table& t, int row, int lon0, int lon1, char* out, int cap)
{
    int a = FirstAtLeast(t, (DWORD)row << 16 | lon0), b = FirstAtLeast(t, (DWORD)row << 16 | (lon1 + 1));
    DWORD first = FirstPoint(t, a), n = FirstPoint(t, b) - first;
    if (n > (DWORD)cap)
        n = cap;
    DWORD off = kHeadBytes + t.nCells * 8 + first * t.size;
    return n && OffRead(f, off, out, n * t.size) ? (int)n : 0;
}

static int ReadBox(HANDLE f, const Table& t, const int* box, char* out, int cap)
{
    int got = 0;
    for (int row = LatCell(box[0]); row <= LatCell(box[2]) && got < cap; row++)
        got += ReadRow(f, t, row, LonCell(box[1]), LonCell(box[3]), out + got * t.size, cap - got);
    return got;
}

static int FileRead(const wchar_t* path, DWORD magic, int size, const int* box, void* out, int cap)
{
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return -1;
    Table t;
    int got = TableRead(f, magic, size, &t) ? ReadBox(f, t, box, (char*)out, cap) : -1;
    TableFree(&t);
    CloseHandle(f);
    return got;
}

int LayersRead(const wchar_t* path, const int* box, LayerPoint* out, int cap)
{
    return FileRead(path, kMagicPoints, sizeof(LayerPoint), box, out, cap);
}

int ParkLinesRead(const wchar_t* path, const int* box, ParkLine* out, int cap)
{
    return FileRead(path, kMagicLines, sizeof(ParkLine), box, out, cap);
}

// ---------------------------------------------------------------- the points in hand

struct Held {
    const wchar_t* name;
    DWORD magic;
    int size, cap;
    void* items;
    int n;
    int cells[4];        // the cells they were read for; 0 0 0 0: none yet
    volatile LONG stale;
};
static LayerPoint   s_pts[kMaxPoints];
static ParkLine     s_lines[kMaxLines];
static Held         s_points = { L"layers.lay", kMagicPoints, sizeof(LayerPoint), kMaxPoints, s_pts },
                    s_parks = { L"parking.lay", kMagicLines, sizeof(ParkLine), kMaxLines, s_lines };
static volatile LONG s_paused;

static int ReadRegion(const Held& h, const int* box)
{
    int mid[2] = { box[0] / 2 + box[2] / 2, box[1] / 2 + box[3] / 2 };
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!OfflineDir(mid, 1, dir))
        return 0;
    _snwprintf(path, MAX_PATH, L"%s\\%s", dir, h.name);
    path[MAX_PATH - 1] = 0;
    int got = FileRead(path, h.magic, h.size, box, h.items, h.cap);
    return got < 0 ? 0 : got;
}

static void* HeldIn(Held& h, const int* box, int* n)
{
    int cells[4] = { LatCell(box[0]), LonCell(box[1]), LatCell(box[2]), LonCell(box[3]) };
    bool stale = InterlockedExchange((LONG*)&h.stale, 0) != 0;
    if (!s_paused && (stale || memcmp(cells, h.cells, sizeof(cells)))) {
        h.n = ReadRegion(h, box);
        memcpy(h.cells, cells, sizeof(cells));
    }
    *n = h.n;
    return h.items;
}

const LayerPoint* LayersIn(const int* box, int* n)
{
    return (const LayerPoint*)HeldIn(s_points, box, n);
}

const ParkLine* ParkLinesIn(const int* box, int* n)
{
    return (const ParkLine*)HeldIn(s_parks, box, n);
}

void LayersPause(bool on)
{
    InterlockedExchange((LONG*)&s_paused, on);
    if (!on) {
        InterlockedExchange((LONG*)&s_points.stale, 1);
        InterlockedExchange((LONG*)&s_parks.stale, 1);
    }
}

static void HeldDrop(Held& h)
{
    h.n = 0;
    memset(h.cells, 0, sizeof(h.cells));
    InterlockedExchange((LONG*)&h.stale, 1);
}

void LayersDrop()
{
    HeldDrop(s_points);
    HeldDrop(s_parks);
}

int ParkPrice(const ParkLine& l, int weekday, int hour)
{
    int part = hour < 8 ? 0 : hour < 21 ? 1 : 2;
    return l.price[(weekday == 0 || weekday == 6 ? 3 : 0) + part] * 10;
}

// ---------------------------------------------------------------- ahead

bool LayerAhead(const LayerPoint* p, int n, int kind, int lat, int lon, double course, int maxM, int* dist)
{
    const double kDegM = 0.111195, rad = course * 3.14159265358979 / 180;   // (metres in 1e-6 degree)
    const double kx = cos(lat * 1e-6 * 3.14159265358979 / 180), dirE = sin(rad), dirN = cos(rad);
    double best = maxM + 1.0;
    for (int i = 0; i < n; i++) {
        if (p[i].kind != kind)
            continue;
        double e = (p[i].lon - lon) * kDegM * kx, nn = (p[i].lat - lat) * kDegM;
        double along = e * dirE + nn * dirN, off = e * dirN - nn * dirE;
        if (along >= 0 && along < best && fabs(off) <= kLayerWideM)
            best = along;
    }
    if (best > maxM)
        return false;
    *dist = (int)(best + 0.5);
    return true;
}

// ---------------------------------------------------------------- the button

void OverlayNext(int* traffic, int* parking)
{
    bool wasTraffic = *traffic != 0, wasParking = *parking != 0;
    *traffic = !wasTraffic && !wasParking;
    *parking = wasTraffic;
}
