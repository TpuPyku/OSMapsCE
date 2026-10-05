#include "yamaps.h"

// Reads NMEA straight from the receiver's serial port (COM6:/115200 on Lada Vesta MMC),
// no GpsGate needed. Only $xxRMC (position/speed/course/date) and $xxGGA (satellites) are used.
// The port can be changed at runtime (GpsConfigure) or searched for (GpsScan).

static CRITICAL_SECTION s_cs;
static GpsState         s_state;
static HANDLE           s_thread;
static HANDLE           s_wake;        // stop / reconfigure / scan
static HWND             s_notify;
static volatile bool    s_stop;
static volatile bool    s_reopen;      // port settings changed
static volatile bool    s_scanReq;
static wchar_t          s_port[16];
static DWORD            s_baud;
static DWORD            s_lastNotify;
static volatile LONG    s_notifyPosted;  // WM_APP_GPS is in the window's queue, cleared by GpsGet

static char             s_line[128];
static int              s_lineLen;

static const DWORD      kReadPauseMs = 50;   // let the driver collect a chunk instead of spinning on bytes

// At most one message a second, and never a second one while the window hasn't read
// the state yet: a busy window must not get a queue of them.
static void Notify(bool force)
{
    DWORD now = GetTickCount();
    if (!force && now - s_lastNotify < 1000)
        return;
    if (InterlockedExchange((LONG*)&s_notifyPosted, 1))
        return;
    s_lastNotify = now;
    if (!PostMessage(s_notify, WM_APP_GPS, 0, 0))
        s_notifyPosted = 0;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "5549.3932" + 'N' -> 55.823220
static double NmeaDeg(const char* v, char hemi)
{
    double raw = atof(v);
    int deg = (int)(raw / 100);
    double d = deg + (raw - deg * 100) / 60.0;
    return (hemi == 'S' || hemi == 'W') ? -d : d;
}

// Returns true for a sentence with a valid checksum.
static bool ParseSentence(char* s)
{
    if (s[0] != '$')
        return false;
    // The port may carry other (binary) data between sentences: everything must be printable.
    for (const char* p = s; *p; p++) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) {
            EnterCriticalSection(&s_cs);
            s_state.badLines++;
            LeaveCriticalSection(&s_cs);
            return false;
        }
    }
    // Checksum is checked when present; like GpsGate's filter (ChecksumMandatory=0)
    // sentences without it are accepted, but then must have all their fields.
    char* star = strchr(s, '*');
    bool checked = star != NULL;
    if (star) {
        unsigned char sum = 0;
        for (char* p = s + 1; p < star; p++)
            sum ^= (unsigned char)*p;
        int hi = HexDigit(star[1]), lo = HexDigit(star[2]);
        if (hi < 0 || lo < 0 || sum != hi * 16 + lo) {
            EnterCriticalSection(&s_cs);
            s_state.badLines++;
            LeaveCriticalSection(&s_cs);
            return false;
        }
        *star = 0;
    }

    EnterCriticalSection(&s_cs);
    strncpy(s_state.last, s, sizeof(s_state.last) - 1);
    s_state.lines++;
    LeaveCriticalSection(&s_cs);

    char* f[24];
    int n = 0;
    f[n++] = s + 1;
    for (char* p = s + 1; *p && n < 24; p++) {
        if (*p == ',') {
            *p = 0;
            f[n++] = p + 1;
        }
    }
    if (strlen(f[0]) != 5)
        return true;
    const char* type = f[0] + 2;   // skip talker id (GP, GN, GL...)
    if (!checked && ((!strcmp(type, "RMC") && n < 12) || (!strcmp(type, "GGA") && n < 14)))
        return false;   // cut sentence without checksum: don't trust it

    EnterCriticalSection(&s_cs);
    s_state.dataTick = GetTickCount();
    bool changed = false, fixChanged = false;
    if (!strcmp(type, "RMC") && n >= 9) {
        bool valid = f[2][0] == 'A' && f[3][0] && f[5][0];
        if (valid) {
            s_state.lat = NmeaDeg(f[3], f[4][0]);
            s_state.lon = NmeaDeg(f[5], f[6][0]);
            s_state.speedKmh = atof(f[7]) * 1.852;
            if (f[8][0])
                s_state.course = atof(f[8]);
            s_state.fixTick = s_state.dataTick;
            if (n >= 10 && strlen(f[9]) == 6) {
                int d = atoi(f[9]);   // ddmmyy
                s_state.date = 20000000 + (d % 100) * 10000 + (d / 100 % 100) * 100 + d / 10000;
            }
            changed = true;
        }
        fixChanged = (s_state.valid != 0) != valid;
        s_state.valid = valid;
    } else if (!strcmp(type, "GGA") && n >= 8) {
        int sats = atoi(f[7]);
        changed = sats != s_state.sats;
        s_state.sats = sats;
    }
    bool nowValid = s_state.valid != 0;
    LeaveCriticalSection(&s_cs);

    if (fixChanged)
        Log("gps: fix %s", nowValid ? "acquired" : "lost");
    (void)changed;
    Notify(fixChanged);   // throttled: also keeps the settings screen counters live
    return true;
}

// Feeds raw bytes, returns the number of valid sentences seen.
static int Feed(const char* buf, DWORD len)
{
    int good = 0;
    for (DWORD i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '$')
            s_lineLen = 0;
        if (c == '\r' || c == '\n') {
            s_line[s_lineLen] = 0;
            if (s_lineLen > 6 && ParseSentence(s_line))
                good++;
            s_lineLen = 0;
        } else if (s_lineLen < (int)sizeof(s_line) - 1) {
            s_line[s_lineLen++] = c;
        }
    }
    return good;
}

static void SetStatus(int status, const wchar_t* port, DWORD baud)
{
    EnterCriticalSection(&s_cs);
    s_state.status = status;
    wcsncpy(s_state.port, port, 15);
    s_state.baud = baud;
    if (status != GPS_OPEN)
        s_state.valid = 0;
    LeaveCriticalSection(&s_cs);
    Notify(true);
}

// On the Vesta MMC the GPS port also belongs to the system shell (parking lines data
// goes through it too). So the port is opened read-only and left as the shell set it up:
// no DTR/RTS, no timeouts, no purge. Only the speed is changed, and only if it differs
// (baud = 0: never touch it).
static HANDLE OpenPort(const wchar_t* port, DWORD baud, DWORD* err)
{
    HANDLE h = CreateFile(port, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return h;
    }
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (baud && GetCommState(h, &dcb) && dcb.BaudRate != baud) {
        Log("gps: %S speed %lu -> %lu", port, dcb.BaudRate, baud);
        dcb.BaudRate = baud;
        SetCommState(h, &dcb);
    }
    *err = 0;
    return h;
}

// Reads what is already in the driver queue, waiting up to waitMs for data.
// Polling instead of comm timeouts: those are shared with the other user of the port.
static int ReadAvailable(HANDLE h, char* buf, DWORD size, DWORD waitMs)
{
    DWORD start = GetTickCount();
    for (;;) {
        DWORD errors = 0;
        COMSTAT st;
        memset(&st, 0, sizeof(st));
        if (!ClearCommError(h, &errors, &st))
            return -1;
        if (st.cbInQue) {
            DWORD got = 0;
            if (!ReadFile(h, buf, st.cbInQue < size ? st.cbInQue : size, &got, NULL))
                return -1;
            return (int)got;
        }
        if (s_stop || s_reopen || s_scanReq || GetTickCount() - start >= waitMs)
            return 0;
        Sleep(100);
    }
}

static bool IsBusyError(DWORD err)
{
    return err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION || err == ERROR_BUSY;
}

// Listens to a port with its current settings; true if it talks valid NMEA.
// The speed is never changed while searching: other ports of the unit serve the car
// (MCU, CAN) and must not be reconfigured.
static bool ProbePort(const wchar_t* port, DWORD* baud)
{
    DWORD err;
    HANDLE h = OpenPort(port, 0, &err);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    *baud = GetCommState(h, &dcb) ? dcb.BaudRate : 0;
    SetStatus(GPS_SCANNING, port, *baud);
    int good = 0;
    s_lineLen = 0;
    DWORD start = GetTickCount();
    while (!s_stop && GetTickCount() - start < 3000 && good < 2) {
        char buf[256];
        int got = ReadAvailable(h, buf, sizeof(buf), 3000 - (GetTickCount() - start));
        if (got < 0)
            break;
        good += Feed(buf, got);
    }
    CloseHandle(h);
    return good >= 2;
}

static void Scan()
{
    Log("gps: scanning ports");
    wchar_t first[16];
    EnterCriticalSection(&s_cs);
    wcscpy(first, s_port);
    LeaveCriticalSection(&s_cs);

    for (int i = 0; i <= 9 && !s_stop; i++) {
        wchar_t port[16];
        if (i == 0)
            wcscpy(port, first);
        else
            _snwprintf(port, 16, L"COM%d:", i);
        if (i > 0 && !wcscmp(port, first))
            continue;
        DWORD baud = 0;
        if (ProbePort(port, &baud)) {
            Log("gps: found NMEA on %S %lu", port, baud);
            EnterCriticalSection(&s_cs);
            wcscpy(s_port, port);
            s_baud = baud;
            s_state.found = 1;
            s_state.baud = baud;
            LeaveCriticalSection(&s_cs);
            PostMessage(s_notify, WM_APP_GPS, 1, 0);
            return;
        }
    }
    Log("gps: no NMEA found");
    EnterCriticalSection(&s_cs);
    s_state.found = -1;
    LeaveCriticalSection(&s_cs);
    PostMessage(s_notify, WM_APP_GPS, 1, 0);
}

#ifndef UNDER_CE
// PC test build only: gps_port=<file>.nmea plays a recorded track from the exe folder,
// one RMC sentence per 250 ms (4x real time).
static void PlayFile(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    PathInDir(path, name);
    unsigned char* data;
    int len;
    if (!ReadWholeFile(path, &data, &len)) {
        SetStatus(GPS_NO_PORT, name, 0);
        WaitForSingleObject(s_wake, 3000);
        return;
    }
    SetStatus(GPS_OPEN, name, 0);
    s_lineLen = 0;
    while (!s_stop && !s_reopen && !s_scanReq) {
        int lineStart = 0;
        for (int i = 0; i < len && !s_stop && !s_reopen && !s_scanReq; i++) {
            Feed((const char*)data + i, 1);
            if (data[i] != '\n')
                continue;
            if (i - lineStart > 6 && !strncmp((const char*)data + lineStart + 3, "RMC", 3))
                Sleep(250);
            lineStart = i + 1;
        }
    }
    free(data);
}
#endif

static DWORD WINAPI GpsThread(LPVOID)
{
    DWORD lastErr = 0xFFFFFFFF;
    while (!s_stop) {
        if (s_scanReq) {
            s_scanReq = false;
            Scan();
            continue;
        }

        wchar_t port[16];
        EnterCriticalSection(&s_cs);
        wcscpy(port, s_port);
        DWORD baud = s_baud;
        s_reopen = false;
        LeaveCriticalSection(&s_cs);
#ifndef UNDER_CE
        if (wcsstr(port, L".nmea")) {
            PlayFile(port);
            continue;
        }
#endif

        DWORD err = 0;
        HANDLE h = OpenPort(port, baud, &err);
        if (h == INVALID_HANDLE_VALUE && IsBusyError(err) && g_cfg.killProcess[0]) {
            if (KillProcess(g_cfg.killProcess))
                Sleep(1000);
            h = OpenPort(port, baud, &err);
        }
        if (h == INVALID_HANDLE_VALUE) {
            if (err != lastErr)
                Log("gps: cannot open %S (%lu), retrying", port, err);
            lastErr = err;
            SetStatus(IsBusyError(err) ? GPS_BUSY : GPS_NO_PORT, port, baud);
            WaitForSingleObject(s_wake, 3000);
            continue;
        }
        Log("gps: %S %lu opened", port, baud);
        lastErr = 0;
        SetStatus(GPS_OPEN, port, baud);
        s_lineLen = 0;

        DWORD lastData = GetTickCount();
        while (!s_stop && !s_reopen && !s_scanReq) {
            char buf[2048];
            int got = ReadAvailable(h, buf, sizeof(buf), 1000);
            if (got < 0) {
                Log("gps: read error (%lu)", GetLastError());
                break;
            }
            if (got == 0) {
                if (GetTickCount() - lastData > 15000) {
                    Log("gps: no data for 15 s, reopening");
                    break;
                }
                continue;
            }
            lastData = GetTickCount();
            EnterCriticalSection(&s_cs);
            s_state.bytes += got;
            LeaveCriticalSection(&s_cs);
            Feed(buf, got);
            Sleep(kReadPauseMs);
        }
        CloseHandle(h);
        SetStatus(GPS_NO_PORT, port, baud);
        if (!s_stop && !s_reopen && !s_scanReq)
            WaitForSingleObject(s_wake, 1000);
    }
    return 0;
}

void GpsStart(HWND notify)
{
    InitializeCriticalSection(&s_cs);
    memset(&s_state, 0, sizeof(s_state));
    s_notify = notify;
    wcscpy(s_port, g_cfg.gpsPort);
    s_baud = g_cfg.gpsBaud;
    s_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_thread = CreateThread(NULL, 0, GpsThread, NULL, 0, NULL);
}

void GpsStop()
{
    if (!s_thread)
        return;
    s_stop = true;
    SetEvent(s_wake);
    WaitForSingleObject(s_thread, 3000);
}

void GpsConfigure(const wchar_t* port, DWORD baud)
{
    if (!s_thread)
        return;
    EnterCriticalSection(&s_cs);
    wcsncpy(s_port, port, 15);
    s_baud = baud;
    s_state.lines = s_state.badLines = 0;
    s_state.last[0] = 0;
    s_state.found = 0;
    s_reopen = true;
    LeaveCriticalSection(&s_cs);
    SetEvent(s_wake);
}

void GpsScan()
{
    if (!s_thread)
        return;
    EnterCriticalSection(&s_cs);
    s_state.lines = s_state.badLines = 0;
    s_state.last[0] = 0;
    s_state.found = 0;
    LeaveCriticalSection(&s_cs);
    s_scanReq = true;
    SetEvent(s_wake);
}

void GpsGet(GpsState* s)
{
    if (!s_thread) {
        memset(s, 0, sizeof(*s));
        return;
    }
    EnterCriticalSection(&s_cs);
    *s = s_state;
    LeaveCriticalSection(&s_cs);
    s_notifyPosted = 0;
}
