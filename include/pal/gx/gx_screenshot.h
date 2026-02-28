/**
 * gx_screenshot.h - Software framebuffer screenshot capture
 *
 * Provides a CPU-side framebuffer that captures 2D textured quad draws
 * from the GX shim, producing a BMP screenshot without requiring a GPU.
 * Works in headless mode (TP_HEADLESS=1) with the bgfx Noop renderer.
 *
 * Activated by setting TP_SCREENSHOT=<output_path.bmp> environment variable.
 */

#ifndef PAL_GX_SCREENSHOT_H
#define PAL_GX_SCREENSHOT_H

#include "dolphin/types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize screenshot system — checks TP_SCREENSHOT env var */
void pal_screenshot_init(void);

/* Blit current GX draw state to software framebuffer (called from pal_tev_flush_draw) */
void pal_screenshot_blit(void);

/* Save the software framebuffer to BMP file */
void pal_screenshot_save(void);

/* Returns 1 if screenshot capture is active */
int pal_screenshot_active(void);

/* Get pointer to the software framebuffer (640x480 RGBA8, or NULL) */
uint8_t* pal_screenshot_get_fb(void);

/* Get framebuffer dimensions */
int pal_screenshot_get_fb_width(void);
int pal_screenshot_get_fb_height(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_SCREENSHOT_H */
