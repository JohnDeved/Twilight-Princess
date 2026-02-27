/**
 * gx_stub_tracker.cpp
 * GX stub hit counter and report for CI coverage tracking.
 * Also provides per-frame stub tracking for honest milestone gating.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include "pal/gx/gx_stub_tracker.h"
#include <stdio.h>

unsigned int gx_stub_hits[GX_STUB_MAX];
const char* gx_stub_names[GX_STUB_MAX];
int gx_shim_active = 0;

/* Per-frame tracking */
unsigned int gx_frame_stub_count = 0;
unsigned int gx_frame_draw_calls = 0;
unsigned int gx_frame_valid_verts = 0;

void gx_stub_frame_reset(void) {
    gx_frame_stub_count = 0;
    gx_frame_draw_calls = 0;
    gx_frame_valid_verts = 0;
}

int gx_stub_frame_is_valid(void) {
    /* A frame is valid for milestone purposes only if:
     * 1. No GX stubs were hit during the frame
     * 2. At least one real draw call was flushed with valid vertices
     * This ensures we are certain the frame produces a valid verifiable image. */
    return (gx_frame_stub_count == 0 && gx_frame_draw_calls > 0 && gx_frame_valid_verts > 0);
}

void gx_stub_report(void) {
    int i;
    for (i = 0; i < GX_STUB_MAX; i++) {
        if (gx_stub_hits[i] > 0) {
            fprintf(stdout, "{\"stub\":\"%s\",\"hits\":%u}\n",
                    gx_stub_names[i] ? gx_stub_names[i] : "unknown", gx_stub_hits[i]);
        }
    }
    /* Report per-frame validation status */
    fprintf(stdout, "{\"frame_validation\":{\"stub_count\":%u,\"draw_calls\":%u,\"valid_verts\":%u,\"valid\":%d}}\n",
            gx_frame_stub_count, gx_frame_draw_calls, gx_frame_valid_verts,
            gx_stub_frame_is_valid());
    fflush(stdout);
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
