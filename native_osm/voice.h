// voice.h - the voice prompts (users' wish 2026-11-07): what to say for a maneuver and when. Pure: no sound, no clock.
// A prompt is a few clips of voice/phrases.tsv (Voice\<lang>\<id>.wav): [distance] action [then action2].
#pragma once

enum {   // what a maneuver asks of the driver (the action clip)
    VA_NONE, VA_RIGHT, VA_LEFT, VA_SLIGHT_RIGHT, VA_SLIGHT_LEFT, VA_SHARP_RIGHT, VA_SHARP_LEFT, VA_UTURN,
    VA_KEEP_RIGHT, VA_KEEP_LEFT, VA_STRAIGHT, VA_EXIT_RIGHT, VA_EXIT_LEFT, VA_ROUND, VA_VIA, VA_FINISH,
    VA_ROUND1   // .. VA_ROUND1 + 5: the roundabout's exit 1..6
};

// OSRM's step: its maneuver's type and modifier, the roundabout's exit (0: none), stop = an "arrive" of a stop on the
// way. VA_NONE: nothing to say (the start, a new name of the same road, straight on).
int VoiceAct(const char* type, const char* mod, int exit, bool stop);

enum { VS_FAR = 1, VS_MID = 2, VS_NOW = 4 };   // the prompts of one maneuver, each said once
struct VoiceState { int man; int said; };      // the maneuver (its index) and its prompts said
void VoiceReset(VoiceState* s);
// The prompt due now (VS_*, 0: none) for the maneuver man at nextM metres, the car at kmh. The far ones come sooner
// when faster; one that is late (the next one's turn already) is skipped unsaid.
int VoiceDue(VoiceState* s, int man, int nextM, int kmh);

// The clips of a prompt (ids of phrases.tsv) into out, their count: stage VS_FAR / VS_MID with the distance,
// VS_NOW without; then = the maneuver after it when it comes within kThenM (VA_NONE: none).
enum { kThenM = 200 };
int VoiceSay(int stage, int act, int nextM, int then, const char** out, int cap);

// The route's own prompt when it changes (an id of phrases.tsv, 0: none): a new route, a new one off the line, the
// arrival, a stop passed. serial: the route's number (a new one each), vias: the stops left.
struct VoiceRoute { bool on, arrived; int serial, vias; };
const char* VoiceEvent(const VoiceRoute& was, const VoiceRoute& now);

// A clip's file (a RIFF WAV, PCM): where its sound is in the bytes and its format (rate, channels, bits); false - not
// a PCM WAV or cut short.
struct VoiceWav { int at, len, rate, channels, bits; };
bool VoiceWavParse(const unsigned char* b, int n, VoiceWav* w);

// The sound device for a prompt (sheet 29 p. 27: not heard over the radio without the shell): via - who turned the
// radio down (0 no one, 1 the stock shell, 2 VestaHome), devs - waveOutGetNumDevs(). Without the shell the MCU lets
// through "Audio Output" 1 only (VestaHome's beep, HU 2026-10-18): 1 there; otherwise -1 (WAVE_MAPPER).
int VoiceDevice(int via, int devs);
// The voice's volume (the user 2026-11-12: three levels, set in the program): the phrase's PCM scaled in place -
// level 1 quiet, 2 medium, 3 loud (as recorded); 16-bit signed or 8-bit unsigned samples
enum { kVoiceQuiet = 1, kVoiceMedium, kVoiceLoud };
void VoiceGain(char* pcm, int len, int bits, int level);
