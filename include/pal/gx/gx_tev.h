/**
 * gx_tev.h - TEV → bgfx shader pipeline (Step 5c)
 *
 * Maps GX TEV combiner configurations to bgfx shader programs.
 * The game uses 5 common TEV presets (PASSCLR, REPLACE, MODULATE, BLEND, DECAL)
 * plus arbitrary J3D-generated configurations.
 *
 * For the initial implementation, we handle the 5 presets with pre-compiled
 * shaders and fall back to a generic TEV shader for other configurations.
 */

#ifndef PAL_GX_TEV_H
#define PAL_GX_TEV_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TEV preset shader IDs */
#define GX_TEV_SHADER_PASSCLR    0   /* Pass vertex color (no texture) */
#define GX_TEV_SHADER_REPLACE    1   /* Replace with texture color */
#define GX_TEV_SHADER_MODULATE   2   /* Texture * vertex color */
#define GX_TEV_SHADER_BLEND      3   /* Blend texture with vertex color */
#define GX_TEV_SHADER_DECAL      4   /* Decal texture (alpha blend) */
#define GX_TEV_SHADER_COUNT      5

/**
 * Initialize the TEV shader system.
 * Creates bgfx shader programs for all preset TEV modes.
 * Must be called after bgfx::init().
 */
void pal_tev_init(void);

/**
 * Shut down the TEV shader system.
 * Destroys all shader programs.
 */
void pal_tev_shutdown(void);

/**
 * Flush current GX draw state to bgfx.
 * Called from pal_gx_end() after vertex data has been captured.
 * Selects appropriate shader based on TEV configuration,
 * builds vertex/index buffers, sets render state, and submits draw call.
 */
void pal_tev_flush_draw(void);

/**
 * Check if TEV shader system is ready for drawing.
 */
int pal_tev_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_TEV_H */
