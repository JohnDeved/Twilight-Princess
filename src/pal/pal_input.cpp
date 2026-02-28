/**
 * pal_input.cpp - SDL3 keyboard/gamepad → PADStatus mapping
 *
 * Maps SDL3 keyboard keys and gamepad buttons to GameCube PADStatus
 * for the PC port. Keyboard is always available; gamepads are optional.
 *
 * Default keyboard mapping (GameCube controller layout):
 *   WASD         = Main stick
 *   Arrow keys   = C-stick (substick)
 *   Space        = A button
 *   Left Shift   = B button
 *   Q            = X button
 *   E            = Y button
 *   Z            = Z trigger
 *   Tab          = L trigger
 *   R            = R trigger
 *   Return/Enter = Start
 *   F/G          = D-pad left/right
 *   T/V          = D-pad up/down
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include "dolphin/pad.h"
#include "pal/pal_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ */
/* Keyboard state tracking                                          */
/* ================================================================ */

static struct {
    /* Stick axes (-128 to 127 range, mapped from key presses) */
    int stick_x;    /* A/D keys */
    int stick_y;    /* W/S keys */
    int substick_x; /* Left/Right arrows */
    int substick_y; /* Up/Down arrows */

    /* Button state bitmask (PAD_BUTTON_* flags) */
    u16 buttons;

    /* Analog trigger values (0 or 255 from key presses) */
    u8  trigger_l;
    u8  trigger_r;

    int initialized;
} s_kb_state;

/* ================================================================ */
/* SDL3 Gamepad state                                               */
/* ================================================================ */

static SDL_Gamepad* s_gamepad = NULL;

/* ================================================================ */
/* Keyboard scancode → button mapping                               */
/* ================================================================ */

static void kb_update_key(SDL_Keycode key, int pressed) {
    switch (key) {
    /* Main stick */
    case SDLK_W: s_kb_state.stick_y = pressed ?  127 : (s_kb_state.stick_y > 0 ? 0 : s_kb_state.stick_y); break;
    case SDLK_S: s_kb_state.stick_y = pressed ? -128 : (s_kb_state.stick_y < 0 ? 0 : s_kb_state.stick_y); break;
    case SDLK_A: s_kb_state.stick_x = pressed ? -128 : (s_kb_state.stick_x < 0 ? 0 : s_kb_state.stick_x); break;
    case SDLK_D: s_kb_state.stick_x = pressed ?  127 : (s_kb_state.stick_x > 0 ? 0 : s_kb_state.stick_x); break;

    /* C-stick (substick) */
    case SDLK_UP:    s_kb_state.substick_y = pressed ?  127 : (s_kb_state.substick_y > 0 ? 0 : s_kb_state.substick_y); break;
    case SDLK_DOWN:  s_kb_state.substick_y = pressed ? -128 : (s_kb_state.substick_y < 0 ? 0 : s_kb_state.substick_y); break;
    case SDLK_LEFT:  s_kb_state.substick_x = pressed ? -128 : (s_kb_state.substick_x < 0 ? 0 : s_kb_state.substick_x); break;
    case SDLK_RIGHT: s_kb_state.substick_x = pressed ?  127 : (s_kb_state.substick_x > 0 ? 0 : s_kb_state.substick_x); break;

    /* Face buttons */
    case SDLK_SPACE:  if (pressed) s_kb_state.buttons |= PAD_BUTTON_A; else s_kb_state.buttons &= ~PAD_BUTTON_A; break;
    case SDLK_LSHIFT: if (pressed) s_kb_state.buttons |= PAD_BUTTON_B; else s_kb_state.buttons &= ~PAD_BUTTON_B; break;
    case SDLK_Q:      if (pressed) s_kb_state.buttons |= PAD_BUTTON_X; else s_kb_state.buttons &= ~PAD_BUTTON_X; break;
    case SDLK_E:      if (pressed) s_kb_state.buttons |= PAD_BUTTON_Y; else s_kb_state.buttons &= ~PAD_BUTTON_Y; break;

    /* Triggers */
    case SDLK_Z:   if (pressed) s_kb_state.buttons |= PAD_TRIGGER_Z; else s_kb_state.buttons &= ~PAD_TRIGGER_Z; break;
    case SDLK_TAB: s_kb_state.trigger_l = pressed ? 255 : 0;
                   if (pressed) s_kb_state.buttons |= PAD_TRIGGER_L; else s_kb_state.buttons &= ~PAD_TRIGGER_L; break;
    case SDLK_R:   s_kb_state.trigger_r = pressed ? 255 : 0;
                   if (pressed) s_kb_state.buttons |= PAD_TRIGGER_R; else s_kb_state.buttons &= ~PAD_TRIGGER_R; break;

    /* Start */
    case SDLK_RETURN: if (pressed) s_kb_state.buttons |= PAD_BUTTON_START; else s_kb_state.buttons &= ~PAD_BUTTON_START; break;

    /* D-pad */
    case SDLK_F: if (pressed) s_kb_state.buttons |= PAD_BUTTON_LEFT;  else s_kb_state.buttons &= ~PAD_BUTTON_LEFT;  break;
    case SDLK_G: if (pressed) s_kb_state.buttons |= PAD_BUTTON_RIGHT; else s_kb_state.buttons &= ~PAD_BUTTON_RIGHT; break;
    case SDLK_T: if (pressed) s_kb_state.buttons |= PAD_BUTTON_UP;    else s_kb_state.buttons &= ~PAD_BUTTON_UP;    break;
    case SDLK_V: if (pressed) s_kb_state.buttons |= PAD_BUTTON_DOWN;  else s_kb_state.buttons &= ~PAD_BUTTON_DOWN;  break;

    default: break;
    }
}

/* ================================================================ */
/* Gamepad → PADStatus mapping                                      */
/* ================================================================ */

static void gamepad_read(PADStatus* status) {
    if (!s_gamepad) return;

    /* Axes: SDL range is -32768..32767, GC range is -128..127 */
    s16 lx = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    s16 ly = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    s16 rx = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    s16 ry = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
    s16 lt = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s16 rt = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    status->stickX    = (s8)(lx >> 8);
    status->stickY    = (s8)(-(ly >> 8)); /* SDL Y is inverted vs GC */
    status->substickX = (s8)(rx >> 8);
    status->substickY = (s8)(-(ry >> 8));

    /* Triggers: SDL range 0..32767 → GC range 0..255 */
    status->triggerLeft  = (u8)(lt >> 7);
    status->triggerRight = (u8)(rt >> 7);

    /* Buttons */
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_SOUTH))      status->button |= PAD_BUTTON_A;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_EAST))       status->button |= PAD_BUTTON_B;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_WEST))       status->button |= PAD_BUTTON_X;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_NORTH))      status->button |= PAD_BUTTON_Y;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_START))      status->button |= PAD_BUTTON_START;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))    status->button |= PAD_BUTTON_UP;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  status->button |= PAD_BUTTON_DOWN;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  status->button |= PAD_BUTTON_LEFT;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) status->button |= PAD_BUTTON_RIGHT;

    /* Map shoulder buttons to GC L/R digital, Z to right shoulder */
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  status->button |= PAD_TRIGGER_L;
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) status->button |= PAD_TRIGGER_R;

    /* Analog trigger threshold → digital */
    if (status->triggerLeft  > 128) status->button |= PAD_TRIGGER_L;
    if (status->triggerRight > 128) status->button |= PAD_TRIGGER_R;

    /* Map guide/back to Z trigger */
    if (SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_BACK)) status->button |= PAD_TRIGGER_Z;
}

/* ================================================================ */
/* Public API                                                       */
/* ================================================================ */

int pal_input_init(void) {
    if (s_kb_state.initialized) return 1;

    memset(&s_kb_state, 0, sizeof(s_kb_state));

    /* Try to initialize SDL3 gamepad subsystem */
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        /* Check for already-connected gamepads */
        int count = 0;
        SDL_JoystickID* joysticks = SDL_GetGamepads(&count);
        if (joysticks && count > 0) {
            s_gamepad = SDL_OpenGamepad(joysticks[0]);
            if (s_gamepad) {
                fprintf(stderr, "{\"pal_input\":\"gamepad_connected\",\"name\":\"%s\"}\n",
                        SDL_GetGamepadName(s_gamepad));
            }
            SDL_free(joysticks);
        }
    }

    s_kb_state.initialized = 1;
    fprintf(stderr, "{\"pal_input\":\"ready\",\"gamepad\":%s}\n",
            s_gamepad ? "true" : "false");
    return 1;
}

void pal_input_handle_event(const void* sdl_event) {
    const SDL_Event* ev = (const SDL_Event*)sdl_event;
    if (!ev) return;

    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!ev->key.repeat) {
            kb_update_key(ev->key.key, 1);
        }
        break;

    case SDL_EVENT_KEY_UP:
        kb_update_key(ev->key.key, 0);
        break;

    case SDL_EVENT_GAMEPAD_ADDED: {
        if (!s_gamepad) {
            s_gamepad = SDL_OpenGamepad(ev->gdevice.which);
            if (s_gamepad) {
                fprintf(stderr, "{\"pal_input\":\"gamepad_added\",\"name\":\"%s\"}\n",
                        SDL_GetGamepadName(s_gamepad));
            }
        }
        break;
    }

    case SDL_EVENT_GAMEPAD_REMOVED: {
        if (s_gamepad) {
            SDL_JoystickID gpad_id = SDL_GetGamepadID(s_gamepad);
            if (gpad_id == ev->gdevice.which) {
                SDL_CloseGamepad(s_gamepad);
                s_gamepad = NULL;
                fprintf(stderr, "{\"pal_input\":\"gamepad_removed\"}\n");
            }
        }
        break;
    }

    default:
        break;
    }
}

int pal_input_read_pad(int port, void* pad_status) {
    PADStatus* status = (PADStatus*)pad_status;
    if (!status) return PAD_ERR_NO_CONTROLLER;

    memset(status, 0, sizeof(PADStatus));

    if (port != 0) {
        /* Only port 0 is supported */
        status->err = PAD_ERR_NO_CONTROLLER;
        return PAD_ERR_NO_CONTROLLER;
    }

    /* Fill from keyboard state */
    status->button      = s_kb_state.buttons;
    status->stickX      = (s8)s_kb_state.stick_x;
    status->stickY      = (s8)s_kb_state.stick_y;
    status->substickX   = (s8)s_kb_state.substick_x;
    status->substickY   = (s8)s_kb_state.substick_y;
    status->triggerLeft  = s_kb_state.trigger_l;
    status->triggerRight = s_kb_state.trigger_r;

    /* Overlay gamepad state (gamepad takes priority for analog axes) */
    gamepad_read(status);

    status->err = PAD_ERR_NONE;
    return PAD_ERR_NONE;
}

void pal_input_shutdown(void) {
    if (s_gamepad) {
        SDL_CloseGamepad(s_gamepad);
        s_gamepad = NULL;
    }
    memset(&s_kb_state, 0, sizeof(s_kb_state));
}

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
