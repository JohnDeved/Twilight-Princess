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
#define GX_TEV_SHADER_TEV        5   /* Generic TEV uber-shader with alpha compare */
#define GX_TEV_SHADER_COUNT      6

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

/* Diagnostic: submit a test quad directly through bgfx */
void pal_tev_submit_test_quad(void);

/**
 * Report TEV config and draw path diagnostics.
 * Emits JSON summaries of unique TEV combiner configurations seen
 * and categorized draw path failure/skip counts.
 */
void pal_tev_report_diagnostics(void);
u32 pal_tev_get_total_attempt_count(void);
u32 pal_tev_get_ok_submitted_count(void);
u32 pal_tev_get_filter_skip_count(void);
u32 pal_tev_get_skip_passclr_fill_count(void);
u32 pal_tev_get_skip_passclr_alpha_count(void);
u32 pal_tev_get_skip_passclr_env_count(void);

/**
 * Notify TEV system of a perspective projection change.
 * Called from pal_gx_set_projection when type == GX_PERSPECTIVE.
 * Saves the projection for use by centroid camera draws, which need
 * perspective projection even when the current g_gx_state.proj_mtx
 * has been overwritten by J2D orthographic code.
 */
void pal_tev_set_persp_proj(const float mtx[4][4]);

/**
 * Get the bgfx program handle for a TEV preset shader.
 * Returns a bgfx::ProgramHandle idx (as unsigned short) for use by test draw calls.
 */
unsigned short pal_tev_get_program_handle(int preset);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_TEV_H */
