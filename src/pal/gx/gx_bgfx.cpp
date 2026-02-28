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
#include <unistd.h>
#include <string.h>

extern "C" {
#include "pal/gx/gx_stub_tracker.h"
#include "pal/gx/gx_state.h"
#include "pal/gx/gx_tev.h"
#include "pal/gx/gx_screenshot.h"
#include "pal/pal_window.h"
#include "pal/pal_verify.h"

/* Provided by pal_gx_stubs.cpp - set to 1 once bgfx is initialized */
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

    /* Initialize SDL3 window first */
    pal_window_init(s_frame_width, s_frame_height, "Twilight Princess");

    bgfx::Init init;

    /* Check for headless mode */
    if (pal_window_is_headless()) {
        init.type = bgfx::RendererType::Noop;
        fprintf(stderr, "{\"gx_bgfx\":\"init\",\"renderer\":\"Noop\",\"headless\":true}\n");
    } else {
        /* Auto-select best renderer for platform */
        init.type = bgfx::RendererType::Count; /* auto */

        /* Set platform data from SDL3 window */
        void* native_handle = pal_window_get_native_handle();
        void* native_display = pal_window_get_native_display();
        if (native_handle) {
            init.platformData.nwh = native_handle;
            init.platformData.ndt = native_display;
            fprintf(stderr, "{\"gx_bgfx\":\"init\",\"renderer\":\"auto\",\"headless\":false,\"nwh\":\"%p\"}\n",
                    native_handle);
        } else {
            fprintf(stderr, "{\"gx_bgfx\":\"init\",\"renderer\":\"auto\",\"headless\":false,\"nwh\":null}\n");
        }
    }

    init.resolution.width = s_frame_width;
    init.resolution.height = s_frame_height;
    init.resolution.reset = BGFX_RESET_VSYNC;

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

    /* Initialize TEV shader system */
    pal_tev_init();

    /* Initialize software framebuffer screenshot system */
    pal_screenshot_init();

    /* Initialize verification/testing system */
    pal_verify_init();

    fprintf(stderr, "{\"gx_bgfx\":\"ready\",\"width\":%u,\"height\":%u}\n",
            s_frame_width, s_frame_height);
    return 1;
}

void pal_gx_bgfx_shutdown(void) {
    if (s_bgfx_initialized) {
        pal_tev_shutdown();
        bgfx::shutdown();
        s_bgfx_initialized = 0;
        gx_shim_active = 0;
    }
    pal_window_shutdown();
}

void pal_gx_begin_frame(void) {
    if (!s_bgfx_initialized)
        return;

    /* Reset per-frame stub tracking for honest milestone gating */
    gx_stub_frame_reset();

    /* Clear software framebuffer so each frame is independently verifiable */
    pal_screenshot_clear_fb();

    /* Process SDL3 events (window close, input, etc.) */
    pal_window_poll();

    /* Apply GX clear color from state machine */
    GXColor cc = g_gx_state.clear_color;
    uint32_t clear_rgba = ((uint32_t)cc.r << 24) | ((uint32_t)cc.g << 16) |
                          ((uint32_t)cc.b << 8) | (uint32_t)cc.a;
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        clear_rgba, 1.0f, 0);

    /* Apply GX viewport from state machine */
    uint16_t vp_x = (uint16_t)g_gx_state.vp_left;
    uint16_t vp_y = (uint16_t)g_gx_state.vp_top;
    uint16_t vp_w = (uint16_t)g_gx_state.vp_wd;
    uint16_t vp_h = (uint16_t)g_gx_state.vp_ht;
    if (vp_w == 0) vp_w = (uint16_t)s_frame_width;
    if (vp_h == 0) vp_h = (uint16_t)s_frame_height;
    bgfx::setViewRect(0, vp_x, vp_y, vp_w, vp_h);

    /* Touch view 0 to ensure it's submitted even with no draw calls */
    bgfx::touch(0);

    /* Reset draw statistics for this frame */
    g_gx_state.draw_calls = 0;
    g_gx_state.total_verts = 0;
}

void pal_gx_end_frame(void) {
    if (!s_bgfx_initialized)
        return;

    static uint32_t s_frame_count = 0;
    s_frame_count++;

    /* In windowed mode, request a screenshot after the logo has rendered */
    if (!pal_window_is_headless() && s_frame_count == 30) {
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, "/tmp/logo_render.tga");
        fprintf(stderr, "{\"gx_bgfx\":\"screenshot_requested\",\"frame\":%u}\n", s_frame_count);
    }

    /* Save software framebuffer screenshot after logo has rendered */
    if (s_frame_count == 30 && pal_screenshot_active()) {
        pal_screenshot_save();
    }

    /* Log draw statistics every 60 frames */
    if (s_frame_count % 60 == 0) {
        fprintf(stderr, "{\"gx_bgfx\":\"frame_stats\",\"frame\":%u,\"draw_calls\":%u,\"verts\":%u}\n",
                s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts);
    }

    /* Verification system: report per-frame rendering metrics */
    pal_verify_frame(s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts,
                     gx_frame_stub_count, (u32)gx_stub_frame_is_valid());

    bgfx::frame();

    /* In windowed mode, add a small delay to let the frame display */
    if (!pal_window_is_headless()) {
        usleep(16000); /* ~60 fps */
    }
}

int pal_gx_bgfx_is_active(void) {
    return s_bgfx_initialized;
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
