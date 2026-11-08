// The voice prompts spoken (what and when: voice.cpp). A prompt's clips Voice\<lang>\<id>.wav (beside the exe; made by
// voice/make_voice.py) are glued into one sound and played as an external navigator's prompt on the unit (Ghidra,
// PROJECT.md; VestaHome's beep does the same): the radio asked down (VoiceChannel), the sound kLeadMs later, the radio
// given back kTailMs after it. No one to ask (the PC): played at once. A prompt due while another is spoken waits (the
// newest one); one that never ends (a device without "done") is cut 2 s after its length.
#include "yamaps.h"
#include "lang.h"
#include "voice.h"
#ifndef UNDER_CE
#include <mmsystem.h>
#endif

enum { kLeadMs = 1000, kTailMs = 800, kGuardMs = 2000, kMaxClips = 8 };
enum { SAY_IDLE, SAY_LEAD, SAY_PLAY, SAY_TAIL };

static HWND         s_wnd;
static int          s_phase;
static HWAVEOUT     s_out;
static WAVEHDR      s_hdr;
static WAVEFORMATEX s_fmt;
static char*        s_pcm;           // the sound said (or about to be)
static int          s_pcmLen;
static char*        s_next;          // the one waiting
static int          s_nextLen;
static VoiceState   s_vs;
static VoiceRoute   s_was;

void SayInit(HWND wnd)
{
    s_wnd = wnd;
    VoiceReset(&s_vs);
}

// The clips' sound one after another (the format of the first one; a clip of another is left out); NULL - none read.
static char* Glue(const char* const* ids, int n, int* len)
{
    char* out = NULL;
    *len = 0;
    for (int i = 0; i < n; i++) {
        wchar_t path[MAX_PATH], id[16];
        int k = 0;
        for (; ids[i][k] && k < 15; k++)
            id[k] = (wchar_t)ids[i][k];
        id[k] = 0;
        _snwprintf(path, MAX_PATH, L"%sVoice\\%s\\%s.wav", g_dir, LangCode()[0] ? L"en" : L"ru", id);
        path[MAX_PATH - 1] = 0;
        unsigned char* b;
        int bn;
        VoiceWav w;
        if (!ReadWholeFile(path, &b, &bn)) {
            Log("voice: no %S", ids[i]);
            continue;
        }
        bool fits = VoiceWavParse(b, bn, &w) && (!out || (w.rate == (int)s_fmt.nSamplesPerSec &&
                                                          w.channels == s_fmt.nChannels && w.bits == s_fmt.wBitsPerSample));
        char* more = fits ? (char*)realloc(out, *len + w.len) : NULL;
        if (more) {
            if (!out) {
                memset(&s_fmt, 0, sizeof(s_fmt));
                s_fmt.wFormatTag = WAVE_FORMAT_PCM;
                s_fmt.nChannels = (WORD)w.channels;
                s_fmt.nSamplesPerSec = w.rate;
                s_fmt.wBitsPerSample = (WORD)w.bits;
                s_fmt.nBlockAlign = (WORD)(w.channels * w.bits / 8);
                s_fmt.nAvgBytesPerSec = w.rate * s_fmt.nBlockAlign;
            }
            out = more;
            memcpy(out + *len, b + w.at, w.len);
            *len += w.len;
        }
        free(b);
    }
    return out;
}

static void Play()
{
    s_phase = SAY_PLAY;
    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr.lpData = s_pcm;
    s_hdr.dwBufferLength = s_pcmLen;
    MMRESULT r = waveOutOpen(&s_out, WAVE_MAPPER, &s_fmt, (DWORD)(UINT_PTR)s_wnd, 0, CALLBACK_WINDOW);
    if (r == MMSYSERR_NOERROR && waveOutPrepareHeader(s_out, &s_hdr, sizeof(s_hdr)) == MMSYSERR_NOERROR &&
        waveOutWrite(s_out, &s_hdr, sizeof(s_hdr)) == MMSYSERR_NOERROR) {
        int ms = s_fmt.nAvgBytesPerSec ? (int)((__int64)s_pcmLen * 1000 / s_fmt.nAvgBytesPerSec) : 0;
        SetTimer(s_wnd, TIMER_VOICE, ms + kGuardMs, NULL);
        return;
    }
    Log("voice: waveOut %d", (int)r);
    SayDone();
}

static void Start()
{
    if (VoiceChannel(true)) {
        s_phase = SAY_LEAD;
        SetTimer(s_wnd, TIMER_VOICE, kLeadMs, NULL);
    } else {
        Play();
    }
}

static void Say(const char* const* ids, int n)
{
    int len;
    char* pcm = Glue(ids, n, &len);
    if (!pcm)
        return;
    Log("voice: %s%s%s%s%s", ids[0], n > 1 ? " " : "", n > 1 ? ids[1] : "", n > 2 ? " " : "", n > 2 ? ids[2] : "");
    if (s_phase == SAY_IDLE || s_phase == SAY_LEAD) {   // (still to come: the newer one instead)
        free(s_pcm);
        s_pcm = pcm;
        s_pcmLen = len;
        if (s_phase == SAY_IDLE)
            Start();
    } else {
        free(s_next);
        s_next = pcm;
        s_nextLen = len;
    }
}

void SayDone()   // MM_WOM_DONE (or the sound could not start)
{
    if (s_out) {
        waveOutUnprepareHeader(s_out, &s_hdr, sizeof(s_hdr));
        waveOutClose(s_out);
        s_out = NULL;
    }
    free(s_pcm);
    s_pcm = NULL;
    if (s_next) {   // (the radio still down)
        s_pcm = s_next;
        s_pcmLen = s_nextLen;
        s_next = NULL;
        Play();
        return;
    }
    s_phase = SAY_TAIL;
    SetTimer(s_wnd, TIMER_VOICE, kTailMs, NULL);
}

void SayTimer()   // TIMER_VOICE: the lead is over, the sound has not ended, or the tail is over
{
    KillTimer(s_wnd, TIMER_VOICE);
    if (s_phase == SAY_LEAD) {
        Play();
    } else if (s_phase == SAY_PLAY) {
        Log("voice: no end from the device - cut");
        if (s_out)
            waveOutReset(s_out);   // (MM_WOM_DONE follows)
    } else if (s_phase == SAY_TAIL) {
        s_phase = SAY_IDLE;
        VoiceChannel(false);
    }
}

void SayStop()   // the program ends: never leave the radio down
{
    KillTimer(s_wnd, TIMER_VOICE);
    if (s_out) {
        waveOutReset(s_out);
        waveOutUnprepareHeader(s_out, &s_hdr, sizeof(s_hdr));
        waveOutClose(s_out);
        s_out = NULL;
    }
    if (s_phase != SAY_IDLE)
        VoiceChannel(false);
    s_phase = SAY_IDLE;
}

void SayTick(int kmh)
{
    RouteInfo ri;
    RouteGet(&ri);
    VoiceRoute now = { ri.state == ROUTE_ON, ri.state == ROUTE_ARRIVED, ri.serial, ri.vias };
    const char* ids[kMaxClips];
    int n = 0;
    const char* ev = VoiceEvent(s_was, now);
    if (now.serial != s_was.serial)
        VoiceReset(&s_vs);
    s_was = now;
    if (ev) {
        ids[n++] = ev;
    } else if (now.on && !ri.off) {
        int stage = VoiceDue(&s_vs, ri.man, ri.nextM, kmh);
        if (stage)
            n = VoiceSay(stage, ri.act, ri.nextM, ri.thenAct, ids, kMaxClips);
    }
    if (n && g_cfg.voice)
        Say(ids, n);
}
