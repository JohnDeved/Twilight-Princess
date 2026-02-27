#ifndef GX_STUB_TRACKER_H
#define GX_STUB_TRACKER_H

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GX_STUB_MAX 256

extern unsigned int gx_stub_hits[GX_STUB_MAX];
extern const char* gx_stub_names[GX_STUB_MAX];

static inline void gx_stub_hit(int id, const char* name) {
    if (id >= 0 && id < GX_STUB_MAX) {
        if (gx_stub_hits[id]++ == 0) {
            gx_stub_names[id] = name;
            fprintf(stderr, "{\"stub_hit\":\"%s\",\"id\":%d}\n", name, id);
            fflush(stderr);
        }
    }
}

void gx_stub_report(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

#endif /* GX_STUB_TRACKER_H */
