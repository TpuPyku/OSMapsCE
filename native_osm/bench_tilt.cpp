// The third person's frame's speed (ring.cpp TiltWarp) on the device or the Device Emulator: frames like the app's
// (800 x 480, the near ring 5 tiles, the far one 6 tiles 600 px ahead, z15..z17's pitches, turning), timed; the
// pixels' sum to see a faster walk gives the same frame. Writes bench_tilt.txt next to the exe (and prints it).
#include "yamaps.h"
#include <stdio.h>
#include <stdlib.h>

void Log(const char*, ...) {}   // (image.cpp's: not this program's)
bool ReadWholeFile(const wchar_t*, unsigned char**, int*) { return false; }

static const int W = 800, H = 480, kNear = 5 * TILE, kFar = 6 * TILE;
static unsigned short* s_near;
static unsigned short* s_far;
static unsigned short* s_dst;

static RingView View(const unsigned short* bits, int px, double cx, double cy, double half)
{
    int i0 = (int)floor((cx - half) / TILE), j0 = (int)floor((cy - half) / TILE);
    int i1 = (int)floor((cx + half) / TILE), j1 = (int)floor((cy + half) / TILE);
    RingView v = { bits + (px - 1) * px, -px, px, 0, i0 * TILE, j0 * TILE, (i1 + 1) * TILE, (j1 + 1) * TILE };
    return v;
}

static unsigned long Frames(int pitch, int frames, DWORD* ms)
{
    unsigned long sum = 0;
    DWORD t0 = GetTickCount();
    for (int f = 0; f < frames; f++) {
        double angle = f * 13.0, vx = 10000.5 + f * 7.25, vy = 20000.25 - f * 5.5, a = angle * M_PI / 180;
        RingView rings[2] = { View(s_near, kNear, vx, vy, 511),
                              View(s_far, kFar, vx + 600 * sin(a), vy - 600 * cos(a), 639) };
        Tilt t;
        TiltMake(&t, H * 0.25, H * 0.25, 1150, pitch);
        TiltWarp(rings, 2, t, W, H, vx, vy, angle, f % 4 == 3, s_dst + (H - 1) * W, -W);
        for (int i = 0; i < W * H; i += 7)
            sum = sum * 31 + s_dst[i];
    }
    *ms = GetTickCount() - t0;
    return sum;
}

#ifdef UNDER_CE
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
#else
int main()
#endif
{
    s_near = (unsigned short*)malloc(kNear * kNear * 2);
    s_far = (unsigned short*)malloc(kFar * kFar * 2);
    s_dst = (unsigned short*)malloc(W * H * 2);
    for (int i = 0; i < kNear * kNear; i++)
        s_near[i] = (unsigned short)((i * 2654435761u) >> 16);
    for (int i = 0; i < kFar * kFar; i++)
        s_far[i] = (unsigned short)((i * 40503u) >> 3);
    char out[600];
    int n = 0;
    static const int pitches[3] = { 45, 55, 65 };
    for (int p = 0; p < 3; p++) {
        DWORD ms;
        unsigned long sum = Frames(pitches[p], 20, &ms);
        n += sprintf(out + n, "pitch %d: %lu ms a frame (20 frames), sum %08lx\r\n", pitches[p], ms / 20, sum);
    }
    {   // the frame to a 32-bit screen (the Vesta's unit: OutFrame)
        static unsigned long d32[W];
        unsigned long sum = 0;
        DWORD t0 = GetTickCount();
        for (int f = 0; f < 20; f++)
            for (int y = 0; y < H; y++) {
                Rgb565To32(s_near + y * W + f, d32, W);
                sum = sum * 31 + d32[y];
            }
        n += sprintf(out + n, "to 32 bits: %lu ms a frame, sum %08lx\r\n", (GetTickCount() - t0) / 20, sum);
    }
    wchar_t path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    wcscpy(slash ? slash + 1 : path, L"bench_tilt.txt");
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fputs(out, f);
        fclose(f);
    }
    printf("%s", out);
    return 0;
}
