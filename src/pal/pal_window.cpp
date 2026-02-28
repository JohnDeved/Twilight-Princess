/**
 * pal_window.cpp - SDL3 window management implementation
 *
 * Creates a game window via SDL3 for the PC port.
 * In headless mode (TP_HEADLESS=1), no window is created.
 * Provides native window handle to bgfx for rendering.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_events.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "pal/pal_window.h"
#include "pal/pal_input.h"

static SDL_Window* s_window = NULL;
static int s_headless = 0;
static int s_initialized = 0;
static int s_quit_requested = 0;

int pal_window_init(u32 width, u32 height, const char* title) {
    if (s_initialized)
        return 1;

    const char* headless_env = getenv("TP_HEADLESS");
    s_headless = (headless_env && headless_env[0] == '1');

    if (s_headless) {
        fprintf(stderr, "{\"pal_window\":\"headless\",\"skip\":true}\n");
        s_initialized = 1;
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "{\"pal_window\":\"sdl_init_failed\",\"error\":\"%s\"}\n", SDL_GetError());
        return 0;
    }

    s_window = SDL_CreateWindow(
        title ? title : "Twilight Princess",
        (int)width, (int)height,
        SDL_WINDOW_RESIZABLE
    );

    if (!s_window) {
        fprintf(stderr, "{\"pal_window\":\"window_create_failed\",\"error\":\"%s\"}\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }

    s_initialized = 1;
    fprintf(stderr, "{\"pal_window\":\"ready\",\"width\":%u,\"height\":%u}\n", width, height);
    return 1;
}

int pal_window_poll(void) {
    SDL_Event event;

    if (s_headless || !s_initialized)
        return !s_quit_requested;

    while (SDL_PollEvent(&event)) {
        /* Forward all events to input handler */
        pal_input_handle_event(&event);

        switch (event.type) {
            case SDL_EVENT_QUIT:
                s_quit_requested = 1;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    s_quit_requested = 1;
                }
                break;
            default:
                break;
        }
    }

    return !s_quit_requested;
}

void* pal_window_get_native_handle(void) {
#ifdef __linux__
    if (!s_window) return NULL;

    SDL_PropertiesID props = SDL_GetWindowProperties(s_window);
    if (!props) return NULL;

    /* Try X11 first - window is a numeric ID, cast to pointer for bgfx */
    Sint64 x11_window = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (x11_window) return (void*)(uintptr_t)x11_window;

    /* Try Wayland */
    void* wl_surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    if (wl_surface) return wl_surface;
#endif

    return NULL;
}

void* pal_window_get_native_display(void) {
#ifdef __linux__
    if (!s_window) return NULL;

    SDL_PropertiesID props = SDL_GetWindowProperties(s_window);
    if (!props) return NULL;

    /* Try X11 display */
    void* x11_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    if (x11_display) return x11_display;

    /* Try Wayland display */
    void* wl_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
    if (wl_display) return wl_display;
#endif

    return NULL;
}

void pal_window_shutdown(void) {
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    if (s_initialized && !s_headless) {
        SDL_Quit();
    }
    s_initialized = 0;
}

int pal_window_is_headless(void) {
    return s_headless;
}

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
