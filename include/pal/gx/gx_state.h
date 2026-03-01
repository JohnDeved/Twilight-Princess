/**
 * gx_state.h - GX state machine for PC port (Step 5b)
 *
 * Captures GX rendering state from game-level function calls and tracks
 * vertex format, TEV stages, texture bindings, matrix state, blend mode,
 * viewport/scissor, etc. This state is flushed to bgfx at draw time.
 *
 * The GX pipeline works as:
 *   1. Set vertex format (GXSetVtxDesc, GXSetVtxAttrFmt)
 *   2. Set TEV/texture/material state
 *   3. Load matrices
 *   4. GXBegin(prim, vtxfmt, nverts) - start vertex submission
 *   5. GXPosition / GXNormal / GXColor / GXTexCoord - vertex data
 *   6. GXEnd() - submit draw
 *   7. GXCopyDisp - present frame
 */

#ifndef PAL_GX_STATE_H
#define PAL_GX_STATE_H

#include "dolphin/types.h"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXStruct.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ */
/* Constants                                                        */
/* ================================================================ */

#define GX_MAX_VTXATTR    26  /* GX_VA_PNMTXIDX through GX_VA_NBT */
#define GX_MAX_VTXFMT      8  /* GX_VTXFMT0 through GX_VTXFMT7    */
#define GX_MAX_TEXMAP      8  /* GX_TEXMAP0 through GX_TEXMAP7     */
#define GX_MAX_TEVSTAGE   16  /* GX_TEVSTAGE0 through GX_TEVSTAGE15 */
#define GX_MAX_TEXCOORD    8  /* GX_TEXCOORD0 through GX_TEXCOORD7 */
#define GX_MAX_COLOR_CHAN  2  /* GX_COLOR0, GX_COLOR1              */
#define GX_MAX_LIGHT       8  /* GX_LIGHT0 through GX_LIGHT7       */
#define GX_MAX_TEVKREG     4  /* Konst color registers             */
#define GX_MAX_TEVREG      4  /* TEV color registers               */

/* Maximum vertices per draw call (64KB should be plenty) */
#define GX_VTX_BUF_SIZE   (256 * 1024)

/* Maximum position matrices */
#define GX_MAX_POS_MTX    10

/* ================================================================ */
/* Vertex attribute descriptor (per-attribute)                      */
/* ================================================================ */

typedef struct {
    GXAttrType type;       /* GX_NONE, GX_DIRECT, GX_INDEX8, GX_INDEX16 */
} GXVtxDescEntry;

/* ================================================================ */
/* Vertex attribute format (per vtxfmt, per attribute)              */
/* ================================================================ */

typedef struct {
    GXCompCnt  cnt;        /* Component count */
    GXCompType comp_type;  /* Component type */
    u8         frac;       /* Fraction bits for fixed-point */
} GXVtxAttrFmtEntry;

/* ================================================================ */
/* TEV stage state                                                  */
/* ================================================================ */

typedef struct {
    /* Order */
    GXTexCoordID tex_coord;
    GXTexMapID   tex_map;
    GXChannelID  color_chan;

    /* Color combiner inputs */
    GXTevColorArg color_a, color_b, color_c, color_d;
    /* Color combiner operation */
    GXTevOp      color_op;
    GXTevBias    color_bias;
    GXTevScale   color_scale;
    GXBool       color_clamp;
    GXTevRegID   color_out;

    /* Alpha combiner inputs */
    GXTevAlphaArg alpha_a, alpha_b, alpha_c, alpha_d;
    /* Alpha combiner operation */
    GXTevOp      alpha_op;
    GXTevBias    alpha_bias;
    GXTevScale   alpha_scale;
    GXBool       alpha_clamp;
    GXTevRegID   alpha_out;

    /* Konst color/alpha selection */
    GXTevKColorSel k_color_sel;
    GXTevKAlphaSel k_alpha_sel;

    /* Swap mode */
    GXTevSwapSel ras_swap;
    GXTevSwapSel tex_swap;
} GXTevStage;

/* ================================================================ */
/* Texture coordinate generation                                    */
/* ================================================================ */

typedef struct {
    GXTexGenType type;
    GXTexGenSrc  src;
    u32          mtx;
    GXBool       normalize;
    u32          pt_texmtx;
} GXTexGenState;

/* ================================================================ */
/* Texture binding state                                            */
/* ================================================================ */

typedef struct {
    void*         image_ptr;
    u16           width;
    u16           height;
    GXTexFmt      format;
    GXTexWrapMode wrap_s;
    GXTexWrapMode wrap_t;
    u8            mipmap;
    GXTexFilter   min_filt;
    GXTexFilter   mag_filt;
    f32           min_lod;
    f32           max_lod;
    f32           lod_bias;
    u8            valid;      /* 1 if this slot has been loaded */
} GXTexBinding;

/* ================================================================ */
/* Channel (lighting) control                                       */
/* ================================================================ */

typedef struct {
    GXBool      enable;
    GXColorSrc  amb_src;
    GXColorSrc  mat_src;
    u32         light_mask;
    GXDiffuseFn diff_fn;
    GXAttnFn    attn_fn;
    GXColor     amb_color;
    GXColor     mat_color;
} GXChanCtrl;

/* ================================================================ */
/* Vertex array pointer (for indexed drawing)                       */
/* ================================================================ */

typedef struct {
    void* base_ptr;
    u8    stride;
} GXArrayState;

/* ================================================================ */
/* Draw state (active during GXBegin..GXEnd)                        */
/* ================================================================ */

typedef struct {
    GXPrimitive prim_type;
    GXVtxFmt   vtx_fmt;
    u16         nverts;
    u16         verts_written;

    /* Vertex data capture buffer */
    u8*         vtx_data;
    u32         vtx_data_pos;
    u32         vtx_data_size;

    u8          active;  /* 1 while between GXBegin and GXEnd */
} GXDrawState;

/* ================================================================ */
/* Complete GX state machine                                        */
/* ================================================================ */

typedef struct {
    /* Vertex descriptor — which attributes are active */
    GXVtxDescEntry vtx_desc[GX_MAX_VTXATTR];

    /* Vertex attribute format — per vtxfmt, per attribute */
    GXVtxAttrFmtEntry vtx_attr_fmt[GX_MAX_VTXFMT][GX_MAX_VTXATTR];

    /* Vertex array pointers (for indexed drawing) */
    GXArrayState vtx_arrays[GX_MAX_VTXATTR];

    /* TEV stages */
    GXTevStage tev_stages[GX_MAX_TEVSTAGE];
    u8         num_tev_stages;

    /* TEV registers (color constants) */
    GXColor    tev_regs[GX_MAX_TEVREG];    /* GX_TEVPREV, GX_TEVREG0-2 */
    GXColor    tev_kregs[GX_MAX_TEVKREG];  /* GX_KCOLOR0-3 */

    /* Texture bindings */
    GXTexBinding tex_bindings[GX_MAX_TEXMAP];

    /* Texture coordinate generation */
    GXTexGenState tex_gens[GX_MAX_TEXCOORD];
    u8            num_tex_gens;

    /* Channel (lighting) state */
    GXChanCtrl chan_ctrl[4]; /* COLOR0, COLOR1, ALPHA0, ALPHA1 */
    u8         num_chans;

    /* Matrices */
    f32 proj_mtx[4][4];
    u32 proj_type;  /* GXProjectionType */
    f32 pos_mtx[GX_MAX_POS_MTX][3][4];
    f32 nrm_mtx[GX_MAX_POS_MTX][3][4];
    f32 tex_mtx[GX_MAX_TEXCOORD][3][4];
    u32 current_pos_mtx;

    /* Viewport */
    f32 vp_left, vp_top, vp_wd, vp_ht, vp_nearz, vp_farz;

    /* Scissor */
    u32 sc_left, sc_top, sc_wd, sc_ht;

    /* Blend mode */
    GXBlendMode   blend_mode;
    GXBlendFactor blend_src;
    GXBlendFactor blend_dst;
    GXLogicOp     blend_logic_op;

    /* Z mode */
    GXBool    z_compare_enable;
    GXCompare z_func;
    GXBool    z_update_enable;

    /* Alpha compare */
    GXCompare alpha_comp0;
    u8        alpha_ref0;
    GXAlphaOp alpha_op;
    GXCompare alpha_comp1;
    u8        alpha_ref1;

    /* Cull mode */
    GXCullMode cull_mode;

    /* Color/alpha update */
    GXBool color_update;
    GXBool alpha_update;

    /* Fog */
    GXFogType fog_type;
    f32       fog_startz, fog_endz, fog_nearz, fog_farz;
    GXColor   fog_color;

    /* Clear */
    GXColor clear_color;
    u32     clear_z;

    /* Current draw state */
    GXDrawState draw;

    /* Statistics */
    u32 draw_calls;
    u32 total_verts;
} GXStateMachine;

/* ================================================================ */
/* Global state machine instance                                    */
/* ================================================================ */

extern GXStateMachine g_gx_state;

/* ================================================================ */
/* State machine API                                                */
/* ================================================================ */

/* Initialize state machine to defaults */
void pal_gx_state_init(void);

/* Vertex format */
void pal_gx_set_vtx_desc(GXAttr attr, GXAttrType type);
void pal_gx_set_vtx_desc_v(const GXVtxDescList* list);
void pal_gx_clear_vtx_desc(void);
void pal_gx_set_vtx_attr_fmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac);
void pal_gx_set_vtx_attr_fmt_v(GXVtxFmt fmt, const GXVtxAttrFmtList* list);
void pal_gx_set_array(GXAttr attr, void* base, u8 stride);

/* TEV */
void pal_gx_set_tev_order(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color);
void pal_gx_set_tev_color_in(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d);
void pal_gx_set_tev_alpha_in(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d);
void pal_gx_set_tev_color_op(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out);
void pal_gx_set_tev_alpha_op(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out);
void pal_gx_set_num_tev_stages(u8 n);
void pal_gx_set_tev_color(GXTevRegID id, GXColor color);
void pal_gx_set_tev_k_color(GXTevKColorID id, GXColor color);
void pal_gx_set_tev_k_color_sel(GXTevStageID stage, GXTevKColorSel sel);
void pal_gx_set_tev_k_alpha_sel(GXTevStageID stage, GXTevKAlphaSel sel);
void pal_gx_set_tev_swap_mode(GXTevStageID stage, GXTevSwapSel ras, GXTevSwapSel tex);

/* Texture */
void pal_gx_init_tex_obj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                         GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap);
void pal_gx_load_tex_obj(GXTexObj* obj, GXTexMapID id);
void pal_gx_set_tex_img(GXTexMapID id, void* image_ptr, u16 width, u16 height, GXTexFmt format);
void pal_gx_set_tex_lookup_mode(GXTexMapID id, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                                GXTexFilter min_filt, GXTexFilter mag_filt,
                                f32 min_lod, f32 max_lod, f32 lod_bias);
void pal_gx_set_num_tex_gens(u8 n);
void pal_gx_set_tex_coord_gen(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 pt_mtx);

/* Transform */
void pal_gx_set_projection(const f32 mtx[4][4], GXProjectionType type);
void pal_gx_load_pos_mtx_imm(const f32 mtx[3][4], u32 id);
void pal_gx_load_nrm_mtx_imm(const f32 mtx[3][4], u32 id);
void pal_gx_load_tex_mtx_imm(const f32 mtx[][4], u32 id, GXTexMtxType type);
void pal_gx_set_current_mtx(u32 id);
void pal_gx_set_viewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);
void pal_gx_set_scissor(u32 left, u32 top, u32 wd, u32 ht);

/* Pixel / Blend / Z */
void pal_gx_set_blend_mode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op);
void pal_gx_set_z_mode(GXBool compare, GXCompare func, GXBool update);
void pal_gx_set_alpha_compare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1);
void pal_gx_set_cull_mode(GXCullMode mode);
void pal_gx_set_color_update(GXBool enable);
void pal_gx_set_alpha_update(GXBool enable);
void pal_gx_set_fog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color);

/* Channel / lighting */
void pal_gx_set_num_chans(u8 n);
void pal_gx_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                          u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn);
void pal_gx_set_chan_amb_color(GXChannelID chan, GXColor color);
void pal_gx_set_chan_mat_color(GXChannelID chan, GXColor color);

/* Draw */
void pal_gx_begin(GXPrimitive prim, GXVtxFmt fmt, u16 nverts);
void pal_gx_end(void);
void pal_gx_write_vtx_f32(f32 val);
void pal_gx_write_vtx_u32(u32 val);
void pal_gx_write_vtx_u16(u16 val);
void pal_gx_write_vtx_u8(u8 val);
void pal_gx_write_vtx_s16(s16 val);
void pal_gx_write_vtx_s8(s8 val);
void pal_gx_write_vtx_s32(s32 val);

/* Clear */
void pal_gx_set_copy_clear(GXColor color, u32 z);

/* Query */
void pal_gx_get_viewport_v(f32* vp);
void pal_gx_get_projection_v(f32* p);
void pal_gx_get_scissor(u32* left, u32* top, u32* wd, u32* ht);

/* Calculate per-vertex stride from current vertex descriptor + format */
u32 pal_gx_calc_vtx_stride(GXVtxFmt fmt);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_STATE_H */
