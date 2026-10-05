/* stdint.h for Visual Studio 2005 (it has none). Only what BearSSL needs. */
#ifndef COMPAT_STDINT_H
#define COMPAT_STDINT_H
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef __int64            int64_t;
typedef unsigned __int64   uint64_t;
#ifdef _WIN64
typedef unsigned __int64   uintptr_t;
#else
typedef unsigned int       uintptr_t;
#endif
#define UINT32_C(x) (x ## U)
#define UINT64_C(x) (x ## ULL)
#define INT64_C(x)  (x ## LL)
#endif
