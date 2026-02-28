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

/* Debug text overlay — stored per-frame, burned into captured pixels */
static char s_debug_line0[128];
static char s_debug_line1[128];

/* Capture format from bgfx::captureBegin */
static uint32_t s_cap_width = 0;
static uint32_t s_cap_height = 0;
static uint32_t s_cap_pitch = 0;
static int s_cap_yflip = 0;

static void write_bmp(const char* path, const uint8_t* rgba, int w, int h);

/* Minimal 5x7 bitmap font for debug overlay (ASCII 0x20-0x7A).
 * Each byte = one row, bits 4..0 = columns left-to-right. */
static const uint8_t s_font[][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}, /* # */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
    {0x19,0x19,0x02,0x04,0x08,0x13,0x13}, /* % */
    {0x08,0x14,0x14,0x08,0x15,0x12,0x0D}, /* & */
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* * */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x04,0x08}, /* , */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, /* . */
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, /* / */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, /* 2 */
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x00,0x00,0x04,0x00,0x04,0x00,0x00}, /* : */
    {0x00,0x00,0x04,0x00,0x04,0x04,0x08}, /* ; */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, /* @ */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* [ */
    {0x10,0x10,0x08,0x04,0x02,0x01,0x01}, /* \ */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ] */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
    {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /* a */
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, /* b */
    {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E}, /* c */
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, /* d */
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* e */
    {0x06,0x08,0x1E,0x08,0x08,0x08,0x08}, /* f */
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}, /* g */
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, /* h */
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* i */
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* j */
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* k */
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* l */
    {0x00,0x00,0x1A,0x15,0x15,0x15,0x15}, /* m */
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, /* n */
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* o */
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, /* p */
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01}, /* q */
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* r */
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, /* s */
    {0x08,0x08,0x1E,0x08,0x08,0x09,0x06}, /* t */
    {0x00,0x00,0x11,0x11,0x11,0x11,0x0F}, /* u */
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* v */
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* w */
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* x */
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* y */
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* z */
};

/* Stamp one character at (px,py) in RGBA buffer, 2x scale, green on dark bg */
static void stamp_char(uint8_t* fb, int fw, int fh, int px, int py, char ch) {
    if (ch < 0x20 || ch > 0x7A) ch = '?';
    const uint8_t* g = s_font[ch - 0x20];
    for (int r = 0; r < 7; r++) {
        for (int c = 0; c < 5; c++) {
            if (g[r] & (1 << (4 - c))) {
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        int x = px + c * 2 + sx, y = py + r * 2 + sy;
                        if (x >= 0 && x < fw && y >= 0 && y < fh) {
                            int i = (y * fw + x) * 4;
                            fb[i] = 0; fb[i+1] = 255; fb[i+2] = 0; fb[i+3] = 255;
                        }
                    }
                }
            }
        }
    }
}

/* Burn a text line into the capture buffer with dark background bar */
static void burn_line(uint8_t* fb, int fw, int fh, int px, int py, const char* t) {
    if (!t || !t[0]) return;
    int len = (int)strlen(t);
    /* Dark background bar */
    int bw = len * 12 + 4, bh = 18;
    for (int by = py - 2; by < py + bh && by < fh; by++) {
        if (by < 0) continue;
        for (int bx = px - 2; bx < px + bw && bx < fw; bx++) {
            if (bx < 0) continue;
            int i = (by * fw + bx) * 4;
            fb[i] = fb[i] / 4; fb[i+1] = fb[i+1] / 4; fb[i+2] = fb[i+2] / 4;
            fb[i+3] = 255;
        }
    }
    for (int ci = 0; t[ci]; ci++)
        stamp_char(fb, fw, fh, px + ci * 12, py, t[ci]);
}

/* Burn debug overlay into capture buffer after pixel copy, before disk write */
static void burn_debug_overlay(uint8_t* fb, int fw, int fh) {
    if (s_debug_line0[0]) burn_line(fb, fw, fh, 4, 4, s_debug_line0);
    if (s_debug_line1[0]) burn_line(fb, fw, fh, 4, 24, s_debug_line1);
}

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
        s_record_frames = 1;
        mkdir("verify_output", 0755);

        /* Open raw video file for ffmpeg conversion */
        s_raw_video = fopen("verify_output/raw_frames.bin", "wb");
        if (s_raw_video) {
            fprintf(stderr, "{\"capture\":\"recording\",\"output\":\"verify_output/\"}\n");
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

    for (uint32_t y = 0; y < copy_h; y++) {
        uint32_t src_y = s_cap_yflip ? (h - 1 - y) : y;
        const uint8_t* src_row = (const uint8_t*)data + src_y * pitch;
        uint8_t* dst_row = s_fb + y * FB_W * 4;

        for (uint32_t x = 0; x < copy_w; x++) {
            /* BGRA → RGBA */
            dst_row[x * 4 + 0] = src_row[x * 4 + 2]; /* R */
            dst_row[x * 4 + 1] = src_row[x * 4 + 1]; /* G */
            dst_row[x * 4 + 2] = src_row[x * 4 + 0]; /* B */
            dst_row[x * 4 + 3] = src_row[x * 4 + 3]; /* A */
        }
    }

    s_has_data = 1;
    s_frame_number++;

    /* Burn debug text directly into the pixel buffer before writing to disk */
    burn_debug_overlay(s_fb, FB_W, FB_H);

    /* Write every frame to raw video file for MP4 generation */
    if (s_raw_video) {
        fwrite(s_fb, 1, FB_W * FB_H * 4, s_raw_video);
    }

    /* Save periodic BMP snapshots */
    if (s_record_frames && (s_frame_number % (uint32_t)s_bmp_interval == 0
                            || s_frame_number == 1)) {
        char bmp_path[256];
        snprintf(bmp_path, sizeof(bmp_path),
                 "verify_output/frame_%04u.bmp", s_frame_number);
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

    /* Save periodic BMP snapshots */
    if (s_record_frames && (s_frame_number % (uint32_t)s_bmp_interval == 0
                            || s_frame_number == 1)) {
        char bmp_path[256];
        snprintf(bmp_path, sizeof(bmp_path),
                 "verify_output/frame_%04u.bmp", s_frame_number);
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
