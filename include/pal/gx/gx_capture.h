/**
 * gx_capture.h - bgfx frame capture buffer
 *
 * Receives rendered frame data from bgfx's capture API (BGFX_RESET_CAPTURE).
 * bgfx renders via real OpenGL on Mesa's software rasterizer (Xvfb in CI) —
 * no GPU hardware needed. Each frame, bgfx calls captureFrame() with the
 * actual rendered pixels, which are stored here for verification.
 *
 * Used by pal_verify for automated testing: CRC32 hashing, pixel analysis,
 * BMP snapshots, and video generation.
 *
 * Activated by TP_SCREENSHOT=<path.bmp> and/or TP_VERIFY=1.
 */

#ifndef PAL_GX_CAPTURE_H
#define PAL_GX_CAPTURE_H

#include "dolphin/types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize capture buffer — checks TP_SCREENSHOT and TP_VERIFY env vars */
void pal_capture_init(void);

/**
 * Configure capture dimensions. Called by BgfxCallback::captureBegin().
 */
void pal_capture_begin(uint32_t width, uint32_t height, uint32_t pitch, int yflip);

/**
 * Receive a rendered frame from bgfx's captureFrame() callback.
 * Converts BGRA → RGBA and stores in the capture buffer.
 */
void pal_capture_frame(const void* data, uint32_t size);

/* Save the capture buffer to BMP file (TP_SCREENSHOT path) */
void pal_capture_save(void);

/* Returns 1 if TP_SCREENSHOT is active and not yet saved */
int pal_capture_screenshot_active(void);

/* Get pointer to the capture buffer (640x480 RGBA8, or NULL) */
uint8_t* pal_capture_get_fb(void);

/* Get capture buffer dimensions */
int pal_capture_get_fb_width(void);
int pal_capture_get_fb_height(void);

/* Clear the capture buffer to black */
void pal_capture_clear_fb(void);

/* Returns 1 if bgfx has delivered frame data since last clear */
int pal_capture_has_data(void);

/* Get total number of frames captured */
uint32_t pal_capture_get_frame_count(void);

/* Close the raw video file (call at shutdown) */
void pal_capture_shutdown(void);

/**
 * Set debug text metadata for captured frames.
 * Called each frame from pal_render_end_frame() before bgfx::frame().
 * Text is written to verify_output/frame_metadata.txt so that external
 * tooling (ffmpeg drawtext / Python) can burn it into BMPs and MP4.
 */
void pal_capture_set_debug_info(const char* line0, const char* line1);

/**
 * Direct OpenGL readback (fallback for when bgfx capture doesn't work).
 * Calls glReadPixels to read the actual framebuffer after bgfx::frame().
 */
void pal_capture_readback_gl(uint32_t width, uint32_t height);

/* CRC32 hash of capture buffer — deterministic for regression detection */
uint32_t pal_capture_hash_fb(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_CAPTURE_H */
