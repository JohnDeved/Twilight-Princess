/**
 * gx_capture.cpp - bgfx frame capture buffer
 *
 * Receives rendered frame data from bgfx's capture API (BGFX_RESET_CAPTURE).
 * The actual rendering is done by bgfx via real OpenGL (Mesa software
 * rasterizer on Xvfb in CI). No GPU hardware needed.
 *
 * Stores captured pixel data for:
 * - BMP file saving (TP_SCREENSHOT)
 * - CRC32 hashing (regression detection)
 * - Pixel analysis (pal_verify integration)
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

/* Capture format from bgfx::captureBegin */
static uint32_t s_cap_width = 0;
static uint32_t s_cap_height = 0;
static uint32_t s_cap_pitch = 0;
static int s_cap_yflip = 0;

extern "C" {

void pal_capture_init(void) {
    const char* path = getenv("TP_SCREENSHOT");
    const char* verify = getenv("TP_VERIFY");

    if ((path && path[0] != '\0') || (verify && verify[0] == '1')) {
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
}

void pal_capture_begin(uint32_t width, uint32_t height, uint32_t pitch, int yflip) {
    s_cap_width = width;
    s_cap_height = height;
    s_cap_pitch = pitch;
    s_cap_yflip = yflip;
    fprintf(stderr, "{\"capture\":\"begin\",\"width\":%u,\"height\":%u,"
            "\"pitch\":%u,\"yflip\":%d}\n", width, height, pitch, yflip);
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

    (void)size;

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
}

int pal_capture_screenshot_active(void) {
    return s_active && !s_saved;
}

void pal_capture_save(void) {
    if (!s_active || s_saved || !s_fb || !s_screenshot_path)
        return;
    if (!s_has_data)
        return;

    FILE* f = fopen(s_screenshot_path, "wb");
    if (!f) {
        fprintf(stderr, "{\"capture\":\"write_failed\",\"path\":\"%s\"}\n",
                s_screenshot_path);
        return;
    }

    uint32_t row_bytes = FB_W * 3;
    uint32_t row_padding = (4 - (row_bytes % 4)) % 4;
    uint32_t pixel_data_size = (row_bytes + row_padding) * FB_H;
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
    hdr[18] = (uint8_t)(FB_W); hdr[19] = (uint8_t)(FB_W >> 8);
    hdr[22] = (uint8_t)(FB_H); hdr[23] = (uint8_t)(FB_H >> 8);
    hdr[26] = 1;
    hdr[28] = 24;

    fwrite(hdr, 1, 54, f);

    uint8_t pad[4] = {0, 0, 0, 0};
    for (int y = FB_H - 1; y >= 0; y--) {
        for (int x = 0; x < FB_W; x++) {
            int idx = (y * FB_W + x) * 4;
            uint8_t bgr[3];
            bgr[0] = s_fb[idx + 2];
            bgr[1] = s_fb[idx + 1];
            bgr[2] = s_fb[idx + 0];
            fwrite(bgr, 1, 3, f);
        }
        if (row_padding > 0)
            fwrite(pad, 1, row_padding, f);
    }

    fclose(f);
    s_saved = 1;
    fprintf(stderr, "{\"capture\":\"saved\",\"path\":\"%s\"}\n", s_screenshot_path);
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

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
