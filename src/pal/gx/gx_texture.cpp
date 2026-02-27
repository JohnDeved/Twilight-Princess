/**
 * gx_texture.cpp - GX texture format decoder implementation (Step 5d)
 *
 * Decodes GX tiled texture formats to linear RGBA8 for bgfx.
 * GX textures use Morton order (Z-curve) within tiles.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <string.h>
#include <stdlib.h>
#include "pal/gx/gx_texture.h"

/* ================================================================ */
/* Helpers                                                          */
/* ================================================================ */

static inline u16 read_be16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}

static inline u32 read_be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Align dimension up to tile boundary */
static inline u16 align_up(u16 val, u16 align) {
    return (u16)((val + align - 1) & ~(align - 1));
}

/* Pack RGBA8 pixel */
static inline void set_pixel(u8* dst, u32 stride, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) {
    u8* p = dst + (y * stride + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

/* RGB565 -> RGBA8 */
static inline void rgb565_to_rgba8(u16 c, u8* r, u8* g, u8* b, u8* a) {
    *r = (u8)(((c >> 11) & 0x1F) * 255 / 31);
    *g = (u8)(((c >> 5)  & 0x3F) * 255 / 63);
    *b = (u8)(((c >> 0)  & 0x1F) * 255 / 31);
    *a = 255;
}

/* RGB5A3 -> RGBA8 */
static inline void rgb5a3_to_rgba8(u16 c, u8* r, u8* g, u8* b, u8* a) {
    if (c & 0x8000) {
        /* RGB555 (opaque) */
        *r = (u8)(((c >> 10) & 0x1F) * 255 / 31);
        *g = (u8)(((c >> 5)  & 0x1F) * 255 / 31);
        *b = (u8)(((c >> 0)  & 0x1F) * 255 / 31);
        *a = 255;
    } else {
        /* ARGB3444 */
        *a = (u8)(((c >> 12) & 0x07) * 255 / 7);
        *r = (u8)(((c >> 8)  & 0x0F) * 255 / 15);
        *g = (u8)(((c >> 4)  & 0x0F) * 255 / 15);
        *b = (u8)(((c >> 0)  & 0x0F) * 255 / 15);
    }
}

/* ================================================================ */
/* I4 - 4-bit intensity, 8x8 tiles                                  */
/* ================================================================ */

static void decode_i4(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 8);
    u16 th = align_up(height, 8);
    u32 si = 0;
    u16 by, bx, y, x;

    for (by = 0; by < th; by += 8) {
        for (bx = 0; bx < tw; bx += 8) {
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x += 2) {
                    u8 val = src[si++];
                    u8 i0 = (u8)((val >> 4) * 17); /* 4-bit to 8-bit: 17 = 255/15 maps [0,15] to [0,255] */
                    u8 i1 = (u8)((val & 0xF) * 17);
                    if (bx + x < width && by + y < height)
                        set_pixel(dst, width, bx + x, by + y, i0, i0, i0, 255);
                    if (bx + x + 1 < width && by + y < height)
                        set_pixel(dst, width, bx + x + 1, by + y, i1, i1, i1, 255);
                }
            }
        }
    }
}

/* ================================================================ */
/* I8 - 8-bit intensity, 8x4 tiles                                  */
/* ================================================================ */

static void decode_i8(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 8);
    u16 th = align_up(height, 4);
    u32 si = 0;
    u16 by, bx, y, x;

    for (by = 0; by < th; by += 4) {
        for (bx = 0; bx < tw; bx += 8) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 8; x++) {
                    u8 i = src[si++];
                    if (bx + x < width && by + y < height)
                        set_pixel(dst, width, bx + x, by + y, i, i, i, 255);
                }
            }
        }
    }
}

/* ================================================================ */
/* IA4 - 4-bit intensity + 4-bit alpha, 8x4 tiles                  */
/* ================================================================ */

static void decode_ia4(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 8);
    u16 th = align_up(height, 4);
    u32 si = 0;
    u16 by, bx, y, x;

    for (by = 0; by < th; by += 4) {
        for (bx = 0; bx < tw; bx += 8) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 8; x++) {
                    u8 val = src[si++];
                    u8 a = (u8)((val >> 4) * 17);
                    u8 i = (u8)((val & 0xF) * 17);
                    if (bx + x < width && by + y < height)
                        set_pixel(dst, width, bx + x, by + y, i, i, i, a);
                }
            }
        }
    }
}

/* ================================================================ */
/* IA8 - 8-bit intensity + 8-bit alpha, 4x4 tiles                  */
/* ================================================================ */

static void decode_ia8(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 4);
    u16 th = align_up(height, 4);
    u32 si = 0;
    u16 by, bx, y, x;

    for (by = 0; by < th; by += 4) {
        for (bx = 0; bx < tw; bx += 4) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u8 a = src[si++];
                    u8 i = src[si++];
                    if (bx + x < width && by + y < height)
                        set_pixel(dst, width, bx + x, by + y, i, i, i, a);
                }
            }
        }
    }
}

/* ================================================================ */
/* RGB565 - 16-bit color, 4x4 tiles                                 */
/* ================================================================ */

static void decode_rgb565(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 4);
    u16 th = align_up(height, 4);
    u32 si = 0;
    u16 by, bx, y, x;

    for (by = 0; by < th; by += 4) {
        for (bx = 0; bx < tw; bx += 4) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u16 c = read_be16(src + si); si += 2;
                    u8 r, g, b, a;
                    rgb565_to_rgba8(c, &r, &g, &b, &a);
                    if (bx + x < width && by + y < height)
                        set_pixel(dst, width, bx + x, by + y, r, g, b, a);
                }
            }
        }
    }
}

/* ================================================================ */
/* RGB5A3 - 16-bit color+alpha, 4x4 tiles                          */
/* ================================================================ */

static void decode_rgb5a3(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 4);
    u16 th = align_up(height, 4);
    u32 si = 0;
    u16 by, bx, y, x;

    for (by = 0; by < th; by += 4) {
        for (bx = 0; bx < tw; bx += 4) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u16 c = read_be16(src + si); si += 2;
                    u8 r, g, b, a;
                    rgb5a3_to_rgba8(c, &r, &g, &b, &a);
                    if (bx + x < width && by + y < height)
                        set_pixel(dst, width, bx + x, by + y, r, g, b, a);
                }
            }
        }
    }
}

/* ================================================================ */
/* RGBA8 - 32-bit color+alpha, 4x4 tiles (2 passes: AR, GB)        */
/* ================================================================ */

static void decode_rgba8(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 4);
    u16 th = align_up(height, 4);
    u32 si = 0;
    u16 by, bx, y, x;

    for (by = 0; by < th; by += 4) {
        for (bx = 0; bx < tw; bx += 4) {
            /* First 32 bytes: AR pairs */
            u8 ar[4][4][2];
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    ar[y][x][0] = src[si++]; /* A */
                    ar[y][x][1] = src[si++]; /* R */
                }
            }
            /* Next 32 bytes: GB pairs */
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u8 g = src[si++];
                    u8 b = src[si++];
                    if (bx + x < width && by + y < height)
                        set_pixel(dst, width, bx + x, by + y,
                                  ar[y][x][1], g, b, ar[y][x][0]);
                }
            }
        }
    }
}

/* ================================================================ */
/* CMPR (S3TC/DXT1) - 4x4 blocks in 2x2 macro blocks (8x8 tiles)  */
/* ================================================================ */

static void decode_dxt1_block(const u8* src, u8 pixels[4][4][4]) {
    u16 c0 = read_be16(src);
    u16 c1 = read_be16(src + 2);
    u32 bits = read_be32(src + 4);

    u8 r0, g0, b0, a0, r1, g1, b1, a1;
    rgb565_to_rgba8(c0, &r0, &g0, &b0, &a0);
    rgb565_to_rgba8(c1, &r1, &g1, &b1, &a1);

    u8 palette[4][4];
    palette[0][0] = r0; palette[0][1] = g0; palette[0][2] = b0; palette[0][3] = 255;
    palette[1][0] = r1; palette[1][1] = g1; palette[1][2] = b1; palette[1][3] = 255;

    if (c0 > c1) {
        palette[2][0] = (u8)((2*r0 + r1 + 1) / 3);
        palette[2][1] = (u8)((2*g0 + g1 + 1) / 3);
        palette[2][2] = (u8)((2*b0 + b1 + 1) / 3);
        palette[2][3] = 255;
        palette[3][0] = (u8)((r0 + 2*r1 + 1) / 3);
        palette[3][1] = (u8)((g0 + 2*g1 + 1) / 3);
        palette[3][2] = (u8)((b0 + 2*b1 + 1) / 3);
        palette[3][3] = 255;
    } else {
        palette[2][0] = (u8)((r0 + r1) / 2);
        palette[2][1] = (u8)((g0 + g1) / 2);
        palette[2][2] = (u8)((b0 + b1) / 2);
        palette[2][3] = 255;
        palette[3][0] = 0;
        palette[3][1] = 0;
        palette[3][2] = 0;
        palette[3][3] = 0; /* transparent */
    }

    int y, x;
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            u32 idx = (bits >> (30 - (y * 4 + x) * 2)) & 3;
            pixels[y][x][0] = palette[idx][0];
            pixels[y][x][1] = palette[idx][1];
            pixels[y][x][2] = palette[idx][2];
            pixels[y][x][3] = palette[idx][3];
        }
    }
}

static void decode_cmpr(const u8* src, u8* dst, u16 width, u16 height) {
    u16 tw = align_up(width, 8);
    u16 th = align_up(height, 8);
    u32 si = 0;
    u16 by, bx;
    int sub;

    for (by = 0; by < th; by += 8) {
        for (bx = 0; bx < tw; bx += 8) {
            /* 4 DXT1 sub-blocks in a 2x2 pattern */
            for (sub = 0; sub < 4; sub++) {
                u16 sx = bx + (sub & 1) * 4;
                u16 sy = by + (sub >> 1) * 4;
                u8 pixels[4][4][4];
                int y, x;

                decode_dxt1_block(src + si, pixels);
                si += 8;

                for (y = 0; y < 4; y++) {
                    for (x = 0; x < 4; x++) {
                        if (sx + x < width && sy + y < height) {
                            set_pixel(dst, width, sx + x, sy + y,
                                      pixels[y][x][0], pixels[y][x][1],
                                      pixels[y][x][2], pixels[y][x][3]);
                        }
                    }
                }
            }
        }
    }
}

/* ================================================================ */
/* Public API                                                       */
/* ================================================================ */

u32 pal_gx_decode_texture(const void* src, void* dst,
                          u16 width, u16 height,
                          GXTexFmt format,
                          const void* tlut, u32 tlut_fmt) {
    (void)tlut; (void)tlut_fmt;

    if (!src || !dst || width == 0 || height == 0)
        return 0;

    u32 out_size = (u32)width * height * 4;
    memset(dst, 0, out_size);

    switch (format) {
        case GX_TF_I4:     decode_i4((const u8*)src, (u8*)dst, width, height);     break;
        case GX_TF_I8:     decode_i8((const u8*)src, (u8*)dst, width, height);     break;
        case GX_TF_IA4:    decode_ia4((const u8*)src, (u8*)dst, width, height);    break;
        case GX_TF_IA8:    decode_ia8((const u8*)src, (u8*)dst, width, height);    break;
        case GX_TF_RGB565: decode_rgb565((const u8*)src, (u8*)dst, width, height); break;
        case GX_TF_RGB5A3: decode_rgb5a3((const u8*)src, (u8*)dst, width, height); break;
        case GX_TF_RGBA8:  decode_rgba8((const u8*)src, (u8*)dst, width, height);  break;
        case GX_TF_CMPR:   decode_cmpr((const u8*)src, (u8*)dst, width, height);   break;
        default:
            /* Unknown format - fill with magenta for visibility */
            {
                u32 i;
                u8* p = (u8*)dst;
                for (i = 0; i < (u32)width * height; i++) {
                    p[i*4+0] = 255; p[i*4+1] = 0; p[i*4+2] = 255; p[i*4+3] = 255;
                }
            }
            break;
    }

    return out_size;
}

u32 pal_gx_tex_size(u16 width, u16 height, GXTexFmt format) {
    u16 tw, th;

    switch (format) {
        case GX_TF_I4:
            tw = align_up(width, 8); th = align_up(height, 8);
            return (u32)(tw * th) / 2;
        case GX_TF_I8:
        case GX_TF_IA4:
            tw = align_up(width, 8); th = align_up(height, 4);
            return (u32)(tw * th);
        case GX_TF_IA8:
        case GX_TF_RGB565:
        case GX_TF_RGB5A3:
            tw = align_up(width, 4); th = align_up(height, 4);
            return (u32)(tw * th) * 2;
        case GX_TF_RGBA8:
            tw = align_up(width, 4); th = align_up(height, 4);
            return (u32)(tw * th) * 4;
        case GX_TF_CMPR:
            tw = align_up(width, 8); th = align_up(height, 8);
            return (u32)(tw * th) / 2;
        default:
            return (u32)width * height * 4;
    }
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
