// Text as ready pixels. GDI's text is the dearest part of a frame on a slow unit (Shturmann 2026-11: names 23-26 ms, panels
// 30-43 ms of 85 ms, a name with its halo being 9 draws), and from frame to frame it is mostly the same text. So a text is
// drawn once on black and once on white into a small picture: where the two agree it is opaque, where they are black and
// white nothing is there, between the two the difference tells how much of the back comes through, channel by channel
// (the unit's smoothing counts too). Kept as runs of pixels per row, within a budget, the longest unused going first.
#include "yamaps.h"
#include <stdlib.h>
#include <string.h>

enum { kFlagPartial = 0x8000, kBuckets = 256 };
enum { kTempW = 400, kTempH = 200, kBudgetWords = 128 * 1024, kWorkWords = 32 * 1024 };   // (the budget: 256 KB)

// How much of the back comes through a pixel: white minus black in each channel (none where rounding turned them over)
static WORD Through(WORD onBlack, WORD onWhite)
{
    int r = (onWhite >> 11) - (onBlack >> 11), g = ((onWhite >> 5) & 63) - ((onBlack >> 5) & 63);
    int b = (onWhite & 31) - (onBlack & 31);
    return (WORD)((r > 0 ? r << 11 : 0) | (g > 0 ? g << 5 : 0) | (b > 0 ? b : 0));
}

enum { PX_NONE, PX_OPAQUE, PX_PARTIAL };

static int PixelKind(WORD onBlack, WORD onWhite)
{
    WORD t = Through(onBlack, onWhite);
    return t == 0xFFFF ? PX_NONE : t == 0 ? PX_OPAQUE : PX_PARTIAL;
}

// A run x..e of a kind: its words at out[n]; the new n, -1 no room
static int PutRun(const WORD* pb, const WORD* pw, int y, int x, int e, int kind, WORD* out, int n, int cap)
{
    int count = e - x, size = 3 + (kind == PX_PARTIAL ? count * 2 : count);
    if (n + size > cap)
        return -1;
    out[n++] = (WORD)y;
    out[n++] = (WORD)x;
    out[n++] = (WORD)(count | (kind == PX_PARTIAL ? kFlagPartial : 0));
    for (; x < e; x++) {
        out[n++] = pb[x];
        if (kind == PX_PARTIAL)
            out[n++] = Through(pb[x], pw[x]);
    }
    return n;
}

int StampBuild(const WORD* onBlack, const WORD* onWhite, int stride, int w, int h, WORD* out, int cap)
{
    int n = 0;
    for (int y = 0; y < h && n >= 0; y++) {
        const WORD* pb = onBlack + y * stride, *pw = onWhite + y * stride;
        for (int x = 0; x < w && n >= 0;) {
            int kind = PixelKind(pb[x], pw[x]), e = x + 1;
            while (e < w && PixelKind(pb[e], pw[e]) == kind)
                e++;
            if (kind != PX_NONE)
                n = PutRun(pb, pw, y, x, e, kind, out, n, cap);
            x = e;
        }
    }
    return n;
}

// The colour plus the back times what comes through (33 / 1024 is 1 / 31, 65 / 4096 is 1 / 63: no division on the ARM)
static WORD Blend(WORD back, WORD color, WORD through)
{
    int r = (color >> 11) + (((back >> 11) * (through >> 11) * 33 + 512) >> 10);
    int g = ((color >> 5) & 63) + ((((back >> 5) & 63) * ((through >> 5) & 63) * 65 + 2048) >> 12);
    int b = (color & 31) + (((back & 31) * (through & 31) * 33 + 512) >> 10);
    return (WORD)(r << 11 | g << 5 | b);
}

// One run on the frame's row, cut to its width: count pixels of step words each from data, at rx
static void BlitRun(const WORD* data, int count, int step, WORD* row, int cw, int rx)
{
    int from = rx < 0 ? -rx : 0, to = rx + count > cw ? cw - rx : count;
    if (from >= to)
        return;
    WORD* d = row + rx + from;
    data += from * step;
    if (step == 1) {
        memcpy(d, data, (to - from) * sizeof(WORD));
        return;
    }
    for (int i = from; i < to; i++, d++, data += 2)
        *d = Blend(*d, data[0], data[1]);
}

void StampBlit(const WORD* runs, int words, WORD* frame, int stride, int cw, int ch, int x, int y)
{
    const WORD* p = runs, *end = runs + words;
    while (p < end) {
        int ry = p[0] + y, count = p[2] & ~kFlagPartial, step = p[2] & kFlagPartial ? 2 : 1;
        if (ry >= 0 && ry < ch)
            BlitRun(p + 3, count, step, frame + ry * stride, cw, p[1] + x);
        p += 3 + count * step;
    }
}

// ---- the kept stamps

struct Entry {
    Entry* next;
    DWORD used;
    int n, a, b, c, d, words;
    // then: the text (n wchar_t), the runs
    const wchar_t* Text() const { return (const wchar_t*)(this + 1); }
    const WORD* Runs() const { return (const WORD*)(Text() + n); }
};

static Entry* s_bucket[kBuckets];
static int s_budget = kBudgetWords, s_words;
static DWORD s_clock;

static int EntryWords(const Entry* e)
{
    return e->words + e->n + sizeof(Entry) / sizeof(WORD);
}

static DWORD KeyHash(const StampKey& k)
{
    DWORD h = 2166136261u;
    for (int i = 0; i < k.n; i++)
        h = (h ^ k.s[i]) * 16777619u;
    h = (h ^ (DWORD)k.a) * 16777619u;
    h = (h ^ (DWORD)k.b) * 16777619u;
    h = (h ^ (DWORD)k.c) * 16777619u;
    return (h ^ (DWORD)k.d) * 16777619u;
}

static bool KeySame(const Entry* e, const StampKey& k)
{
    return e->n == k.n && e->a == k.a && e->b == k.b && e->c == k.c && e->d == k.d &&
           memcmp(e->Text(), k.s, k.n * sizeof(wchar_t)) == 0;
}

void StampReset(int budgetWords)
{
    for (int i = 0; i < kBuckets; i++)
        while (s_bucket[i]) {
            Entry* e = s_bucket[i];
            s_bucket[i] = e->next;
            free(e);
        }
    s_words = 0;
    s_budget = budgetWords;
}

const WORD* StampFind(const StampKey& k, int* words)
{
    for (Entry* e = s_bucket[KeyHash(k) % kBuckets]; e; e = e->next)
        if (KeySame(e, k)) {
            e->used = ++s_clock;
            *words = e->words;
            return e->Runs();
        }
    return NULL;
}

// The longest unused one out
static void DropOldest()
{
    Entry** oldest = NULL;
    for (int i = 0; i < kBuckets; i++)
        for (Entry** pe = &s_bucket[i]; *pe; pe = &(*pe)->next)
            if (!oldest || (*pe)->used < (*oldest)->used)
                oldest = pe;
    Entry* e = *oldest;
    *oldest = e->next;
    s_words -= EntryWords(e);
    free(e);
}

bool StampPut(const StampKey& k, const WORD* runs, int words)
{
    int need = words + k.n + sizeof(Entry) / sizeof(WORD);
    if (need > s_budget)
        return false;
    while (s_words + need > s_budget)
        DropOldest();
    Entry* e = (Entry*)malloc(need * sizeof(WORD));
    if (!e)
        return false;
    DWORD h = KeyHash(k);
    e->used = ++s_clock;
    e->n = k.n;
    e->a = k.a;
    e->b = k.b;
    e->c = k.c;
    e->d = k.d;
    e->words = words;
    memcpy((void*)e->Text(), k.s, k.n * sizeof(wchar_t));
    memcpy((void*)e->Runs(), runs, words * sizeof(WORD));
    e->next = s_bucket[h % kBuckets];
    s_bucket[h % kBuckets] = e;
    s_words += need;
    return true;
}

int StampWords()
{
    return s_words;
}

// ---- on the frame

static HDC s_target;
static WORD* s_frame;
static int s_stride, s_cw, s_ch;
static int s_hits, s_misses, s_direct;

static Image s_temp;       // what a new text is drawn on, kept
static WORD* s_tempBits, *s_tempBlack, *s_work;
static HDC s_tempDc;

void StampTarget(HDC dc, WORD* frame, int stride, int cw, int ch)
{
    s_target = dc;
    s_frame = frame;
    s_stride = stride;
    s_cw = cw;
    s_ch = ch;
}

void StampStats(int* hits, int* misses, int* direct)
{
    *hits = s_hits;
    *misses = s_misses;
    *direct = s_direct;
    s_hits = s_misses = s_direct = 0;
}

static bool TempReady(HDC dc)
{
    if (s_tempDc)
        return true;
    s_tempBlack = (WORD*)malloc(kTempW * kTempH * sizeof(WORD));
    s_work = (WORD*)malloc(kWorkWords * sizeof(WORD));
    s_tempBits = s_tempBlack && s_work ? ImageCreate(kTempW, kTempH, &s_temp) : NULL;
    if (!s_tempBits)
        return false;
    s_tempDc = CreateCompatibleDC(dc);
    SelectObject(s_tempDc, s_temp.bmp);
    SetBkMode(s_tempDc, TRANSPARENT);
    return true;
}

// The box drawn on a plain back; into "to" (rows top-down, w wide)
static void DrawOn(WORD back, int w, int h, StampPaint paint, void* arg, int ox, int oy, WORD* to)
{
    for (int y = 0; y < h; y++) {
        WORD* row = s_tempBits + (kTempH - 1 - y) * kTempW;
        for (int x = 0; x < w; x++)
            row[x] = back;
    }
    paint(s_tempDc, ox, oy, arg);
#ifndef UNDER_CE
    GdiFlush();
#endif
    for (int y = 0; y < h; y++)
        memcpy(to + y * w, s_tempBits + (kTempH - 1 - y) * kTempW, w * sizeof(WORD));
}

// A new text's runs into s_work: their words, -1 none (no memory, too many)
static int MakeRuns(int w, int h, StampPaint paint, void* arg, int ox, int oy)
{
    WORD* onWhite = (WORD*)malloc(w * h * sizeof(WORD));
    if (!onWhite)
        return -1;
    DrawOn(0, w, h, paint, arg, ox, oy, s_tempBlack);
    DrawOn(0xFFFF, w, h, paint, arg, ox, oy, onWhite);
    int words = StampBuild(s_tempBlack, onWhite, w, w, h, s_work, kWorkWords);
    free(onWhite);
    return words;
}

static bool Stampable(HDC dc, int w, int h)
{
    return dc == s_target && w > 0 && h > 0 && w <= kTempW && h <= kTempH && TempReady(dc);
}

void StampDraw(HDC dc, const StampKey& key, int x, int y, int w, int h, StampPaint paint, void* arg, int ox, int oy)
{
    if (!Stampable(dc, w, h)) {
        s_direct++;
        paint(dc, x + ox, y + oy, arg);
        return;
    }
#ifndef UNDER_CE
    GdiFlush();   // (the frame's pixels up to date before they are touched here)
#endif
    int words;
    const WORD* runs = StampFind(key, &words);
    if (runs) {
        s_hits++;
        StampBlit(runs, words, s_frame, s_stride, s_cw, s_ch, x, y);
        return;
    }
    s_misses++;
    words = MakeRuns(w, h, paint, arg, ox, oy);
    if (words < 0) {
        paint(dc, x + ox, y + oy, arg);
        return;
    }
    StampPut(key, s_work, words);
    StampBlit(s_work, words, s_frame, s_stride, s_cw, s_ch, x, y);
}
