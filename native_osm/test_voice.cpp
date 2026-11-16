// The voice prompts (voice.cpp): OSRM's maneuver -> the action, when each prompt is due, its clips.
#include "voice.h"
#include <stdio.h>
#include <string.h>

static int s_checks, s_failed;

static void Check(bool ok, const char* what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

static void TestAct()
{
    Check(VoiceAct("turn", "right", 0, false) == VA_RIGHT, "act: turn right");
    Check(VoiceAct("turn", "left", 0, false) == VA_LEFT, "act: turn left");
    Check(VoiceAct("end of road", "right", 0, false) == VA_RIGHT, "act: end of the road - a turn");
    Check(VoiceAct("turn", "slight right", 0, false) == VA_SLIGHT_RIGHT, "act: slight right");
    Check(VoiceAct("turn", "slight left", 0, false) == VA_SLIGHT_LEFT, "act: slight left");
    Check(VoiceAct("turn", "sharp right", 0, false) == VA_SHARP_RIGHT, "act: sharp right");
    Check(VoiceAct("turn", "sharp left", 0, false) == VA_SHARP_LEFT, "act: sharp left");
    Check(VoiceAct("turn", "uturn", 0, false) == VA_UTURN, "act: a U-turn");
    Check(VoiceAct("continue", "uturn", 0, false) == VA_UTURN, "act: a U-turn of any type");
    Check(VoiceAct("fork", "slight right", 0, false) == VA_KEEP_RIGHT, "act: a fork - keep right");
    Check(VoiceAct("fork", "left", 0, false) == VA_KEEP_LEFT, "act: a fork - keep left");
    Check(VoiceAct("merge", "slight left", 0, false) == VA_KEEP_LEFT, "act: a merge - keep");
    Check(VoiceAct("use lane", "right", 0, false) == VA_KEEP_RIGHT, "act: a lane - keep");
    Check(VoiceAct("fork", "straight", 0, false) == VA_STRAIGHT, "act: a fork straight on");
    Check(VoiceAct("off ramp", "slight right", 0, false) == VA_EXIT_RIGHT, "act: an off ramp right");
    Check(VoiceAct("on ramp", "left", 0, false) == VA_EXIT_LEFT, "act: an on ramp left");
    Check(VoiceAct("roundabout", "right", 2, false) == VA_ROUND1 + 1, "act: the roundabout's 2nd exit");
    Check(VoiceAct("rotary", "", 6, false) == VA_ROUND1 + 5, "act: a rotary's 6th exit");
    Check(VoiceAct("roundabout", "", 7, false) == VA_ROUND, "act: beyond the 6th exit - just the roundabout");
    Check(VoiceAct("roundabout", "", 1, false) == VA_ROUND1, "act: the 1st exit");
    Check(VoiceAct("depart", "right", 0, false) == VA_NONE, "act: the start, whatever its side");
    Check(VoiceAct("roundabout", "", 0, false) == VA_ROUND, "act: no exit number - the roundabout");
    Check(VoiceAct("exit roundabout", "right", 0, false) == VA_NONE, "act: leaving the roundabout - said before");
    Check(VoiceAct("arrive", "", 0, false) == VA_FINISH, "act: the destination");
    Check(VoiceAct("arrive", "", 0, true) == VA_VIA, "act: a stop on the way");
    Check(VoiceAct("depart", "", 0, false) == VA_NONE, "act: the start - nothing");
    Check(VoiceAct("new name", "straight", 0, false) == VA_NONE, "act: a new name - nothing");
    Check(VoiceAct("continue", "straight", 0, false) == VA_NONE, "act: straight on - nothing");
    Check(VoiceAct("continue", "slight left", 0, false) == VA_SLIGHT_LEFT, "act: the road bends - bear");
    Check(VoiceAct("turn", "straight", 0, false) == VA_STRAIGHT, "act: a turn straight across");
}

static void TestDue()
{
    VoiceState s;
    VoiceReset(&s);
    // in town (50 km/h): far at 1 km, mid at 300 m, now at 97 m (7 s)
    Check(VoiceDue(&s, 3, 1500, 50) == 0, "due: too far");
    Check(VoiceDue(&s, 3, 990, 50) == VS_FAR, "due: far at 1 km");
    Check(VoiceDue(&s, 3, 900, 50) == 0, "due: far said once");
    Check(VoiceDue(&s, 3, 290, 50) == VS_MID, "due: mid at 300 m");
    Check(VoiceDue(&s, 3, 200, 50) == 0, "due: mid said once");
    Check(VoiceDue(&s, 3, 90, 50) == VS_NOW, "due: now at 7 s");
    Check(VoiceDue(&s, 3, 20, 50) == 0, "due: now said once");
    // the next maneuver: its own prompts; close behind - the far one skipped
    Check(VoiceDue(&s, 4, 120, 50) == VS_MID, "due: the next one at 120 m - its mid, the far one unsaid");
    Check(VoiceDue(&s, 4, 95, 50) == VS_NOW, "due: then its now");
    Check(VoiceDue(&s, 5, 60, 50) == VS_NOW, "due: one within now's range - only now");
    Check(VoiceDue(&s, 5, 50, 50) == 0, "due: and no more");
    // on a highway (100 km/h): far at 2 km, mid at 800 m, now at 194 m
    VoiceReset(&s);
    Check(VoiceDue(&s, 0, 1990, 100) == VS_FAR, "due: highway far at 2 km");
    Check(VoiceDue(&s, 0, 790, 100) == VS_MID, "due: highway mid at 800 m");
    Check(VoiceDue(&s, 0, 190, 100) == VS_NOW, "due: highway now at 7 s");
    // slow: far at 500 m, mid at 200 m, now at 40 m at least
    VoiceReset(&s);
    Check(VoiceDue(&s, 0, 490, 10) == VS_FAR, "due: slow far at 500 m");
    Check(VoiceDue(&s, 0, 190, 10) == VS_MID, "due: slow mid at 200 m");
    Check(VoiceDue(&s, 0, 45, 10) == 0, "due: slow - not at 45 m");
    Check(VoiceDue(&s, 0, 40, 10) == VS_NOW, "due: slow now at 40 m");
    // a route's first maneuver in the mid range: only mid and now
    VoiceReset(&s);
    Check(VoiceDue(&s, 0, 250, 50) == VS_MID, "due: started in the mid range");
    Check(VoiceDue(&s, 0, 240, 50) == 0, "due: far skipped for good");
    // a reset forgets the maneuver and what was said of it
    s.man = 5;
    s.said = VS_FAR | VS_MID;
    VoiceReset(&s);
    Check(s.man == -1 && s.said == 0, "due: reset");
}

static int Due(int nextM, int kmh)   // the first prompt due for a new maneuver
{
    VoiceState s;
    VoiceReset(&s);
    return VoiceDue(&s, 0, nextM, kmh);
}

static void TestThresholds()
{
    // the far one: 2 km from 80 km/h, 1 km from 50, else 500 m - just at it, not a metre farther
    Check(Due(2000, 80) == VS_FAR && Due(2001, 80) == 0, "threshold: far at 80 km/h - 2000 m");
    Check(Due(1001, 79) == 0 && Due(1000, 79) == VS_FAR, "threshold: far at 79 km/h - 1000 m");
    Check(Due(1000, 50) == VS_FAR && Due(1001, 50) == 0, "threshold: far at 50 km/h - 1000 m");
    Check(Due(501, 49) == 0 && Due(500, 49) == VS_FAR, "threshold: far at 49 km/h - 500 m");
    // the mid one: 800 / 300 / 200 m (the far one then late)
    Check(Due(800, 80) == VS_MID && Due(801, 80) == VS_FAR, "threshold: mid at 80 km/h - 800 m");
    Check(Due(301, 79) == VS_FAR && Due(300, 79) == VS_MID, "threshold: mid at 79 km/h - 300 m");
    Check(Due(300, 50) == VS_MID && Due(301, 50) == VS_FAR, "threshold: mid at 50 km/h - 300 m");
    Check(Due(201, 49) == VS_FAR && Due(200, 49) == VS_MID, "threshold: mid at 49 km/h - 200 m");
    // now: 7 s ahead (36 km/h: 70 m), 40 m at least
    Check(Due(70, 36) == VS_NOW && Due(71, 36) == VS_MID, "threshold: now at 36 km/h - 70 m");
    Check(Due(42, 22) == VS_NOW && Due(43, 22) == VS_MID, "threshold: now at 22 km/h - 42 m");
    Check(Due(40, 5) == VS_NOW && Due(41, 5) == VS_MID, "threshold: now at least 40 m");
}

static bool Clips(int stage, int act, int m, int then, const char* want)
{
    const char* c[8];
    int n = VoiceSay(stage, act, m, then, c, 8);
    char got[200] = "";
    for (int i = 0; i < n; i++) {
        if (i)
            strcat(got, " ");
        strcat(got, c[i]);
    }
    if (strcmp(got, want))
        printf("  got \"%s\", want \"%s\"\n", got, want);
    return !strcmp(got, want);
}

static void TestSay()
{
    Check(Clips(VS_FAR, VA_RIGHT, 990, VA_NONE, "d1000 right"), "say: far - the distance and the turn");
    Check(Clips(VS_MID, VA_LEFT, 290, VA_NONE, "d300 left"), "say: mid");
    Check(Clips(VS_NOW, VA_KEEP_LEFT, 90, VA_NONE, "keep_left"), "say: now - just the action");
    Check(Clips(VS_NOW, VA_RIGHT, 90, VA_LEFT, "right then left"), "say: now and the one close behind");
    Check(Clips(VS_MID, VA_RIGHT, 290, VA_LEFT, "d300 right"), "say: then - only with now");
    Check(Clips(VS_FAR, VA_ROUND1 + 2, 480, VA_NONE, "d500 round3"), "say: a roundabout's exit");
    Check(Clips(VS_MID, VA_FINISH, 300, VA_NONE, "d300 finish"), "say: the destination ahead");
    Check(Clips(VS_NOW, VA_FINISH, 50, VA_NONE, ""), "say: at the destination - the arrival says it");
    Check(Clips(VS_NOW, VA_NONE, 50, VA_NONE, ""), "say: nothing to say");
    // the distances: the nearest clip
    Check(Clips(VS_MID, VA_RIGHT, 60, VA_NONE, "d100 right"), "say: under 100 m - 100");
    Check(Clips(VS_MID, VA_RIGHT, 170, VA_NONE, "d150 right"), "say: 170 - 150");
    Check(Clips(VS_MID, VA_RIGHT, 180, VA_NONE, "d200 right"), "say: 180 - 200");
    Check(Clips(VS_FAR, VA_RIGHT, 700, VA_NONE, "d600 right"), "say: 700 - the nearer below");
    Check(Clips(VS_FAR, VA_RIGHT, 1300, VA_NONE, "d1500 right"), "say: 1300 - 1.5 km");
    Check(Clips(VS_FAR, VA_RIGHT, 1980, VA_NONE, "d2000 right"), "say: 2 km");
    Check(Clips(VS_FAR, VA_RIGHT, 9000, VA_NONE, "d5000 right"), "say: far beyond - 5 km");
    Check(Clips(VS_MID, VA_RIGHT, 125, VA_NONE, "d100 right"), "say: halfway 100-150 - the lower");
    Check(Clips(VS_MID, VA_RIGHT, 120, VA_NONE, "d100 right") && Clips(VS_MID, VA_RIGHT, 130, VA_NONE, "d150 right"),
          "say: 120 - 100, 130 - 150");
    Check(Clips(VS_FAR, VA_RIGHT, 4000, VA_NONE, "d3000 right") && Clips(VS_FAR, VA_RIGHT, 4001, VA_NONE, "d5000 right"),
          "say: halfway 3-5 km - the lower, a metre on - 5 km");
    Check(Clips(VS_FAR, VA_RIGHT, 5000, VA_NONE, "d5000 right"), "say: the last one itself");
    const char* c[2];
    Check(VoiceSay(VS_NOW, VA_RIGHT, 90, VA_LEFT, c, 2) == 1 && !strcmp(c[0], "right"),
          "say: no room for \"then\" and its action - left out whole");
    Check(VoiceSay(VS_MID, VA_RIGHT, 290, VA_NONE, c, 1) == 0, "say: no room for the distance - nothing");
}

static void Le(unsigned char* p, unsigned v, int bytes)   // little-endian
{
    for (int i = 0; i < bytes; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static int Put(unsigned char* b, int at, const char* id, unsigned size)
{
    memcpy(b + at, id, 4);
    Le(b + at + 4, size, 4);
    return at + 8;
}

static int Wav(unsigned char* b, int fmt, unsigned rate, bool list, int data)   // a WAV; list: a chunk before "data"
{
    memset(b, 0, 128);
    int at = Put(b, 0, "RIFF", 0x1234);   // (its size: not looked at)
    memcpy(b + at, "WAVE", 4);
    at = Put(b, at + 4, "fmt ", 16);
    Le(b + at, fmt, 2);           // 1 = PCM
    Le(b + at + 2, 0x0102, 2);    // the channels (every byte of the numbers counts)
    Le(b + at + 4, rate, 4);
    Le(b + at + 14, 0x0110, 2);   // the bits
    at += 16;
    if (list)
        at = Put(b, at, "LIST", 5) + 6;   // odd size: padded to even
    at = Put(b, at, "data", data);
    return at;
}

static void TestWav()
{
    unsigned char b[128];
    VoiceWav w;
    int at = Wav(b, 1, 0x01020304, false, 40);
    Check(VoiceWavParse(b, at + 40, &w) && w.at == at && w.len == 40 && w.rate == 0x01020304 && w.channels == 0x0102 &&
          w.bits == 0x0110, "wav: PCM, its sound and format (all the numbers' bytes)");
    Check(VoiceWavParse(b, at, &w) && w.at == at && w.len == 0, "wav: the data chunk's head at the very end - empty");
    at = Wav(b, 1, 22050, true, 40);
    Check(VoiceWavParse(b, at + 40, &w) && w.at == at && w.at == 44 + 14, "wav: another chunk (odd, padded) skipped");
    Check(VoiceWavParse(b, at + 30, &w) && w.len == 30, "wav: cut short - only what is there");
    at = Wav(b, 3, 22050, false, 40);
    Check(!VoiceWavParse(b, at + 40, &w), "wav: not PCM - no");
    at = Wav(b, 1, 22050, false, 40);
    b[0] = 'X';
    Check(!VoiceWavParse(b, at + 40, &w), "wav: not RIFF - no");
    Check(!VoiceWavParse(b, 20, &w), "wav: too short - no");
    at = Wav(b, 1, 22050, false, 40);
    Check(!VoiceWavParse(b, at - 8, &w), "wav: no data chunk - no");
}

static bool Event(VoiceRoute was, VoiceRoute now, const char* want)
{
    const char* got = VoiceEvent(was, now);
    return want ? got && !strcmp(got, want) : !got;
}

static void TestEvent()
{
    VoiceRoute none = { false, false, 0, 0 }, on1 = { true, false, 1, 2 }, on2 = { true, false, 2, 2 };
    VoiceRoute via1 = { true, false, 1, 1 }, arrived = { false, true, 1, 0 };
    Check(Event(none, on1, "route_ready"), "event: the first route");
    Check(Event(on1, on1, 0), "event: the same - nothing");
    Check(Event(on1, on2, "route_new"), "event: another one while on the route - recalculated");
    Check(Event(on1, via1, "via_reached"), "event: a stop passed");
    Check(Event(via1, on2, "route_new"), "event: a new route with more stops - not a stop passed");
    Check(Event(on1, arrived, "arrived"), "event: arrived");
    Check(Event(arrived, arrived, 0), "event: arrived once");
    Check(Event(arrived, none, 0), "event: the route gone - nothing");
    VoiceRoute busy = { false, false, 1, 2 };
    Check(Event(on1, busy, 0), "event: off the route (asked anew) - nothing yet");
    Check(Event(busy, on2, "route_ready"), "event: a route after none on - ready");
}

static void TestGain()
{
    short s[4] = { 10000, -10000, 32767, -32768 };
    VoiceGain((char*)s, sizeof(s), 16, kVoiceLoud);
    Check(s[0] == 10000 && s[1] == -10000 && s[2] == 32767 && s[3] == -32768, "gain: loud - as recorded");
    VoiceGain((char*)s, sizeof(s), 16, kVoiceMedium);
    Check(s[0] == 5500 && s[1] == -5500 && s[2] == 18021 && s[3] == -18022, "gain: medium - 55 %");
    short q[2] = { 10000, -10000 };
    VoiceGain((char*)q, 3, 16, kVoiceQuiet);   // (an odd byte at the end: left)
    Check(q[0] == 3000 && q[1] == -10000, "gain: quiet - 30 %, whole samples only");
    unsigned char b[4] = { 228, 128, 28, 228 };
    VoiceGain((char*)b, 3, 8, kVoiceQuiet);
    Check(b[0] == 158 && b[1] == 128 && b[2] == 98 && b[3] == 228, "gain: 8-bit - round its middle (128), its length");
    short z[1] = { 10000 };
    VoiceGain((char*)z, 2, 16, 0);
    Check(z[0] == 10000, "gain: no level (an old setting) - as recorded");
    VoiceGain((char*)z, 2, 16, 4);
    Check(z[0] == 10000, "gain: past loud - as recorded");
}

static void TestDevice()
{
    Check(VoiceDevice(2, 4) == 1, "device: VestaHome turned the radio down (no shell) - Audio Output 1");
    Check(VoiceDevice(2, 2) == 1, "device: two devices, VestaHome - 1");
    Check(VoiceDevice(2, 1) == -1, "device: one device only - the mapper");
    Check(VoiceDevice(1, 4) == -1, "device: the stock shell (as CityGuide) - the mapper");
    Check(VoiceDevice(0, 4) == -1, "device: no one asked (the PC) - the mapper");
}

int main()
{
    TestEvent();
    TestAct();
    TestDue();
    TestThresholds();
    TestSay();
    TestWav();
    TestDevice();
    TestGain();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
