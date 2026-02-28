/**
 * pal_window.h - SDL3 window management for PC port
 *
 * Provides window creation and event handling for the PC port.
 * In headless mode (TP_HEADLESS=1), no window is created.
 */

#ifndef PAL_WINDOW_H
#define PAL_WINDOW_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SDL3 and create a window.
 * Returns 1 on success, 0 on failure.
 * In headless mode, succeeds without creating a window.
 */
int pal_window_init(u32 width, u32 height, const char* title);

/**
 * Process pending SDL events (input, window close, etc.).
 * Returns 0 if the application should quit, 1 to continue.
 */
int pal_window_poll(void);

/**
 * Get the native window handle for bgfx integration.
 * Returns NULL in headless mode.
 */
void* pal_window_get_native_handle(void);

/**
 * Get the native display handle (X11 Display, etc.) for bgfx.
 * Returns NULL if not applicable.
 */
void* pal_window_get_native_display(void);

/**
 * Shut down SDL3 and destroy the window.
 */
void pal_window_shutdown(void);

/**
 * Check if we're in headless mode.
 */
int pal_window_is_headless(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_WINDOW_H */
