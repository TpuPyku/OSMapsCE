// The voice prompts: what to say for a maneuver and when (voice.h).
#include "voice.h"
#include <string.h>

static bool Has(const char* s, const char* part) { return strstr(s, part) != 0; }

static int Side(const char* mod, int right, int left) { return Has(mod, "right") ? right : Has(mod, "left") ? left : VA_STRAIGHT; }

static int Turn(const char* mod)
{
    if (Has(mod, "slight"))
        return Side(mod, VA_SLIGHT_RIGHT, VA_SLIGHT_LEFT);
    if (Has(mod, "sharp"))
        return Side(mod, VA_SHARP_RIGHT, VA_SHARP_LEFT);
    return Side(mod, VA_RIGHT, VA_LEFT);
}

static int Round(const char* type, int exit)   // -1: not a roundabout
{
    if (strcmp(type, "roundabout") && strcmp(type, "rotary"))
        return -1;
    return exit >= 1 && exit <= 6 ? VA_ROUND1 + exit - 1 : VA_ROUND;
}

// The types said by their side alone (VA_NONE both: nothing to say - the start, the same road, leaving a roundabout)
static const struct { const char* type; int right, left; } kByType[] = {
    { "depart", VA_NONE, VA_NONE }, { "new name", VA_NONE, VA_NONE }, { "exit roundabout", VA_NONE, VA_NONE },
    { "exit rotary", VA_NONE, VA_NONE }, { "fork", VA_KEEP_RIGHT, VA_KEEP_LEFT }, { "merge", VA_KEEP_RIGHT, VA_KEEP_LEFT },
    { "use lane", VA_KEEP_RIGHT, VA_KEEP_LEFT }, { "on ramp", VA_EXIT_RIGHT, VA_EXIT_LEFT },
    { "off ramp", VA_EXIT_RIGHT, VA_EXIT_LEFT }
};

static int ByType(const char* type, const char* mod)   // -1: not one of kByType
{
    for (int i = 0; i < (int)(sizeof(kByType) / sizeof(kByType[0])); i++)
        if (!strcmp(type, kByType[i].type))
            return kByType[i].right == VA_NONE ? VA_NONE : Side(mod, kByType[i].right, kByType[i].left);
    return -1;
}

static bool Onward(const char* type, const char* mod) { return !strcmp(type, "continue") && !strcmp(mod, "straight"); }

int VoiceAct(const char* type, const char* mod, int exit, bool stop)
{
    if (!strcmp(type, "arrive"))
        return stop ? VA_VIA : VA_FINISH;
    int act = Round(type, exit);
    if (act < 0)
        act = !strcmp(mod, "uturn") ? VA_UTURN : ByType(type, mod);
    if (act < 0)
        act = Onward(type, mod) ? VA_NONE : Turn(mod);
    return act;
}

void VoiceReset(VoiceState* s)
{
    s->man = -1;
    s->said = 0;
}

// Each prompt's distance at kmh: the far and mid ones by the road's speed, "now" 7 s ahead (40 m at least).
static int Threshold(int stage, int kmh)
{
    if (stage == VS_FAR)
        return kmh >= 80 ? 2000 : kmh >= 50 ? 1000 : 500;
    if (stage == VS_MID)
        return kmh >= 80 ? 800 : kmh >= 50 ? 300 : 200;
    int now = kmh * 70 / 36;
    return now > 40 ? now : 40;
}

int VoiceDue(VoiceState* s, int man, int nextM, int kmh)
{
    if (man != s->man) {
        s->man = man;
        s->said = 0;
    }
    for (int stage = VS_FAR; stage <= VS_NOW; stage <<= 1) {
        if ((s->said & stage) || nextM > Threshold(stage, kmh))
            continue;
        s->said |= stage;
        if (stage == VS_NOW || nextM > Threshold(stage << 1, kmh))
            return stage;
    }
    return 0;
}

static const char* const kAct[] = {
    0, "right", "left", "slight_right", "slight_left", "sharp_right", "sharp_left", "uturn", "keep_right", "keep_left",
    "straight", "exit_right", "exit_left", "round", "via", "finish", "round1", "round2", "round3", "round4", "round5",
    "round6"
};

static const char* Distance(int m)
{
    static const int kM[] = { 100, 150, 200, 300, 400, 500, 600, 800, 1000, 1500, 2000, 3000, 5000 };
    static const char* const kId[] = { "d100", "d150", "d200", "d300", "d400", "d500", "d600", "d800", "d1000",
                                       "d1500", "d2000", "d3000", "d5000" };
    int best = 0;
    for (int i = 1; i < (int)(sizeof(kM) / sizeof(kM[0])); i++) {
        int d = m - kM[i], b = m - kM[best];
        if ((d < 0 ? -d : d) < (b < 0 ? -b : b))
            best = i;
    }
    return kId[best];
}

const char* VoiceEvent(const VoiceRoute& was, const VoiceRoute& now)
{
    if (now.arrived)
        return was.arrived ? 0 : "arrived";
    if (!now.on)
        return 0;
    if (now.serial != was.serial || !was.on)
        return was.on ? "route_new" : "route_ready";
    return now.vias < was.vias ? "via_reached" : 0;
}

static unsigned U32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int U16(const unsigned char* p) { return p[0] | p[1] << 8; }

static bool Riff(const unsigned char* b, int n) { return n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4); }

// A chunk of the file: where its bytes start (-1: none), its size
static int Chunk(const unsigned char* b, int n, const char* id, unsigned* size)
{
    for (int at = 12; at + 8 <= n; at += 8 + (int)*size + (int)(*size & 1)) {
        *size = U32(b + at + 4);
        if (!memcmp(b + at, id, 4))
            return at + 8;
        if (*size > (unsigned)n)
            return -1;
    }
    return -1;
}

bool VoiceWavParse(const unsigned char* b, int n, VoiceWav* w)
{
    unsigned fs = 0, ds = 0;
    int f = Riff(b, n) ? Chunk(b, n, "fmt ", &fs) : -1, d = f >= 0 ? Chunk(b, n, "data", &ds) : -1;
    if (d < 0 || fs < 16 || f + 16 > n || U16(b + f) != 1)
        return false;
    w->channels = U16(b + f + 2);
    w->rate = (int)U32(b + f + 4);
    w->bits = U16(b + f + 14);
    w->at = d;
    w->len = ds < (unsigned)(n - d) ? (int)ds : n - d;
    return true;
}

static bool Silent(int stage, int act) { return act == VA_NONE || (act == VA_FINISH && stage == VS_NOW); }

int VoiceSay(int stage, int act, int nextM, int then, const char** out, int cap)
{
    const char* c[4];
    int n = 0;
    if (stage != VS_NOW)
        c[n++] = Distance(nextM);
    c[n++] = kAct[act];
    int must = n;   // ("then" and its action: whole or not at all)
    if (stage == VS_NOW && then != VA_NONE) {
        c[n++] = "then";
        c[n++] = kAct[then];
    }
    n = n > cap ? must : n;
    if (Silent(stage, act) || n > cap)
        return 0;
    memcpy(out, c, n * sizeof(c[0]));
    return n;
}

int VoiceDevice(int via, int devs)
{
    return via == 2 && devs > 1 ? 1 : -1;
}
