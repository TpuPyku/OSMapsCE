#include "yamaps.h"

// JSON read in place, no tree (route.cpp's OSRM and Photon answers, offmaps.cpp's catalog of the regions).


const char* Ws(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static const char* SkipString(const char* p)   // p at its opening quote
{
    for (p++; *p && *p != '"'; p++)
        if (*p == '\\' && p[1])
            p++;
    return *p ? p + 1 : p;
}

static const char* SkipNested(const char* p)   // p at its opening { or [
{
    int depth = 0;
    while (*p) {
        if (*p == '"') {
            p = SkipString(p);
            continue;
        }
        if (*p == '{' || *p == '[')
            depth++;
        else if ((*p == '}' || *p == ']') && --depth == 0)
            return p + 1;
        p++;
    }
    return p;
}

const char* JSkip(const char* p)
{
    p = Ws(p);
    if (*p == '"')
        return SkipString(p);
    if (*p == '{' || *p == '[')
        return SkipNested(p);
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

// The value of key in the object at p, NULL if none.
const char* JKey(const char* p, const char* key)
{
    if (!p)
        return NULL;
    p = Ws(p);
    if (*p != '{')
        return NULL;
    p = Ws(p + 1);
    int kl = (int)strlen(key);
    while (*p == '"') {
        const char* k = p + 1;
        p = JSkip(p);
        bool match = p - k - 1 == kl && !strncmp(k, key, kl);
        p = Ws(p);
        if (*p != ':')
            return NULL;
        p = Ws(p + 1);
        if (match)
            return p;
        p = Ws(JSkip(p));
        if (*p != ',')
            return NULL;
        p = Ws(p + 1);
    }
    return NULL;
}

const char* JFirst(const char* p)   // the first item of the array at p
{
    if (!p)
        return NULL;
    p = Ws(p);
    if (*p != '[')
        return NULL;
    p = Ws(p + 1);
    return *p == ']' || !*p ? NULL : p;
}

const char* JNext(const char* p)
{
    p = Ws(JSkip(p));
    return *p == ',' ? Ws(p + 1) : NULL;
}

double JNum(const char* p, double def)
{
    if (!p || !((*p >= '0' && *p <= '9') || *p == '-'))
        return def;
    return atof(p);
}

// A string value, unescaped, UTF-8 into out (cap bytes); "" if p is not a string.
static int Utf8(unsigned u, char* out)   // the code point's bytes (surrogate pairs: not in Russian place names)
{
    if (u < 0x80) {
        out[0] = (char)u;
        return 1;
    }
    if (u < 0x800) {
        out[0] = (char)(0xC0 | (u >> 6));
        out[1] = (char)(0x80 | (u & 63));
        return 2;
    }
    out[0] = (char)(0xE0 | (u >> 12));
    out[1] = (char)(0x80 | ((u >> 6) & 63));
    out[2] = (char)(0x80 | (u & 63));
    return 3;
}

static unsigned Hex4(const char* p)   // \uXXXX's four digits at p (fewer: as many as there are)
{
    unsigned u = 0;
    for (int i = 0; i < 4 && p[i]; i++) {
        char c = p[i];
        u = u * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return u;
}

// The escape at p (the character after the backslash) into out at *n; returns the last character it took
static const char* Escape(const char* p, char* out, int* n)
{
    switch (*p) {
    case 'n': case 't': out[(*n)++] = ' '; return p;
    case 'r': case 'b': case 'f': return p;
    case 'u': *n += Utf8(Hex4(p + 1), out + *n); return p + 4;
    case 0: return p - 1;
    default: out[(*n)++] = *p; return p;   // \" \\ \/
    }
}

int JStr(const char* p, char* out, int cap)
{
    int n = 0;
    if (p && *p == '"')
        for (p++; *p && *p != '"' && n < cap - 4; p++) {
            if (*p != '\\')
                out[n++] = *p;
            else
                p = Escape(p + 1, out, &n);
        }
    out[n] = 0;
    return n;
}

void JWide(const char* p, wchar_t* out, int cap)
{
    char u[512];
    JStr(p, u, sizeof(u));
    if (!MultiByteToWideChar(CP_UTF8, 0, u, -1, out, cap))
        out[0] = 0;
    out[cap - 1] = 0;
}
