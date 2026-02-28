#ifndef GX_BGFX_H
#define GX_BGFX_H

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize bgfx backend.
 * Uses Noop renderer if TP_HEADLESS=1, auto-selects otherwise.
 * Returns 1 on success, 0 on failure. */
int pal_gx_bgfx_init(void);

/* Shut down bgfx. */
void pal_gx_bgfx_shutdown(void);

/* Called at start of each frame (before GX draw calls). */
void pal_gx_begin_frame(void);

/* Called at end of each frame (submits to bgfx). */
void pal_gx_end_frame(void);

/* Returns 1 if bgfx is initialized and active. */
int pal_gx_bgfx_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

#endif /* GX_BGFX_H */
