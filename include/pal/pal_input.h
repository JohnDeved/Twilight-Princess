/**
 * pal_input.h - SDL3 input mapping for PC port
 *
 * Maps SDL3 keyboard and gamepad input to GameCube PADStatus.
 * Keyboard provides a default mapping for development/testing.
 * SDL3 gamepads are auto-detected and mapped to GC controller layout.
 */

#ifndef PAL_INPUT_H
#define PAL_INPUT_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SDL3 input subsystem (gamepad + keyboard tracking).
 * Call after pal_window_init(). Safe in headless mode (keyboard-only).
 * Returns 1 on success, 0 on failure.
 */
int pal_input_init(void);

/**
 * Process an SDL event for input state tracking.
 * Call from pal_window_poll() for each SDL event.
 */
void pal_input_handle_event(const void* sdl_event);

/**
 * Read current input state into PADStatus for the given port (0-3).
 * Port 0 reads keyboard + first gamepad.
 * Returns PAD_ERR_NONE if input is available, PAD_ERR_NO_CONTROLLER otherwise.
 */
int pal_input_read_pad(int port, void* pad_status);

/**
 * Shut down SDL3 input subsystem.
 */
void pal_input_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_INPUT_H */
