/**
 * gx_capture.cpp - bgfx frame capture buffer + raw video recording
 *
 * Receives rendered frame data from bgfx's capture API (BGFX_RESET_CAPTURE).
 * The actual rendering is done by bgfx via real OpenGL (Mesa software
 * rasterizer on Xvfb in CI). No GPU hardware needed.
 *
 * Stores captured pixel data for:
 * - Per-frame BMP saving to verify_output/ (every frame captured)
 * - Raw RGBA video file for ffmpeg conversion to MP4
 * - CRC32 hashing (regression detection)
 * - Pixel analysis (pal_verify integration)
 *
 * Activated by TP_VERIFY=1 or TP_SCREENSHOT=<path>.
 * Raw video written to verify_output/raw_frames.bin for ffmpeg:
 *   ffmpeg -f rawvideo -pixel_format rgba -video_size 640x480 \
 *          -framerate 30 -i raw_frames.bin -c:v libx264 \
 *          -pix_fmt yuv420p render_output.mp4
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* For direct GL readback fallback */
#ifdef __linux__
#include <GL/gl.h>
#endif

extern "C" {
#include "pal/gx/gx_capture.h"
}

#define FB_W 640
#define FB_H 480

static uint8_t* s_fb = NULL;
static const char* s_screenshot_path = NULL;
static int s_active = 0;       /* TP_SCREENSHOT mode */
static int s_saved = 0;
static int s_has_data = 0;     /* bgfx has delivered frame data */
static int s_fb_allocated = 0;

/* Per-frame recording */
static FILE* s_raw_video = NULL;     /* raw RGBA video file */
static int s_record_frames = 0;     /* record every frame */
static uint32_t s_frame_number = 0; /* current frame counter */
static int s_bmp_interval = 30;     /* save BMP every N frames */
static char s_capture_dir[300];     /* output directory (from TP_VERIFY_DIR) */

/* Debug text overlay — stored per-frame, written to metadata file */
static char s_debug_line0[128];
static char s_debug_line1[128];

/* Capture format from bgfx::captureBegin */
static uint32_t s_cap_width = 0;
static uint32_t s_cap_height = 0;
static uint32_t s_cap_pitch = 0;
static int s_cap_yflip = 0;

static void write_bmp(const char* path, const uint8_t* rgba, int w, int h);

/* Metadata file for debug text overlay — written per-frame so that
 * external tooling (ffmpeg drawtext / Python) can burn it into BMPs
 * and MP4 without needing a bitmap font in C. */
static FILE* s_metadata_file = NULL;

extern "C" {

void pal_capture_init(void) {
    const char* path = getenv("TP_SCREENSHOT");
    const char* verify = getenv("TP_VERIFY");

    int need_fb = 0;
    if (path && path[0] != '\0') need_fb = 1;
    if (verify && verify[0] == '1') need_fb = 1;

    if (need_fb) {
        s_fb = (uint8_t*)calloc(FB_W * FB_H * 4, 1);
        if (!s_fb)
            return;
        s_fb_allocated = 1;
    }

    if (path && path[0] != '\0') {
        s_screenshot_path = path;
        s_active = 1;
        s_saved = 0;
        s_has_data = 0;
        fprintf(stderr, "{\"capture\":\"init\",\"path\":\"%s\"}\n", path);
    }

    /* Enable per-frame recording when TP_VERIFY=1 */
    if (verify && verify[0] == '1') {
        const char* dir = getenv("TP_VERIFY_DIR");
        if (dir && dir[0] != '\0')
            snprintf(s_capture_dir, sizeof(s_capture_dir), "%s", dir);
        else
            snprintf(s_capture_dir, sizeof(s_capture_dir), "verify_output");

        s_record_frames = 1;
        mkdir(s_capture_dir, 0755);

        /* Open raw video file for ffmpeg conversion */
        char raw_path[300];
        snprintf(raw_path, sizeof(raw_path), "%s/raw_frames.bin", s_capture_dir);
        s_raw_video = fopen(raw_path, "wb");
        if (s_raw_video) {
            fprintf(stderr, "{\"capture\":\"recording\",\"output\":\"%s/\"}\n", s_capture_dir);
        }

        /* Open metadata file for debug text (burned in by external tooling) */
        char meta_path[300];
        snprintf(meta_path, sizeof(meta_path), "%s/frame_metadata.txt", s_capture_dir);
        s_metadata_file = fopen(meta_path, "w");
        if (s_metadata_file) {
            fprintf(s_metadata_file, "# frame_number|line0|line1\n");
        }

        /* BMP save interval (configurable via TP_BMP_INTERVAL, default 30) */
        const char* interval = getenv("TP_BMP_INTERVAL");
        if (interval) s_bmp_interval = atoi(interval);
        if (s_bmp_interval < 1) s_bmp_interval = 1;
    }
}

void pal_capture_begin(uint32_t width, uint32_t height, uint32_t pitch, int yflip) {
    /* Only log when dimensions change */
    if (s_cap_width != width || s_cap_height != height) {
        fprintf(stderr, "{\"capture\":\"begin\",\"width\":%u,\"height\":%u,"
                "\"pitch\":%u,\"yflip\":%d}\n", width, height, pitch, yflip);
    }
    s_cap_width = width;
    s_cap_height = height;
    s_cap_pitch = pitch;
    s_cap_yflip = yflip;
}

void pal_capture_set_debug_info(const char* line0, const char* line1) {
    s_debug_line0[0] = '\0';
    s_debug_line1[0] = '\0';
    if (line0) { strncpy(s_debug_line0, line0, 127); s_debug_line0[127] = '\0'; }
    if (line1) { strncpy(s_debug_line1, line1, 127); s_debug_line1[127] = '\0'; }

    /* Write to metadata file for external tooling to burn in.
     * Frame number is +1 because s_frame_number increments later in
     * pal_capture_frame(), and BMP filenames use the post-increment value. */
    if (s_metadata_file) {
        fprintf(s_metadata_file, "%u|%s|%s\n",
                s_frame_number + 1,
                s_debug_line0, s_debug_line1);
        fflush(s_metadata_file);
    }
}

void pal_capture_frame(const void* data, uint32_t size) {
    if (!s_fb || !s_fb_allocated || !data)
        return;

    uint32_t w = s_cap_width;
    uint32_t h = s_cap_height;
    uint32_t pitch = s_cap_pitch;
    if (w == 0 || h == 0) {
        w = FB_W;
        h = FB_H;
        pitch = w * 4;
    }
    uint32_t copy_w = (w < (uint32_t)FB_W) ? w : (uint32_t)FB_W;
    uint32_t copy_h = (h < (uint32_t)FB_H) ? h : (uint32_t)FB_H;

    /* Validate size covers the rows we'll read */
    uint32_t required = h * pitch;
    if (size > 0 && size < required)
        return;

    uint32_t nonblack_rgb = 0;
    for (uint32_t y = 0; y < copy_h; y++) {
        uint32_t src_y = s_cap_yflip ? (h - 1 - y) : y;
        const uint8_t* src_row = (const uint8_t*)data + src_y * pitch;
        uint8_t* dst_row = s_fb + y * FB_W * 4;

        for (uint32_t x = 0; x < copy_w; x++) {
            /* BGRA → RGBA */
            uint8_t b = src_row[x * 4 + 0];
            uint8_t g = src_row[x * 4 + 1];
            uint8_t r = src_row[x * 4 + 2];
            uint8_t a = src_row[x * 4 + 3];
            dst_row[x * 4 + 0] = r;
            dst_row[x * 4 + 1] = g;
            dst_row[x * 4 + 2] = b;
            dst_row[x * 4 + 3] = a;
            if (r > 0 || g > 0 || b > 0)
                nonblack_rgb++;
        }
    }

    s_has_data = 1;
    s_frame_number++;

    /* Log capture diagnostics for every 10th frame and key frames.
     * This tracks whether bgfx is sending non-black pixel data. */
    if (s_frame_number <= 5 || s_frame_number % 10 == 0 ||
        (s_frame_number >= 10 && s_frame_number <= 20) ||
        (s_frame_number >= 128 && s_frame_number <= 135)) {
        /* Find the Y range of nonblack pixels for spatial debugging */
        uint32_t first_nb_y = copy_h, last_nb_y = 0;
        if (nonblack_rgb > 0) {
            for (uint32_t y = 0; y < copy_h; y++) {
                const uint8_t* row = s_fb + y * FB_W * 4;
                for (uint32_t x = 0; x < copy_w; x++) {
                    if (row[x*4+0] > 0 || row[x*4+1] > 0 || row[x*4+2] > 0) {
                        if (y < first_nb_y) first_nb_y = y;
                        if (y > last_nb_y) last_nb_y = y;
                        break; /* found nonblack in this row */
                    }
                }
            }
        }
        /* Sample a few pixels from different positions for debugging */
        uint8_t p0[4] = {0}, pm[4] = {0}, pe[4] = {0};
        if (s_fb) {
            /* Row 0, col 0 */
            memcpy(p0, s_fb, 4);
            /* Row 240, col 320 (center) */
            memcpy(pm, s_fb + (240 * FB_W + 320) * 4, 4);
            /* Row 460, col 320 (near bottom, in the 456-480 gap) */
            if (copy_h > 460)
                memcpy(pe, s_fb + (460 * FB_W + 320) * 4, 4);
        }
        fprintf(stderr, "{\"capture_diag\":{\"frame\":%u,\"size\":%u,"
                "\"nonblack_rgb\":%u,\"total_px\":%u,\"pct\":%.1f,"
                "\"nb_y_range\":[%u,%u],"
                "\"px_0_0\":[%u,%u,%u,%u],"
                "\"px_center\":[%u,%u,%u,%u],"
                "\"px_bottom\":[%u,%u,%u,%u]}}\n",
                s_frame_number, size, nonblack_rgb, copy_w * copy_h,
                copy_w * copy_h > 0 ? 100.0f * nonblack_rgb / (copy_w * copy_h) : 0.0f,
                first_nb_y, last_nb_y,
                p0[0], p0[1], p0[2], p0[3],
                pm[0], pm[1], pm[2], pm[3],
                pe[0], pe[1], pe[2], pe[3]);
    }

    /* Write every frame to raw video file for MP4 generation */
    if (s_raw_video) {
        fwrite(s_fb, 1, FB_W * FB_H * 4, s_raw_video);
    }

    /* Save periodic BMP snapshots — only on interval-aligned frames.
     * NOTE: do NOT add an s_frame_number==1 exception; see the readback_noop
     * version for the rationale. */
    if (s_record_frames && (s_frame_number % (uint32_t)s_bmp_interval == 0)) {
        char bmp_path[300];
        snprintf(bmp_path, sizeof(bmp_path),
                 "%s/frame_%04u.bmp", s_capture_dir, s_frame_number);
        write_bmp(bmp_path, s_fb, FB_W, FB_H);
    }
}

int pal_capture_screenshot_active(void) {
    return s_active && !s_saved;
}

void pal_capture_save(void) {
    if (!s_active || s_saved || !s_fb || !s_screenshot_path)
        return;
    if (!s_has_data)
        return;

    write_bmp(s_screenshot_path, s_fb, FB_W, FB_H);
    s_saved = 1;
    fprintf(stderr, "{\"capture\":\"saved\",\"path\":\"%s\"}\n", s_screenshot_path);
}

void pal_capture_readback_gl(uint32_t width, uint32_t height) {
#ifdef __linux__
    if (!s_fb || !s_fb_allocated)
        return;

    uint32_t read_w = (width < (uint32_t)FB_W) ? width : (uint32_t)FB_W;
    uint32_t read_h = (height < (uint32_t)FB_H) ? height : (uint32_t)FB_H;

    /* Read back the OpenGL front buffer directly.
     * After bgfx::frame() swaps buffers, the rendered frame is in GL_FRONT.
     * GL_BACK (default read buffer) is now empty/cleared. */
    static uint8_t* s_gl_buf = NULL;
    if (!s_gl_buf) {
        s_gl_buf = (uint8_t*)calloc(read_w * read_h * 4, 1);
        if (!s_gl_buf) return;
    }

    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, (GLsizei)read_w, (GLsizei)read_h,
                 GL_RGBA, GL_UNSIGNED_BYTE, s_gl_buf);
    glReadBuffer(GL_BACK);

    /* glReadPixels returns bottom-up, flip to top-down */
    for (uint32_t y = 0; y < read_h; y++) {
        uint32_t src_y = read_h - 1 - y;
        const uint8_t* src_row = s_gl_buf + src_y * read_w * 4;
        uint8_t* dst_row = s_fb + y * FB_W * 4;
        memcpy(dst_row, src_row, read_w * 4);
    }

    s_has_data = 1;
    s_frame_number++;

    /* Write every frame to raw video file for MP4 generation */
    if (s_raw_video) {
        fwrite(s_fb, 1, FB_W * FB_H * 4, s_raw_video);
    }

    /* Save periodic BMP snapshots — only on interval-aligned frames.
     * NOTE: do NOT add an s_frame_number==1 exception here; the
     * BMP_INTERVAL=9999 override in quick-test.sh Phase 3 relies on this
     * guard to suppress all periodic saves and keep verify_output_3d/
     * clean (only frame_0129 from pal_verify_capture_frame). */
    if (s_record_frames && (s_frame_number % (uint32_t)s_bmp_interval == 0)) {
        char bmp_path[300];
        snprintf(bmp_path, sizeof(bmp_path),
                 "%s/frame_%04u.bmp", s_capture_dir, s_frame_number);
        write_bmp(bmp_path, s_fb, FB_W, FB_H);
    }
#else
    (void)width; (void)height;
#endif
}

void pal_capture_shutdown(void) {
    if (s_raw_video) {
        fclose(s_raw_video);
        s_raw_video = NULL;
        fprintf(stderr, "{\"capture\":\"video_closed\",\"frames\":%u}\n",
                s_frame_number);
    }
    if (s_metadata_file) {
        fclose(s_metadata_file);
        s_metadata_file = NULL;
    }
}

uint8_t* pal_capture_get_fb(void) {
    return s_fb;
}

int pal_capture_get_fb_width(void) {
    return FB_W;
}

int pal_capture_get_fb_height(void) {
    return FB_H;
}

void pal_capture_clear_fb(void) {
    if (s_fb && s_fb_allocated) {
        memset(s_fb, 0, FB_W * FB_H * 4);
        s_has_data = 0;
    }
}

int pal_capture_has_data(void) {
    return s_has_data;
}

uint32_t pal_capture_get_frame_count(void) {
    return s_frame_number;
}

uint32_t pal_capture_hash_fb(void) {
    if (!s_fb || !s_fb_allocated)
        return 0;

    static uint32_t crc_table[256];
    static int table_init = 0;
    if (!table_init) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (uint32_t k = 0; k < 8; k++) {
                if (c & 1)
                    c = (c >> 1) ^ 0xEDB88320;
                else
                    c >>= 1;
            }
            crc_table[n] = c;
        }
        table_init = 1;
    }

    uint32_t crc = 0xFFFFFFFF;
    uint32_t total = (uint32_t)(FB_W * FB_H * 4);
    for (uint32_t i = 0; i < total; i++) {
        crc = crc_table[(crc ^ s_fb[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

} /* extern "C" */

/* Write RGBA buffer as 24-bit BMP file */
static void write_bmp(const char* path, const uint8_t* rgba, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;

    uint32_t row_bytes = (uint32_t)w * 3;
    uint32_t row_padding = (4 - (row_bytes % 4)) % 4;
    uint32_t pixel_data_size = (row_bytes + row_padding) * (uint32_t)h;
    uint32_t file_size = 54 + pixel_data_size;

    uint8_t hdr[54];
    memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(file_size);
    hdr[3] = (uint8_t)(file_size >> 8);
    hdr[4] = (uint8_t)(file_size >> 16);
    hdr[5] = (uint8_t)(file_size >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)(w); hdr[19] = (uint8_t)(w >> 8);
    hdr[22] = (uint8_t)(h); hdr[23] = (uint8_t)(h >> 8);
    hdr[26] = 1;
    hdr[28] = 24;

    fwrite(hdr, 1, 54, f);

    uint8_t pad[4] = {0, 0, 0, 0};
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 4;
            uint8_t bgr[3];
            bgr[0] = rgba[idx + 2]; /* B */
            bgr[1] = rgba[idx + 1]; /* G */
            bgr[2] = rgba[idx + 0]; /* R */
            fwrite(bgr, 1, 3, f);
        }
        if (row_padding > 0)
            fwrite(pad, 1, row_padding, f);
    }

    fclose(f);
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
