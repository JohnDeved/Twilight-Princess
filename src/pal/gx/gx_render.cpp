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
#include <signal.h>
#include <setjmp.h>

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
/* TP_SYNC_RENDER=1: single-threaded bgfx mode for synchronous frame completion.
 * bgfx::renderFrame() before bgfx::init() disables the internal render thread;
 * the API thread must call bgfx::renderFrame() after each bgfx::frame() to
 * actually execute the GL draw calls.  This ensures Mesa softpipe has fully
 * rasterised a frame before the next submission, eliminating TVB overflow
 * on heavy 3-D frames.  captureFrame() fires inside renderFrame(), so
 * pal_verify_frame() reads the CURRENT frame's pixels (no 1-frame pipeline lag). */
static int s_sync_render = 0;
/* Crash protection for bgfx::renderFrame() — Mesa softpipe can SIGSEGV on
 * heavy geometry or first-frame shader JIT.  We catch the crash and skip
 * rendering for that frame instead of killing the entire test process. */
static sigjmp_buf s_render_jmpbuf;
static int s_render_crash_count = 0;
static void pal_render_crash_handler(int sig) {
    siglongjmp(s_render_jmpbuf, sig);
}

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

    /* TP_SYNC_RENDER=1: activate bgfx single-threaded mode by calling
     * bgfx::renderFrame() exactly once BEFORE bgfx::init().  This signals
     * bgfx not to spawn an internal render thread; the application is
     * responsible for calling bgfx::renderFrame() after each bgfx::frame().
     * Each bgfx::renderFrame() call blocks until all GL commands for that
     * frame are complete, giving Mesa softpipe time to rasterise heavy 3-D
     * frames (7587 draws) without TVB pool exhaustion. */
    {
        const char* ev = getenv("TP_SYNC_RENDER");
        if (ev && ev[0] == '1') {
            bgfx::renderFrame(); /* single-threaded mode activation */
            s_sync_render = 1;
            fprintf(stderr, "{\"render\":\"sync_render_mode\",\"active\":true,"
                    "\"note\":\"bgfx single-threaded; renderFrame() called per frame\"}\n");
        }
    }

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

    /* TP_SYNC_RENDER: bgfx single-threaded mode crashes with Mesa software
     * renderers (softpipe/llvmpipe) on Ubuntu 22.04 CI runners due to a
     * SIGSEGV in the first bgfx::renderFrame() call.  Multithreaded mode
     * (the default) works fine.  If TP_SYNC_RENDER is requested but we
     * detect a software renderer, fall back to multithreaded mode. */
    if (s_sync_render && !s_using_noop) {
        /* Test if renderFrame works with a warmup frame */
        bgfx::frame(); /* submit empty frame */
        struct sigaction sa_new, sa_segv_old, sa_abrt_old;
        memset(&sa_new, 0, sizeof(sa_new));
        sa_new.sa_handler = pal_render_crash_handler;
        sigemptyset(&sa_new.sa_mask);
        sa_new.sa_flags = SA_NODEFER;
        sigaction(SIGSEGV, &sa_new, &sa_segv_old);
        sigaction(SIGABRT, &sa_new, &sa_abrt_old);
        if (sigsetjmp(s_render_jmpbuf, 1) == 0) {
            bgfx::renderFrame(); /* warm-up render */
            fprintf(stderr, "{\"render\":\"sync_render_warmup_ok\"}\n");
        } else {
            /* bgfx::renderFrame() crashed — fall back to multithreaded mode.
             * The OpenGL context may be broken, but bgfx::frame() still works
             * (the bgfx internal render thread takes over). */
            s_sync_render = 0;
            s_render_crash_count++;
            fprintf(stderr, "{\"render\":\"sync_render_disabled\","
                    "\"reason\":\"warmup_crash\","
                    "\"note\":\"falling back to multithreaded bgfx\"}\n");
            fflush(stderr);
        }
        sigaction(SIGSEGV, &sa_segv_old, NULL);
        sigaction(SIGABRT, &sa_abrt_old, NULL);
    }

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
        /* In single-threaded mode, bgfx::shutdown() puts a terminate command in
         * the queue; one more bgfx::renderFrame() is required to process it and
         * tear down the GL context cleanly. */
        if (s_sync_render) {
            bgfx::renderFrame();
        }
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

    /* Always clear to black — GXSetCopyClear (g_gx_state.clear_color) sets the
     * EFB-to-XFB copy clear color used on GCN hardware after GXCopyDisp.  On PC
     * there is no XFB copy step; the game's clearEfb draw calls (JFWDisplay::
     * clearEfb, z_func=GX_ALWAYS) are the mechanism that paints the background.
     * Using clear_color here caused a white-background regression: before the
     * logo scene GXSetCopyClear(white,...) makes clear_color={255,255,255} and
     * the bgfx view would start white instead of black, producing avg_rgb near
     * [242,242,242] at frame_0010 even though clearEfb draws are working. */
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        0x000000ff, 1.0f, 0);

    uint16_t vp_x = (uint16_t)g_gx_state.vp_left;
    uint16_t vp_y = (uint16_t)g_gx_state.vp_top;
    uint16_t vp_w = (uint16_t)g_gx_state.vp_wd;
    uint16_t vp_h = (uint16_t)g_gx_state.vp_ht;
    if (vp_w == 0) vp_w = (uint16_t)s_frame_width;
    if (vp_h == 0) vp_h = (uint16_t)s_frame_height;

    bgfx::setViewRect(0, vp_x, vp_y, vp_w, vp_h);

    /* Log viewport for first few frames to debug clear area vs framebuffer */
    {
        static int s_vp_logged = 0;
        if (s_vp_logged < 5) {
            fprintf(stderr, "{\"viewport\":{\"frame\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                    "\"fb_w\":%d,\"fb_h\":%d}}\n",
                    s_vp_logged, vp_x, vp_y, vp_w, vp_h,
                    s_frame_width, s_frame_height);
            s_vp_logged++;
        }
    }

    /* Explicitly set view and projection to identity so that our MVP
     * (set via bgfx::setTransform) is the only transform applied.
     * bgfx's vertex shader computes: pos = u_viewProj * u_model * a_position */
    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bgfx::setViewTransform(0, identity, identity);

    /* Single-view rendering: all draws go to view 0.
     * Multi-view compositing (views 1+2) was unreliable on Mesa softpipe —
     * view 1 clears weren't visible in captures.  The depth-conflict between
     * pre-centroid PASSCLR draws (depth≈0.950) and post-centroid room draws
     * (depth≈0.9996) is solved by disabling depth test for centroid draws
     * (see pal_tev_flush_draw). */
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
        fflush(stderr);
    }

    /* Submit frame — triggers rendering and capture.
     *
     * bgfx uses multi-threaded rendering by default (BGFX_CONFIG_MULTITHREADED=1).
     * The capture at frame N contains frame N-1's rendering (1-frame pipeline
     * delay). This is acceptable for regression testing — we compare frame
     * content rather than frame number.
     *
     * In TP_SYNC_RENDER=1 mode (single-threaded), bgfx::renderFrame() is called
     * immediately after bgfx::frame().  renderFrame() blocks until ALL GL draw
     * calls for this frame complete, then fires captureFrame() with current
     * frame's pixels — no pipeline delay. */

    /* --- Diagnostic: submit a known-good bright quad to verify bgfx pipeline ---
     * This draws a small white quad in NDC space with identity transform.
     * If the pipeline works, captured frames should have a visible white patch.
     * Remove once 3D rendering is confirmed working. */
    {
        struct { float x, y, z; uint32_t abgr; } dv[4] = {
            { -0.3f, -0.3f, 0.0f, 0xffffffff },
            {  0.3f, -0.3f, 0.0f, 0xffffffff },
            {  0.3f,  0.3f, 0.0f, 0xffffffff },
            { -0.3f,  0.3f, 0.0f, 0xffffffff },
        };
        bgfx::VertexLayout dl;
        dl.begin()
          .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
          .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
          .end();
        bgfx::TransientVertexBuffer dtv;
        bgfx::TransientIndexBuffer dti;
        if (bgfx::getAvailTransientVertexBuffer(4, dl) >= 4 &&
            bgfx::getAvailTransientIndexBuffer(6) >= 6) {
            bgfx::allocTransientVertexBuffer(&dtv, 4, dl);
            bgfx::allocTransientIndexBuffer(&dti, 6);
            memcpy(dtv.data, dv, sizeof(dv));
            uint16_t* di = (uint16_t*)dti.data;
            di[0] = 0; di[1] = 1; di[2] = 2;
            di[3] = 0; di[4] = 2; di[5] = 3;
            float iden[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            bgfx::setTransform(iden);
            bgfx::setVertexBuffer(0, &dtv);
            bgfx::setIndexBuffer(&dti);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
            bgfx::submit(0, {pal_tev_get_program_handle(0)});
        }
    }

    bgfx::frame();

    /* TP_SYNC_RENDER=1: process the submitted frame synchronously.
     * bgfx::renderFrame() executes the OpenGL draw calls and blocks until
     * Mesa softpipe has fully rasterised the frame.  captureFrame() fires
     * inside renderFrame(), so s_fb contains the CURRENT frame's pixels
     * after this call returns — pal_verify_frame() below sees fresh data.
     *
     * Crash protection: Mesa softpipe can SIGSEGV on heavy geometry frames
     * (e.g., 7600+ DL draws at frame 128-129) or during first-frame shader
     * JIT compilation.  We wrap renderFrame() in a signal handler so that a
     * softpipe crash skips rendering for that frame instead of killing the
     * entire test process. */
    if (s_sync_render) {
        struct sigaction sa_new, sa_segv_old, sa_abrt_old;
        memset(&sa_new, 0, sizeof(sa_new));
        sa_new.sa_handler = pal_render_crash_handler;
        sigemptyset(&sa_new.sa_mask);
        sa_new.sa_flags = SA_NODEFER;
        sigaction(SIGSEGV, &sa_new, &sa_segv_old);
        sigaction(SIGABRT, &sa_new, &sa_abrt_old);
        if (sigsetjmp(s_render_jmpbuf, 1) == 0) {
            bgfx::renderFrame();
        } else {
            s_render_crash_count++;
            fprintf(stderr, "{\"sync_render\":\"crash\",\"frame\":%u,\"count\":%d}\n",
                    s_frame_count, s_render_crash_count);
            fflush(stderr);
        }
        sigaction(SIGSEGV, &sa_segv_old, NULL);
        sigaction(SIGABRT, &sa_abrt_old, NULL);
        /* Log sync completion for every frame in the 3D-geometry window
         * (frame 128+) and every 10 frames otherwise — limits log volume
         * while preserving visibility around the heavy-render transition. */
        if (s_frame_count % 10 == 0 || s_frame_count >= 128) {
            fprintf(stderr, "{\"sync_render\":\"frame_complete\",\"frame\":%u,"
                    "\"draw_calls\":%u}\n",
                    s_frame_count, g_gx_state.draw_calls);
        }
    }

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

    /* Per-frame draw_calls log for frames 125-410 to diagnose proc transition.
     * The 3D BG actor is active at frame 129 (~7400 draws); PROC_TITLE takes
     * over at frame ~152.  This range shows when draws drop to zero and when
     * PROC_TITLE/J2D submits visible draws in headless mode.
     * Extended to 410 to cover the Phase 4 capture window (up to frame 400). */
    if (s_frame_count >= 125 && s_frame_count <= 410) {
        fprintf(stderr, "{\"frame_dc\":{\"f\":%u,\"dc\":%u,\"verts\":%u}}\n",
                s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts);
    }

    /* TP_FRAME_DELAY_MS=N: sleep N milliseconds after bgfx::frame() starting at
     * TP_FRAME_DELAY_START frame (default 129), stopping after TP_FRAME_DELAY_END
     * (default = TP_FRAME_DELAY_START, i.e. delay fires exactly once).
     *
     * IMPORTANT: this sleep must fire BEFORE pal_verify_frame() so that Mesa
     * softpipe has fully rasterised the submitted batch before we read s_fb.
     * bgfx is multi-threaded: bgfx::frame(N) queues the accumulated draw commands
     * for the render thread, but pal_verify_frame reads s_fb which is populated by
     * the render thread's captureFrame callback.  Without the sleep here, the
     * capture may still contain the previous frame's data when verify runs.
     *
     * Default start frame = 129: the J3D 3D room draws (7587 calls) are accumulated
     * during game iteration 129 and flushed by bgfx::frame(129).  Sleeping at
     * frame 128 (old default) flushed game iter 128's 2-draw title batch instead.
     *
     * Used by Phase 3 CI to let Mesa softpipe rasterise the 7587-draw 3D frame
     * before the BMP capture.  Set to 0 (default) for normal operation. */
    {
        static int s_frame_delay_ms = -1;
        static uint32_t s_frame_delay_start = 129; /* default: 3D geometry batch ready at 129 */
        static uint32_t s_frame_delay_end = 0;     /* 0 = same as start (fire exactly once) */
        if (s_frame_delay_ms < 0) {
            const char* ev = getenv("TP_FRAME_DELAY_MS");
            s_frame_delay_ms = ev ? atoi(ev) : 0;
            const char* ev_start = getenv("TP_FRAME_DELAY_START");
            if (ev_start) s_frame_delay_start = (uint32_t)atoi(ev_start);
            const char* ev_end = getenv("TP_FRAME_DELAY_END");
            s_frame_delay_end = ev_end ? (uint32_t)atoi(ev_end) : s_frame_delay_start;
            if (s_frame_delay_ms > 0)
                fprintf(stderr, "{\"render\":\"frame_delay_ms\":%d,"
                        "\"start_frame\":%u,\"end_frame\":%u}\n",
                        s_frame_delay_ms, s_frame_delay_start, s_frame_delay_end);
        }
        if (s_frame_delay_ms > 0 && s_frame_count >= s_frame_delay_start
                && s_frame_count <= s_frame_delay_end) {
            fprintf(stderr, "{\"frame_delay\":{\"frame\":%u,\"ms\":%d}}\n",
                    s_frame_count, s_frame_delay_ms);
            usleep((useconds_t)s_frame_delay_ms * 1000u);
        }
    }

    /* Verify after bgfx::frame() (and after any TP_FRAME_DELAY_MS sleep) so the
     * capture buffer contains the just-rendered frame's pixels. */
    pal_verify_frame(s_frame_count, g_gx_state.draw_calls, g_gx_state.total_verts,
                     gx_frame_stub_count, (u32)gx_stub_frame_is_valid());

    /* Save screenshot at frame 120 (title scene with 85+ draw calls) */
    if (s_frame_count == 120 && pal_capture_screenshot_active()) {
        pal_capture_save();
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
