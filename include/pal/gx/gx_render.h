/**
 * gx_render.h - bgfx rendering backend for GX shim
 *
 * Renders via real OpenGL using Mesa's software rasterizer on Xvfb —
 * no GPU hardware needed. Every frame is captured via BGFX_RESET_CAPTURE
 * for verification. Falls back to Noop when no display is available.
 */

#ifndef PAL_GX_RENDER_H
#define PAL_GX_RENDER_H

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize bgfx rendering backend.
 * Uses real OpenGL (Mesa software renderer on Xvfb) when DISPLAY is set,
 * falls back to Noop when no display is available.
 * Returns 1 on success, 0 on failure. */
int pal_render_init(void);

/* Shut down bgfx. */
void pal_render_shutdown(void);

/* Called at start of each frame (before GX draw calls). */
void pal_render_begin_frame(void);

/* Called at end of each frame (submits to bgfx). */
void pal_render_end_frame(void);

/* Returns 1 if bgfx is initialized and active. */
int pal_render_is_active(void);

/* Returns 1 if using Noop renderer (no frame capture possible). */
int pal_render_is_noop(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

#endif /* PAL_GX_RENDER_H */
