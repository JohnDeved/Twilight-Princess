/**
 * gx_tev.cpp - TEV → bgfx shader pipeline (Step 5c)
 *
 * Maps GX TEV combiner configurations to bgfx shader programs.
 * Handles the 5 common TEV presets (PASSCLR, REPLACE, MODULATE, BLEND, DECAL)
 * and flushes GX draw state to bgfx draw calls.
 *
 * The pipeline:
 *   1. Game sets TEV state via GX calls → captured in gx_state
 *   2. Game submits vertices via GXBegin/GXEnd → captured in vertex buffer
 *   3. pal_tev_flush_draw() converts captured state to bgfx draw call:
 *      a. Select shader program based on TEV config
 *      b. Build bgfx vertex layout from GX vertex descriptor
 *      c. Upload vertex data as transient buffer
 *      d. Convert GX primitive to bgfx primitive
 *      e. Set render state (blend, depth, cull)
 *      f. Set transform matrices
 *      g. Submit draw call
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <bgfx/bgfx.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "pal/gx/gx_state.h"
#include "pal/gx/gx_tev.h"
#include "pal/gx/gx_texture.h"
#include "pal/gx/gx_stub_tracker.h"
#include "revolution/gx/GXEnum.h"
}

/* ================================================================ */
/* Embedded shader data                                             */
/* ================================================================ */

/* Include pre-compiled shader binaries.
 * These are checked-in and generated from .sc source files using bgfx shaderc.
 * No build-time shader compilation needed (BGFX_BUILD_TOOLS=OFF). */
#include "pal/gx/gx_tev_shaders_precompiled.h"

/* Shader binary selection based on renderer type */
struct ShaderBinEntry {
    const uint8_t* data;
    uint32_t       size;
};

/* Helper to select the correct shader binary for the active renderer */
static ShaderBinEntry select_shader_bin(
    bgfx::RendererType::Enum renderer,
    const uint8_t* glsl, uint32_t glsl_size,
    const uint8_t* essl, uint32_t essl_size,
    const uint8_t* spv,  uint32_t spv_size)
{
    ShaderBinEntry entry = {NULL, 0};
    switch (renderer) {
    case bgfx::RendererType::OpenGLES:
        entry.data = essl; entry.size = essl_size; break;
    case bgfx::RendererType::OpenGL:
        entry.data = glsl; entry.size = glsl_size; break;
    case bgfx::RendererType::Vulkan:
        entry.data = spv; entry.size = spv_size; break;
    case bgfx::RendererType::Noop:
        /* Noop doesn't need real shaders, use GLSL as dummy */
        entry.data = glsl; entry.size = glsl_size; break;
    default:
        /* Try SPIR-V as fallback */
        entry.data = spv; entry.size = spv_size; break;
    }
    return entry;
}

static bgfx::ShaderHandle create_shader_from_bin(const uint8_t* data, uint32_t size) {
    if (!data || size == 0) return BGFX_INVALID_HANDLE;
    const bgfx::Memory* mem = bgfx::copy(data, size);
    return bgfx::createShader(mem);
}

/* Fragment shader binary table */
struct FSShaderBins {
    const uint8_t *glsl, *essl, *spv;
    uint32_t glsl_size, essl_size, spv_size;
};

static const FSShaderBins s_fs_bins[GX_TEV_SHADER_COUNT] = {
    { fs_gx_passclr_glsl,  fs_gx_passclr_essl,  fs_gx_passclr_spv,
      sizeof(fs_gx_passclr_glsl), sizeof(fs_gx_passclr_essl), sizeof(fs_gx_passclr_spv) },
    { fs_gx_replace_glsl,  fs_gx_replace_essl,  fs_gx_replace_spv,
      sizeof(fs_gx_replace_glsl), sizeof(fs_gx_replace_essl), sizeof(fs_gx_replace_spv) },
    { fs_gx_modulate_glsl, fs_gx_modulate_essl, fs_gx_modulate_spv,
      sizeof(fs_gx_modulate_glsl), sizeof(fs_gx_modulate_essl), sizeof(fs_gx_modulate_spv) },
    { fs_gx_blend_glsl,    fs_gx_blend_essl,    fs_gx_blend_spv,
      sizeof(fs_gx_blend_glsl), sizeof(fs_gx_blend_essl), sizeof(fs_gx_blend_spv) },
    { fs_gx_decal_glsl,    fs_gx_decal_essl,    fs_gx_decal_spv,
      sizeof(fs_gx_decal_glsl), sizeof(fs_gx_decal_essl), sizeof(fs_gx_decal_spv) },
};

static const char* s_fs_names[GX_TEV_SHADER_COUNT] = {
    "fs_gx_passclr",
    "fs_gx_replace",
    "fs_gx_modulate",
    "fs_gx_blend",
    "fs_gx_decal",
};

/* ================================================================ */
/* State                                                            */
/* ================================================================ */

static bgfx::ProgramHandle s_programs[GX_TEV_SHADER_COUNT];
static bgfx::UniformHandle s_tex_uniform;
static bgfx::UniformHandle s_tev_reg0_uniform;
static bgfx::UniformHandle s_tev_reg1_uniform;
static int s_tev_ready = 0;

/* Texture cache: decoded RGBA8 textures cached as bgfx handles */
#define TEV_TEX_CACHE_SIZE 256
struct TexCacheEntry {
    void*                 src_ptr;
    uint16_t              width;
    uint16_t              height;
    GXTexFmt              format;
    bgfx::TextureHandle   handle;
    uint8_t               valid;
};
static TexCacheEntry s_tex_cache[TEV_TEX_CACHE_SIZE];
static uint32_t s_tex_cache_next = 0;

/* ================================================================ */
/* Texture cache                                                    */
/* ================================================================ */

static bgfx::TextureHandle tex_cache_get(void* ptr, uint16_t w, uint16_t h, GXTexFmt fmt) {
    /* Search cache */
    for (uint32_t i = 0; i < TEV_TEX_CACHE_SIZE; i++) {
        if (s_tex_cache[i].valid && s_tex_cache[i].src_ptr == ptr &&
            s_tex_cache[i].width == w && s_tex_cache[i].height == h &&
            s_tex_cache[i].format == fmt) {
            return s_tex_cache[i].handle;
        }
    }
    return BGFX_INVALID_HANDLE;
}

static void tex_cache_put(void* ptr, uint16_t w, uint16_t h, GXTexFmt fmt, bgfx::TextureHandle handle) {
    uint32_t slot = s_tex_cache_next % TEV_TEX_CACHE_SIZE;
    /* Evict old entry */
    if (s_tex_cache[slot].valid) {
        bgfx::destroy(s_tex_cache[slot].handle);
    }
    s_tex_cache[slot].src_ptr = ptr;
    s_tex_cache[slot].width = w;
    s_tex_cache[slot].height = h;
    s_tex_cache[slot].format = fmt;
    s_tex_cache[slot].handle = handle;
    s_tex_cache[slot].valid = 1;
    s_tex_cache_next++;
}

/* Decode GX texture and upload to bgfx */
static bgfx::TextureHandle upload_gx_texture(const GXTexBinding* binding) {
    if (!binding || !binding->valid || !binding->image_ptr)
        return BGFX_INVALID_HANDLE;

    /* Check cache first */
    bgfx::TextureHandle cached = tex_cache_get(
        binding->image_ptr, binding->width, binding->height, binding->format);
    if (bgfx::isValid(cached))
        return cached;

    /* Decode GX tiled texture to linear RGBA8 */
    uint32_t rgba_size = (uint32_t)binding->width * (uint32_t)binding->height * 4;
    uint8_t* rgba_data = (uint8_t*)malloc(rgba_size);
    if (!rgba_data)
        return BGFX_INVALID_HANDLE;

    u32 decoded = pal_gx_decode_texture(
        binding->image_ptr, rgba_data,
        binding->width, binding->height,
        binding->format,
        NULL, 0);

    if (decoded == 0) {
        free(rgba_data);
        return BGFX_INVALID_HANDLE;
    }

    /* Upload to bgfx */
    const bgfx::Memory* mem = bgfx::copy(rgba_data, rgba_size);
    free(rgba_data);

    bgfx::TextureHandle tex = bgfx::createTexture2D(
        binding->width, binding->height,
        false, /* hasMips */
        1,     /* numLayers */
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        mem);

    if (bgfx::isValid(tex)) {
        tex_cache_put(binding->image_ptr, binding->width, binding->height,
                      binding->format, tex);
    }
    return tex;
}

/* ================================================================ */
/* TEV preset detection                                             */
/* ================================================================ */

/**
 * Detect which TEV preset the current state matches.
 * Returns shader ID (0-4) or -1 for unknown configs.
 */
static int detect_tev_preset(void) {
    const GXTevStage* s0 = &g_gx_state.tev_stages[0];

    /* Check if any texture is bound for stage 0 */
    int has_texture = (s0->tex_map != GX_TEXMAP_NULL && s0->tex_map < GX_MAX_TEXMAP &&
                       g_gx_state.tex_bindings[s0->tex_map].valid);

    /* Check if the TEV combiner actually references texture color */
    int uses_texc = (s0->color_a == GX_CC_TEXC || s0->color_b == GX_CC_TEXC ||
                     s0->color_c == GX_CC_TEXC || s0->color_d == GX_CC_TEXC);

    if (g_gx_state.num_tev_stages == 0 || g_gx_state.num_tev_stages == 1) {
        /* If combiner doesn't reference texture, treat as PASSCLR.
         * clearEfb uses TEV (ZERO,ZERO,ZERO,C0) which outputs constant C0 regardless
         * of bound texture — no texture lookup needed. */
        if (!uses_texc || !has_texture) {
            return GX_TEV_SHADER_PASSCLR;
        }

        /* REPLACE: color_a=ZERO, color_b=ZERO, color_c=ZERO, color_d=TEXC */
        if (s0->color_a == GX_CC_ZERO && s0->color_b == GX_CC_ZERO &&
            s0->color_c == GX_CC_ZERO && s0->color_d == GX_CC_TEXC) {
            return GX_TEV_SHADER_REPLACE;
        }

        /* MODULATE: typical GX_MODULATE pattern */
        if ((s0->color_a == GX_CC_ZERO && s0->color_b == GX_CC_TEXC &&
             s0->color_c == GX_CC_RASC && s0->color_d == GX_CC_ZERO) ||
            (s0->color_a == GX_CC_ZERO && s0->color_b == GX_CC_RASC &&
             s0->color_c == GX_CC_TEXC && s0->color_d == GX_CC_ZERO)) {
            return GX_TEV_SHADER_MODULATE;
        }

        /* DECAL: blend texture with vertex color using texture alpha */
        if (s0->color_a == GX_CC_RASC && s0->color_b == GX_CC_TEXC &&
            s0->color_c == GX_CC_TEXA && s0->color_d == GX_CC_ZERO) {
            return GX_TEV_SHADER_DECAL;
        }

        /* Has texture but no matching preset → use MODULATE as default */
        return GX_TEV_SHADER_MODULATE;
    }

    /* Multi-stage TEV — detect common patterns */
    if (has_texture && g_gx_state.num_tev_stages >= 2) {
        const GXTevStage* s1 = &g_gx_state.tev_stages[1];

        /* J2DPicture mBlack/mWhite tinting:
         * Stage 0: color=(TEXC, ZERO, ZERO, ZERO) — texture REPLACE
         * Stage 1: color=(C0, C1, CPREV, ZERO) — lerp(mBlack, mWhite, texColor)
         * Use BLEND shader with TEV register uniforms. */
        if (uses_texc &&
            s1->color_a == GX_CC_C0 && s1->color_b == GX_CC_C1 &&
            s1->color_c == GX_CC_CPREV && s1->color_d == GX_CC_ZERO) {
            return GX_TEV_SHADER_BLEND;
        }
    }

    /* Generic multi-stage: MODULATE for textured, PASSCLR otherwise */
    return has_texture ? GX_TEV_SHADER_MODULATE : GX_TEV_SHADER_PASSCLR;
}

/**
 * Override shader preset when vertex data is incompatible.
 * MODULATE requires vertex color (multiplies texture × color).
 * If no color attribute exists, fall back to REPLACE (texture only).
 */
static int fixup_preset_for_vertex(int preset) {
    if (preset == GX_TEV_SHADER_MODULATE || preset == GX_TEV_SHADER_DECAL ||
        preset == GX_TEV_SHADER_BLEND) {
        const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
        if (desc[GX_VA_CLR0].type == GX_NONE) {
            /* No vertex color — can't modulate. Use REPLACE to show texture. */
            return GX_TEV_SHADER_REPLACE;
        }
    }
    return preset;
}

/* ================================================================ */
/* bgfx state conversion                                            */
/* ================================================================ */

static uint64_t convert_blend_state(void) {
    uint64_t state = 0;

    switch (g_gx_state.blend_mode) {
    case GX_BM_NONE:
        /* No blending */
        break;
    case GX_BM_BLEND: {
        uint64_t src = BGFX_STATE_BLEND_ONE;
        uint64_t dst = BGFX_STATE_BLEND_ZERO;

        switch (g_gx_state.blend_src) {
        case GX_BL_ZERO:        src = BGFX_STATE_BLEND_ZERO; break;
        case GX_BL_ONE:         src = BGFX_STATE_BLEND_ONE; break;
        case GX_BL_SRCALPHA:    src = BGFX_STATE_BLEND_SRC_ALPHA; break;
        case GX_BL_INVSRCALPHA: src = BGFX_STATE_BLEND_INV_SRC_ALPHA; break;
        case GX_BL_DSTALPHA:    src = BGFX_STATE_BLEND_DST_ALPHA; break;
        case GX_BL_INVDSTALPHA: src = BGFX_STATE_BLEND_INV_DST_ALPHA; break;
        default: break;
        }

        switch (g_gx_state.blend_dst) {
        case GX_BL_ZERO:        dst = BGFX_STATE_BLEND_ZERO; break;
        case GX_BL_ONE:         dst = BGFX_STATE_BLEND_ONE; break;
        case GX_BL_SRCALPHA:    dst = BGFX_STATE_BLEND_SRC_ALPHA; break;
        case GX_BL_INVSRCALPHA: dst = BGFX_STATE_BLEND_INV_SRC_ALPHA; break;
        case GX_BL_DSTALPHA:    dst = BGFX_STATE_BLEND_DST_ALPHA; break;
        case GX_BL_INVDSTALPHA: dst = BGFX_STATE_BLEND_INV_DST_ALPHA; break;
        default: break;
        }

        state |= BGFX_STATE_BLEND_FUNC(src, dst);
        break;
    }
    case GX_BM_SUBTRACT:
        state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE)
               | BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_REVSUB);
        break;
    default:
        break;
    }

    return state;
}

static uint64_t convert_depth_state(void) {
    uint64_t state = 0;

    if (g_gx_state.z_compare_enable) {
        switch (g_gx_state.z_func) {
        case GX_NEVER:   state |= BGFX_STATE_DEPTH_TEST_NEVER; break;
        case GX_LESS:    state |= BGFX_STATE_DEPTH_TEST_LESS; break;
        case GX_EQUAL:   state |= BGFX_STATE_DEPTH_TEST_EQUAL; break;
        case GX_LEQUAL:  state |= BGFX_STATE_DEPTH_TEST_LEQUAL; break;
        case GX_GREATER: state |= BGFX_STATE_DEPTH_TEST_GREATER; break;
        case GX_NEQUAL:  state |= BGFX_STATE_DEPTH_TEST_NOTEQUAL; break;
        case GX_GEQUAL:  state |= BGFX_STATE_DEPTH_TEST_GEQUAL; break;
        case GX_ALWAYS:  state |= BGFX_STATE_DEPTH_TEST_ALWAYS; break;
        default: break;
        }
    }

    if (g_gx_state.z_update_enable) {
        state |= BGFX_STATE_WRITE_Z;
    }

    return state;
}

static uint64_t convert_cull_state(void) {
    switch (g_gx_state.cull_mode) {
    case GX_CULL_FRONT: return BGFX_STATE_CULL_CW;
    case GX_CULL_BACK:  return BGFX_STATE_CULL_CCW;
    case GX_CULL_ALL:   return BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW;
    default:            return 0;
    }
}

static uint64_t convert_primitive_state(GXPrimitive prim) {
    switch (prim) {
    case GX_TRIANGLES:      return 0; /* bgfx default is triangle list */
    case GX_TRIANGLESTRIP:  return BGFX_STATE_PT_TRISTRIP;
    case GX_TRIANGLEFAN:    return BGFX_STATE_PT_TRISTRIP; /* approx */
    case GX_LINES:          return BGFX_STATE_PT_LINES;
    case GX_LINESTRIP:      return BGFX_STATE_PT_LINESTRIP;
    case GX_POINTS:         return BGFX_STATE_PT_POINTS;
    case GX_QUADS:          return 0; /* convert to triangles */
    default:                return 0;
    }
}

/* ================================================================ */
/* Vertex layout builder                                            */
/* ================================================================ */

/**
 * Build a bgfx vertex layout from the current GX vertex descriptor.
 * Returns the stride (bytes per vertex) and populates the layout.
 *
 * GX vertex attributes → bgfx attributes:
 *   GX_VA_POS    → bgfx::Attrib::Position  (3 floats)
 *   GX_VA_NRM    → bgfx::Attrib::Normal    (3 floats)
 *   GX_VA_CLR0   → bgfx::Attrib::Color0    (4 bytes RGBA)
 *   GX_VA_CLR1   → bgfx::Attrib::Color1    (4 bytes RGBA)
 *   GX_VA_TEX0   → bgfx::Attrib::TexCoord0 (2 floats)
 *   GX_VA_TEX1-7 → bgfx::Attrib::TexCoord1-7
 */
/**
 * Get the byte size of a GX component type.
 */
static uint32_t gx_comp_size(GXCompType t) {
    switch (t) {
    case GX_U8: case GX_S8: return 1;
    case GX_U16: case GX_S16: return 2;
    case GX_F32: return 4;
    default: return 4; /* default to float */
    }
}

/**
 * Map GX component type to bgfx attribute type.
 */
static bgfx::AttribType::Enum gx_to_bgfx_type(GXCompType t) {
    switch (t) {
    case GX_U8:  return bgfx::AttribType::Uint8;
    case GX_S8:  return bgfx::AttribType::Uint8;  /* no Int8 in bgfx; size matches */
    case GX_U16: return bgfx::AttribType::Int16;
    case GX_S16: return bgfx::AttribType::Int16;
    case GX_F32: return bgfx::AttribType::Float;
    default:     return bgfx::AttribType::Float;
    }
}

static uint32_t build_vertex_layout(bgfx::VertexLayout& layout) {
    layout.begin();

    uint32_t stride = 0;
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    const GXVtxAttrFmtEntry* afmt = g_gx_state.vtx_attr_fmt[g_gx_state.draw.vtx_fmt];

    /* PNMTXIDX — 1 byte matrix index (not a bgfx attribute, skip it) */
    if (desc[GX_VA_PNMTXIDX].type != GX_NONE) {
        layout.skip(1);
        stride += 1;
    }

    /* Position — always Float in bgfx.
     * GX integer positions use frac bits (fixed-point) which bgfx
     * doesn't understand, so we always use float and convert data
     * in convert_vertex_to_float(). */
    if (desc[GX_VA_POS].type != GX_NONE) {
        int ncomps = (afmt[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;
        layout.add(bgfx::Attrib::Position, (uint8_t)ncomps, bgfx::AttribType::Float);
        stride += ncomps * 4;
    }

    /* Normal — always Float */
    if (desc[GX_VA_NRM].type != GX_NONE) {
        layout.add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float);
        stride += 3 * 4;
    }

    /* Colors — always Uint8 normalized */
    if (desc[GX_VA_CLR0].type != GX_NONE) {
        layout.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true);
        stride += 4;
    }
    if (desc[GX_VA_CLR1].type != GX_NONE) {
        layout.add(bgfx::Attrib::Color1, 4, bgfx::AttribType::Uint8, true);
        stride += 4;
    }

    /* Texture coordinates — always Float (frac-bit safe) */
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0 + i].type != GX_NONE) {
            int ncomps = (afmt[GX_VA_TEX0 + i].cnt == GX_TEX_S) ? 1 : 2;
            layout.add((bgfx::Attrib::Enum)(bgfx::Attrib::TexCoord0 + i),
                       (uint8_t)ncomps, bgfx::AttribType::Float);
            stride += ncomps * 4;
        }
    }

    layout.end();
    return stride;
}

/**
 * Calculate the raw GX vertex stride (bytes per vertex as stored in vtx_data).
 * This may differ from the bgfx layout stride when integer components are
 * promoted to float.
 */
static uint32_t calc_raw_vertex_stride(void) {
    uint32_t stride = 0;
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    const GXVtxAttrFmtEntry* afmt = g_gx_state.vtx_attr_fmt[g_gx_state.draw.vtx_fmt];

    if (desc[GX_VA_PNMTXIDX].type != GX_NONE) stride += 1;
    if (desc[GX_VA_POS].type != GX_NONE) {
        int n = (afmt[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;
        stride += n * gx_comp_size(afmt[GX_VA_POS].comp_type);
    }
    if (desc[GX_VA_NRM].type != GX_NONE)
        stride += 3 * gx_comp_size(afmt[GX_VA_NRM].comp_type);
    if (desc[GX_VA_CLR0].type != GX_NONE) stride += 4;
    if (desc[GX_VA_CLR1].type != GX_NONE) stride += 4;
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0 + i].type != GX_NONE) {
            int n = (afmt[GX_VA_TEX0 + i].cnt == GX_TEX_S) ? 1 : 2;
            stride += n * gx_comp_size(afmt[GX_VA_TEX0 + i].comp_type);
        }
    }
    return stride;
}

/**
 * Read a GX component value as float, applying frac-bit scaling.
 */
static float read_gx_component(const uint8_t* src, GXCompType type, uint8_t frac) {
    float scale = (frac > 0) ? (1.0f / (float)(1 << frac)) : 1.0f;
    switch (type) {
    case GX_S16: { int16_t v; memcpy(&v, src, 2); return (float)v * scale; }
    case GX_U16: { uint16_t v; memcpy(&v, src, 2); return (float)v * scale; }
    case GX_S8:  return (float)(*(const int8_t*)src) * scale;
    case GX_U8:  return (float)(*src) * scale;
    case GX_F32: { float v; memcpy(&v, src, 4); return v; }
    default: return 0.0f;
    }
}

/**
 * Convert one vertex from raw GX format to the bgfx float layout.
 * Walks through each attribute, converting integer components to float
 * with frac-bit scaling applied.
 */
static void convert_vertex_to_float(const uint8_t* src, uint8_t* dst) {
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    const GXVtxAttrFmtEntry* afmt = g_gx_state.vtx_attr_fmt[g_gx_state.draw.vtx_fmt];
    uint32_t si = 0, di = 0;

    /* PNMTXIDX — copy 1 byte */
    if (desc[GX_VA_PNMTXIDX].type != GX_NONE) {
        dst[di++] = src[si++];
    }

    /* Position → Float */
    if (desc[GX_VA_POS].type != GX_NONE) {
        GXCompType pt = afmt[GX_VA_POS].comp_type;
        uint8_t frac = afmt[GX_VA_POS].frac;
        int ncomps = (afmt[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;
        uint32_t csz = gx_comp_size(pt);
        for (int c = 0; c < ncomps; c++) {
            float v = read_gx_component(src + si, pt, frac);
            memcpy(dst + di, &v, 4);
            si += csz; di += 4;
        }
    }

    /* Normal → Float */
    if (desc[GX_VA_NRM].type != GX_NONE) {
        GXCompType nt = afmt[GX_VA_NRM].comp_type;
        uint8_t frac = afmt[GX_VA_NRM].frac;
        uint32_t csz = gx_comp_size(nt);
        for (int c = 0; c < 3; c++) {
            float v = read_gx_component(src + si, nt, frac);
            memcpy(dst + di, &v, 4);
            si += csz; di += 4;
        }
    }

    /* Color0 — copy 4 bytes as-is */
    if (desc[GX_VA_CLR0].type != GX_NONE) {
        memcpy(dst + di, src + si, 4); si += 4; di += 4;
    }
    /* Color1 — copy 4 bytes as-is */
    if (desc[GX_VA_CLR1].type != GX_NONE) {
        memcpy(dst + di, src + si, 4); si += 4; di += 4;
    }

    /* Texture coordinates → Float */
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0 + i].type != GX_NONE) {
            GXCompType tt = afmt[GX_VA_TEX0 + i].comp_type;
            uint8_t frac = afmt[GX_VA_TEX0 + i].frac;
            int ncomps = (afmt[GX_VA_TEX0 + i].cnt == GX_TEX_S) ? 1 : 2;
            uint32_t csz = gx_comp_size(tt);
            for (int c = 0; c < ncomps; c++) {
                float v = read_gx_component(src + si, tt, frac);
                memcpy(dst + di, &v, 4);
                si += csz; di += 4;
            }
        }
    }
}

/* ================================================================ */
/* Quad → triangle conversion                                       */
/* ================================================================ */

/**
 * Convert quads to triangles by generating index buffer.
 * Each quad (4 verts) becomes 2 triangles (6 indices).
 * Returns number of indices, or 0 if not quads.
 */
static uint32_t build_quad_indices(uint16_t nverts, uint16_t* indices, uint32_t max_indices) {
    uint32_t nquads = nverts / 4;
    uint32_t nidx = nquads * 6;
    if (nidx > max_indices) return 0;

    for (uint32_t q = 0; q < nquads; q++) {
        uint16_t base = (uint16_t)(q * 4);
        indices[q * 6 + 0] = base + 0;
        indices[q * 6 + 1] = base + 1;
        indices[q * 6 + 2] = base + 2;
        indices[q * 6 + 3] = base + 0;
        indices[q * 6 + 4] = base + 2;
        indices[q * 6 + 5] = base + 3;
    }
    return nidx;
}

/**
 * Convert triangle fan to triangle list indices.
 * Fan with N verts → (N-2) triangles → (N-2)*3 indices.
 */
static uint32_t build_fan_indices(uint16_t nverts, uint16_t* indices, uint32_t max_indices) {
    if (nverts < 3) return 0;
    uint32_t ntris = nverts - 2;
    uint32_t nidx = ntris * 3;
    if (nidx > max_indices) return 0;

    for (uint32_t t = 0; t < ntris; t++) {
        indices[t * 3 + 0] = 0;
        indices[t * 3 + 1] = (uint16_t)(t + 1);
        indices[t * 3 + 2] = (uint16_t)(t + 2);
    }
    return nidx;
}

/* ================================================================ */
/* Public API                                                       */
/* ================================================================ */

extern "C" {

void pal_tev_init(void) {
    if (s_tev_ready) return;

    bgfx::RendererType::Enum renderer = bgfx::getRendererType();

    /* Select vertex shader binary for current renderer */
    ShaderBinEntry vs_bin = select_shader_bin(renderer,
        vs_gx_tev_glsl, sizeof(vs_gx_tev_glsl),
        vs_gx_tev_essl, sizeof(vs_gx_tev_essl),
        vs_gx_tev_spv,  sizeof(vs_gx_tev_spv));

    /* Create fragment shaders and programs for each preset */
    int all_ok = 1;
    for (int i = 0; i < GX_TEV_SHADER_COUNT; i++) {
        /* Create vertex shader (each program needs its own copy) */
        bgfx::ShaderHandle vsh = create_shader_from_bin(vs_bin.data, vs_bin.size);
        if (!bgfx::isValid(vsh)) {
            fprintf(stderr, "{\"tev\":\"vs_create_failed\",\"preset\":%d}\n", i);
            all_ok = 0;
            s_programs[i] = BGFX_INVALID_HANDLE;
            continue;
        }

        /* Select and create fragment shader */
        ShaderBinEntry fs_bin = select_shader_bin(renderer,
            s_fs_bins[i].glsl, s_fs_bins[i].glsl_size,
            s_fs_bins[i].essl, s_fs_bins[i].essl_size,
            s_fs_bins[i].spv,  s_fs_bins[i].spv_size);

        bgfx::ShaderHandle fsh = create_shader_from_bin(fs_bin.data, fs_bin.size);
        if (!bgfx::isValid(fsh)) {
            fprintf(stderr, "{\"tev\":\"fs_create_failed\",\"shader\":\"%s\"}\n", s_fs_names[i]);
            bgfx::destroy(vsh);
            all_ok = 0;
            s_programs[i] = BGFX_INVALID_HANDLE;
            continue;
        }

        s_programs[i] = bgfx::createProgram(vsh, fsh, true /* destroy shaders */);
        if (!bgfx::isValid(s_programs[i])) {
            fprintf(stderr, "{\"tev\":\"program_create_failed\",\"preset\":%d}\n", i);
            all_ok = 0;
        }
    }

    /* Create texture sampler uniform */
    s_tex_uniform = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);

    /* Create TEV register uniforms for BLEND shader (mBlack/mWhite lerp) */
    s_tev_reg0_uniform = bgfx::createUniform("u_tevReg0", bgfx::UniformType::Vec4);
    s_tev_reg1_uniform = bgfx::createUniform("u_tevReg1", bgfx::UniformType::Vec4);

    s_tev_ready = all_ok;

    /* For Noop renderer, mark ready even if shader creation "fails" (it's a no-op anyway) */
    if (renderer == bgfx::RendererType::Noop) {
        s_tev_ready = 1;
    }

    fprintf(stderr, "{\"tev\":\"init\",\"ready\":%d,\"renderer\":\"%s\"}\n",
            s_tev_ready, bgfx::getRendererName(renderer));

    /* Initialize texture cache */
    memset(s_tex_cache, 0, sizeof(s_tex_cache));
}

void pal_tev_shutdown(void) {
    if (!s_tev_ready) return;

    for (int i = 0; i < GX_TEV_SHADER_COUNT; i++) {
        if (bgfx::isValid(s_programs[i])) {
            bgfx::destroy(s_programs[i]);
            s_programs[i] = BGFX_INVALID_HANDLE;
        }
    }

    if (bgfx::isValid(s_tex_uniform)) {
        bgfx::destroy(s_tex_uniform);
        s_tex_uniform = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(s_tev_reg0_uniform)) {
        bgfx::destroy(s_tev_reg0_uniform);
        s_tev_reg0_uniform = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(s_tev_reg1_uniform)) {
        bgfx::destroy(s_tev_reg1_uniform);
        s_tev_reg1_uniform = BGFX_INVALID_HANDLE;
    }

    /* Destroy cached textures */
    for (uint32_t i = 0; i < TEV_TEX_CACHE_SIZE; i++) {
        if (s_tex_cache[i].valid && bgfx::isValid(s_tex_cache[i].handle)) {
            bgfx::destroy(s_tex_cache[i].handle);
        }
    }
    memset(s_tex_cache, 0, sizeof(s_tex_cache));

    s_tev_ready = 0;
}

int pal_tev_is_ready(void) {
    return s_tev_ready;
}

void pal_tev_submit_test_quad(void) {
    if (!s_tev_ready || !bgfx::isValid(s_programs[GX_TEV_SHADER_PASSCLR]))
        return;

    /* Simple quad: position (3 floats) + color (4 bytes) = 16 bytes/vert */
    struct TestVert {
        float x, y, z;
        uint32_t abgr;  /* bgfx uses ABGR packing for Uint8 color */
    };

    bgfx::VertexLayout layout;
    layout.begin()
          .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
          .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
          .end();

    bgfx::TransientVertexBuffer tvb;
    if (!bgfx::getAvailTransientVertexBuffer(4, layout)) return;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout);

    /* NDC coordinates: centered green quad */
    TestVert* v = (TestVert*)tvb.data;
    v[0] = { -0.5f, -0.5f, 0.0f, 0xff00ff00 }; /* green */
    v[1] = {  0.5f, -0.5f, 0.0f, 0xff00ff00 };
    v[2] = {  0.5f,  0.5f, 0.0f, 0xff00ff00 };
    v[3] = { -0.5f,  0.5f, 0.0f, 0xff00ff00 };

    bgfx::TransientIndexBuffer tib;
    if (!bgfx::getAvailTransientIndexBuffer(6)) return;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    uint16_t* idx = (uint16_t*)tib.data;
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;

    /* Identity transform */
    float identity[16] = {
        1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
    };
    bgfx::setTransform(identity);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(0, s_programs[GX_TEV_SHADER_PASSCLR]);

    fprintf(stderr, "{\"tev\":\"test_quad_submitted\"}\n");
}

void pal_tev_flush_draw(void) {
    if (!s_tev_ready) return;

    const GXDrawState* ds = &g_gx_state.draw;
    if (ds->verts_written == 0 || ds->vtx_data_pos == 0) return;

    static uint32_t s_total_draw_count = 0;
    s_total_draw_count++;

    /* 1. Select shader based on TEV config */
    int preset = detect_tev_preset();
    if (preset < 0 || preset >= GX_TEV_SHADER_COUNT) preset = GX_TEV_SHADER_PASSCLR;
    preset = fixup_preset_for_vertex(preset);
    if (!bgfx::isValid(s_programs[preset])) return;

    /* 2. Build vertex layout (always Float for pos/texcoord) */
    bgfx::VertexLayout layout;
    uint32_t bgfx_stride = build_vertex_layout(layout);
    if (bgfx_stride == 0) return;

    /* Raw stride matches the actual GX vertex data byte layout */
    uint32_t raw_stride = calc_raw_vertex_stride();
    if (raw_stride == 0) return;

    /* 3. Check if we need to inject constant color for PASSCLR without vertex color */
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    int inject_color = (preset == GX_TEV_SHADER_PASSCLR &&
                        desc[GX_VA_CLR0].type == GX_NONE);
    uint8_t const_clr[4] = {0, 0, 0, 255};

    if (inject_color) {
        /* Determine the constant color from TEV state */
        const GXTevStage* s0 = &g_gx_state.tev_stages[0];
        if (s0->color_d == GX_CC_C0 || s0->color_d == GX_CC_CPREV) {
            const_clr[0] = g_gx_state.tev_regs[GX_TEVREG0].r;
            const_clr[1] = g_gx_state.tev_regs[GX_TEVREG0].g;
            const_clr[2] = g_gx_state.tev_regs[GX_TEVREG0].b;
            const_clr[3] = g_gx_state.tev_regs[GX_TEVREG0].a;
        } else if (s0->color_d == GX_CC_RASC) {
            const_clr[0] = g_gx_state.chan_ctrl[0].mat_color.r;
            const_clr[1] = g_gx_state.chan_ctrl[0].mat_color.g;
            const_clr[2] = g_gx_state.chan_ctrl[0].mat_color.b;
            const_clr[3] = g_gx_state.chan_ctrl[0].mat_color.a;
        }

        /* Rebuild layout with color attribute (position always Float) */
        const GXVtxAttrFmtEntry* af = g_gx_state.vtx_attr_fmt[ds->vtx_fmt];
        int has_pnmtx = desc[GX_VA_PNMTXIDX].type != GX_NONE;
        int npos = (af[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;

        layout.begin();
        if (has_pnmtx) layout.skip(1);
        layout.add(bgfx::Attrib::Position, (uint8_t)npos, bgfx::AttribType::Float);
        layout.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true);
        layout.end();
        bgfx_stride = layout.getStride();
    }

    /* Allocate transient vertex buffer */
    uint16_t nverts = ds->verts_written;
    if (nverts == 0) return;

    bgfx::TransientVertexBuffer tvb;
    if (!bgfx::getAvailTransientVertexBuffer(nverts, layout)) return;
    bgfx::allocTransientVertexBuffer(&tvb, nverts, layout);

    /* Copy vertex data — convert from raw GX format to bgfx float format */
    if (inject_color) {
        /* Convert position to float, then append constant color */
        const GXVtxAttrFmtEntry* af = g_gx_state.vtx_attr_fmt[ds->vtx_fmt];
        int has_pnmtx = desc[GX_VA_PNMTXIDX].type != GX_NONE;
        int npos = (af[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;
        uint32_t pnmtx_sz = has_pnmtx ? 1 : 0;
        uint32_t pos_raw_sz = npos * gx_comp_size(af[GX_VA_POS].comp_type);
        uint32_t pos_float_sz = npos * 4;
        uint32_t new_stride = pnmtx_sz + pos_float_sz + 4;

        for (uint16_t vi = 0; vi < nverts; vi++) {
            uint8_t* dst = tvb.data + vi * new_stride;
            const uint8_t* src = ds->vtx_data + vi * raw_stride;
            uint32_t si = 0, di = 0;

            /* PNMTXIDX */
            if (has_pnmtx) { dst[di++] = src[si++]; }

            /* Position → Float with frac-bit scaling */
            GXCompType pt = af[GX_VA_POS].comp_type;
            uint8_t frac = af[GX_VA_POS].frac;
            uint32_t csz = gx_comp_size(pt);
            for (int c = 0; c < npos; c++) {
                float v = read_gx_component(src + si, pt, frac);
                memcpy(dst + di, &v, 4);
                si += csz; di += 4;
            }

            /* Append constant color */
            memcpy(dst + di, const_clr, 4);
        }
    } else {
        /* Full vertex conversion: raw GX → float */
        for (uint16_t vi = 0; vi < nverts; vi++) {
            const uint8_t* src = ds->vtx_data + vi * raw_stride;
            uint8_t* dst = tvb.data + vi * bgfx_stride;
            convert_vertex_to_float(src, dst);
        }
    }

    /* 4. Handle primitive conversion */
    bgfx::TransientIndexBuffer tib;
    int use_index_buffer = 0;
    uint32_t num_indices = 0;

    if (ds->prim_type == GX_QUADS) {
        /* Convert quads to triangles */
        uint32_t max_idx = (uint32_t)(nverts / 4) * 6;
        if (max_idx > 0 && bgfx::getAvailTransientIndexBuffer(max_idx)) {
            bgfx::allocTransientIndexBuffer(&tib, max_idx);
            num_indices = build_quad_indices(nverts, (uint16_t*)tib.data, max_idx);
            if (num_indices > 0) use_index_buffer = 1;
        }
    } else if (ds->prim_type == GX_TRIANGLEFAN) {
        /* Convert triangle fan to triangle list */
        uint32_t max_idx = (uint32_t)(nverts - 2) * 3;
        if (max_idx > 0 && bgfx::getAvailTransientIndexBuffer(max_idx)) {
            bgfx::allocTransientIndexBuffer(&tib, max_idx);
            num_indices = build_fan_indices(nverts, (uint16_t*)tib.data, max_idx);
            if (num_indices > 0) use_index_buffer = 1;
        }
    }

    /* 5. Set transform (model-view-projection) */
    /* Build MVP from GX matrices: proj * pos_mtx[current] */
    float mvp[16];
    {
        const float (*proj)[4] = g_gx_state.proj_mtx;
        uint32_t mtx_idx = g_gx_state.current_pos_mtx;
        if (mtx_idx >= GX_MAX_POS_MTX) mtx_idx = 0;
        const float (*model)[4] = g_gx_state.pos_mtx[mtx_idx];

        /* model is 3x4, expand to 4x4 */
        float m44[16] = {
            model[0][0], model[0][1], model[0][2], model[0][3],
            model[1][0], model[1][1], model[1][2], model[1][3],
            model[2][0], model[2][1], model[2][2], model[2][3],
            0.0f,        0.0f,        0.0f,        1.0f
        };

        /* Multiply: mvp = proj * model (row-major) */
        float mvp_rm[16];
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                mvp_rm[r * 4 + c] = 0.0f;
                for (int k = 0; k < 4; k++) {
                    mvp_rm[r * 4 + c] += proj[r][k] * m44[k * 4 + c];
                }
            }
        }

        /* Transpose to column-major for bgfx (OpenGL convention) */
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                mvp[c * 4 + r] = mvp_rm[r * 4 + c];
            }
        }
    }
    bgfx::setTransform(mvp);

    /* 6. Set vertex buffer */
    bgfx::setVertexBuffer(0, &tvb);

    /* 7. Set index buffer if needed */
    if (use_index_buffer && num_indices > 0) {
        bgfx::setIndexBuffer(&tib, 0, num_indices);
    }

    /* 8. Bind texture if shader needs it */
    if (preset != GX_TEV_SHADER_PASSCLR) {
        const GXTevStage* s0 = &g_gx_state.tev_stages[0];
        if (s0->tex_map < GX_MAX_TEXMAP) {
            const GXTexBinding* binding = &g_gx_state.tex_bindings[s0->tex_map];
            bgfx::TextureHandle tex = upload_gx_texture(binding);
            if (bgfx::isValid(tex)) {
                bgfx::setTexture(0, s_tex_uniform, tex);
            }
            /* Log first few textured draws for diagnostics */
            static int s_tex_log_count = 0;
            if (s_tex_log_count < 10) {
                s_tex_log_count++;
                fprintf(stderr, "{\"tev_tex_draw\":{\"n\":%d,\"draw_id\":%u,\"preset\":\"%s\","
                        "\"tex_valid\":%d,\"tex_map\":%d,"
                        "\"w\":%u,\"h\":%u,\"fmt\":%d,\"img_ptr\":\"%p\","
                        "\"nverts\":%u,\"stride\":%u,\"prim\":%d,"
                        "\"blend_mode\":%d,\"blend_src\":%d,\"blend_dst\":%d,"
                        "\"z_enable\":%d,\"z_func\":%d,"
                        "\"mvp_diag\":[%.3f,%.3f,%.3f,%.3f],"
                        "\"tev\":{\"color_abcd\":[%d,%d,%d,%d],\"alpha_abcd\":[%d,%d,%d,%d]}"
                        "}}\n",
                        s_tex_log_count, s_total_draw_count, s_fs_names[preset],
                        bgfx::isValid(tex) ? 1 : 0,
                        s0->tex_map,
                        (unsigned)binding->width, (unsigned)binding->height,
                        (int)binding->format, binding->image_ptr,
                        (unsigned)nverts, raw_stride, ds->prim_type,
                        g_gx_state.blend_mode, g_gx_state.blend_src, g_gx_state.blend_dst,
                        g_gx_state.z_compare_enable, g_gx_state.z_func,
                        mvp[0], mvp[5], mvp[10], mvp[15],
                        s0->color_a, s0->color_b, s0->color_c, s0->color_d,
                        s0->alpha_a, s0->alpha_b, s0->alpha_c, s0->alpha_d);
            }
        }

        /* Set TEV register uniforms for BLEND shader (mBlack/mWhite lerp) */
        if (preset == GX_TEV_SHADER_BLEND) {
            float reg0[4] = {
                g_gx_state.tev_regs[GX_TEVREG0].r / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG0].g / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG0].b / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG0].a / 255.0f
            };
            float reg1[4] = {
                g_gx_state.tev_regs[GX_TEVREG1].r / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG1].g / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG1].b / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG1].a / 255.0f
            };
            bgfx::setUniform(s_tev_reg0_uniform, reg0);
            bgfx::setUniform(s_tev_reg1_uniform, reg1);
        }
    }

    /* 9. Set render state */
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    state |= convert_blend_state();
    state |= convert_depth_state();
    state |= convert_cull_state();
    state |= convert_primitive_state(ds->prim_type);
    bgfx::setState(state);

    /* 10. Submit draw call */
    bgfx::submit(0, s_programs[preset]);

    /* Track valid draw call for per-frame milestone validation */
    gx_frame_draw_calls++;
    gx_frame_valid_verts += nverts;
    gx_stub_draw_call_crosscheck();

    /* Track render pipeline metrics for verification */
    gx_frame_shader_mask |= (1u << preset);
    /* Map GX primitive type to sequential bit position 0-6.
     * Keep this mapping in sync with tools/verify_port.py:GX_PRIM_NAMES. */
    {
        uint32_t prim_bit;
        int prim_bit_valid = 1;
        switch (ds->prim_type) {
        case GX_QUADS:         prim_bit = 0; break;
        case GX_TRIANGLES:     prim_bit = 1; break;
        case GX_TRIANGLESTRIP: prim_bit = 2; break;
        case GX_TRIANGLEFAN:   prim_bit = 3; break;
        case GX_LINES:         prim_bit = 4; break;
        case GX_LINESTRIP:     prim_bit = 5; break;
        case GX_POINTS:        prim_bit = 6; break;
        default:
            prim_bit_valid = 0;
            prim_bit = 0;
            break;
        }
        if (prim_bit_valid)
            gx_frame_prim_mask |= (1u << prim_bit);
    }
    if (preset != GX_TEV_SHADER_PASSCLR) {
        gx_frame_textured_draws++;
    } else {
        gx_frame_untextured_draws++;
    }
    if (g_gx_state.z_compare_enable)
        gx_frame_depth_draws++;
    if (g_gx_state.blend_mode != 0 /* GX_BM_NONE */)
        gx_frame_blend_draws++;

    /* Track unique textures bound */
    if (preset != GX_TEV_SHADER_PASSCLR) {
        const GXTevStage* ts0 = &g_gx_state.tev_stages[0];
        if (ts0->tex_map < GX_MAX_TEXMAP) {
            const GXTexBinding* tb = &g_gx_state.tex_bindings[ts0->tex_map];
            if (tb->valid && tb->image_ptr)
                gx_stub_track_texture(tb->image_ptr);
        }
    }
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
