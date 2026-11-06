// The text stamps (stamp.cpp): the runs, their blit, the budget, and GDI's own text against the stamp's.
#include "yamaps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void Log(const char*, ...) {}
bool ReadWholeFile(const wchar_t*, unsigned char**, int*) { return false; }

static int s_checks, s_failed;

static void Check(bool ok, const char* what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

enum { kT = 0, kW = 0xFFFF };

static void TestBuild()
{
    // a row: nothing, opaque red, half see-through, nothing
    WORD black[4] = { 0, 0xF800, 0x4208, 0 }, white[4] = { kW, 0xF800, 0xC618, kW };
    WORD out[16];
    int n = StampBuild(black, white, 4, 4, 1, out, 16);
    Check(n == 3 + 1 + 3 + 2, "build: two runs, the transparent ends left out");
    Check(out[0] == 0 && out[1] == 1 && out[2] == 1 && out[3] == 0xF800, "build: the opaque run: y, x, count, its colour");
    Check(out[4] == 0 && out[5] == 2 && out[6] == (1 | 0x8000) && out[7] == 0x4208 && out[8] == 0x8410,
          "build: the partial run: flagged, the colour and how much of the back comes through");
    Check(StampBuild(black, white, 4, 4, 1, out, n) == n, "build: exactly fits");
    Check(StampBuild(black, white, 4, 4, 1, out, n - 1) == -1, "build: no room - no stamp");
    WORD none[2] = { 0, 0 }, noneW[2] = { kW, kW };
    Check(StampBuild(none, noneW, 2, 2, 1, out, 16) == 0, "build: an empty box - no runs");
    WORD two[4] = { 0x1234, 0, 0, 0x1234 }, twoW[4] = { 0x1234, kW, kW, 0x1234 };
    n = StampBuild(two, twoW, 2, 2, 2, out, 16);
    Check(n == 8 && out[0] == 0 && out[1] == 0 && out[4] == 1 && out[5] == 1, "build: rows by the stride, each run its y");
    WORD up[1] = { 0xFFFF }, upW[1] = { 0x0000 };
    Check(StampBuild(up, upW, 1, 1, 1, out, 16) == 4 && out[3] == 0xFFFF, "build: white under black (rounding) - nothing comes through");
    WORD little[1] = { 0 }, littleW[1] = { 0x0841 };
    Check(StampBuild(little, littleW, 1, 1, 1, out, 16) == 5 && out[4] == 0x0841, "build: one step of each channel comes through");
    WORD gr[2] = { 0x07E0, 0x07E0 }, grW[2] = { 0x07E0, 0x07E0 };
    n = StampBuild(gr, grW, 2, 2, 1, out, 16);
    Check(n == 5 && out[2] == 2, "build: a run of two");
}

static void TestBlit()
{
    WORD runs[] = { 0, 1, 2, 0xF800, 0xF801 };
    WORD s[3 * 4];   // 4 wide, 3 high; the row's stride 4; top-down here
    memset(s, 0, sizeof(s));
    StampBlit(runs, 5, s, 4, 4, 3, 1, 1);
    Check(s[4 + 2] == 0xF800 && s[4 + 3] == 0xF801 && s[4 + 1] == 0, "blit: opaque, at its place");
    memset(s, 0, sizeof(s));
    StampBlit(runs, 5, s, 4, 4, 3, 2, 0);
    Check(s[3] == 0xF800 && s[4] == 0, "blit: cut at the right edge, the next row untouched");
    memset(s, 0, sizeof(s));
    StampBlit(runs, 5, s, 4, 4, 3, -2, 0);
    Check(s[0] == 0xF801 && s[1] == 0, "blit: cut at the left edge");
    memset(s, 0, sizeof(s));
    StampBlit(runs, 5, s, 4, 4, 3, -9, 0);
    StampBlit(runs, 5, s, 4, 4, 3, 9, 0);
    StampBlit(runs, 5, s, 4, 4, 3, 0, 3);
    StampBlit(runs, 5, s, 4, 4, 3, 0, -1);
    WORD zero[3 * 4];
    memset(zero, 0, sizeof(zero));
    Check(memcmp(s, zero, sizeof(s)) == 0, "blit: wholly outside - nothing");
    // the second run still found after a row cut off
    WORD two[] = { 0, 0, 1, 0x1111, 1, 0, 1, 0x2222 };
    memset(s, 0, sizeof(s));
    StampBlit(two, 8, s, 4, 4, 3, 0, -1);
    Check(s[0] == 0x2222, "blit: a row above the top skipped, the next drawn");
    WORD part[] = { 0, 0, 1 | 0x8000, 0x0000, 0xFFFF, 1, 0, 1 | 0x8000, 0x0000, 0x0000, 2, 0, 1 | 0x8000, 0x4208, 0x8410 };
    s[0] = 0x1234, s[4] = 0x1234, s[8] = 0xFFFF;
    StampBlit(part, 15, s, 4, 4, 3, 0, 0);
    Check(s[0] == 0x1234, "blit: all coming through: the back as it was");
    Check(s[4] == 0, "blit: nothing through, colour 0: black");
    int r = s[8] >> 11, g = (s[8] >> 5) & 63, b = s[8] & 31;
    Check(r == 8 + 16 && g == 16 + 32 && b == 8 + 16, "blit: half through: colour + half the back, each channel");
    WORD pc[] = { 0, 0, 2 | 0x8000, 0x0000, 0xFFFF, 0x0000, 0x0000, 1, 1, 1, 0x7777 };
    memset(s, 0, sizeof(s));
    s[0] = 0x5555;
    StampBlit(pc, 11, s, 4, 4, 3, -1, 0);
    Check(s[0] == 0 && s[4 + 0] == 0x7777, "blit: a partial run cut on the left: the second pixel from its own place");
}

static void TestBlitEdges()
{
    WORD s[4 * 4];
    // what lies after the runs is not theirs
    WORD runs[] = { 0, 0, 2, 0xF800, 0xF801, 0x9999, 0x9999, 0x9999 };
    memset(s, 0, sizeof(s));
    StampBlit(runs, 5, s, 4, 4, 3, -1, 0);
    Check(s[0] == 0xF801 && s[1] == 0 && s[2] == 0, "blit: cut on the left: only what is left of the run");
    memset(s, 0, sizeof(s));
    StampBlit(runs, 5, s, 4, 4, 3, 0, 3);
    Check(s[12] == 0 && s[13] == 0, "blit: the row just under the frame - nothing");
    // bottom-up: a negative stride from the last row
    memset(s, 0, sizeof(s));
    StampBlit(runs, 5, s + 2 * 4, -4, 4, 3, 1, 1);
    Check(s[4 + 1] == 0xF800 && s[4 + 2] == 0xF801, "blit: rows bottom-up");
    WORD full[] = { 0, 0, 1 | 0x8000, 0x0000, 0xFFFF };
    s[0] = 0xFFFF;
    StampBlit(full, 5, s, 4, 4, 3, 0, 0);
    Check(s[0] == 0xFFFF, "blit: the brightest back comes through whole, no carry");
}

static void TestCacheBudget()
{
    StampKey a = { L"abc", 3, 1, 2, 3, 4 }, b = { L"abc", 3, 1, 2, 3, 5 };
    WORD w[8];
    memset(w, 0, sizeof(w));
    int n;
    StampReset(100000);
    StampPut(a, w, 8);
    int one = StampWords();
    Check(one > 8 + 3, "budget: the runs, the text and the entry counted");
    StampPut(b, w, 8);
    Check(StampWords() == one * 2, "budget: each counted the same");
    StampReset(one);
    Check(StampPut(a, w, 8) && StampWords() == one, "budget: an exact fit is kept");
    Check(StampPut(b, w, 8) && StampWords() == one && !StampFind(a, &n) && StampFind(b, &n), "budget: the next one replaces it");
    StampReset(one - 1);
    Check(!StampPut(a, w, 8) && StampWords() == 0, "budget: one word short - not kept");
}

static StampKey ManyKey(int i, const wchar_t* s)
{
    StampKey k = { s, 1, i % 7, i % 11, i % 13, i };
    return k;
}

// More than the buckets: each finds its own whichever bucket it shares
static void TestCacheMany()
{
    enum { N = 3000 };
    WORD w[1];
    int n, wrong = 0;
    StampReset(10000000);
    for (int i = 0; i < N; i++) {
        w[0] = (WORD)i;
        StampPut(ManyKey(i, L"x"), w, 1);
    }
    for (int i = 0; i < N; i++) {
        const WORD* f = StampFind(ManyKey(i, L"x"), &n);
        if (!f || f[0] != (WORD)i)
            wrong++;
    }
    Check(wrong == 0, "many: each its own");
    StampKey other[5] = { { L"y", 1, 1, 1, 1, 1 }, { L"x", 1, 2, 1, 1, 1 }, { L"x", 1, 1, 2, 1, 1 }, { L"x", 1, 1, 1, 2, 1 }, { L"x", 1, 1, 1, 1, 2 } };
    StampReset(10000000);
    wrong = 0;
    for (int i = 0; i < 5; i++) {
        w[0] = (WORD)i;
        StampPut(other[i], w, 1);
    }
    for (int i = 0; i < 5; i++) {
        const WORD* f = StampFind(other[i], &n);
        if (!f || f[0] != (WORD)i)
            wrong++;
    }
    Check(wrong == 0, "many: one field's difference is another stamp");
    StampReset(10000000);
    for (int i = 0; i < N; i++)
        StampPut(ManyKey(i, L"x"), w, 1);
    int one = StampWords() / N;
    StampReset(one * 500);
    for (int i = 0; i < N; i++)
        StampPut(ManyKey(i, L"x"), w, 1);
    int kept = 0;
    bool newest = true;
    for (int i = 0; i < N; i++)
        if (StampFind(ManyKey(i, L"x"), &n)) {
            kept++;
            if (i < N - 500)
                newest = false;
        }
    Check(kept == 500 && newest, "many: the budget keeps the last ones, from any bucket");
    StampReset(10000000);
    for (int i = 0; i < N; i++)
        StampPut(ManyKey(i, L"x"), w, 1);
    StampReset(10000000);
    kept = 0;
    for (int i = 0; i < N; i++)
        if (StampFind(ManyKey(i, L"x"), &n))
            kept++;
    Check(kept == 0 && StampWords() == 0, "many: reset clears every bucket");
}

static void TestCache()
{
    StampReset(1000);
    StampKey a = { L"abc", 3, 1, 2, 3, 4 }, b = { L"abd", 3, 1, 2, 3, 4 }, c = { L"abc", 2, 1, 2, 3, 4 }, d = { L"abc", 3, 1, 2, 3, 5 };
    WORD w[400];
    memset(w, 0, sizeof(w));
    int n;
    Check(!StampFind(a, &n), "cache: empty");
    Check(StampPut(a, w, 300), "cache: put");
    const WORD* f = StampFind(a, &n);
    Check(f && n == 300, "cache: found, its length");
    Check(!StampFind(b, &n), "cache: another text");
    Check(!StampFind(c, &n), "cache: another length");
    Check(!StampFind(d, &n), "cache: another int");
    Check(StampPut(b, w, 300) && StampFind(b, &n) && StampFind(a, &n), "cache: both held in the budget");
    Check(StampPut(d, w, 300), "cache: the third");
    StampFind(a, &n);   // a used last of the old ones
    StampFind(d, &n);
    StampKey e = { L"xyz", 3, 9, 9, 9, 9 };
    Check(StampPut(e, w, 300), "cache: the fourth");
    Check(StampFind(a, &n) && StampFind(d, &n) && StampFind(e, &n) && !StampFind(b, &n), "cache: the longest unused went");
    Check(StampWords() <= 1000, "cache: within the budget");
    Check(!StampPut(c, w, 1001), "cache: bigger than the budget - not kept");
    StampReset(1000);
    Check(!StampFind(a, &n) && StampWords() == 0, "cache: reset");
}

// GDI's own drawing and the stamp's over the same picture: the same
struct TextArg { HFONT font; COLORREF c; const wchar_t* s; };

static void TestPaint(HDC dc, int x, int y, void* arg)
{
    TextArg* t = (TextArg*)arg;
    SelectObject(dc, t->font);
    SetTextColor(dc, t->c);
    ExtTextOut(dc, x, y, 0, NULL, t->s, (UINT)wcslen(t->s), NULL);
}

static void Picture(WORD* bits, int w, int h)
{
    for (int i = 0; i < w * h; i++)
        bits[i] = (WORD)(i * 7);
}

struct Seen { HDC dc; int x, y, n; };

static void SeePaint(HDC dc, int x, int y, void* arg)
{
    Seen* s = (Seen*)arg;
    s->dc = dc;
    s->x = x;
    s->y = y;
    s->n++;
}

// A box that needs more words than a stamp may have: fills its box, opaque
static void FillPaint(HDC dc, int x, int y, void* arg)
{
    RECT r = { x, y, x + 300, y + 150 };
    HBRUSH b = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(dc, &r, b);
    DeleteObject(b);
    Seen* s = (Seen*)arg;
    s->x = x;
    s->y = y;
    s->n++;
}

static void TestDirect(HDC other, HDC target, WORD* bits, int W, int H)
{
    StampKey k = { L"d", 1, 1, 1, 1, 1 };
    Seen s = { NULL, 0, 0, 0 };
    int h, m, d;
    StampTarget(target, bits + (H - 1) * W, -W, W, H);
    StampStats(&h, &m, &d);
    StampDraw(other, k, 5, 6, 10, 10, SeePaint, &s, 20, 30);
    Check(s.dc == other && s.x == 25 && s.y == 36, "direct: another dc - at the box + the origin");
    s.n = 0;
    StampDraw(target, k, 5, 6, 401, 10, SeePaint, &s, 20, 30);
    Check(s.dc == target && s.x == 25 && s.y == 36 && s.n == 1, "direct: wider than the temp");
    StampDraw(target, k, 5, 6, 10, 201, SeePaint, &s, 20, 30);
    Check(s.n == 2, "direct: higher than the temp");
    StampDraw(target, k, 5, 6, 0, 10, SeePaint, &s, 20, 30);
    StampDraw(target, k, 5, 6, 10, 0, SeePaint, &s, 20, 30);
    Check(s.n == 4, "direct: an empty box");
    StampStats(&h, &m, &d);
    Check(d == 5 && m == 0, "direct: all counted as such");
    s.n = 0;
    StampKey k2 = { L"e", 1, 1, 1, 1, 1 };
    StampDraw(target, k2, 0, 0, 400, 200, SeePaint, &s, 0, 0);
    StampStats(&h, &m, &d);
    Check(s.dc != target && s.n == 2 && m == 1 && d == 0, "temp: exactly its size fits; black and white");
    s.n = 0;
    s.dc = NULL;
    StampKey k3 = { L"f", 1, 1, 1, 1, 1 };
    StampDraw(target, k3, 7, 8, 300, 150, FillPaint, &s, 3, 4);
    Check(s.n == 3 && s.x == 10 && s.y == 12, "too many words: drawn on black, on white, then directly, at its place");
    int was;
    Check(!StampFind(k3, &was), "too many words: not kept");
}

static void TestGdi(int quality)
{
    enum { W = 300, H = 60 };
    Image im, im2;
    WORD* a = ImageCreate(W, H, &im), *b = ImageCreate(W, H, &im2);
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen), dc2 = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    SelectObject(dc, im.bmp);
    SelectObject(dc2, im2.bmp);
    SetBkMode(dc, TRANSPARENT);
    SetBkMode(dc2, TRANSPARENT);
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -19;
    lf.lfQuality = (BYTE)quality;
    wcscpy(lf.lfFaceName, L"Tahoma");
    HFONT font = CreateFontIndirect(&lf);
    TextArg t = { font, RGB(200, 90, 20), L"Ленинградский 15" };
    Picture(a, W, H);
    Picture(b, W, H);
    GdiFlush();
    TestPaint(dc, 12, 20, &t);
    GdiFlush();
    StampReset(32 * 1024);
    StampTarget(dc2, b + (H - 1) * W, -W, W, H);
    StampKey key = { t.s, (int)wcslen(t.s), (int)(INT_PTR)font, (int)t.c, 0, 0 };
    int ht, ms, dr0;
    StampStats(&ht, &ms, &dr0);
    StampDraw(dc2, key, 2, 10, 280, 40, TestPaint, &t, 10, 10);   // first: makes the stamp
    Picture(b, W, H);
    StampDraw(dc2, key, 2, 10, 280, 40, TestPaint, &t, 10, 10);   // then: from it
    StampStats(&ht, &ms, &dr0);
    Check(ht == 1 && ms == 1 && dr0 == 0, "gdi: made once, then ready");
    StampTarget(NULL, NULL, 0, 0, 0);
    int worst = 0, inked = 0;
    for (int i = 0; i < W * H; i++) {
        int x = i % W, y = i / W;
        WORD p = a[i], q = b[i];
        int base = (WORD)(i * 7);
        if (p != base)
            inked++;
        int dr = abs((p >> 11) - (q >> 11)), dg = abs(((p >> 5) & 63) - ((q >> 5) & 63)), db = abs((p & 31) - (q & 31));
        int d = dr > db ? dr : db;
        d = d > dg / 2 ? d : dg / 2;
        if (d > worst)
            worst = d;
        (void)x, (void)y;
    }
    char m[120];
    sprintf(m, "gdi: the stamp is GDI's text (quality %d): worst %d of 31, %d px inked", quality, worst, inked);
    Check(inked > 100, "gdi: there is text at all");
    Check(worst <= (quality == NONANTIALIASED_QUALITY ? 0 : 3), m);
    StampKey big = { L"x", 1, 0, 0, 0, 0 };
    int hits0, misses0, direct0, hits, misses, direct;
    StampStats(&hits0, &misses0, &direct0);
    StampTarget(dc2, b + (H - 1) * W, -W, W, H);
    StampDraw(dc2, big, 0, 0, 2000, 20, TestPaint, &t, 0, 0);
    StampDraw(dc, big, 0, 0, 20, 20, TestPaint, &t, 0, 0);
    StampStats(&hits, &misses, &direct);
    Check(direct == 2 && hits == 0 && misses == 0, "gdi: too wide for the temp, or another dc - drawn directly, counted");
    TestDirect(dc, dc2, b, W, H);
    StampTarget(NULL, NULL, 0, 0, 0);
    DeleteObject(font);
    DeleteDC(dc);
    DeleteDC(dc2);
    ImageFree(&im);
    ImageFree(&im2);
}

int main()
{
    TestBuild();
    TestBlit();
    TestBlitEdges();
    TestCacheBudget();
    TestCacheMany();
    TestCache();
    TestGdi(NONANTIALIASED_QUALITY);
    TestGdi(ANTIALIASED_QUALITY);
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
