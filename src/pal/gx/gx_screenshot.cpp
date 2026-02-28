/**
 * gx_screenshot.cpp - Software framebuffer screenshot capture
 *
 * CPU-side 640x480 RGBA framebuffer that captures textured quad draws
 * from the GX state machine. Produces a BMP screenshot without GPU.
 *
 * Activated by TP_SCREENSHOT=<path.bmp> environment variable.
 * Captures the first frame that has valid draw calls with textures.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

extern "C" {
#include "pal/gx/gx_screenshot.h"
#include "pal/gx/gx_state.h"
#include "pal/gx/gx_texture.h"
#include "revolution/gx/GXEnum.h"
}

#define FB_W 640
#define FB_H 480

static uint8_t* s_fb = NULL;
static const char* s_screenshot_path = NULL;
static int s_active = 0;      /* TP_SCREENSHOT mode */
static int s_saved = 0;
static int s_has_draws = 0;    /* tracks if any textured draws hit the framebuffer */
static int s_fb_allocated = 0; /* framebuffer exists (screenshot or verify mode) */

extern "C" {

void pal_screenshot_init(void) {
    const char* path = getenv("TP_SCREENSHOT");
    const char* verify = getenv("TP_VERIFY");

    /* Allocate framebuffer if either screenshot or verify mode is active */
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
        s_has_draws = 0;
        fprintf(stderr, "{\"screenshot\":\"init\",\"path\":\"%s\"}\n", path);
    }
}

int pal_screenshot_active(void) {
    return s_active && !s_saved;
}

/**
 * Calculate per-vertex stride from active vertex descriptors.
 * Returns offsets for position, color, and texcoord within each vertex.
 */
static uint32_t calc_layout(uint32_t* pos_off, uint32_t* clr_off, uint32_t* tc_off,
                            int* has_pos, int* has_clr, int* has_tc) {
    uint32_t offset = 0;
    *has_pos = *has_clr = *has_tc = 0;

    if (g_gx_state.vtx_desc[GX_VA_PNMTXIDX].type != GX_NONE) {
        offset += 1; /* u8 matrix index */
    }

    if (g_gx_state.vtx_desc[GX_VA_POS].type != GX_NONE) {
        *pos_off = offset;
        *has_pos = 1;
        offset += 12; /* 3 floats */
    }

    if (g_gx_state.vtx_desc[GX_VA_NRM].type != GX_NONE) {
        offset += 12; /* 3 floats */
    }

    if (g_gx_state.vtx_desc[GX_VA_CLR0].type != GX_NONE) {
        *clr_off = offset;
        *has_clr = 1;
        offset += 4; /* RGBA u32 */
    }

    if (g_gx_state.vtx_desc[GX_VA_CLR1].type != GX_NONE) {
        offset += 4;
    }

    if (g_gx_state.vtx_desc[GX_VA_TEX0].type != GX_NONE) {
        *tc_off = offset;
        *has_tc = 1;
        offset += 8; /* 2 floats */
    }

    /* Skip remaining tex coords for stride calculation */
    for (int i = 1; i < 8; i++) {
        if (g_gx_state.vtx_desc[GX_VA_TEX0 + i].type != GX_NONE) {
            offset += 8;
        }
    }

    return offset;
}

void pal_screenshot_blit(void) {
    /* Blit whenever framebuffer is allocated (screenshot or verify mode).
     * In screenshot mode, stop after save. In verify mode, blit every frame. */
    if (!s_fb)
        return;
    if (s_active && s_saved)
        return;  /* screenshot already captured */

    const GXDrawState* ds = &g_gx_state.draw;
    if (ds->verts_written < 4 || ds->vtx_data_pos == 0)
        return;

    /* Calculate vertex layout */
    uint32_t pos_off = 0, clr_off = 0, tc_off = 0;
    int has_pos = 0, has_clr = 0, has_tc = 0;
    uint32_t stride = calc_layout(&pos_off, &clr_off, &tc_off, &has_pos, &has_clr, &has_tc);
    if (!has_pos || stride == 0)
        return;

    /* Verify vertex data is consistent */
    if (stride * 4 > ds->vtx_data_pos)
        return;

    /* Read first 4 vertices (quad corners) */
    float pos[4][3];
    uint8_t clr[4][4];
    float tc[4][2];

    for (int v = 0; v < 4; v++) {
        const uint8_t* base = ds->vtx_data + v * stride;
        memcpy(pos[v], base + pos_off, 12);

        if (has_clr) {
            /* Color is stored as u32 RGBA (packed by GXColor1u32) */
            uint32_t c;
            memcpy(&c, base + clr_off, 4);
            clr[v][0] = (c >> 24) & 0xFF; /* R */
            clr[v][1] = (c >> 16) & 0xFF; /* G */
            clr[v][2] = (c >> 8) & 0xFF;  /* B */
            clr[v][3] = c & 0xFF;         /* A */
        } else {
            clr[v][0] = clr[v][1] = clr[v][2] = clr[v][3] = 255;
        }

        if (has_tc) {
            memcpy(tc[v], base + tc_off, 8);
        } else {
            tc[v][0] = (v == 1 || v == 2) ? 1.0f : 0.0f;
            tc[v][1] = (v == 2 || v == 3) ? 1.0f : 0.0f;
        }
    }

    /* Calculate screen-space bounding box */
    float x0 = pos[0][0], y0 = pos[0][1];
    float x1 = pos[0][0], y1 = pos[0][1];
    for (int v = 1; v < 4; v++) {
        if (pos[v][0] < x0) x0 = pos[v][0];
        if (pos[v][0] > x1) x1 = pos[v][0];
        if (pos[v][1] < y0) y0 = pos[v][1];
        if (pos[v][1] > y1) y1 = pos[v][1];
    }

    /* Clamp to framebuffer */
    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > FB_W) ix1 = FB_W;
    if (iy1 > FB_H) iy1 = FB_H;
    if (ix1 <= ix0 || iy1 <= iy0)
        return;

    float qw = x1 - x0, qh = y1 - y0;
    if (qw <= 0.0f || qh <= 0.0f)
        return;

    /* Get texture for this draw (from TEV stage 0) */
    const GXTevStage* s0 = &g_gx_state.tev_stages[0];
    uint8_t* tex_rgba = NULL;
    uint32_t tw = 0, th = 0;

    if (s0->tex_map < GX_MAX_TEXMAP && g_gx_state.tex_bindings[s0->tex_map].valid) {
        const GXTexBinding* tb = &g_gx_state.tex_bindings[s0->tex_map];
        tw = tb->width;
        th = tb->height;
        if (tw > 0 && th > 0 && tb->image_ptr) {
            tex_rgba = (uint8_t*)malloc(tw * th * 4);
            if (tex_rgba) {
                u32 decoded = pal_gx_decode_texture(tb->image_ptr, tex_rgba,
                                                     (u16)tw, (u16)th, tb->format, NULL, 0);
                if (decoded == 0) {
                    free(tex_rgba);
                    tex_rgba = NULL;
                }
            }
        }
    }

    /* Blit to software framebuffer */
    for (int sy = iy0; sy < iy1; sy++) {
        float v = (float)(sy - iy0) / qh;
        int ty = (int)(v * (float)th);
        if (ty >= (int)th) ty = (int)th - 1;
        if (ty < 0) ty = 0;

        for (int sx = ix0; sx < ix1; sx++) {
            float u = (float)(sx - ix0) / qw;
            int tx = (int)(u * (float)tw);
            if (tx >= (int)tw) tx = (int)tw - 1;
            if (tx < 0) tx = 0;

            uint8_t r, g, b, a;
            if (tex_rgba && tw > 0 && th > 0) {
                int tidx = (ty * (int)tw + tx) * 4;
                r = tex_rgba[tidx + 0];
                g = tex_rgba[tidx + 1];
                b = tex_rgba[tidx + 2];
                a = tex_rgba[tidx + 3];
            } else {
                /* No texture — use vertex color */
                r = g = b = a = 255;
            }

            /* Modulate with vertex color (MODULATE TEV mode) */
            uint8_t cr = clr[0][0], cg = clr[0][1], cb = clr[0][2], ca = clr[0][3];
            r = (uint8_t)((r * cr) / 255);
            g = (uint8_t)((g * cg) / 255);
            b = (uint8_t)((b * cb) / 255);
            a = (uint8_t)((a * ca) / 255);

            /* Alpha blend with framebuffer */
            int fb_idx = (sy * FB_W + sx) * 4;
            uint8_t fb_r = s_fb[fb_idx + 0];
            uint8_t fb_g = s_fb[fb_idx + 1];
            uint8_t fb_b = s_fb[fb_idx + 2];

            s_fb[fb_idx + 0] = (uint8_t)((r * a + fb_r * (255 - a)) / 255);
            s_fb[fb_idx + 1] = (uint8_t)((g * a + fb_g * (255 - a)) / 255);
            s_fb[fb_idx + 2] = (uint8_t)((b * a + fb_b * (255 - a)) / 255);
            s_fb[fb_idx + 3] = 255;
        }
    }

    if (tex_rgba)
        free(tex_rgba);

    s_has_draws = 1;
}

void pal_screenshot_save(void) {
    if (!s_active || s_saved || !s_fb || !s_screenshot_path)
        return;
    if (!s_has_draws)
        return;

    /* Write BMP file */
    FILE* f = fopen(s_screenshot_path, "wb");
    if (!f) {
        fprintf(stderr, "{\"screenshot\":\"write_failed\",\"path\":\"%s\"}\n", s_screenshot_path);
        return;
    }

    /* BMP row size must be aligned to 4 bytes */
    uint32_t row_bytes = FB_W * 3;
    uint32_t row_padding = (4 - (row_bytes % 4)) % 4;
    uint32_t pixel_data_size = (row_bytes + row_padding) * FB_H;
    uint32_t file_size = 54 + pixel_data_size;

    /* BMP file header (14 bytes) */
    uint8_t bmp_header[54];
    memset(bmp_header, 0, 54);
    bmp_header[0] = 'B';
    bmp_header[1] = 'M';
    bmp_header[2] = (uint8_t)(file_size & 0xFF);
    bmp_header[3] = (uint8_t)((file_size >> 8) & 0xFF);
    bmp_header[4] = (uint8_t)((file_size >> 16) & 0xFF);
    bmp_header[5] = (uint8_t)((file_size >> 24) & 0xFF);
    bmp_header[10] = 54; /* pixel data offset */

    /* DIB header (40 bytes) */
    bmp_header[14] = 40; /* DIB header size */
    bmp_header[18] = (uint8_t)(FB_W & 0xFF);
    bmp_header[19] = (uint8_t)((FB_W >> 8) & 0xFF);
    bmp_header[22] = (uint8_t)(FB_H & 0xFF);
    bmp_header[23] = (uint8_t)((FB_H >> 8) & 0xFF);
    bmp_header[26] = 1;  /* color planes */
    bmp_header[28] = 24; /* bits per pixel */
    /* Compression = 0 (BI_RGB), image size can be 0 for BI_RGB */

    fwrite(bmp_header, 1, 54, f);

    /* Pixel data — BMP is bottom-up, BGR order */
    uint8_t pad[4] = {0, 0, 0, 0};
    for (int y = FB_H - 1; y >= 0; y--) {
        for (int x = 0; x < FB_W; x++) {
            int idx = (y * FB_W + x) * 4;
            uint8_t bgr[3];
            bgr[0] = s_fb[idx + 2]; /* B */
            bgr[1] = s_fb[idx + 1]; /* G */
            bgr[2] = s_fb[idx + 0]; /* R */
            fwrite(bgr, 1, 3, f);
        }
        if (row_padding > 0) {
            fwrite(pad, 1, row_padding, f);
        }
    }

    fclose(f);
    s_saved = 1;
    fprintf(stderr, "{\"screenshot\":\"saved\",\"path\":\"%s\",\"width\":%d,\"height\":%d}\n",
            s_screenshot_path, FB_W, FB_H);
}

uint8_t* pal_screenshot_get_fb(void) {
    return s_fb;
}

int pal_screenshot_get_fb_width(void) {
    return FB_W;
}

int pal_screenshot_get_fb_height(void) {
    return FB_H;
}

void pal_screenshot_clear_fb(void) {
    if (s_fb && s_fb_allocated) {
        memset(s_fb, 0, FB_W * FB_H * 4);
        s_has_draws = 0;
    }
}

int pal_screenshot_has_draws(void) {
    return s_has_draws;
}

uint32_t pal_screenshot_hash_fb(void) {
    if (!s_fb || !s_fb_allocated)
        return 0;

    /* Table-based CRC32 using the standard polynomial (0xEDB88320, reflected).
     * Much faster than per-bit loop — processes one byte per table lookup. */
    static uint32_t crc_table[256];
    static int table_init = 0;
    if (!table_init) {
        uint32_t n, k;
        for (n = 0; n < 256; n++) {
            uint32_t c = n;
            for (k = 0; k < 8; k++) {
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
    uint32_t i;
    for (i = 0; i < total; i++) {
        crc = crc_table[(crc ^ s_fb[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
