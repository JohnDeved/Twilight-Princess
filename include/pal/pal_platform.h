#ifndef PAL_PLATFORM_H
#define PAL_PLATFORM_H

/*
 * pal_platform.h
 * Force-included before every compilation unit in the PC port build.
 * Provides standard library headers and MWCC compatibility shims that the
 * original compiler (Metrowerks CodeWarrior) included implicitly.
 */

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstddef>
#include <cmath>
#include <strings.h>

/* MWCC implicitly provides these in the global namespace */
using std::strcmp;
using std::strlen;
using std::memset;
using std::memcpy;
using std::memmove;
using std::sprintf;
using std::printf;
using std::sscanf;
using std::abs;

/* GCC C++ doesn't provide std::fabsf -- alias to fabsf from C */
namespace std {
    static inline float fabsf(float x) { return ::fabsf(x); }
}

/* Bring C99 math functions into global namespace */
using std::isnan;
using std::isinf;

/* Case-insensitive string compare (MWCC/MSVC: stricmp, POSIX: strcasecmp) */
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif

/* Math macros that MWCC provides implicitly */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD(deg) ((deg) * (M_PI / 180.0f))
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG(rad) ((rad) * (180.0f / M_PI))
#endif

/* MWCC PPC intrinsics -- provide fallbacks for GCC/Clang.
 * Note: __cntlzw, __dcbf, __dcbz, __sync, __abs, __rlwimi are in global.h.
 * We only provide math intrinsics that aren't defined elsewhere. */
#ifndef __MWERKS__

/* PPC reciprocal estimate intrinsics -- use standard math */
static inline float __fres(float x) { return 1.0f / x; }

#endif /* !__MWERKS__ */

#endif /* PAL_PLATFORM_H */
