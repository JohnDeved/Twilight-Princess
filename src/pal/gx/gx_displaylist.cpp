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
#include "pal/pal_error.h"

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
     * (none, direct, index8, index16). */

    if (addr == GX_CP_REG_VCD_LO) {
        /* VCD_Lo bit layout:
         * [0]    PNMTXIDX (1=direct)
         * [1-8]  T0MTXIDX..T7MTXIDX (1=direct each)
         * [10:9] POS  (2 bits: 0=none, 1=direct, 2=idx8, 3=idx16)
         * [12:11] NRM
         * [14:13] CLR0
         * [16:15] CLR1 */
        pal_gx_set_vtx_desc(GX_VA_PNMTXIDX, (GXAttrType)((value >> 0) & 0x1 ? GX_DIRECT : GX_NONE));
        for (int i = 0; i < 8; i++) {
            pal_gx_set_vtx_desc((GXAttr)(GX_VA_TEX0MTXIDX + i),
                                (GXAttrType)((value >> (1 + i)) & 0x1 ? GX_DIRECT : GX_NONE));
        }
        pal_gx_set_vtx_desc(GX_VA_POS,  (GXAttrType)((value >> 9) & 0x3));
        pal_gx_set_vtx_desc(GX_VA_NRM,  (GXAttrType)((value >> 11) & 0x3));
        pal_gx_set_vtx_desc(GX_VA_CLR0, (GXAttrType)((value >> 13) & 0x3));
        pal_gx_set_vtx_desc(GX_VA_CLR1, (GXAttrType)((value >> 15) & 0x3));
        return;
    }

    if (addr == GX_CP_REG_VCD_HI) {
        /* VCD_Hi bit layout:
         * [1:0] TEX0, [3:2] TEX1, ..., [15:14] TEX7 */
        for (int i = 0; i < 8; i++) {
            pal_gx_set_vtx_desc((GXAttr)(GX_VA_TEX0 + i),
                                (GXAttrType)((value >> (i * 2)) & 0x3));
        }
        return;
    }

    /* CP registers 0x70-0x77: VAT group A (Vertex Attribute Table)
     * These set component counts, types, and frac bits per vtxfmt.
     * Register address = 0x70 + vtxfmt (0-7).
     *
     * VAT_A bit layout:
     * [0]     POS_CNT  (0=XY, 1=XYZ)
     * [3:1]   POS_TYPE (0=u8, 1=s8, 2=u16, 3=s16, 4=f32)
     * [8:4]   POS_FRAC
     * [9]     NRM_CNT  (0=NRM, 1=NBT)
     * [12:10] NRM_TYPE
     * [13]    CLR0_CNT (0=RGB, 1=RGBA)
     * [16:14] CLR0_TYPE
     * [17]    CLR1_CNT
     * [20:18] CLR1_TYPE
     * [21]    TEX0_CNT (0=S, 1=ST)
     * [24:22] TEX0_TYPE
     * [29:25] TEX0_FRAC
     * [30]    ByteDequant (always 1)
     * [31]    NormalIndex3 */
    if (addr >= 0x70 && addr <= 0x77) {
        int fmt = addr - 0x70;
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_POS,
            (GXCompCnt)((value) & 0x1),
            (GXCompType)((value >> 1) & 0x7),
            (u8)((value >> 4) & 0x1F));
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_NRM,
            (GXCompCnt)((value >> 9) & 0x1),
            (GXCompType)((value >> 10) & 0x7), 0);
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_CLR0,
            (GXCompCnt)((value >> 13) & 0x1),
            (GXCompType)((value >> 14) & 0x7), 0);
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_CLR1,
            (GXCompCnt)((value >> 17) & 0x1),
            (GXCompType)((value >> 18) & 0x7), 0);
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX0,
            (GXCompCnt)((value >> 21) & 0x1),
            (GXCompType)((value >> 22) & 0x7),
            (u8)((value >> 25) & 0x1F));
        return;
    }

    /* CP registers 0x80-0x87: VAT group B
     * [4:0]   TEX1_FRAC
     * [5]     TEX1_CNT
     * [8:6]   TEX1_TYPE
     * [13:9]  TEX2_FRAC
     * [14]    TEX2_CNT
     * [17:15] TEX2_TYPE
     * [22:18] TEX3_FRAC
     * [23]    TEX3_CNT
     * [26:24] TEX3_TYPE
     * [31:27] TEX4_FRAC */
    if (addr >= 0x80 && addr <= 0x87) {
        int fmt = addr - 0x80;
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX1,
            (GXCompCnt)((value >> 5) & 0x1),
            (GXCompType)((value >> 6) & 0x7),
            (u8)((value >> 0) & 0x1F));
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX2,
            (GXCompCnt)((value >> 14) & 0x1),
            (GXCompType)((value >> 15) & 0x7),
            (u8)((value >> 9) & 0x1F));
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX3,
            (GXCompCnt)((value >> 23) & 0x1),
            (GXCompType)((value >> 24) & 0x7),
            (u8)((value >> 18) & 0x1F));
        /* TEX4 frac bits [31:27] — cnt/type are in VAT_C.
         * Store frac now; cnt/type will be set when VAT_C is written.
         * Read existing cnt/type from state to avoid overwriting them. */
        {
            GXVtxAttrFmtEntry* tex4 = &g_gx_state.vtx_attr_fmt[fmt][GX_VA_TEX4];
            pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX4,
                tex4->cnt, tex4->comp_type,
                (u8)((value >> 27) & 0x1F));
        }
        return;
    }

    /* CP registers 0x90-0x97: VAT group C
     * [0]     TEX4_CNT
     * [3:1]   TEX4_TYPE
     * [8:4]   TEX5_FRAC
     * [9]     TEX5_CNT
     * [12:10] TEX5_TYPE
     * [17:13] TEX6_FRAC
     * [18]    TEX6_CNT
     * [21:19] TEX6_TYPE
     * [26:22] TEX7_FRAC
     * [27]    TEX7_CNT
     * [30:28] TEX7_TYPE */
    if (addr >= 0x90 && addr <= 0x97) {
        int fmt = addr - 0x90;
        /* TEX4 cnt/type from VAT_C — frac was set from VAT_B bits [31:27] */
        {
            GXVtxAttrFmtEntry* tex4 = &g_gx_state.vtx_attr_fmt[fmt][GX_VA_TEX4];
            pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX4,
                (GXCompCnt)((value >> 0) & 0x1),
                (GXCompType)((value >> 1) & 0x7),
                tex4->frac);
        }
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX5,
            (GXCompCnt)((value >> 9) & 0x1),
            (GXCompType)((value >> 10) & 0x7),
            (u8)((value >> 4) & 0x1F));
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX6,
            (GXCompCnt)((value >> 18) & 0x1),
            (GXCompType)((value >> 19) & 0x7),
            (u8)((value >> 13) & 0x1F));
        pal_gx_set_vtx_attr_fmt((GXVtxFmt)fmt, GX_VA_TEX7,
            (GXCompCnt)((value >> 27) & 0x1),
            (GXCompType)((value >> 28) & 0x7),
            (u8)((value >> 22) & 0x1F));
        return;
    }
}

/* ================================================================ */
/* XF register handler                                              */
/* ================================================================ */

/**
 * Handle LOAD_XF_REG command.
 * XF registers handle transform matrices and lighting.
 */
static void dl_handle_xf_reg(u16 addr, const u32* values, u16 count) {
    /* XF register space:
     * 0x0000-0x00FF: Position/Normal matrix memory (4x3 f32, 12 regs per matrix)
     * 0x0400-0x04FF: Post-transform matrix memory
     * 0x0500-0x05FF: Texture matrix memory
     * 0x1000-0x10FF: Light parameter memory */

    /* Position matrix memory: 0x0000 + slot*12 (each matrix = 12 f32 = 3 rows × 4 cols) */
    if (addr < 0x0400) {
        int mtxSlot = addr / 12;
        if (mtxSlot < 10 && count >= 12) {
            /* Load 3x4 matrix (row-major, 12 floats) */
            f32 mtx[3][4];
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 4; j++) {
                    union { u32 u; f32 f; } conv;
                    conv.u = values[i * 4 + j];
                    mtx[i][j] = conv.f;
                }
            }
            pal_gx_load_pos_mtx_imm(mtx, (u32)(mtxSlot * 3));
        }
        return;
    }

    /* Texture matrix memory: 0x0500 + mtxIdx*3 */
    if (addr >= 0x0500 && addr < 0x0600) {
        int mtxSlot = (addr - 0x0500) / 12;
        if (mtxSlot < 10 && count >= 12) {
            f32 mtx[3][4];
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 4; j++) {
                    union { u32 u; f32 f; } conv;
                    conv.u = values[i * 4 + j];
                    mtx[i][j] = conv.f;
                }
            }
            pal_gx_load_tex_mtx_imm(mtx, (u32)(GX_TEXMTX0 + mtxSlot * 3), GX_MTX3x4);
        }
        return;
    }
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
         * [14:12] = map1, [17:15] = coord1, [18] = enable1, [21:19] = color1 (stage N+1)
         *
         * BP color channel encoding (3 bits):
         *   0=COLOR0A0, 1=COLOR1A1, 2..6=unused, 7=NULL
         * (NOT the same as GXChannelID enum) */
        if (base_stage < GX_MAX_TEVSTAGE) {
            GXTexMapID map0 = (GXTexMapID)((data >> 0) & 0x7);
            GXTexCoordID coord0 = (GXTexCoordID)((data >> 3) & 0x7);
            int enable0 = (data >> 6) & 0x1;
            u32 bp_chan0 = (data >> 7) & 0x7;
            GXChannelID color0 = (bp_chan0 == 0) ? GX_COLOR0A0 :
                                 (bp_chan0 == 1) ? GX_COLOR1A1 : GX_COLOR_NULL;
            if (!enable0) map0 = GX_TEXMAP_NULL;
            pal_gx_set_tev_order((GXTevStageID)base_stage, coord0, map0, color0);
        }
        if (base_stage + 1 < GX_MAX_TEVSTAGE) {
            GXTexMapID map1 = (GXTexMapID)((data >> 12) & 0x7);
            GXTexCoordID coord1 = (GXTexCoordID)((data >> 15) & 0x7);
            int enable1 = (data >> 18) & 0x1;
            u32 bp_chan1 = (data >> 19) & 0x7;
            GXChannelID color1 = (bp_chan1 == 0) ? GX_COLOR0A0 :
                                 (bp_chan1 == 1) ? GX_COLOR1A1 : GX_COLOR_NULL;
            if (!enable1) map1 = GX_TEXMAP_NULL;
            pal_gx_set_tev_order((GXTevStageID)(base_stage + 1), coord1, map1, color1);
        }
        return;
    }

    /* GEN_MODE (register 0x00): number of TEV stages, tex gens, color channels */
    if (addr == 0x00) {
        /* BP_GEN_MODE bit layout:
         * [3:0] = nTexGens, [5:4] = nChans, [13:10] = nTevs, [18:16] = nInds */
        u8 nTevs = (u8)(((data >> 10) & 0xF) + 1); /* stored as n-1 */
        pal_gx_set_num_tev_stages(nTevs);
        u8 nChans = (u8)((data >> 4) & 0x3);
        pal_gx_set_num_chans(nChans);
        u8 nTexGens = (u8)(data & 0xF);
        pal_gx_set_num_tex_gens(nTexGens);
        return;
    }

    /* Alpha compare (register 0xF3) */
    if (addr == 0xF3) {
        /* BP_ALPHA_COMPARE bit layout:
         * [7:0]=ref0, [10:8]=comp0, [12:11]=op, [15:13]=comp1, [23:16]=ref1 */
        u8 ref0 = (u8)(data & 0xFF);
        GXCompare comp0 = (GXCompare)((data >> 8) & 0x7);
        GXAlphaOp op = (GXAlphaOp)((data >> 11) & 0x3);
        GXCompare comp1 = (GXCompare)((data >> 13) & 0x7);
        u8 ref1 = (u8)((data >> 16) & 0xFF);
        pal_gx_set_alpha_compare(comp0, ref0, op, comp1, ref1);
        return;
    }

    /* Z mode (register 0x40) */
    if (addr == 0x40) {
        /* BP_ZMODE bit layout:
         * [0] = test enable, [3:1] = func, [4] = update enable */
        GXBool enable = (GXBool)(data & 0x1);
        GXCompare func = (GXCompare)((data >> 1) & 0x7);
        GXBool update = (GXBool)((data >> 4) & 0x1);
        pal_gx_set_z_mode(enable, func, update);
        return;
    }

    /* Blend mode (register 0x41) */
    if (addr == 0x41) {
        /* BP_BLENDMODE bit layout:
         * [0] = blend enable, [1] = logic enable, [2] = dither,
         * [3] = color update, [4] = alpha update,
         * [7:5] = dst factor, [10:8] = src factor, [11] = subtract,
         * [15:12] = logic op */
        int blend_en = data & 0x1;
        int logic_en = (data >> 1) & 0x1;
        int subtract = (data >> 11) & 0x1;
        GXBlendMode mode = GX_BM_NONE;
        if (subtract) mode = GX_BM_SUBTRACT;
        else if (blend_en) mode = GX_BM_BLEND;
        else if (logic_en) mode = GX_BM_LOGIC;
        GXBlendFactor src = (GXBlendFactor)((data >> 8) & 0x7);
        GXBlendFactor dst = (GXBlendFactor)((data >> 5) & 0x7);
        GXLogicOp logic = (GXLogicOp)((data >> 12) & 0xF);
        pal_gx_set_blend_mode(mode, src, dst, logic);

        GXBool color_upd = (GXBool)((data >> 3) & 0x1);
        GXBool alpha_upd = (GXBool)((data >> 4) & 0x1);
        pal_gx_set_color_update(color_upd);
        pal_gx_set_alpha_update(alpha_upd);
        return;
    }

    /* TEV konst color selector (register 0x18-0x1F, covers 2 stages each) */
    if (addr >= 0x18 && addr <= 0x1F) {
        /* 4 stages per register, alternating k_color_sel and k_alpha_sel */
        /* Actually: 0x18-0x1B = kcolor sel for stages 0-15 (4 per reg)
         * 0x1C-0x1F = kalpha sel for stages 0-15 (4 per reg) */
        if (addr <= 0x1B) {
            int base = (addr - 0x18) * 4;
            for (int i = 0; i < 4 && base + i < GX_MAX_TEVSTAGE; i++) {
                GXTevKColorSel sel = (GXTevKColorSel)((data >> (i * 5)) & 0x1F);
                pal_gx_set_tev_k_color_sel((GXTevStageID)(base + i), sel);
            }
        } else {
            int base = (addr - 0x1C) * 4;
            for (int i = 0; i < 4 && base + i < GX_MAX_TEVSTAGE; i++) {
                GXTevKAlphaSel sel = (GXTevKAlphaSel)((data >> (i * 5)) & 0x1F);
                pal_gx_set_tev_k_alpha_sel((GXTevStageID)(base + i), sel);
            }
        }
        return;
    }

    /* TEV color register (registers 0xE0-0xE7): Pairs of 24-bit values (R/A then B/G)
     * 0xE0/0xE1 = TEVREG0, 0xE2/0xE3 = TEVREG1, etc. */
    if (addr >= 0xE0 && addr <= 0xE7) {
        int reg = (addr - 0xE0) / 2;
        int is_bg = (addr & 1); /* even = R+A (lo), odd = B+G (hi) */
        if (reg < GX_MAX_TEVREG) {
            if (!is_bg) {
                /* [10:0] = A, [22:12] = R */
                u16 a_val = (u16)(data & 0x7FF);
                u16 r_val = (u16)((data >> 12) & 0x7FF);
                g_gx_state.tev_regs[reg].r = (u8)(r_val & 0xFF);
                g_gx_state.tev_regs[reg].a = (u8)(a_val & 0xFF);
            } else {
                /* [10:0] = B, [22:12] = G */
                u16 b_val = (u16)(data & 0x7FF);
                u16 g_val = (u16)((data >> 12) & 0x7FF);
                g_gx_state.tev_regs[reg].g = (u8)(g_val & 0xFF);
                g_gx_state.tev_regs[reg].b = (u8)(b_val & 0xFF);
            }
        }
        return;
    }

    /* BP TexImage0 (texture dimensions + format):
     * Registers: 0x88-0x8B (texmap 0-3), 0xA8-0xAB (texmap 4-7)
     * Bit layout: [9:0]=width-1, [19:10]=height-1, [23:20]=format */
    {
        int texmap = -1;
        if (addr >= 0x88 && addr <= 0x8B) texmap = addr - 0x88;
        else if (addr >= 0xA8 && addr <= 0xAB) texmap = addr - 0xA8 + 4;
        if (texmap >= 0 && texmap < GX_MAX_TEXMAP) {
            u16 w = (u16)((data & 0x3FF) + 1);
            u16 h = (u16)(((data >> 10) & 0x3FF) + 1);
            GXTexFmt fmt = (GXTexFmt)((data >> 20) & 0xF);
            GXTexBinding* bind = &g_gx_state.tex_bindings[texmap];
            bind->width = w;
            bind->height = h;
            bind->format = fmt;
            return;
        }
    }

    /* BP TexImage3 (texture image pointer):
     * Registers: 0x94-0x97 (texmap 0-3), 0xB4-0xB7 (texmap 4-7)
     * On PC: data field = pointer table index (from pal_gx_tex_ptr_register) */
    {
        int texmap = -1;
        if (addr >= 0x94 && addr <= 0x97) texmap = addr - 0x94;
        else if (addr >= 0xB4 && addr <= 0xB7) texmap = addr - 0xB4 + 4;
        if (texmap >= 0 && texmap < GX_MAX_TEXMAP) {
            void* ptr = pal_gx_tex_ptr_resolve(data);
            if (ptr) {
                g_gx_state.tex_bindings[texmap].image_ptr = ptr;
                g_gx_state.tex_bindings[texmap].valid = 1;
            }
            return;
        }
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
            /* Nested display list call — the address is a physical GCN
             * pointer that we cannot resolve on PC. J3D typically builds
             * material and shape DLs as flat buffers, so nested calls are
             * rare in practice. Log when encountered for diagnostics. */
            u32 list_addr = dl_read_u32(&reader);
            u32 list_size = dl_read_u32(&reader);
            (void)list_addr; (void)list_size;
            pal_error(PAL_ERR_DL_PARSE, "nested CALL_DL (unresolvable GCN address)");
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

        /* Unknown opcode — report and break */
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "opcode=0x%02X pos=%u", opcode, reader.pos);
            pal_error(PAL_ERR_DL_PARSE, buf);
        }
        break;
    }
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
