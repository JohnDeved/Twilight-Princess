/**
 * gx_state.cpp — GX state machine implementation (Step 5b)
 *
 * Captures GX rendering state from game function calls and provides
 * vertex data capture for bgfx draw submission.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "pal/gx/gx_state.h"
#include "pal/gx/gx_tev.h"

/* ================================================================ */
/* Global state machine instance                                    */
/* ================================================================ */

GXStateMachine g_gx_state;

/* Static vertex data buffer */
static u8 s_vtx_buf[GX_VTX_BUF_SIZE];

/* ================================================================ */
/* Initialize                                                       */
/* ================================================================ */

void pal_gx_state_init(void) {
    memset(&g_gx_state, 0, sizeof(g_gx_state));

    /* Default viewport (full EFB) */
    g_gx_state.vp_left = 0.0f;
    g_gx_state.vp_top = 0.0f;
    g_gx_state.vp_wd = 640.0f;
    g_gx_state.vp_ht = 480.0f;
    g_gx_state.vp_nearz = 0.0f;
    g_gx_state.vp_farz = 1.0f;

    /* Default scissor (full EFB) */
    g_gx_state.sc_wd = 640;
    g_gx_state.sc_ht = 480;

    /* Default blend */
    g_gx_state.blend_mode = GX_BM_NONE;
    g_gx_state.blend_src = GX_BL_ONE;
    g_gx_state.blend_dst = GX_BL_ZERO;

    /* Default Z mode */
    g_gx_state.z_compare_enable = GX_TRUE;
    g_gx_state.z_func = GX_LEQUAL;
    g_gx_state.z_update_enable = GX_TRUE;

    /* Default cull */
    g_gx_state.cull_mode = GX_CULL_BACK;

    /* Default color/alpha update */
    g_gx_state.color_update = GX_TRUE;
    g_gx_state.alpha_update = GX_TRUE;

    /* Default clear */
    g_gx_state.clear_color.r = 0;
    g_gx_state.clear_color.g = 0;
    g_gx_state.clear_color.b = 0;
    g_gx_state.clear_color.a = 255;
    g_gx_state.clear_z = 0x00FFFFFF;

    /* 1 TEV stage, passthrough */
    g_gx_state.num_tev_stages = 1;
    g_gx_state.tev_stages[0].color_d = GX_CC_RASC;
    g_gx_state.tev_stages[0].color_op = GX_TEV_ADD;
    g_gx_state.tev_stages[0].color_clamp = GX_TRUE;
    g_gx_state.tev_stages[0].alpha_d = GX_CA_RASA;
    g_gx_state.tev_stages[0].alpha_op = GX_TEV_ADD;
    g_gx_state.tev_stages[0].alpha_clamp = GX_TRUE;

    /* Identity projection */
    g_gx_state.proj_mtx[0][0] = 1.0f;
    g_gx_state.proj_mtx[1][1] = 1.0f;
    g_gx_state.proj_mtx[2][2] = 1.0f;
    g_gx_state.proj_mtx[3][3] = 1.0f;

    /* Identity position matrix 0 */
    g_gx_state.pos_mtx[0][0][0] = 1.0f;
    g_gx_state.pos_mtx[0][1][1] = 1.0f;
    g_gx_state.pos_mtx[0][2][2] = 1.0f;

    /* Draw state */
    g_gx_state.draw.vtx_data = s_vtx_buf;
    g_gx_state.draw.vtx_data_size = GX_VTX_BUF_SIZE;
    g_gx_state.draw.active = 0;

    /* 1 channel default */
    g_gx_state.num_chans = 1;
    g_gx_state.chan_ctrl[0].mat_color.r = 255;
    g_gx_state.chan_ctrl[0].mat_color.g = 255;
    g_gx_state.chan_ctrl[0].mat_color.b = 255;
    g_gx_state.chan_ctrl[0].mat_color.a = 255;
}

/* ================================================================ */
/* Vertex format                                                    */
/* ================================================================ */

void pal_gx_set_vtx_desc(GXAttr attr, GXAttrType type) {
    if ((unsigned)attr < GX_MAX_VTXATTR) {
        g_gx_state.vtx_desc[attr].type = type;
    }
}

void pal_gx_set_vtx_desc_v(const GXVtxDescList* list) {
    if (!list) return;
    while (list->attr != GX_VA_NULL) {
        pal_gx_set_vtx_desc(list->attr, list->type);
        list++;
    }
}

void pal_gx_clear_vtx_desc(void) {
    int i;
    for (i = 0; i < GX_MAX_VTXATTR; i++) {
        g_gx_state.vtx_desc[i].type = GX_NONE;
    }
}

void pal_gx_set_vtx_attr_fmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    if ((unsigned)fmt < GX_MAX_VTXFMT && (unsigned)attr < GX_MAX_VTXATTR) {
        g_gx_state.vtx_attr_fmt[fmt][attr].cnt = cnt;
        g_gx_state.vtx_attr_fmt[fmt][attr].comp_type = type;
        g_gx_state.vtx_attr_fmt[fmt][attr].frac = frac;
    }
}

void pal_gx_set_vtx_attr_fmt_v(GXVtxFmt fmt, const GXVtxAttrFmtList* list) {
    if (!list) return;
    while (list->attr != GX_VA_NULL) {
        pal_gx_set_vtx_attr_fmt(fmt, list->attr, list->cnt, list->type, list->frac);
        list++;
    }
}

void pal_gx_set_array(GXAttr attr, void* base, u8 stride) {
    if ((unsigned)attr < GX_MAX_VTXATTR) {
        g_gx_state.vtx_arrays[attr].base_ptr = base;
        g_gx_state.vtx_arrays[attr].stride = stride;
    }
}

/* ================================================================ */
/* TEV                                                              */
/* ================================================================ */

void pal_gx_set_tev_order(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].tex_coord = coord;
        g_gx_state.tev_stages[stage].tex_map = map;
        g_gx_state.tev_stages[stage].color_chan = color;
    }
}

void pal_gx_set_tev_color_in(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                              GXTevColorArg c, GXTevColorArg d) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].color_a = a;
        g_gx_state.tev_stages[stage].color_b = b;
        g_gx_state.tev_stages[stage].color_c = c;
        g_gx_state.tev_stages[stage].color_d = d;
    }
}

void pal_gx_set_tev_alpha_in(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                              GXTevAlphaArg c, GXTevAlphaArg d) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].alpha_a = a;
        g_gx_state.tev_stages[stage].alpha_b = b;
        g_gx_state.tev_stages[stage].alpha_c = c;
        g_gx_state.tev_stages[stage].alpha_d = d;
    }
}

void pal_gx_set_tev_color_op(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                              GXTevScale scale, GXBool clamp, GXTevRegID out) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].color_op = op;
        g_gx_state.tev_stages[stage].color_bias = bias;
        g_gx_state.tev_stages[stage].color_scale = scale;
        g_gx_state.tev_stages[stage].color_clamp = clamp;
        g_gx_state.tev_stages[stage].color_out = out;
    }
}

void pal_gx_set_tev_alpha_op(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                              GXTevScale scale, GXBool clamp, GXTevRegID out) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].alpha_op = op;
        g_gx_state.tev_stages[stage].alpha_bias = bias;
        g_gx_state.tev_stages[stage].alpha_scale = scale;
        g_gx_state.tev_stages[stage].alpha_clamp = clamp;
        g_gx_state.tev_stages[stage].alpha_out = out;
    }
}

void pal_gx_set_num_tev_stages(u8 n) {
    if (n <= GX_MAX_TEVSTAGE) {
        g_gx_state.num_tev_stages = n;
    }
}

void pal_gx_set_tev_color(GXTevRegID id, GXColor color) {
    if ((unsigned)id < GX_MAX_TEVREG) {
        g_gx_state.tev_regs[id] = color;
    }
}

void pal_gx_set_tev_k_color(GXTevKColorID id, GXColor color) {
    if ((unsigned)id < GX_MAX_TEVKREG) {
        g_gx_state.tev_kregs[id] = color;
    }
}

void pal_gx_set_tev_k_color_sel(GXTevStageID stage, GXTevKColorSel sel) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].k_color_sel = sel;
    }
}

void pal_gx_set_tev_k_alpha_sel(GXTevStageID stage, GXTevKAlphaSel sel) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].k_alpha_sel = sel;
    }
}

void pal_gx_set_tev_swap_mode(GXTevStageID stage, GXTevSwapSel ras, GXTevSwapSel tex) {
    if ((unsigned)stage < GX_MAX_TEVSTAGE) {
        g_gx_state.tev_stages[stage].ras_swap = ras;
        g_gx_state.tev_stages[stage].tex_swap = tex;
    }
}

/* ================================================================ */
/* Texture                                                          */
/* ================================================================ */

void pal_gx_init_tex_obj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                         GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap) {
    if (!obj) return;
    /* Store texture parameters in the GXTexObj's 32-byte dummy space.
     * Binary layout:
     *   offset 0-1: magic marker 'T','P' (identifies PC-initialized texobj)
     *   offset 2-7: reserved
     *   offset 8-15: image_ptr (void*, 8 bytes on 64-bit)
     *   offset 16-17: width (u16)
     *   offset 18-19: height (u16)
     *   offset 20-23: format (GXTexFmt, 4 bytes)
     *   offset 24: wrap_s (u8)
     *   offset 25: wrap_t (u8)
     *   offset 26: mipmap (u8)
     *   offset 27-31: reserved
     */
    memset(obj, 0, sizeof(GXTexObj));
    /* Store directly in the struct's memory for retrieval later */
    u8* data = (u8*)obj;
    /* Store a magic marker so we know this is a PC-initialized texobj */
    data[0] = 'T';
    data[1] = 'P';
    /* Pointer stored at offset 8 (8-byte aligned) */
    memcpy(data + 8, &image_ptr, sizeof(void*));
    /* Width/height at offset 16 */
    memcpy(data + 16, &width, 2);
    memcpy(data + 18, &height, 2);
    /* Format at offset 20 */
    memcpy(data + 20, &format, 4);
    /* Wrap modes at offset 24 */
    data[24] = (u8)wrap_s;
    data[25] = (u8)wrap_t;
    data[26] = mipmap;
}

void pal_gx_load_tex_obj(GXTexObj* obj, GXTexMapID id) {
    if (!obj || (unsigned)id >= GX_MAX_TEXMAP) return;

    GXTexBinding* bind = &g_gx_state.tex_bindings[id];
    u8* data = (u8*)obj;

    /* Check magic marker */
    if (data[0] == 'T' && data[1] == 'P') {
        memcpy(&bind->image_ptr, data + 8, sizeof(void*));
        memcpy(&bind->width, data + 16, 2);
        memcpy(&bind->height, data + 18, 2);
        memcpy(&bind->format, data + 20, 4);
        bind->wrap_s = (GXTexWrapMode)data[24];
        bind->wrap_t = (GXTexWrapMode)data[25];
        bind->mipmap = data[26];
        bind->valid = 1;
    }
}

void pal_gx_set_num_tex_gens(u8 n) {
    if (n <= GX_MAX_TEXCOORD) {
        g_gx_state.num_tex_gens = n;
    }
}

void pal_gx_set_tex_coord_gen(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src,
                               u32 mtx, GXBool normalize, u32 pt_mtx) {
    if ((unsigned)dst < GX_MAX_TEXCOORD) {
        g_gx_state.tex_gens[dst].type = func;
        g_gx_state.tex_gens[dst].src = src;
        g_gx_state.tex_gens[dst].mtx = mtx;
        g_gx_state.tex_gens[dst].normalize = normalize;
        g_gx_state.tex_gens[dst].pt_texmtx = pt_mtx;
    }
}

/* ================================================================ */
/* Transform                                                        */
/* ================================================================ */

void pal_gx_set_projection(const f32 mtx[4][4], GXProjectionType type) {
    memcpy(g_gx_state.proj_mtx, mtx, sizeof(g_gx_state.proj_mtx));
    g_gx_state.proj_type = type;
}

void pal_gx_load_pos_mtx_imm(const f32 mtx[3][4], u32 id) {
    /* GX matrix IDs are in multiples of 3 (0, 3, 6, 9, ...) to leave room
     * for optional derivative matrices in the hardware XF register layout.
     * Divide by 3 to get the storage slot index. */
    u32 slot = id / 3;
    if (slot < GX_MAX_POS_MTX) {
        memcpy(g_gx_state.pos_mtx[slot], mtx, 3 * 4 * sizeof(f32));
    }
}

void pal_gx_load_nrm_mtx_imm(const f32 mtx[3][4], u32 id) {
    u32 slot = id / 3;
    if (slot < GX_MAX_POS_MTX) {
        memcpy(g_gx_state.nrm_mtx[slot], mtx, 3 * 4 * sizeof(f32));
    }
}

void pal_gx_load_tex_mtx_imm(const f32 mtx[][4], u32 id, GXTexMtxType type) {
    /* Texture matrix IDs start at 30 and go in steps of 3 */
    u32 slot;
    (void)type;
    if (id >= 30) {
        slot = (id - 30) / 3;
    } else {
        slot = id / 3;
    }
    if (slot < GX_MAX_TEXCOORD) {
        memcpy(g_gx_state.tex_mtx[slot], mtx, 3 * 4 * sizeof(f32));
    }
}

void pal_gx_set_current_mtx(u32 id) {
    g_gx_state.current_pos_mtx = id / 3;
}

void pal_gx_set_viewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    g_gx_state.vp_left = left;
    g_gx_state.vp_top = top;
    g_gx_state.vp_wd = wd;
    g_gx_state.vp_ht = ht;
    g_gx_state.vp_nearz = nearz;
    g_gx_state.vp_farz = farz;
}

void pal_gx_set_scissor(u32 left, u32 top, u32 wd, u32 ht) {
    g_gx_state.sc_left = left;
    g_gx_state.sc_top = top;
    g_gx_state.sc_wd = wd;
    g_gx_state.sc_ht = ht;
}

/* ================================================================ */
/* Pixel / Blend / Z                                                */
/* ================================================================ */

void pal_gx_set_blend_mode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    g_gx_state.blend_mode = type;
    g_gx_state.blend_src = src;
    g_gx_state.blend_dst = dst;
    g_gx_state.blend_logic_op = op;
}

void pal_gx_set_z_mode(GXBool compare, GXCompare func, GXBool update) {
    g_gx_state.z_compare_enable = compare;
    g_gx_state.z_func = func;
    g_gx_state.z_update_enable = update;
}

void pal_gx_set_alpha_compare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    g_gx_state.alpha_comp0 = comp0;
    g_gx_state.alpha_ref0 = ref0;
    g_gx_state.alpha_op = op;
    g_gx_state.alpha_comp1 = comp1;
    g_gx_state.alpha_ref1 = ref1;
}

void pal_gx_set_cull_mode(GXCullMode mode) {
    g_gx_state.cull_mode = mode;
}

void pal_gx_set_color_update(GXBool enable) {
    g_gx_state.color_update = enable;
}

void pal_gx_set_alpha_update(GXBool enable) {
    g_gx_state.alpha_update = enable;
}

void pal_gx_set_fog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    g_gx_state.fog_type = type;
    g_gx_state.fog_startz = startz;
    g_gx_state.fog_endz = endz;
    g_gx_state.fog_nearz = nearz;
    g_gx_state.fog_farz = farz;
    g_gx_state.fog_color = color;
}

/* ================================================================ */
/* Channel / Lighting                                               */
/* ================================================================ */

void pal_gx_set_num_chans(u8 n) {
    g_gx_state.num_chans = n;
}

void pal_gx_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                          u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    u32 idx;
    switch (chan) {
        case GX_COLOR0:   idx = 0; break;
        case GX_COLOR1:   idx = 1; break;
        case GX_ALPHA0:   idx = 2; break;
        case GX_ALPHA1:   idx = 3; break;
        case GX_COLOR0A0: idx = 0; break;
        case GX_COLOR1A1: idx = 1; break;
        default: return;
    }
    if (idx < 4) {
        g_gx_state.chan_ctrl[idx].enable = enable;
        g_gx_state.chan_ctrl[idx].amb_src = amb_src;
        g_gx_state.chan_ctrl[idx].mat_src = mat_src;
        g_gx_state.chan_ctrl[idx].light_mask = light_mask;
        g_gx_state.chan_ctrl[idx].diff_fn = diff_fn;
        g_gx_state.chan_ctrl[idx].attn_fn = attn_fn;
    }
}

void pal_gx_set_chan_amb_color(GXChannelID chan, GXColor color) {
    u32 idx;
    switch (chan) {
        case GX_COLOR0:   case GX_COLOR0A0: idx = 0; break;
        case GX_COLOR1:   case GX_COLOR1A1: idx = 1; break;
        default: return;
    }
    if (idx < 4) {
        g_gx_state.chan_ctrl[idx].amb_color = color;
    }
}

void pal_gx_set_chan_mat_color(GXChannelID chan, GXColor color) {
    u32 idx;
    switch (chan) {
        case GX_COLOR0:   case GX_COLOR0A0: idx = 0; break;
        case GX_COLOR1:   case GX_COLOR1A1: idx = 1; break;
        default: return;
    }
    if (idx < 4) {
        g_gx_state.chan_ctrl[idx].mat_color = color;
    }
}

/* ================================================================ */
/* Draw — vertex data capture                                       */
/* ================================================================ */

void pal_gx_begin(GXPrimitive prim, GXVtxFmt fmt, u16 nverts) {
    g_gx_state.draw.prim_type = prim;
    g_gx_state.draw.vtx_fmt = fmt;
    g_gx_state.draw.nverts = nverts;
    g_gx_state.draw.verts_written = 0;
    g_gx_state.draw.vtx_data_pos = 0;
    g_gx_state.draw.active = 1;
}

void pal_gx_end(void) {
    if (!g_gx_state.draw.active) return;

    /* Derive vertex count from data written */
    if (g_gx_state.draw.vtx_data_pos > 0 && g_gx_state.draw.verts_written == 0) {
        g_gx_state.draw.verts_written = g_gx_state.draw.nverts;
    }

    /* Track statistics */
    g_gx_state.draw_calls++;
    g_gx_state.total_verts += g_gx_state.draw.verts_written;

    /* Flush draw call to bgfx via TEV pipeline */
    pal_tev_flush_draw();

    g_gx_state.draw.active = 0;
}

/* Vertex data write helpers — append to vertex buffer */
static void vtx_write(const void* data, u32 size) {
    GXDrawState* ds = &g_gx_state.draw;
    if (!ds->active || !ds->vtx_data || !data) return;
    if (ds->vtx_data_pos + size <= ds->vtx_data_size) {
        memcpy(ds->vtx_data + ds->vtx_data_pos, data, size);
        ds->vtx_data_pos += size;
    }
}

void pal_gx_write_vtx_f32(f32 val) { vtx_write(&val, sizeof(val)); }
void pal_gx_write_vtx_u32(u32 val) { vtx_write(&val, sizeof(val)); }
void pal_gx_write_vtx_u16(u16 val) { vtx_write(&val, sizeof(val)); }
void pal_gx_write_vtx_u8(u8 val) { vtx_write(&val, sizeof(val)); }
void pal_gx_write_vtx_s16(s16 val) { vtx_write(&val, sizeof(val)); }
void pal_gx_write_vtx_s8(s8 val) { vtx_write(&val, sizeof(val)); }
void pal_gx_write_vtx_s32(s32 val) { vtx_write(&val, sizeof(val)); }

/* ================================================================ */
/* Clear                                                            */
/* ================================================================ */

void pal_gx_set_copy_clear(GXColor color, u32 z) {
    g_gx_state.clear_color = color;
    g_gx_state.clear_z = z;
}

/* ================================================================ */
/* Query                                                            */
/* ================================================================ */

void pal_gx_get_viewport_v(f32* vp) {
    if (!vp) return;
    vp[0] = g_gx_state.vp_left;
    vp[1] = g_gx_state.vp_top;
    vp[2] = g_gx_state.vp_wd;
    vp[3] = g_gx_state.vp_ht;
    vp[4] = g_gx_state.vp_nearz;
    vp[5] = g_gx_state.vp_farz;
}

void pal_gx_get_projection_v(f32* p) {
    if (!p) return;
    /* GX projection format: type + 6 params */
    p[0] = (f32)g_gx_state.proj_type;
    p[1] = g_gx_state.proj_mtx[0][0];
    p[2] = g_gx_state.proj_mtx[1][1];
    p[3] = g_gx_state.proj_mtx[2][2];
    p[4] = g_gx_state.proj_mtx[2][3];
    p[5] = g_gx_state.proj_mtx[3][2];
    p[6] = g_gx_state.proj_mtx[3][3];
}

void pal_gx_get_scissor(u32* left, u32* top, u32* wd, u32* ht) {
    if (left) *left = g_gx_state.sc_left;
    if (top) *top = g_gx_state.sc_top;
    if (wd) *wd = g_gx_state.sc_wd;
    if (ht) *ht = g_gx_state.sc_ht;
}

/* ================================================================ */
/* Vertex stride calculation                                        */
/* ================================================================ */

static u32 comp_size(GXCompType type) {
    /* Note: GXCompType numeric types overlap with color types
     * (GX_U8=0 == GX_RGB565=0, GX_S8=1 == GX_RGB8=1, etc.)
     * This function handles the numeric interpretation.
     * Use color_comp_size() for color attribute types. */
    switch (type) {
        case GX_U8:  return 1; /* also GX_RGB565=0, but numeric path */
        case GX_S8:  return 1; /* also GX_RGB8=1 */
        case GX_U16: return 2; /* also GX_RGBX8=2 */
        case GX_S16: return 2; /* also GX_RGBA4=3 */
        case GX_F32: return 4; /* also GX_RGBA6=4 */
        case GX_RGBA8: return 4; /* =5, unique to color */
        default: return 0;
    }
}

/* Color component type size - separate function because GXCompType enum
 * values 0-4 are shared between numeric types (U8/S8/U16/S16/F32) and
 * color types (RGB565/RGB8/RGBX8/RGBA4/RGBA6). The caller must know
 * whether the attribute is a color (CLR0/CLR1) to dispatch correctly. */
static u32 color_comp_size(GXCompType type) {
    switch (type) {
        case GX_RGB565: return 2;  /* =0 */
        case GX_RGB8:   return 3;  /* =1 */
        case GX_RGBX8:  return 4;  /* =2 */
        case GX_RGBA4:  return 2;  /* =3 */
        case GX_RGBA6:  return 3;  /* =4 */
        case GX_RGBA8:  return 4;  /* =5 */
        default: return 0;
    }
}

static u32 comp_count_pos(GXCompCnt cnt) {
    return (cnt == GX_POS_XY) ? 2 : 3;
}

static u32 comp_count_nrm(GXCompCnt cnt) {
    /* NRM_XYZ=0, NRM_NBT=1, NRM_NBT3=2 */
    return (cnt == GX_NRM_XYZ) ? 3 : 9;
}

static u32 comp_count_tex(GXCompCnt cnt) {
    return (cnt == GX_TEX_S) ? 1 : 2;
}

u32 pal_gx_calc_vtx_stride(GXVtxFmt fmt) {
    u32 stride = 0;
    int i;

    if ((unsigned)fmt >= GX_MAX_VTXFMT) return 0;

    for (i = 0; i < GX_MAX_VTXATTR; i++) {
        GXAttrType desc = g_gx_state.vtx_desc[i].type;
        if (desc == GX_NONE) continue;

        if (desc == GX_INDEX8) {
            stride += 1;
        } else if (desc == GX_INDEX16) {
            stride += 2;
        } else if (desc == GX_DIRECT) {
            GXVtxAttrFmtEntry* af = &g_gx_state.vtx_attr_fmt[fmt][i];
            u32 n_comps = 0;

            if (i == GX_VA_POS) {
                n_comps = comp_count_pos(af->cnt);
            } else if (i == GX_VA_NRM || i == GX_VA_NBT) {
                n_comps = comp_count_nrm(af->cnt);
            } else if (i == GX_VA_CLR0 || i == GX_VA_CLR1) {
                /* Color is special -- size depends on color type format */
                stride += color_comp_size(af->comp_type);
                continue;
            } else if (i >= GX_VA_TEX0 && i <= GX_VA_TEX7) {
                n_comps = comp_count_tex(af->cnt);
            } else if (i >= GX_VA_PNMTXIDX && i <= GX_VA_TEX7MTXIDX) {
                /* Matrix index — 1 byte */
                stride += 1;
                continue;
            }

            stride += n_comps * comp_size(af->comp_type);
        }
    }

    return stride;
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
