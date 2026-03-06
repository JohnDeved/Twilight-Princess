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

/* Draw call diagnostics */
static int s_dl_draw_count = 0;
static int s_dl_vert_count = 0;

/* DL validation counters (task 3: stride/offset/index bounds checks) */
static int s_dl_stride_zero = 0;      /* vert_size == 0 skips */
static int s_dl_overflow = 0;         /* buffer overflow skips */
static int s_dl_bulk_copy_ok = 0;     /* successful bulk copies */
static int s_dl_bulk_copy_fail = 0;   /* bulk copy buffer overflows */
static int s_dl_total_draws = 0;      /* cumulative draw count (not reset per frame) */
static int s_dl_total_verts = 0;      /* cumulative vert count (not reset per frame) */

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
 *
 * CP register address space (from dolsdk2004 GXSave.c __SaveCPRegs):
 *   cpAddr = (raw_addr >> 4) & 0xF  (upper nibble = register group)
 *   vatIdx = raw_addr & 0xF         (lower nibble = sub-index)
 *
 * Register groups:
 *   0x30 (grp 3): MatIdxA — position/texture matrix index A
 *   0x40 (grp 4): MatIdxB — position/texture matrix index B
 *   0x50 (grp 5): VCD_Lo  — vertex component descriptor low
 *   0x60 (grp 6): VCD_Hi  — vertex component descriptor high
 *   0x70-0x77 (grp 7): VAT_A  — vertex attribute table group A
 *   0x80-0x87 (grp 8): VAT_B  — vertex attribute table group B
 *   0x90-0x97 (grp 9): VAT_C  — vertex attribute table group C
 *   0xA0-0xAF (grp 10): Array base addresses
 *   0xB0-0xBF (grp 11): Array strides
 */
static void dl_handle_cp_reg(u8 addr, u32 value) {
    /* CP register 0x30: MatIdxA — matrix index register A
     * From dolsdk2004 GXAttr.c: controls which position/normal/texture matrices
     * are active for subsequent draws.
     *
     * Bit layout:
     * [5:0]   POSIDX   — position matrix index (÷3 = slot in pos_mtx[])
     * [11:6]  TEXIDX0  — texture matrix 0 index
     * [17:12] TEXIDX1  — texture matrix 1 index
     * [23:18] TEXIDX2  — texture matrix 2 index
     * [29:24] TEXIDX3  — texture matrix 3 index
     */
    if (addr == 0x30) {
        u32 pos_idx = (value >> 0) & 0x3F;
        /* Position matrix: idx/3 gives slot (0-9) */
        pal_gx_set_current_mtx(pos_idx / 3);
        return;
    }

    /* CP register 0x40: MatIdxB — matrix index register B
     * [5:0]   TEXIDX4, [11:6] TEXIDX5, [17:12] TEXIDX6, [23:18] TEXIDX7 */
    if (addr == 0x40) {
        /* Texture matrix indices 4-7 — rarely used in TP, log for tracking */
        return;
    }

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

    /* CP registers 0xA0-0xAF: Array base addresses
     * From dolsdk2004 GXSave.c __SaveCPRegs (cpAddr=10):
     *   sub-index 0x5 = POS array base     (indexed base for LOAD_INDX_A)
     *   sub-index 0x6 = NRM array base     (indexed base for LOAD_INDX_B)
     *   sub-index 0x8-0xB = TEX array base (indexed base for LOAD_INDX_C)
     *   sub-index 0xC-0xF = LIGHT array base (indexed base for LOAD_INDX_D)
     *
     * On PC, DL-based array base writes are GCN physical addresses which
     * we cannot resolve. J3D sets arrays via GXSetArray before the DL. */
    if (addr >= 0xA0 && addr <= 0xAF) {
        return;
    }

    /* CP registers 0xB0-0xBF: Array strides
     * Same sub-index scheme as base addresses. */
    if (addr >= 0xB0 && addr <= 0xBF) {
        return;
    }

    /* Unknown CP register — log once for diagnostics */
    {
        static int s_cp_unknown_log = 0;
        if (s_cp_unknown_log < 10) {
            fprintf(stderr, "{\"dl_cp_unknown\":{\"addr\":\"0x%02X\",\"value\":\"0x%08X\"}}\n",
                    (unsigned)addr, (unsigned)value);
            s_cp_unknown_log++;
        }
    }
}

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

    /* Material/ambient color block: XF 0x100A-0x100D
     * The J3D material DL often writes the entire block in one XF command
     * starting at 0x100A with count=4 (ambient0, ambient1, material0, material1).
     * Must handle contiguous writes that span multiple register types. */
    if (addr >= 0x100A && addr <= 0x100D) {
        for (u16 i = 0; i < count; i++) {
            u16 cur_addr = addr + i;
            if (cur_addr > 0x100D) break;
            GXColor c;
            c.r = (u8)((values[i] >> 24) & 0xFF);
            c.g = (u8)((values[i] >> 16) & 0xFF);
            c.b = (u8)((values[i] >> 8) & 0xFF);
            c.a = (u8)(values[i] & 0xFF);
            if (cur_addr <= 0x100B) {
                pal_gx_set_chan_amb_color((GXChannelID)(GX_COLOR0A0 + (cur_addr - 0x100A)), c);
            } else {
                pal_gx_set_chan_mat_color((GXChannelID)(GX_COLOR0A0 + (cur_addr - 0x100C)), c);
            }
        }
        return;
    }

    /* Number of color channels: XF 0x1009 */
    if (addr == 0x1009 && count >= 1) {
        u8 n_chans = (u8)(values[0] & 0x3);
        pal_gx_set_num_chans(n_chans);
        return;
    }

    /* Texture coordinate generators: XF 0x1040-0x1047 */
    if (addr >= 0x1040 && addr <= 0x1047) {
        static const GXTexGenSrc geom_rows[] = {
            GX_TG_POS, GX_TG_NRM, GX_TG_COLOR0, GX_TG_BINRM, GX_TG_TANGENT
        };
        u16 start_coord = addr - 0x1040;
        for (u16 i = 0; i < count && start_coord + i < GX_MAX_TEXCOORD; i++) {
            u32 v = values[i];
            /* XF_REG_TEX bit layout:
             * [1] = proj, [2] = form, [5:4] = tg_type, [11:7] = row
             * [14:12] = embossRow, [17:15] = embossLit */
            int proj = (v >> 1) & 0x1;
            int form = (v >> 2) & 0x1;
            int tg_type = (v >> 4) & 0x3;
            int row = (v >> 7) & 0x1F;

            /* Map XF row + tg_type back to GXTexGenType + GXTexGenSrc */
            GXTexGenType func = proj ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
            if (tg_type == 1) func = GX_TG_BUMP0; /* emboss */
            else if (tg_type == 2) func = GX_TG_SRTG;
            else if (tg_type == 3) func = GX_TG_SRTG;

            GXTexGenSrc src = GX_TG_TEX0;
            if (form == 1) {
                /* geometry source */
                if (row < 5) src = geom_rows[row];
            } else {
                /* texture coordinate source: row 5=TEX0, 6=TEX1, etc. */
                if (row >= 5 && row <= 12) src = (GXTexGenSrc)(GX_TG_TEX0 + (row - 5));
            }

            pal_gx_set_tex_coord_gen(
                (GXTexCoordID)(start_coord + i), func, src,
                GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
        }
        return;
    }

    /* Number of texture generators: XF 0x103F */
    if (addr == 0x103F && count >= 1) {
        u8 n_tex_gens = (u8)(values[0] & 0xF);
        pal_gx_set_num_tex_gens(n_tex_gens);
        return;
    }

    /* Channel control registers: XF 0x100E-0x1011
     * From dolsdk2004 GXLight.c GXSetChanCtrl:
     *   XF reg idx+14 where idx = channel index (0=COLOR0, 1=COLOR1)
     *   COLOR0A0 → writes XF 0x100E (color) and XF 0x1010 (alpha)
     *   COLOR1A1 → writes XF 0x100F (color) and XF 0x1011 (alpha)
     *
     * ChanCtrl bit layout:
     *   [0]    mat_src  (0=REG, 1=VTX)
     *   [1]    enable   (0=no lighting, 1=use lights)
     *   [5:2]  light_mask_lo (lights 0-3)
     *   [6]    amb_src  (0=REG, 1=VTX)
     *   [8:7]  diff_fn  (0=NONE, 1=SIGN, 2=CLAMP)
     *   [9]    attn_enable (1 if attn_fn != GX_AF_SPEC)
     *   [10]   attn_select (1 if attn_fn != GX_AF_NONE)
     *   [14:11] light_mask_hi (lights 4-7)
     *
     * This is CRITICAL for J3D materials that set lighting per-material
     * via display lists — without this, materials inherit the wrong
     * lighting channel configuration. */
    if (addr >= 0x100E && addr <= 0x1011) {
        for (u16 i = 0; i < count && (addr + i) <= 0x1011; i++) {
            u16 cur_addr = addr + i;
            u32 v = values[i];

            GXColorSrc mat_src = (GXColorSrc)((v >> 0) & 0x1);
            GXBool     enable  = (GXBool)((v >> 1) & 0x1);
            u32        lmask_lo = (v >> 2) & 0xF;
            GXColorSrc amb_src = (GXColorSrc)((v >> 6) & 0x1);
            u32        diff_bits = (v >> 7) & 0x3;
            u32        attn_enable = (v >> 9) & 0x1;
            u32        attn_select = (v >> 10) & 0x1;
            u32        lmask_hi = (v >> 11) & 0xF;
            u32        light_mask = lmask_lo | (lmask_hi << 4);

            /* Reconstruct diff_fn and attn_fn from hardware bits.
             * SDK encodes: diff_fn = (attn_fn == 0) ? 0 : actual_diff_fn
             *              attn_enable = (attn_fn != GX_AF_SPEC)
             *              attn_select = (attn_fn != GX_AF_NONE)
             * Reverse: if !attn_select → GX_AF_NONE
             *          else if !attn_enable → GX_AF_SPEC
             *          else → GX_AF_SPOT */
            GXDiffuseFn diff_fn;
            GXAttnFn    attn_fn;
            if (!attn_select) {
                attn_fn = GX_AF_NONE;
                diff_fn = (GXDiffuseFn)diff_bits;
            } else if (!attn_enable) {
                attn_fn = GX_AF_SPEC;
                diff_fn = (GXDiffuseFn)diff_bits;
            } else {
                attn_fn = GX_AF_SPOT;
                diff_fn = (GXDiffuseFn)diff_bits;
            }

            /* Map XF register address to GXChannelID:
             * 0x100E = COLOR0, 0x100F = COLOR1,
             * 0x1010 = ALPHA0, 0x1011 = ALPHA1 */
            GXChannelID chan;
            switch (cur_addr) {
            case 0x100E: chan = GX_COLOR0; break;
            case 0x100F: chan = GX_COLOR1; break;
            case 0x1010: chan = GX_ALPHA0; break;
            case 0x1011: chan = GX_ALPHA1; break;
            default: continue;
            }

            pal_gx_set_chan_ctrl(chan, enable, amb_src, mat_src,
                                light_mask, diff_fn, attn_fn);
        }
        return;
    }

    /* Light parameter memory: XF 0x0600-0x067F
     * From dolsdk2004 GXLight.c GXLoadLightObjImm:
     *   addr = light_idx * 0x10 + 0x600
     *   writes 16 u32 values: 3 reserved, Color, a[3], k[3], lpos[3], ldir[3]
     * This is how J3D loads light data via display lists. */
    if (addr >= 0x0600 && addr < 0x0680) {
        u32 light_offset = addr - 0x0600;
        u32 light_idx = light_offset / 0x10;
        u32 reg_offset = light_offset % 0x10;

        if (light_idx < GX_MAX_LIGHT && reg_offset == 0 && count >= 16) {
            /* Full light object write: 16 words starting at base.
             * Layout: [0-2]=reserved, [3]=Color, [4-6]=a[3], [7-9]=k[3],
             *         [10-12]=lpos[3], [13-15]=ldir[3] */
            GXLightState* lt = &g_gx_state.lights[light_idx];

            /* Color (word 3): packed RGBA8 */
            u32 clr = values[3];
            lt->color.r = (u8)((clr >> 24) & 0xFF);
            lt->color.g = (u8)((clr >> 16) & 0xFF);
            lt->color.b = (u8)((clr >> 8) & 0xFF);
            lt->color.a = (u8)(clr & 0xFF);

            /* Angle attenuation a[3] (words 4-6) */
            for (int j = 0; j < 3; j++) {
                union { u32 u; f32 f; } conv;
                conv.u = values[4 + j];
                lt->a[j] = conv.f;
            }

            /* Distance attenuation k[3] (words 7-9) */
            for (int j = 0; j < 3; j++) {
                union { u32 u; f32 f; } conv;
                conv.u = values[7 + j];
                lt->k[j] = conv.f;
            }

            /* Position (words 10-12) */
            for (int j = 0; j < 3; j++) {
                union { u32 u; f32 f; } conv;
                conv.u = values[10 + j];
                lt->lpos[j] = conv.f;
            }

            /* Direction (words 13-15) — stored negated per SDK */
            for (int j = 0; j < 3; j++) {
                union { u32 u; f32 f; } conv;
                conv.u = values[13 + j];
                lt->ldir[j] = conv.f;
            }

            lt->active = 1;
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
         * BP color channel encoding (3 bits, from dolsdk2004 GXTev.c c2r[]):
         *   0=COLOR0A0, 1=COLOR1A1, 5=ALPHA_BUMP, 6=ALPHA_BUMPN, 7=NULL
         * (reverse of GXChannelID → c2r mapping in SDK) */
        if (base_stage < GX_MAX_TEVSTAGE) {
            GXTexMapID map0 = (GXTexMapID)((data >> 0) & 0x7);
            GXTexCoordID coord0 = (GXTexCoordID)((data >> 3) & 0x7);
            int enable0 = (data >> 6) & 0x1;
            u32 bp_chan0 = (data >> 7) & 0x7;
            GXChannelID color0;
            switch (bp_chan0) {
            case 0:  color0 = GX_COLOR0A0;    break;
            case 1:  color0 = GX_COLOR1A1;    break;
            case 5:  color0 = GX_ALPHA_BUMP;  break;
            case 6:  color0 = GX_ALPHA_BUMPN; break;
            default: color0 = GX_COLOR_NULL;  break;
            }
            if (!enable0) map0 = GX_TEXMAP_NULL;
            pal_gx_set_tev_order((GXTevStageID)base_stage, coord0, map0, color0);
        }
        if (base_stage + 1 < GX_MAX_TEVSTAGE) {
            GXTexMapID map1 = (GXTexMapID)((data >> 12) & 0x7);
            GXTexCoordID coord1 = (GXTexCoordID)((data >> 15) & 0x7);
            int enable1 = (data >> 18) & 0x1;
            u32 bp_chan1 = (data >> 19) & 0x7;
            GXChannelID color1;
            switch (bp_chan1) {
            case 0:  color1 = GX_COLOR0A0;    break;
            case 1:  color1 = GX_COLOR1A1;    break;
            case 5:  color1 = GX_ALPHA_BUMP;  break;
            case 6:  color1 = GX_ALPHA_BUMPN; break;
            default: color1 = GX_COLOR_NULL;  break;
            }
            if (!enable1) map1 = GX_TEXMAP_NULL;
            pal_gx_set_tev_order((GXTevStageID)(base_stage + 1), coord1, map1, color1);
        }
        return;
    }

    /* GEN_MODE (register 0x00): number of TEV stages, tex gens, color channels, cull mode */
    if (addr == 0x00) {
        /* BP_GEN_MODE bit layout (from dolsdk2004 GXGeometry.c, GXInit.c):
         * [3:0]   = nTexGens
         * [5:4]   = nChans
         * [13:10] = nTevs (stored as n-1)
         * [15:14] = cull mode (HARDWARE values — swapped from API!)
         * [18:16] = nInds
         * [19]    = coplanar
         *
         * IMPORTANT: dolsdk2004 GXSetCullMode swaps FRONT↔BACK:
         *   GX_CULL_FRONT (API=1) → hw value 2 (BACK)
         *   GX_CULL_BACK  (API=2) → hw value 1 (FRONT)
         * We need to reverse this swap when reading from BP. */
        u8 nTevs = (u8)(((data >> 10) & 0xF) + 1); /* stored as n-1 */
        pal_gx_set_num_tev_stages(nTevs);
        u8 nChans = (u8)((data >> 4) & 0x3);
        pal_gx_set_num_chans(nChans);
        u8 nTexGens = (u8)(data & 0xF);
        pal_gx_set_num_tex_gens(nTexGens);

        /* Extract cull mode and reverse the HW swap */
        u8 hw_cull = (u8)((data >> 14) & 0x3);
        GXCullMode api_cull;
        switch (hw_cull) {
        case 0:  api_cull = GX_CULL_NONE; break;
        case 1:  api_cull = GX_CULL_BACK;  break;  /* hw FRONT → API BACK */
        case 2:  api_cull = GX_CULL_FRONT; break;  /* hw BACK → API FRONT */
        case 3:  api_cull = GX_CULL_ALL;   break;
        default: api_cull = GX_CULL_BACK;  break;
        }
        pal_gx_set_cull_mode(api_cull);
        return;
    }

    /* Alpha compare (register 0xF3) */
    if (addr == 0xF3) {
        /* BP_ALPHA_COMPARE bit layout (from dolsdk2004 GXTev.c GXSetAlphaCompare):
         * [7:0]   = ref0
         * [15:8]  = ref1
         * [18:16] = comp0
         * [21:19] = comp1
         * [23:22] = op */
        u8 ref0 = (u8)(data & 0xFF);
        u8 ref1 = (u8)((data >> 8) & 0xFF);
        GXCompare comp0 = (GXCompare)((data >> 16) & 0x7);
        GXCompare comp1 = (GXCompare)((data >> 19) & 0x7);
        GXAlphaOp op = (GXAlphaOp)((data >> 22) & 0x3);
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

    /* TEV konst color/alpha selector + swap mode table
     * BP registers 0xF6-0xFD (from dolsdk2004 GXInit.c: tevKsel[0-7] at 0xF6+idx)
     *
     * Each register covers 2 stages and swap table entries:
     * Bit layout:
     *   [1:0]   swap_r/b  (swap table entry, even/odd based on register index)
     *   [3:2]   swap_g/a
     *   [8:4]   kcsel_even (konst color sel for even stage)
     *   [13:9]  kasel_even (konst alpha sel for even stage)
     *   [18:14] kcsel_odd  (konst color sel for odd stage)
     *   [23:19] kasel_odd  (konst alpha sel for odd stage)
     *
     * Stage mapping: register N covers stages 2*N and 2*N+1 */
    if (addr >= 0xF6 && addr <= 0xFD) {
        int ksel_idx = addr - 0xF6;
        int stage_even = ksel_idx * 2;
        int stage_odd  = ksel_idx * 2 + 1;

        /* Konst color selector */
        if (stage_even < GX_MAX_TEVSTAGE) {
            GXTevKColorSel kcsel_even = (GXTevKColorSel)((data >> 4) & 0x1F);
            pal_gx_set_tev_k_color_sel((GXTevStageID)stage_even, kcsel_even);
        }
        if (stage_odd < GX_MAX_TEVSTAGE) {
            GXTevKColorSel kcsel_odd = (GXTevKColorSel)((data >> 14) & 0x1F);
            pal_gx_set_tev_k_color_sel((GXTevStageID)stage_odd, kcsel_odd);
        }

        /* Konst alpha selector */
        if (stage_even < GX_MAX_TEVSTAGE) {
            GXTevKAlphaSel kasel_even = (GXTevKAlphaSel)((data >> 9) & 0x1F);
            pal_gx_set_tev_k_alpha_sel((GXTevStageID)stage_even, kasel_even);
        }
        if (stage_odd < GX_MAX_TEVSTAGE) {
            GXTevKAlphaSel kasel_odd = (GXTevKAlphaSel)((data >> 19) & 0x1F);
            pal_gx_set_tev_k_alpha_sel((GXTevStageID)stage_odd, kasel_odd);
        }
        return;
    }

    /* TEV color / konst color registers (0xE0-0xE7): Pairs covering TEVREG0-3 or KCOLOR0-3
     *
     * From dolsdk2004 GXTev.c:
     *   GXSetTevColor: R/A pair at even addr, B/G pair at odd addr
     *     RA: [10:0]=A(11bit), [22:12]=R(11bit), type=0 at [23:20]
     *     BG: [10:0]=B(11bit), [22:12]=G(11bit), type=0 at [23:20]
     *   GXSetTevKColor: same addresses but type=8 at [23:20]
     *     RA: [7:0]=R(8bit), [19:12]=A(8bit), [23:20]=8
     *     BG: [7:0]=B(8bit), [19:12]=G(8bit), [23:20]=8
     *
     * 0xE0/0xE1 = reg 0, 0xE2/0xE3 = reg 1, etc. */
    if (addr >= 0xE0 && addr <= 0xE7) {
        int reg_idx = (addr - 0xE0) / 2;
        int is_bg = (addr & 1);
        u32 type_field = (data >> 20) & 0xF;

        if (type_field == 0x8 && reg_idx < GX_MAX_TEVKREG) {
            /* Konst color register (GXSetTevKColor) */
            if (!is_bg) {
                g_gx_state.tev_kregs[reg_idx].r = (u8)(data & 0xFF);
                g_gx_state.tev_kregs[reg_idx].a = (u8)((data >> 12) & 0xFF);
            } else {
                g_gx_state.tev_kregs[reg_idx].b = (u8)(data & 0xFF);
                g_gx_state.tev_kregs[reg_idx].g = (u8)((data >> 12) & 0xFF);
            }
        } else if (reg_idx < GX_MAX_TEVREG) {
            /* TEV color register (GXSetTevColor / GXSetTevColorS10) */
            if (!is_bg) {
                u16 a_val = (u16)(data & 0x7FF);
                u16 r_val = (u16)((data >> 12) & 0x7FF);
                g_gx_state.tev_regs[reg_idx].r = (u8)(r_val & 0xFF);
                g_gx_state.tev_regs[reg_idx].a = (u8)(a_val & 0xFF);
            } else {
                u16 b_val = (u16)(data & 0x7FF);
                u16 g_val = (u16)((data >> 12) & 0x7FF);
                g_gx_state.tev_regs[reg_idx].g = (u8)(g_val & 0xFF);
                g_gx_state.tev_regs[reg_idx].b = (u8)(b_val & 0xFF);
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
    /* BP TexMode0 (wrap/filter modes):
     * Registers: 0x80-0x83 (texmap 0-3), 0xA0-0xA3 (texmap 4-7)
     * Bit layout: [1:0]=wrap_s, [3:2]=wrap_t, [4]=mag_filt, [7:5]=min_filt,
     * [8]=edge_lod, [15:9]=lod_bias, [17:16]=max_aniso, [18]=bias_clamp */
    {
        int texmap = -1;
        if (addr >= 0x80 && addr <= 0x83) texmap = addr - 0x80;
        else if (addr >= 0xA0 && addr <= 0xA3) texmap = addr - 0xA0 + 4;
        if (texmap >= 0 && texmap < GX_MAX_TEXMAP) {
            GXTexWrapMode ws = (GXTexWrapMode)(data & 0x3);
            GXTexWrapMode wt = (GXTexWrapMode)((data >> 2) & 0x3);
            GXTexFilter mag = (GXTexFilter)((data >> 4) & 0x1);
            GXTexFilter min_f = (GXTexFilter)((data >> 5) & 0x7);
            g_gx_state.tex_bindings[texmap].wrap_s = ws;
            g_gx_state.tex_bindings[texmap].wrap_t = wt;
            g_gx_state.tex_bindings[texmap].mag_filt = mag;
            g_gx_state.tex_bindings[texmap].min_filt = min_f;
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
        if (vert_size == 0) s_dl_stride_zero++;
        else s_dl_overflow++;
        static int s_dl_skip_log = 0;
        if (s_dl_skip_log < 10) {
            fprintf(stderr, "{\"dl_draw_skip\":{\"vtxfmt\":%d,\"nverts\":%u,\"vert_size\":%u,"
                    "\"total_bytes\":%u,\"pos\":%u,\"size\":%u,\"prim\":0x%02X}}\n",
                    vtxfmt, (unsigned)nverts, vert_size, total_bytes, r->pos, r->size, prim_type);
            s_dl_skip_log++;
        }
        if (vert_size > 0) r->pos += total_bytes;
        return;
    }

    /* Begin draw call */
    s_dl_draw_count++;
    s_dl_vert_count += nverts;
    s_dl_total_draws++;
    s_dl_total_verts += nverts;

    /* TP_SKIP_DL_DRAWS=1: count DL draws (for regression gates) but skip actual
     * geometry submission.  Used by Phase 2 softpipe runs to let the J2D title-
     * screen overlays render quickly without rasterising the heavy 3-D room data. */
    {
        static int s_skip_dl = -1;
        if (s_skip_dl < 0) {
            const char* ev = getenv("TP_SKIP_DL_DRAWS");
            s_skip_dl = (ev && ev[0] == '1') ? 1 : 0;
        }
        if (s_skip_dl) { r->pos += total_bytes; return; }
    }

    pal_gx_begin(prim, (GXVtxFmt)vtxfmt, nverts);

    /* FAST PATH: Bulk-copy vertex data from DL into draw state buffer.
     * The per-component parse loop is O(nverts * nattrs) with function call
     * overhead per component. For large models (13000+ vertices), this takes
     * minutes. Instead, memcpy the raw DL data directly and let the TEV
     * flush path handle byte-swapping during vertex layout conversion.
     *
     * The raw DL vertex data is big-endian with the component layout matching
     * the current vertex descriptor — exactly what the draw state expects. */
    {
        GXDrawState* ds = &g_gx_state.draw;
        if (ds->vtx_data && ds->vtx_data_pos + total_bytes <= ds->vtx_data_size) {
            memcpy(ds->vtx_data + ds->vtx_data_pos, r->data + r->pos, total_bytes);
            ds->vtx_data_pos += total_bytes;
            ds->verts_written = nverts;
            ds->vtx_data_be = 1;  /* Mark as big-endian from DL bulk copy */
            s_dl_bulk_copy_ok++;
        } else {
            s_dl_bulk_copy_fail++;
        }
        r->pos += total_bytes;
    }

    /* End draw call — this flushes to bgfx via TEV pipeline */
    pal_gx_end();
}

/* ================================================================ */
/* Public API                                                       */
/* ================================================================ */

static int s_dl_call_count = 0;

int pal_gx_dl_get_draw_count() { return s_dl_draw_count; }
int pal_gx_dl_get_vert_count() { return s_dl_vert_count; }
int pal_gx_dl_get_call_count() { return s_dl_call_count; }
void pal_gx_dl_reset_counters() {
    s_dl_draw_count = 0;
    s_dl_vert_count = 0;
    s_dl_call_count = 0;
}

void pal_gx_dl_report_validation() {
    fprintf(stdout, "{\"dl_validation\":{\"total_draws\":%d,\"total_verts\":%d,\"calls\":%d,"
            "\"stride_zero\":%d,\"overflow\":%d,\"bulk_copy_ok\":%d,\"bulk_copy_fail\":%d}}\n",
            s_dl_total_draws, s_dl_total_verts, s_dl_call_count,
            s_dl_stride_zero, s_dl_overflow, s_dl_bulk_copy_ok, s_dl_bulk_copy_fail);
}

void pal_gx_call_display_list(const void* list, u32 nbytes) {
    if (!list || nbytes == 0) return;

    s_dl_call_count++;
    static int s_dl_call_log = 0;
    if (s_dl_call_log < 5) {
        fprintf(stderr, "{\"dl_call\":{\"count\":%d,\"ptr\":\"%p\",\"size\":%u,\"first_bytes\":[%u,%u,%u,%u]}}\n",
                s_dl_call_count, list, nbytes,
                ((const u8*)list)[0], ((const u8*)list)[1],
                nbytes > 2 ? ((const u8*)list)[2] : 0,
                nbytes > 3 ? ((const u8*)list)[3] : 0);
        s_dl_call_log++;
    }

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
            /* Index load: u16 index_value + u16 register_addr
             * Forward to pal_gx_fifo_load_indx for position/normal matrix
             * resolution from vertex arrays (Recipe 6). */
            u16 indx = dl_read_u16(&reader);
            u16 addr = dl_read_u16(&reader);
            pal_gx_fifo_load_indx(opcode, indx, addr);
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
