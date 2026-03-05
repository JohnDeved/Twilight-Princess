/* gx_render.cpp — bgfx rendering backend for GX shim
 *
 * Renders via real OpenGL using Mesa's software rasterizer (llvmpipe) on
 * Xvfb — no GPU hardware needed. Every frame is captured via bgfx's
 * BGFX_RESET_CAPTURE API into the capture buffer (gx_capture.cpp).
 *
 * Falls back to Noop renderer when no display is available.
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
#include "pal/gx/gx_capture.h"

void gx_fifo_reset(void);
#include "pal/pal_window.h"
#include "pal/pal_verify.h"
#include "pal/pal_milestone.h"
#include "pal/pal_error.h"

/* Provided by pal_gx_stubs.cpp */
extern int gx_shim_active;
}

#define GX_DEFAULT_WIDTH  640
#define GX_DEFAULT_HEIGHT 480

static int s_initialized = 0;
static uint32_t s_frame_width = GX_DEFAULT_WIDTH;
static uint32_t s_frame_height = GX_DEFAULT_HEIGHT;
static int s_using_noop = 0;
static int s_fb_capture_enabled = 0;

/**
 * bgfx callback — captures every rendered frame via BGFX_RESET_CAPTURE.
 *
 * captureFrame() is called after each bgfx::frame() with the actual
 * rendered pixel data (BGRA). Forwarded to gx_capture buffer for
 * verification, BMP snapshots, and video generation.
 */
class BgfxCallback : public bgfx::CallbackI {
public:
    virtual ~BgfxCallback() {}

    void fatal(const char* _filePath, uint16_t _line,
               bgfx::Fatal::Enum _code, const char* _str) override {
        fprintf(stderr, "{\"bgfx_fatal\":\"%s\",\"file\":\"%s\",\"line\":%u}\n",
                _str, _filePath, _line);
        /* bgfx expects fatal() to never return.  If we don't abort, bgfx
         * continues with broken state and crashes.  Abort immediately. */
        abort();
    }

    void traceVargs(const char*, uint16_t, const char*, va_list) override {}
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}

    void screenShot(const char* _filePath, uint32_t _width, uint32_t _height,
                    uint32_t _pitch, const void* _data, uint32_t _size,
                    bool _yflip) override {
        (void)_filePath;
        /* Forward screenshot data to capture buffer */
        pal_capture_begin(_width, _height, _pitch, _yflip ? 1 : 0);
        pal_capture_frame(_data, _size);
    }

    void captureBegin(uint32_t _width, uint32_t _height, uint32_t _pitch,
                      bgfx::TextureFormat::Enum _format, bool _yflip) override {
        fprintf(stderr, "{\"capture\":\"begin\",\"width\":%u,\"height\":%u,"
                "\"pitch\":%u,\"format\":%d,\"yflip\":%d}\n",
                _width, _height, _pitch, (int)_format, _yflip ? 1 : 0);
        pal_capture_begin(_width, _height, _pitch, _yflip ? 1 : 0);
    }

    void captureEnd() override {}

    void captureFrame(const void* _data, uint32_t _size) override {
        pal_capture_frame(_data, _size);
    }
};

static BgfxCallback s_callback;

extern "C" {

int pal_render_init(void) {
    if (s_initialized)
        return 1;

    pal_window_init(s_frame_width, s_frame_height, "Twilight Princess");

    bgfx::Init init;
    init.callback = &s_callback;

    int headless = pal_window_is_headless();
    void* native_handle = pal_window_get_native_handle();
    void* native_display = pal_window_get_native_display();

    if (native_handle) {
        /* Real window (physical or Xvfb).
         * Force OpenGL on headless Xvfb — auto-select may try EGL which crashes
         * in Mesa's software renderer (LLVM SIGSEGV before bgfx::init returns). */
        if (headless) {
            init.type = bgfx::RendererType::OpenGL;
        } else {
            init.type = bgfx::RendererType::Count; /* auto */
        }
        init.platformData.nwh = native_handle;
        init.platformData.ndt = native_display;
        s_using_noop = 0;
        fprintf(stderr, "{\"render\":\"init\",\"renderer\":\"%s\","
                "\"headless\":%s,\"nwh\":\"%p\"}\n",
                headless ? "OpenGL" : "auto",
                headless ? "true" : "false", native_handle);
    } else if (headless) {
        /* No display at all — Noop fallback */
        init.type = bgfx::RendererType::Noop;
        s_using_noop = 1;
        fprintf(stderr, "{\"render\":\"init\",\"renderer\":\"Noop\","
                "\"headless\":true,\"note\":\"no display\"}\n");
    } else {
        init.type = bgfx::RendererType::Count; /* auto */
        s_using_noop = 0;
        fprintf(stderr, "{\"render\":\"init\",\"renderer\":\"auto\"}\n");
    }

    init.resolution.width = s_frame_width;
    init.resolution.height = s_frame_height;
    /* VSYNC only for windowed mode (useless on Xvfb/Noop). */
    init.resolution.reset = 0;
    if (!headless)
        init.resolution.reset |= BGFX_RESET_VSYNC;

    /* Check if capture is needed before bgfx::init so we can set CAPTURE flag */
    {
        const char* verify = getenv("TP_VERIFY");
        const char* ss = getenv("TP_SCREENSHOT");
        if ((verify && verify[0] == '1') || (ss && ss[0] != '\0')) {
            if (!s_using_noop) {
                init.resolution.reset |= BGFX_RESET_CAPTURE;
                s_fb_capture_enabled = 1;
            }
        }
    }

    /* Increase transient buffer pools to handle sustained high-draw frames.
     * Default 6MB TVB exhausts after ~7 frames of 7615 dl_draws × ~80-byte
     * vertex stride.  32MB provides headroom for sustained play-scene rendering
     * without alloc FAIL stalls.  TIB scaled proportionally from 2MB → 8MB. */
    init.limits.transientVbSize = 32u * 1024u * 1024u;  /* 32 MB (was 6 MB) */
    init.limits.transientIbSize =  8u * 1024u * 1024u;  /*  8 MB (was 2 MB) */

    if (!bgfx::init(init)) {
        if (!s_using_noop) {
            fprintf(stderr, "{\"render\":\"opengl_failed\",\"fallback\":\"Noop\"}\n");
            init.type = bgfx::RendererType::Noop;
            s_using_noop = 1;
            s_fb_capture_enabled = 0;
            init.resolution.reset = 0; /* Noop needs neither VSYNC nor CAPTURE */
            if (!bgfx::init(init)) {
                fprintf(stderr, "{\"render\":\"init_failed\"}\n");
                return 0;
            }
        } else {
            fprintf(stderr, "{\"render\":\"init_failed\"}\n");
            return 0;
        }
    }

    fprintf(stderr, "{\"render\":\"active\",\"backend\":\"%s\",\"noop\":%d}\n",
            bgfx::getRendererName(bgfx::getRendererType()), s_using_noop);

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        0x000000ff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, (uint16_t)s_frame_width, (uint16_t)s_frame_height);

    /* GX processes draws sequentially — preserve submission order.
     * Without this, bgfx sorts by render state hash, scrambling the
     * draw order and breaking alpha-blended overlays (e.g. JUTFader). */
    bgfx::setViewMode(0, bgfx::ViewMode::Sequential);

    s_initialized = 1;
    gx_shim_active = 1;

    pal_error_init();
    pal_tev_init();
    pal_capture_init();
    pal_verify_init();

    /* s_fb_capture_enabled was already set before bgfx::init above */

    fprintf(stderr, "{\"render\":\"ready\",\"width\":%u,\"height\":%u,"
            "\"capture\":%d}\n", s_frame_width, s_frame_height, s_fb_capture_enabled);
    return 1;
}

void pal_render_shutdown(void) {
    if (s_initialized) {
        pal_capture_shutdown();
        pal_error_shutdown();
        pal_tev_shutdown();
        bgfx::shutdown();
        s_initialized = 0;
        gx_shim_active = 0;
    }
    pal_window_shutdown();
}

void pal_render_begin_frame(void) {
    if (!s_initialized)
        return;

    gx_stub_frame_reset();
    gx_fifo_reset();
    pal_window_poll();

    GXColor cc = g_gx_state.clear_color;
    cc.a = 255;
    uint32_t clear_rgba = ((uint32_t)cc.r << 24) | ((uint32_t)cc.g << 16) |
                          ((uint32_t)cc.b << 8) | (uint32_t)cc.a;

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        clear_rgba, 1.0f, 0);

    uint16_t vp_x = (uint16_t)g_gx_state.vp_left;
    uint16_t vp_y = (uint16_t)g_gx_state.vp_top;
    uint16_t vp_w = (uint16_t)g_gx_state.vp_wd;
    uint16_t vp_h = (uint16_t)g_gx_state.vp_ht;
    if (vp_w == 0) vp_w = (uint16_t)s_frame_width;
    if (vp_h == 0) vp_h = (uint16_t)s_frame_height;

    bgfx::setViewRect(0, vp_x, vp_y, vp_w, vp_h);

    /* Explicitly set view and projection to identity so that our MVP
     * (set via bgfx::setTransform) is the only transform applied.
     * bgfx's vertex shader computes: pos = u_viewProj * u_model * a_position */
    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bgfx::setViewTransform(0, identity, identity);

    /* View 1: fade overlay — renders after all view 0 content.
     * No clear (preserves view 0 framebuffer), same rect. */
    bgfx::setViewRect(1, vp_x, vp_y, vp_w, vp_h);
    bgfx::setViewClear(1, BGFX_CLEAR_NONE);
    bgfx::setViewMode(1, bgfx::ViewMode::Sequential);
    bgfx::setViewTransform(1, identity, identity);
    bgfx::touch(1);

    bgfx::touch(0);

    /* Enable bgfx debug text overlay for frame diagnostics */
    bgfx::setDebug(BGFX_DEBUG_TEXT);
    bgfx::dbgTextClear();

    g_gx_state.draw_calls = 0;
    g_gx_state.total_verts = 0;
}

void pal_render_end_frame(void) {
    if (!s_initialized)
        return;

    static uint32_t s_frame_count = 0;
    s_frame_count++;
    pal_error_set_frame(s_frame_count);

    /* Debug text overlay — visible on the live window via bgfx debug text */
    bgfx::dbgTextPrintf(1, 1, 0x0f,
        "TP-PC Frame %u  DC:%u  V:%u  Stubs:%u",
        s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts,
        gx_frame_stub_count);
    bgfx::dbgTextPrintf(1, 2, 0x0f,
        "Renderer: %s  Capture: %s",
        bgfx::getRendererName(bgfx::getRendererType()),
        s_fb_capture_enabled ? "ON" : "OFF");

    /* Pass debug info to capture metadata file — external tooling (ffmpeg/Python)
     * burns it into BMPs and MP4. bgfx captures the backbuffer BEFORE rendering
     * its debug text overlay, so we use a metadata approach instead of pixel burn-in. */
    if (s_fb_capture_enabled) {
        char line0[128], line1[128];
        snprintf(line0, sizeof(line0),
            "TP-PC Frame %u  DC:%u  V:%u  Stubs:%u",
            s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts,
            gx_frame_stub_count);
        snprintf(line1, sizeof(line1),
            "Renderer: %s  Capture: ON",
            bgfx::getRendererName(bgfx::getRendererType()));
        pal_capture_set_debug_info(line0, line1);
    }

    if (!pal_milestone_was_reached(MILESTONE_RENDER_FRAME)
        && gx_stub_frame_is_valid()) {
        pal_milestone("RENDER_FRAME", MILESTONE_RENDER_FRAME,
                      "first stub-free frame with valid draw calls");
    }

    if (s_frame_count % 60 == 0) {
        fprintf(stderr, "{\"render\":\"frame_stats\",\"frame\":%u,"
                "\"draw_calls\":%u,\"verts\":%u}\n",
                s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts);
        fprintf(stderr, "{\"zblend_prop\":{\"frame\":%u,\"gx_set_z\":%u,\"gx_set_blend\":%u,"
                "\"submit_depth\":%u,\"submit_blend\":%u}}\n",
                s_frame_count, gx_frame_zmode_calls, gx_frame_blendmode_calls,
                gx_frame_submit_with_depth_state, gx_frame_submit_with_blend_state);
    }

    /* Submit frame — triggers rendering and capture.
     *
     * bgfx uses multi-threaded rendering by default (BGFX_CONFIG_MULTITHREADED=1).
     * The capture at frame N contains frame N-1's rendering (1-frame pipeline
     * delay). This is acceptable for regression testing — we compare frame
     * content rather than frame number. */
    bgfx::frame();

    /* Log bgfx stats for title scene frames to see actual GPU draw counts */
    if (s_frame_count >= 120 && s_frame_count <= 125) {
        const bgfx::Stats* stats = bgfx::getStats();
        if (stats) {
            fprintf(stderr, "{\"bgfx_stats\":{\"frame\":%u,"
                    "\"numDraw\":%u,\"numCompute\":%u,"
                    "\"maxGpuLatency\":%d,"
                    "\"numViews\":%u,"
                    "\"width\":%d,\"height\":%d}}\n",
                    s_frame_count,
                    stats->numDraw, stats->numCompute,
                    stats->maxGpuLatency,
                    stats->numViews,
                    stats->width, stats->height);
        }
    }

    /* Verify after bgfx::frame() so capture buffer is up-to-date */
    pal_verify_frame(s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts,
                     gx_frame_stub_count, (u32)gx_stub_frame_is_valid());

    /* Save screenshot at frame 120 (title scene with 85+ draw calls) */
    if (s_frame_count == 120 && pal_capture_screenshot_active()) {
        pal_capture_save();
    }

    /* TP_FRAME_DELAY_MS=N: sleep N milliseconds after bgfx::frame() starting at
     * TP_FRAME_DELAY_START frame (default 128).
     * Used by Phase 3 CI to throttle CPU frame submission so Mesa softpipe /
     * llvmpipe can finish rasterising each heavy 3-D frame (7587 draws) before
     * the next frame's TVB allocation begins.  Set to 0 (default) for normal
     * operation; a value of ~30000-60000 is appropriate for softpipe on CI. */
    {
        static int s_frame_delay_ms = -1;
        static uint32_t s_frame_delay_start = 128; /* default: 3D geometry starts at 128 */
        if (s_frame_delay_ms < 0) {
            const char* ev = getenv("TP_FRAME_DELAY_MS");
            s_frame_delay_ms = ev ? atoi(ev) : 0;
            const char* ev_start = getenv("TP_FRAME_DELAY_START");
            if (ev_start) s_frame_delay_start = (uint32_t)atoi(ev_start);
            if (s_frame_delay_ms > 0)
                fprintf(stderr, "{\"render\":\"frame_delay_ms\":%d,"
                        "\"start_frame\":%u}\n",
                        s_frame_delay_ms, s_frame_delay_start);
        }
        if (s_frame_delay_ms > 0 && s_frame_count >= s_frame_delay_start)
            usleep((useconds_t)s_frame_delay_ms * 1000u);
    }

    if (!pal_window_is_headless()) {
        usleep(16000);
    }
}

int pal_render_is_active(void) {
    return s_initialized;
}

int pal_render_is_noop(void) {
    return s_using_noop;
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
