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

/* Set to 1 when the GX shim has a real rendering backend (bgfx).
 * GXCopyDisp checks this before firing RENDER_FRAME milestone. */
extern int gx_shim_active;

/* Per-frame stub tracking for honest milestone gating.
 * Reset at frame start, incremented by gx_stub_hit().
 * If gx_frame_stub_count > 0, the frame hit unimplemented stubs
 * and cannot produce a valid verifiable image. */
extern unsigned int gx_frame_stub_count;

/* Per-frame draw quality metrics.
 * gx_frame_draw_calls:  number of real draw calls flushed via TEV pipeline
 * gx_frame_valid_verts: total vertices submitted with valid vertex data */
extern unsigned int gx_frame_draw_calls;
extern unsigned int gx_frame_valid_verts;

/* Reset per-frame counters. Call at beginning of each frame. */
void gx_stub_frame_reset(void);

/* Check if the current frame produced a valid verifiable image:
 * - No GX stubs were hit during rendering
 * - At least one real draw call with valid vertices was submitted
 * - Draw call counter matches the cross-check counter (anti-tamper)
 * Returns 1 if valid, 0 if not. */
int gx_stub_frame_is_valid(void);

/* Anti-cheat: must be called from the actual TEV draw path only.
 * Provides independent verification that gx_frame_draw_calls is honest. */
void gx_stub_draw_call_crosscheck(void);

static inline void gx_stub_hit(int id, const char* name) {
    if (id >= 0 && id < GX_STUB_MAX) {
        if (gx_stub_hits[id]++ == 0) {
            gx_stub_names[id] = name;
            fprintf(stderr, "{\"stub_hit\":\"%s\",\"id\":%d}\n", name, id);
            fflush(stderr);
        }
        /* Track per-frame stub hits for milestone gating */
        gx_frame_stub_count++;
    }
}

void gx_stub_report(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

#endif /* GX_STUB_TRACKER_H */
