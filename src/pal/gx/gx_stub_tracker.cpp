/**
 * gx_stub_tracker.cpp
 * GX stub hit counter and report for CI coverage tracking.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include "pal/gx/gx_stub_tracker.h"
#include <stdio.h>

unsigned int gx_stub_hits[GX_STUB_MAX];
const char* gx_stub_names[GX_STUB_MAX];

void gx_stub_report(void) {
    int i;
    for (i = 0; i < GX_STUB_MAX; i++) {
        if (gx_stub_hits[i] > 0) {
            fprintf(stdout, "{\"stub\":\"%s\",\"hits\":%u}\n",
                    gx_stub_names[i] ? gx_stub_names[i] : "unknown", gx_stub_hits[i]);
        }
    }
    fflush(stdout);
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
