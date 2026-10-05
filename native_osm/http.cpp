#include "yamaps.h"

extern "C" {
#include "bearssl.h"
}
#include "trust_anchors.inc"

// HTTP/1.1 client with keep-alive, plain or TLS (BearSSL). One connection per host,
// a few hosts at a time (tile server, traffic provider). Used only by the download thread.
//
// TLS: certificates are checked against the built-in roots (make_ta.py). The unit's clock
// is reset on boot, so the validation date is the latest of: system clock, GPS date,
// build date.

static const int kConnectTimeoutSec = 10;
static const int kReadTimeoutSec    = 15;
static const int kMaxBody           = 2 * 1024 * 1024;
static const int kMaxConns          = 3;

struct Conn {
    char                    host[64];
    bool                    https;
    SOCKET                  s;
    bool                    open;
    DWORD                   lastUse;
    br_ssl_client_context   sc;
    br_x509_minimal_context xc;
    br_sslio_context        ioc;
    unsigned char           iobuf[BR_SSL_BUFSIZE_BIDI];
    bool                    tlsReady;      // context initialized: next reset may resume the session
};

static Conn*                s_conns[kMaxConns];
static br_x509_trust_anchor s_tas[TA_COUNT];
static volatile LONG        s_gpsDays;     // days since 0 AD from GPS, 0 = unknown

// ---------------------------------------------------------------- time for X.509

// Days since January 1st, 0 AD (proleptic Gregorian), as BearSSL wants it.
static unsigned long DaysFromCivil(int y, int m, int d)
{
    // days since 1970-01-01, H. Hinnant's algorithm; 1970-01-01 = 719528 days since 0 AD
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (unsigned long)(era * 146097 + (long)doe - 719468 + 719528);
}

static unsigned long BuildDays()
{
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* d = __DATE__;   // "Oct  2 2026"
    char mon[4] = { d[0], d[1], d[2], 0 };
    const char* p = strstr(months, mon);
    int month = p ? (int)(p - months) / 3 + 1 : 1;
    return DaysFromCivil(atoi(d + 7), month, atoi(d + 4));
}

void HttpSetGpsDate(int yyyymmdd)
{
    if (yyyymmdd > 20000000)
        s_gpsDays = (LONG)DaysFromCivil(yyyymmdd / 10000, yyyymmdd / 100 % 100, yyyymmdd % 100);
}

static void ValidationTime(unsigned long* days, unsigned long* secs)
{
    SYSTEMTIME t;
    GetSystemTime(&t);
    unsigned long d = DaysFromCivil(t.wYear, t.wMonth, t.wDay);
    *secs = t.wHour * 3600UL + t.wMinute * 60UL + t.wSecond;
    unsigned long b = BuildDays();
    if ((unsigned long)s_gpsDays > d)
        d = (unsigned long)s_gpsDays;
    if (b > d) {
        d = b;
        *secs = 12 * 3600UL;
    }
    *days = d;
}

// ---------------------------------------------------------------- entropy
// BearSSL has no system random source on Windows CE. The OS one is taken if the image has
// it (CeGenRandom or CryptGenRandom in coredll), plus timers and addresses as extra seed.

typedef BOOL (WINAPI *CeGenRandomFn)(DWORD, PBYTE);

static void InjectEntropy(br_ssl_engine_context* eng)
{
    unsigned char buf[64];
    int n = 0;
#ifdef UNDER_CE
    HMODULE core = GetModuleHandle(L"coredll.dll");
    CeGenRandomFn gen = core ? (CeGenRandomFn)GetProcAddress(core, L"CeGenRandom") : NULL;
    if (gen && gen(32, buf))
        n = 32;
#endif
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    DWORD extra[6] = { GetTickCount(), pc.LowPart, pc.HighPart, (DWORD)(size_t)&pc,
                       GetCurrentThreadId(), (DWORD)(size_t)eng };
    memcpy(buf + n, extra, sizeof(extra));
    n += sizeof(extra);
    SYSTEMTIME t;
    GetSystemTime(&t);
    memcpy(buf + n, &t, sizeof(t) < sizeof(buf) - n ? sizeof(t) : sizeof(buf) - n);
    n += sizeof(t) < sizeof(buf) - n ? sizeof(t) : sizeof(buf) - n;
    br_ssl_engine_inject_entropy(eng, buf, n);
}

// ---------------------------------------------------------------- sockets

static bool WaitSocket(SOCKET s, bool write, int sec)
{
    fd_set set, err;
    FD_ZERO(&set);
    FD_ZERO(&err);
    FD_SET(s, &set);
    FD_SET(s, &err);
    timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    int r = write ? select(0, NULL, &set, &err, &tv) : select(0, &set, NULL, &err, &tv);
    return r > 0 && FD_ISSET(s, &set);
}

static int SockRead(void* ctx, unsigned char* buf, size_t len)
{
    SOCKET s = *(SOCKET*)ctx;
    if (!WaitSocket(s, false, kReadTimeoutSec))
        return -1;
    int n = recv(s, (char*)buf, (int)len, 0);
    return n > 0 ? n : -1;
}

static int SockWrite(void* ctx, const unsigned char* buf, size_t len)
{
    SOCKET s = *(SOCKET*)ctx;
    if (!WaitSocket(s, true, kReadTimeoutSec))
        return -1;
    int n = send(s, (const char*)buf, (int)len, 0);
    return n > 0 ? n : -1;
}

static int Connect(const char* host, int port, SOCKET* out)
{
    hostent* he = gethostbyname(host);
    if (!he) {
        Log("http: gethostbyname(%s) failed (%d)", host, WSAGetLastError());
        return NET_DNS;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return NET_CONNECT;
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    if ((connect(s, (sockaddr*)&sa, sizeof(sa)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) ||
        !WaitSocket(s, true, kConnectTimeoutSec)) {
        Log("http: connect to %s:%d failed (%d)", host, port, WSAGetLastError());
        closesocket(s);
        return NET_CONNECT;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    *out = s;
    return NET_OK;
}

// ---------------------------------------------------------------- connections

static void CloseConn(Conn* c)
{
    if (c->open)
        closesocket(c->s);
    c->open = false;
}

static Conn* GetConn(const char* host, bool https)
{
    int freeSlot = -1;
    Conn* oldest = NULL;
    for (int i = 0; i < kMaxConns; i++) {
        Conn* c = s_conns[i];
        if (!c) {
            if (freeSlot < 0)
                freeSlot = i;
            continue;
        }
        if (!strcmp(c->host, host) && c->https == https)
            return c;
        if (!oldest || c->lastUse < oldest->lastUse)
            oldest = c;
    }
    Conn* c;
    if (freeSlot >= 0) {
        c = (Conn*)calloc(1, sizeof(Conn));
        if (!c)
            return NULL;
        s_conns[freeSlot] = c;
    } else {
        c = oldest;
        CloseConn(c);
        c->tlsReady = false;   // other host: no session to resume
    }
    strncpy(c->host, host, sizeof(c->host) - 1);
    c->host[sizeof(c->host) - 1] = 0;
    c->https = https;
    return c;
}

static int OpenConn(Conn* c)
{
    int err = Connect(c->host, c->https ? 443 : 80, &c->s);
    if (err != NET_OK)
        return err;
    c->open = true;
    if (!c->https)
        return NET_OK;

    bool resume = c->tlsReady;
    if (!c->tlsReady) {
        if (!s_tas[0].dn.data)
            InitTrustAnchors(s_tas);
        br_ssl_client_init_full(&c->sc, &c->xc, s_tas, TA_COUNT);
        br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);
        c->tlsReady = true;
    }
    unsigned long days, secs;
    ValidationTime(&days, &secs);
    br_x509_minimal_set_time(&c->xc, days, secs);
    InjectEntropy(&c->sc.eng);
    if (!br_ssl_client_reset(&c->sc, c->host, resume ? 1 : 0)) {
        Log("http: TLS reset failed (%d)", br_ssl_engine_last_error(&c->sc.eng));
        CloseConn(c);
        return NET_TLS;
    }
    br_sslio_init(&c->ioc, &c->sc.eng, SockRead, &c->s, SockWrite, &c->s);
    return NET_OK;
}

static int ConnWrite(Conn* c, const char* data, int len)
{
    if (!c->https)
        return send(c->s, data, len, 0) == len ? 0 : -1;
    if (br_sslio_write_all(&c->ioc, data, len) < 0 || br_sslio_flush(&c->ioc) < 0)
        return -1;
    return 0;
}

static int ConnRead(Conn* c, char* buf, int len)
{
    if (!c->https)
        return SockRead(&c->s, (unsigned char*)buf, len);
    return br_sslio_read(&c->ioc, buf, len);
}

// ---------------------------------------------------------------- HTTP

// Reads exactly len bytes (from the leftover buffer first).
struct Reader {
    Conn* c;
    char  buf[4096];
    int   pos, end;
};

static int ReadByte(Reader* r)
{
    if (r->pos == r->end) {
        int n = ConnRead(r->c, r->buf, sizeof(r->buf));
        if (n <= 0)
            return -1;
        r->pos = 0;
        r->end = n;
    }
    return (unsigned char)r->buf[r->pos++];
}

static bool ReadLine(Reader* r, char* line, int max)
{
    int n = 0;
    for (;;) {
        int ch = ReadByte(r);
        if (ch < 0)
            return false;
        if (ch == '\n')
            break;
        if (ch != '\r' && n < max - 1)
            line[n++] = (char)ch;
    }
    line[n] = 0;
    return true;
}

static bool ReadBytes(Reader* r, char* out, int len)
{
    while (len > 0) {
        if (r->pos == r->end) {
            int n = ConnRead(r->c, r->buf, sizeof(r->buf));
            if (n <= 0)
                return false;
            r->pos = 0;
            r->end = n;
        }
        int k = r->end - r->pos < len ? r->end - r->pos : len;
        memcpy(out, r->buf + r->pos, k);
        r->pos += k;
        out += k;
        len -= k;
    }
    return true;
}

static bool Grow(char** body, int* cap, int need)
{
    if (need <= *cap)
        return true;
    if (need > kMaxBody)
        return false;
    int c = *cap ? *cap : 64 * 1024;
    while (c < need)
        c *= 2;
    char* b = (char*)realloc(*body, c + 1);
    if (!b)
        return false;
    *body = b;
    *cap = c;
    return true;
}

// One request on an open connection. *answered tells whether the server answered anything:
// a dead keep-alive connection fails before that and the request is retried.
static int Exchange(Conn* c, const char* path, unsigned char** bodyOut, int* bodyLen, int* status, bool* answered)
{
    *answered = false;
    char req[1024];
    int n = _snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " HTTP_USER_AGENT "\r\n"
        "Accept: image/png,image/*\r\nConnection: keep-alive\r\n\r\n", path, c->host);
    if (n < 0 || ConnWrite(c, req, n) < 0)
        return NET_CONNECT;

    Reader* r = (Reader*)malloc(sizeof(Reader));
    if (!r)
        return NET_MEMORY;
    r->c = c;
    r->pos = r->end = 0;

    char line[512];
    int result = NET_OK;
    char* body = NULL;
    int cap = 0, len = 0;
    int contentLength = -1;
    bool chunked = false, closeAfter = false;

    if (!ReadLine(r, line, sizeof(line))) {
        result = NET_TIMEOUT;
        goto done;
    }
    *answered = true;
    if (strncmp(line, "HTTP/1.", 7) != 0) {
        result = NET_HTTP;
        goto done;
    }
    *status = atoi(line + 9);
    closeAfter = line[7] == '0';
    for (;;) {
        if (!ReadLine(r, line, sizeof(line))) {
            result = NET_TIMEOUT;
            goto done;
        }
        if (!line[0])
            break;
        if (!_strnicmp(line, "Content-Length:", 15))
            contentLength = atoi(line + 15);
        else if (!_strnicmp(line, "Transfer-Encoding:", 18) && strstr(line + 18, "chunked"))
            chunked = true;
        else if (!_strnicmp(line, "Connection:", 11) && strstr(line + 11, "close"))
            closeAfter = true;
    }

    if (chunked) {
        for (;;) {
            if (!ReadLine(r, line, sizeof(line))) {
                result = NET_TIMEOUT;
                goto done;
            }
            int size = (int)strtol(line, NULL, 16);
            if (size <= 0)
                break;
            if (!Grow(&body, &cap, len + size)) {
                result = NET_MEMORY;
                goto done;
            }
            if (!ReadBytes(r, body + len, size) || !ReadLine(r, line, sizeof(line))) {
                result = NET_TIMEOUT;
                goto done;
            }
            len += size;
        }
        while (ReadLine(r, line, sizeof(line)) && line[0]) {}   // trailers
    } else if (contentLength >= 0) {
        if (!Grow(&body, &cap, contentLength)) {
            result = NET_MEMORY;
            goto done;
        }
        if (!ReadBytes(r, body, contentLength)) {
            result = NET_TIMEOUT;
            goto done;
        }
        len = contentLength;
    } else {
        // no length: the body ends when the server closes the connection
        closeAfter = true;
        for (;;) {
            if (!Grow(&body, &cap, len + 4096)) {
                result = NET_MEMORY;
                goto done;
            }
            int k = r->end - r->pos;
            if (k > 0) {
                memcpy(body + len, r->buf + r->pos, k);
                len += k;
                r->pos = r->end;
                continue;
            }
            k = ConnRead(c, body + len, 4096);
            if (k <= 0)
                break;
            len += k;
        }
    }

done:
    free(r);
    if (result != NET_OK) {
        free(body);
        CloseConn(c);
        return result;
    }
    if (closeAfter)
        CloseConn(c);
    if (!body && !Grow(&body, &cap, 1))
        return NET_MEMORY;
    body[len] = 0;
    *bodyOut = (unsigned char*)body;
    *bodyLen = len;
    return NET_OK;
}

int HttpGet(const char* host, bool https, const char* path, unsigned char** body, int* bodyLen, int* status)
{
    *status = 0;
    Conn* c = GetConn(host, https);
    if (!c)
        return NET_MEMORY;
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = c->open;
        if (!c->open) {
            int err = OpenConn(c);
            if (err != NET_OK)
                return err;
        }
        bool answered;
        int err = Exchange(c, path, body, bodyLen, status, &answered);
        c->lastUse = GetTickCount();
        if (err == NET_OK)
            return NET_OK;
        if (c->https && !answered) {
            int tls = br_ssl_engine_last_error(&c->sc.eng);
            if (tls != BR_ERR_OK && tls != BR_ERR_IO) {
                Log("http: TLS error %d with %s", tls, host);
                CloseConn(c);
                c->tlsReady = false;
                return NET_TLS;
            }
        }
        if (!reused || answered)
            return err;
        // the server had closed the idle keep-alive connection: try once more on a new one
    }
    return NET_CONNECT;
}

void HttpCloseAll()
{
    for (int i = 0; i < kMaxConns; i++)
        if (s_conns[i]) {
            CloseConn(s_conns[i]);
            free(s_conns[i]);
            s_conns[i] = NULL;
        }
}
