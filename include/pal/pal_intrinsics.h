#ifndef PAL_INTRINSICS_H
#define PAL_INTRINSICS_H

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

/* MWCC PPC intrinsics -> GCC/Clang equivalents (19 occurrences outside SDK) */
#ifdef __cntlzw
#undef __cntlzw
#endif
#define __cntlzw(x)  __builtin_clz(x)

#ifdef __dcbf
#undef __dcbf
#endif
#define __dcbf(a, b) ((void)0)

#ifdef __dcbz
#undef __dcbz
#endif
#define __dcbz(a, b) ((void)0)

#ifdef __sync
#undef __sync
#endif
#define __sync()     ((void)0)

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

#endif /* PAL_INTRINSICS_H */
