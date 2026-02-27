/* gx_bgfx.cpp — bgfx rendering backend for GX shim (Step 5a)
 *
 * Initializes bgfx with Noop renderer in headless mode (TP_HEADLESS=1)
 * or with a platform-appropriate renderer for windowed mode.
 * Provides pal_gx_begin_frame() / pal_gx_end_frame() for the game loop.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "pal/gx/gx_stub_tracker.h"

/* Provided by pal_gx_stubs.cpp — set to 1 once bgfx is initialized */
extern int gx_shim_active;

/* Default frame dimensions (GameCube EFB resolution) */
#define GX_DEFAULT_WIDTH  640
#define GX_DEFAULT_HEIGHT 480

/* bgfx backend state */
static int s_bgfx_initialized = 0;
static uint32_t s_frame_width = GX_DEFAULT_WIDTH;
static uint32_t s_frame_height = GX_DEFAULT_HEIGHT;

int pal_gx_bgfx_init(void) {
    if (s_bgfx_initialized)
        return 1;

    bgfx::Init init;

    /* Check for headless mode */
    const char* headless = getenv("TP_HEADLESS");
    if (headless && headless[0] == '1') {
        init.type = bgfx::RendererType::Noop;
        fprintf(stderr, "{\"gx_bgfx\":\"init\",\"renderer\":\"Noop\",\"headless\":true}\n");
    } else {
        /* Auto-select best renderer for platform */
        init.type = bgfx::RendererType::Count; /* auto */
        fprintf(stderr, "{\"gx_bgfx\":\"init\",\"renderer\":\"auto\",\"headless\":false}\n");
    }

    init.resolution.width = s_frame_width;
    init.resolution.height = s_frame_height;
    init.resolution.reset = BGFX_RESET_VSYNC;

    /* For headless mode, no platform data needed (Noop renderer).
     * For windowed mode, SDL3 window handle would be set here. */

    if (!bgfx::init(init)) {
        fprintf(stderr, "{\"gx_bgfx\":\"init_failed\"}\n");
        return 0;
    }

    /* Set clear color to black */
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        0x000000ff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, (uint16_t)s_frame_width, (uint16_t)s_frame_height);

    s_bgfx_initialized = 1;
    gx_shim_active = 1;

    fprintf(stderr, "{\"gx_bgfx\":\"ready\",\"width\":%u,\"height\":%u}\n",
            s_frame_width, s_frame_height);
    return 1;
}

void pal_gx_bgfx_shutdown(void) {
    if (s_bgfx_initialized) {
        bgfx::shutdown();
        s_bgfx_initialized = 0;
        gx_shim_active = 0;
    }
}

void pal_gx_begin_frame(void) {
    if (!s_bgfx_initialized)
        return;
    /* Touch view 0 to ensure it's submitted even with no draw calls */
    bgfx::touch(0);
}

void pal_gx_end_frame(void) {
    if (!s_bgfx_initialized)
        return;
    bgfx::frame();
}

int pal_gx_bgfx_is_active(void) {
    return s_bgfx_initialized;
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
