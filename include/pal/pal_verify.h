/**
 * pal_verify.h - Subsystem verification for automated testing
 *
 * Provides structured JSON logging and validation for:
 * - Rendering: frame captures, draw stats, pixel analysis
 * - Input: event injection and response tracking
 * - Audio: buffer capture and non-silence detection
 *
 * Activated by TP_VERIFY=1 environment variable.
 * Results are emitted as JSON lines to stdout for CI parsing.
 */

#ifndef PAL_VERIFY_H
#define PAL_VERIFY_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ */
/* Initialization                                                   */
/* ================================================================ */

/**
 * Initialize the verification system.
 * Checks TP_VERIFY env var. If not set, all verification is disabled.
 * Call once at startup (from pal_render_init or main).
 */
void pal_verify_init(void);

/**
 * Returns 1 if verification is active.
 */
int pal_verify_active(void);

/* ================================================================ */
/* Rendering verification                                           */
/* ================================================================ */

/**
 * Report per-frame rendering metrics as JSON.
 * Called at end of each frame (from pal_render_end_frame).
 *
 * Emits: draw_calls, total_verts, stub_count, valid, fb_hash, fb_has_draws,
 *        textured_draws, untextured_draws, unique_textures, shader_mask,
 *        depth_draws, blend_draws, prim_mask.
 *
 * Also captures the software framebuffer at configurable intervals
 * (set by TP_VERIFY_CAPTURE_FRAMES env var, e.g. "30,60,120,300").
 */
void pal_verify_frame(u32 frame_num, u32 draw_calls, u32 total_verts,
                      u32 stub_count, u32 valid);

/**
 * Capture current software framebuffer to a BMP file.
 * Path format: <TP_VERIFY_DIR>/frame_NNNN.bmp
 */
void pal_verify_capture_frame(u32 frame_num);

/**
 * Analyze the current software framebuffer and report pixel stats.
 * Returns the percentage of non-black pixels (0-100).
 * This gives a quick "is something rendering?" check without
 * needing reference images.
 */
int pal_verify_analyze_fb(u32 frame_num);

/* ================================================================ */
/* Input verification                                               */
/* ================================================================ */

/**
 * Log an input event for verification.
 * Called when synthetic or real input is processed.
 */
void pal_verify_input(u32 frame_num, u16 buttons, s8 stick_x, s8 stick_y);

/**
 * Log a game response to input (e.g., scene change, menu action).
 * Called from game code hooks when detectable state changes occur.
 */
void pal_verify_input_response(u32 frame_num, const char* event, const char* detail);

/* ================================================================ */
/* Audio verification                                               */
/* ================================================================ */

/**
 * Log audio subsystem status.
 * Called periodically to track audio health.
 */
void pal_verify_audio(u32 frame_num, int audio_active, u32 samples_mixed,
                      u32 buffers_queued, int has_nonsilence);

/**
 * Check if an audio buffer contains non-silent samples.
 * Returns 1 if any sample exceeds the silence threshold.
 */
int pal_verify_audio_check_silence(const s16* samples, u32 num_samples,
                                   s16 threshold);

/* ================================================================ */
/* Summary                                                          */
/* ================================================================ */

/**
 * Emit a final verification summary as JSON.
 * Call at test completion (before exit).
 * Reports overall health: frames rendered, non-black frames,
 * input events processed, audio status.
 */
void pal_verify_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_VERIFY_H */
