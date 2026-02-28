/**
 * gx_displaylist.cpp - GX Display List replay for PC port (Step 5e)
 *
 * Parses pre-recorded GX display lists from J3D model data (BMD/BDL files).
 * Display lists are raw GX FIFO command byte streams in big-endian format.
 *
 * GX FIFO command format:
 *   0x00       = NOP (skip)
 *   0x08       = LOAD_CP_REG:  u8 addr + u32 value
 *   0x10       = LOAD_XF_REG:  u16 length_minus1 + u16 addr + (length) u32 values
 *   0x20       = LOAD_INDX_A:  u16 index + u16 addr (position/normal matrix)
 *   0x28       = LOAD_INDX_B:  u16 index + u16 addr (normal matrix)
 *   0x30       = LOAD_INDX_C:  u16 index + u16 addr (texture matrix)
 *   0x38       = LOAD_INDX_D:  u16 index + u16 addr (light)
 *   0x40       = CALL_DL:      u32 addr + u32 size (nested — skip on PC)
 *   0x48       = INVAL_VTX:    (skip)
 *   0x61       = LOAD_BP_REG:  u32 value
 *   0x80-0xB8  = DRAW commands: opcode & 0xF8 = prim type, & 0x07 = vtxfmt
 *                u16 vertex_count + vertex data
 *
 * All multi-byte values are big-endian (PowerPC byte order).
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <string.h>
#include "revolution/gx/GXCommandList.h"
#include "revolution/gx/GXEnum.h"
#include "pal/gx/gx_displaylist.h"
#include "pal/gx/gx_state.h"

/* ================================================================ */
/* Big-endian byte stream reader                                    */
/* ================================================================ */

typedef struct {
    const u8* data;
    u32       size;
    u32       pos;
} DLReader;

static inline u8 dl_read_u8(DLReader* r) {
    if (r->pos >= r->size) return 0;
    return r->data[r->pos++];
}

static inline u16 dl_read_u16(DLReader* r) {
    if (r->pos + 2 > r->size) { r->pos = r->size; return 0; }
    u16 val = ((u16)r->data[r->pos] << 8) | r->data[r->pos + 1];
    r->pos += 2;
    return val;
}

static inline u32 dl_read_u32(DLReader* r) {
    if (r->pos + 4 > r->size) { r->pos = r->size; return 0; }
    u32 val = ((u32)r->data[r->pos] << 24) | ((u32)r->data[r->pos + 1] << 16) |
              ((u32)r->data[r->pos + 2] << 8) | r->data[r->pos + 3];
    r->pos += 4;
    return val;
}

static inline f32 dl_read_f32(DLReader* r) {
    u32 bits = dl_read_u32(r);
    f32 val;
    memcpy(&val, &bits, sizeof(val));
    return val;
}

/* ================================================================ */
/* Vertex data size calculation                                     */
/* ================================================================ */

/**
 * Calculate the size of one vertex in a display list draw command.
 * This depends on the current vertex descriptor and format.
 * Returns the byte size of one vertex, or 0 if unknown.
 */
static u32 calc_dl_vertex_size(u8 vtxfmt) {
    /* Use the state machine's vertex stride calculation.
     * This accounts for the current vertex descriptor (which attributes
     * are enabled) and format (component types, sizes). */
    return pal_gx_calc_vtx_stride((GXVtxFmt)vtxfmt);
}

/* ================================================================ */
/* CP register handler                                              */
/* ================================================================ */

/**
 * Handle LOAD_CP_REG command.
 * CP registers 0x50-0x5F set vertex attribute descriptors.
 * CP registers 0x70-0x9F set vertex attribute formats.
 */
static void dl_handle_cp_reg(u8 addr, u32 value) {
    /* CP register 0x50: VCD_Lo — vertex descriptor low bits
     * CP register 0x60: VCD_Hi — vertex descriptor high bits
     * These set which vertex attributes are present and how they're specified
     * (direct, index8, index16).
     *
     * For now, we skip CP register parsing — the game code typically
     * calls GXSetVtxDesc/GXSetVtxAttrFmt before display lists,
     * and the display list's CP commands just reinforce the same state.
     * A full implementation would parse the CP register bits. */
    (void)addr;
    (void)value;
}

/* ================================================================ */
/* XF register handler                                              */
/* ================================================================ */

/**
 * Handle LOAD_XF_REG command.
 * XF registers handle transform matrices and lighting.
 */
static void dl_handle_xf_reg(u16 addr, const u32* values, u16 count) {
    /* XF registers 0x0000-0x03FF: Position/normal matrix memory
     * XF registers 0x0400-0x04FF: Texture matrix memory
     * For now, skip XF register parsing — matrix state is set
     * by GXLoadPosMtxImm/GXLoadTexMtxImm before display lists. */
    (void)addr;
    (void)values;
    (void)count;
}

/* ================================================================ */
/* BP register handler                                              */
/* ================================================================ */

/**
 * Handle LOAD_BP_REG command.
 * BP registers control TEV, textures, alpha test, z-mode, etc.
 */
static void dl_handle_bp_reg(u32 value) {
    /* BP register format: bits [31:24] = register address, [23:0] = data.
     * Parse TEV stage configuration from display list BP commands.
     * This enables J3D material rendering which sets TEV state via display lists. */
    u8 addr = (u8)(value >> 24);
    u32 data = value & 0x00FFFFFF;

    /* TEV color combiner: registers 0xC0, 0xC2, 0xC4, ... (even = color) */
    if (addr >= 0xC0 && addr <= 0xDF && (addr & 1) == 0) {
        int stage = (addr - 0xC0) / 2;
        if (stage < GX_MAX_TEVSTAGE) {
            /* BP_TEV_COLOR bit layout:
             * [3:0]   = d, [7:4]   = c, [11:8]  = b, [15:12] = a
             * [17:16] = bias, [18] = op, [19] = clamp, [21:20] = scale, [23:22] = out_reg */
            GXTevColorArg d_arg = (GXTevColorArg)((data >> 0) & 0xF);
            GXTevColorArg c_arg = (GXTevColorArg)((data >> 4) & 0xF);
            GXTevColorArg b_arg = (GXTevColorArg)((data >> 8) & 0xF);
            GXTevColorArg a_arg = (GXTevColorArg)((data >> 12) & 0xF);
            pal_gx_set_tev_color_in((GXTevStageID)stage, a_arg, b_arg, c_arg, d_arg);

            GXTevOp op = (GXTevOp)((data >> 18) & 0x1);
            GXTevBias bias = (GXTevBias)((data >> 16) & 0x3);
            GXBool clamp = (GXBool)((data >> 19) & 0x1);
            GXTevScale scale = (GXTevScale)((data >> 20) & 0x3);
            GXTevRegID out_reg = (GXTevRegID)((data >> 22) & 0x3);
            pal_gx_set_tev_color_op((GXTevStageID)stage, op, bias, scale, clamp, out_reg);
        }
        return;
    }

    /* TEV alpha combiner: registers 0xC1, 0xC3, 0xC5, ... (odd = alpha) */
    if (addr >= 0xC0 && addr <= 0xDF && (addr & 1) == 1) {
        int stage = (addr - 0xC0) / 2;
        if (stage < GX_MAX_TEVSTAGE) {
            /* BP_TEV_ALPHA bit layout:
             * [1:0] = ras_sel, [3:2] = tex_sel
             * [6:4] = d, [9:7] = c, [12:10] = b, [15:13] = a
             * [17:16] = bias, [18] = op, [19] = clamp, [21:20] = scale, [23:22] = out_reg */
            GXTevAlphaArg d_arg = (GXTevAlphaArg)((data >> 4) & 0x7);
            GXTevAlphaArg c_arg = (GXTevAlphaArg)((data >> 7) & 0x7);
            GXTevAlphaArg b_arg = (GXTevAlphaArg)((data >> 10) & 0x7);
            GXTevAlphaArg a_arg = (GXTevAlphaArg)((data >> 13) & 0x7);
            pal_gx_set_tev_alpha_in((GXTevStageID)stage, a_arg, b_arg, c_arg, d_arg);

            GXTevOp op = (GXTevOp)((data >> 18) & 0x1);
            GXTevBias bias = (GXTevBias)((data >> 16) & 0x3);
            GXBool clamp = (GXBool)((data >> 19) & 0x1);
            GXTevScale scale = (GXTevScale)((data >> 20) & 0x3);
            GXTevRegID out_reg = (GXTevRegID)((data >> 22) & 0x3);
            pal_gx_set_tev_alpha_op((GXTevStageID)stage, op, bias, scale, clamp, out_reg);

            GXTevSwapSel ras_sel = (GXTevSwapSel)((data >> 0) & 0x3);
            GXTevSwapSel tex_sel = (GXTevSwapSel)((data >> 2) & 0x3);
            pal_gx_set_tev_swap_mode((GXTevStageID)stage, ras_sel, tex_sel);
        }
        return;
    }

    /* TEV order: registers 0x28-0x2F (each covers 2 stages) */
    if (addr >= 0x28 && addr <= 0x2F) {
        int base_stage = (addr - 0x28) * 2;
        /* BP_TEV_ORDER bit layout:
         * [2:0] = map0, [5:3] = coord0, [6] = enable0, [9:7] = color0 (stage N)
         * [14:12] = map1, [17:15] = coord1, [18] = enable1, [21:19] = color1 (stage N+1) */
        if (base_stage < GX_MAX_TEVSTAGE) {
            GXTexMapID map0 = (GXTexMapID)((data >> 0) & 0x7);
            GXTexCoordID coord0 = (GXTexCoordID)((data >> 3) & 0x7);
            int enable0 = (data >> 6) & 0x1;
            GXChannelID color0 = (GXChannelID)((data >> 7) & 0x7);
            if (!enable0) map0 = GX_TEXMAP_NULL;
            pal_gx_set_tev_order((GXTevStageID)base_stage, coord0, map0, color0);
        }
        if (base_stage + 1 < GX_MAX_TEVSTAGE) {
            GXTexMapID map1 = (GXTexMapID)((data >> 12) & 0x7);
            GXTexCoordID coord1 = (GXTexCoordID)((data >> 15) & 0x7);
            int enable1 = (data >> 18) & 0x1;
            GXChannelID color1 = (GXChannelID)((data >> 19) & 0x7);
            if (!enable1) map1 = GX_TEXMAP_NULL;
            pal_gx_set_tev_order((GXTevStageID)(base_stage + 1), coord1, map1, color1);
        }
        return;
    }
}

/* ================================================================ */
/* Draw command handler                                             */
/* ================================================================ */

/**
 * Handle a DRAW command from a display list.
 * Reads vertex data and feeds it to the GX state machine.
 */
static void dl_handle_draw(DLReader* r, u8 opcode) {
    u8 prim_type = opcode & GX_OPCODE_MASK;
    u8 vtxfmt = opcode & GX_VAT_MASK;
    u16 nverts = dl_read_u16(r);

    if (nverts == 0) return;

    /* Map GX FIFO primitive opcode to GXPrimitive enum */
    GXPrimitive prim;
    switch (prim_type) {
    case GX_DRAW_QUADS:          prim = GX_QUADS; break;
    case GX_DRAW_TRIANGLES:      prim = GX_TRIANGLES; break;
    case GX_DRAW_TRIANGLE_STRIP: prim = GX_TRIANGLESTRIP; break;
    case GX_DRAW_TRIANGLE_FAN:   prim = GX_TRIANGLEFAN; break;
    case GX_DRAW_LINES:          prim = GX_LINES; break;
    case GX_DRAW_LINE_STRIP:     prim = GX_LINESTRIP; break;
    case GX_DRAW_POINTS:         prim = GX_POINTS; break;
    default: return; /* Unknown primitive */
    }

    /* Calculate vertex size from current format */
    u32 vert_size = calc_dl_vertex_size(vtxfmt);
    u32 total_bytes = (u32)nverts * vert_size;

    if (vert_size == 0 || r->pos + total_bytes > r->size) {
        /* Can't determine vertex size or not enough data — skip */
        if (vert_size > 0) r->pos += total_bytes;
        return;
    }

    /* Begin draw call */
    pal_gx_begin(prim, (GXVtxFmt)vtxfmt, nverts);

    /* Feed vertex data byte-by-byte to the state machine.
     * The display list stores vertices in big-endian format with
     * the component layout matching the current vertex descriptor.
     * We read each component according to its type and size. */
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    const GXVtxAttrFmtEntry* fmt = g_gx_state.vtx_attr_fmt[vtxfmt];

    for (u16 v = 0; v < nverts; v++) {
        /* For each active attribute, read and write its components */
        for (int attr = GX_VA_PNMTXIDX; attr < GX_MAX_VTXATTR; attr++) {
            if (desc[attr].type == GX_NONE) continue;

            if (desc[attr].type == GX_INDEX8) {
                u8 idx = dl_read_u8(r);
                pal_gx_write_vtx_u8(idx);
                continue;
            }
            if (desc[attr].type == GX_INDEX16) {
                u16 idx = dl_read_u16(r);
                pal_gx_write_vtx_u16(idx);
                continue;
            }

            /* Direct mode — read components based on attribute type */
            int num_comps = 1;
            if (attr == GX_VA_POS) {
                num_comps = (fmt[attr].cnt == GX_POS_XYZ) ? 3 : 2;
            } else if (attr == GX_VA_NRM || attr == GX_VA_NBT) {
                num_comps = 3;
                if (attr == GX_VA_NBT && fmt[attr].cnt == GX_NRM_NBT3) num_comps = 9;
            } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                /* Color components — read as u32 (RGBA8) or u16 (RGB565) etc. */
                switch (fmt[attr].comp_type) {
                case GX_RGB8:
                case GX_RGBX8: {
                    u8 cr = dl_read_u8(r);
                    u8 cg = dl_read_u8(r);
                    u8 cb = dl_read_u8(r);
                    u8 ca = (fmt[attr].comp_type == GX_RGBX8) ? dl_read_u8(r) : 0xFF;
                    u32 color = ((u32)cr << 24) | ((u32)cg << 16) | ((u32)cb << 8) | ca;
                    pal_gx_write_vtx_u32(color);
                    continue;
                }
                case GX_RGBA8: {
                    u8 cr = dl_read_u8(r);
                    u8 cg = dl_read_u8(r);
                    u8 cb = dl_read_u8(r);
                    u8 ca = dl_read_u8(r);
                    u32 color = ((u32)cr << 24) | ((u32)cg << 16) | ((u32)cb << 8) | ca;
                    pal_gx_write_vtx_u32(color);
                    continue;
                }
                case GX_RGB565: {
                    u16 rgb = dl_read_u16(r);
                    pal_gx_write_vtx_u16(rgb);
                    continue;
                }
                case GX_RGBA4: {
                    u16 rgba = dl_read_u16(r);
                    pal_gx_write_vtx_u16(rgba);
                    continue;
                }
                case GX_RGBA6: {
                    /* 24-bit color */
                    u8 b0 = dl_read_u8(r);
                    u8 b1 = dl_read_u8(r);
                    u8 b2 = dl_read_u8(r);
                    u32 color = ((u32)b0 << 16) | ((u32)b1 << 8) | b2;
                    pal_gx_write_vtx_u32(color);
                    continue;
                }
                default: {
                    /* Default: read 4 bytes as RGBA */
                    u32 color = dl_read_u32(r);
                    pal_gx_write_vtx_u32(color);
                    continue;
                }
                }
            } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
                num_comps = (fmt[attr].cnt == GX_TEX_ST) ? 2 : 1;
            } else if (attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX)) {
                /* Matrix index — 1 byte */
                u8 idx = dl_read_u8(r);
                pal_gx_write_vtx_u8(idx);
                continue;
            }

            /* Read numeric components based on component type */
            for (int c = 0; c < num_comps; c++) {
                switch (fmt[attr].comp_type) {
                case GX_U8:
                    pal_gx_write_vtx_u8(dl_read_u8(r));
                    break;
                case GX_S8:
                    pal_gx_write_vtx_s8((s8)dl_read_u8(r));
                    break;
                case GX_U16:
                    pal_gx_write_vtx_u16(dl_read_u16(r));
                    break;
                case GX_S16:
                    pal_gx_write_vtx_s16((s16)dl_read_u16(r));
                    break;
                case GX_F32:
                    pal_gx_write_vtx_f32(dl_read_f32(r));
                    break;
                default:
                    pal_gx_write_vtx_f32(dl_read_f32(r));
                    break;
                }
            }
        }
        g_gx_state.draw.verts_written++;
    }

    /* End draw call — this flushes to bgfx via TEV pipeline */
    pal_gx_end();
}

/* ================================================================ */
/* Public API                                                       */
/* ================================================================ */

void pal_gx_call_display_list(const void* list, u32 nbytes) {
    if (!list || nbytes == 0) return;

    DLReader reader;
    reader.data = (const u8*)list;
    reader.size = nbytes;
    reader.pos = 0;

    while (reader.pos < reader.size) {
        u8 opcode = dl_read_u8(&reader);

        if (opcode == GX_NOP) {
            continue;
        }

        if (opcode == GX_LOAD_CP_REG) {
            u8 addr = dl_read_u8(&reader);
            u32 value = dl_read_u32(&reader);
            dl_handle_cp_reg(addr, value);
            continue;
        }

        if (opcode == GX_LOAD_XF_REG) {
            u16 len_minus1 = dl_read_u16(&reader);
            u16 addr = dl_read_u16(&reader);
            u16 count = len_minus1 + 1;
            /* Read XF register values */
            u32 xf_values[64];
            u16 read_count = (count > 64) ? 64 : count;
            for (u16 i = 0; i < read_count; i++) {
                xf_values[i] = dl_read_u32(&reader);
            }
            /* Skip remaining if more than 64 */
            for (u16 i = read_count; i < count; i++) {
                dl_read_u32(&reader);
            }
            dl_handle_xf_reg(addr, xf_values, read_count);
            continue;
        }

        if (opcode == GX_LOAD_INDX_A || opcode == GX_LOAD_INDX_B ||
            opcode == GX_LOAD_INDX_C || opcode == GX_LOAD_INDX_D) {
            /* Index load: u16 index_value + u16 register_addr */
            dl_read_u16(&reader); /* index */
            dl_read_u16(&reader); /* addr */
            continue;
        }

        if (opcode == GX_CMD_CALL_DL) {
            /* Nested display list call — skip on PC
             * (we'd need to resolve the 32-bit address) */
            dl_read_u32(&reader); /* list addr */
            dl_read_u32(&reader); /* size */
            continue;
        }

        if (opcode == GX_CMD_INVAL_VTX) {
            /* Invalidate vertex cache — no-op on PC */
            continue;
        }

        if (opcode == GX_LOAD_BP_REG) {
            u32 value = dl_read_u32(&reader);
            dl_handle_bp_reg(value);
            continue;
        }

        /* Check if it's a DRAW command (0x80-0xBF) */
        if ((opcode & 0x80) != 0) {
            dl_handle_draw(&reader, opcode);
            continue;
        }

        /* Unknown opcode — skip one byte and try to recover */
        break;
    }
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
