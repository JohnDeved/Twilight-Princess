/**
 * gx_texture.h - GX texture format decoder for PC port (Step 5d)
 *
 * Decodes GX tile formats (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8,
 * CMPR/S3TC, CI4, CI8) to linear RGBA8 for bgfx texture creation.
 *
 * GX textures are stored in a tiled (Morton/Z-order) layout:
 * - Each format has a tile size (e.g., 8x8 for I4, 4x4 for RGBA8)
 * - Tiles are stored left-to-right, top-to-bottom
 * - Within each tile, texels are in a specific order
 */

#ifndef PAL_GX_TEXTURE_H
#define PAL_GX_TEXTURE_H

#include "dolphin/types.h"
#include "revolution/gx/GXEnum.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode a GX-format texture to linear RGBA8.
 *
 * @param src       Source texture data in GX tiled format
 * @param dst       Destination buffer (must be width * height * 4 bytes)
 * @param width     Texture width in pixels
 * @param height    Texture height in pixels
 * @param format    GX texture format (GXTexFmt)
 * @param tlut      Color lookup table for CI formats (NULL for non-CI)
 * @param tlut_fmt  TLUT format (GX_TL_IA8, GX_TL_RGB565, GX_TL_RGB5A3)
 * @return          Number of bytes written, or 0 on error
 */
u32 pal_gx_decode_texture(const void* src, void* dst,
                          u16 width, u16 height,
                          GXTexFmt format,
                          const void* tlut, u32 tlut_fmt);

/**
 * Get the size in bytes of a GX-format texture.
 */
u32 pal_gx_tex_size(u16 width, u16 height, GXTexFmt format);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_TEXTURE_H */
