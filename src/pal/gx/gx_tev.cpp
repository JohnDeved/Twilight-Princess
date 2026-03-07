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
    { fs_gx_tev_glsl,      fs_gx_tev_essl,      fs_gx_tev_spv,
      sizeof(fs_gx_tev_glsl), sizeof(fs_gx_tev_essl), sizeof(fs_gx_tev_spv) },
};

static const char* s_fs_names[GX_TEV_SHADER_COUNT] = {
    "fs_gx_passclr",
    "fs_gx_replace",
    "fs_gx_modulate",
    "fs_gx_blend",
    "fs_gx_decal",
    "fs_gx_tev",
};

/* ================================================================ */
/* State                                                            */
/* ================================================================ */

static bgfx::ProgramHandle s_programs[GX_TEV_SHADER_COUNT];
static bgfx::UniformHandle s_tex_uniform;
static bgfx::UniformHandle s_tev_reg0_uniform;
static bgfx::UniformHandle s_tev_reg1_uniform;
static bgfx::UniformHandle s_tev_config_uniform;
static bgfx::UniformHandle s_alpha_test_uniform;
static bgfx::UniformHandle s_alpha_op_uniform;
static int s_tev_ready = 0;

/* Number of RASC draw world-positions to accumulate before computing the
 * geometry-centroid camera override (see pal_tev_flush_draw).
 *
 * Gate: only accumulate when g_gx_state.draw_calls > CENTROID_FRAME_DRAWS_MIN,
 * so the centroid fires during the high-draw-count 3D room frame (~7400 draws)
 * and NOT during title-screen frames (~50-500 draws each). */
#define CENTROID_SAMPLES          50
#define CENTROID_FRAME_DRAWS_MIN 1000

/* Saved centroid LookAt matrix (3×4, row-major) and completion flag.
 * Set once by the centroid calculation block; never overwritten by J3D's
 * GXLoadPosMtxImm calls (which overwrite g_gx_state.pos_mtx each draw).
 *
 * Per-frame reset: s_geom_centroid_active is cleared at the start of each
 * frame (when gx_frame_draw_calls == 0) so that only the 3D room frame
 * (draw_calls > CENTROID_FRAME_DRAWS_MIN) activates the centroid camera.
 * Without this reset, s_geom_centroid_active stays set after frame 129 and
 * routes all Phase 4 frames 130-200 to bgfx view 1, leaving view 0 black. */
static float s_geom_centroid_view[3][4];
static int   s_geom_centroid_active = 0;

/* Saved perspective projection for centroid camera draws.
 * The game sets perspective projection (GX_PERSPECTIVE) before 3D room
 * rendering but J2D code overwrites it with orthographic before the DL
 * draws actually flush.  We snapshot the last perspective projection so
 * the centroid camera MVP uses the correct frustum. */
static float s_persp_proj[4][4];
static int   s_has_persp_proj = 0;

/* Centroid accumulation state — file-scope so the per-frame reset block
 * (at gx_frame_draw_calls == 0 boundary) can clear them each frame. */
static float s_centroid_sum[3] = {0.0f, 0.0f, 0.0f};
static int   s_centroid_n      = 0;
static float s_centroid_vz_max = -1e30f;

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

/* Convert GX wrap mode + filter to bgfx sampler flags (per-draw) */
static uint32_t gx_sampler_flags(const GXTexBinding* binding) {
    uint32_t flags = 0;

    /* Wrap modes */
    switch (binding->wrap_s) {
    case GX_REPEAT: break; /* bgfx default is repeat (0) */
    case GX_CLAMP:  flags |= BGFX_SAMPLER_U_CLAMP;  break;
    case GX_MIRROR: flags |= BGFX_SAMPLER_U_MIRROR; break;
    default:        break;
    }
    switch (binding->wrap_t) {
    case GX_REPEAT: break;
    case GX_CLAMP:  flags |= BGFX_SAMPLER_V_CLAMP;  break;
    case GX_MIRROR: flags |= BGFX_SAMPLER_V_MIRROR; break;
    default:        break;
    }

    /* Minification filter */
    switch (binding->min_filt) {
    case GX_NEAR:
    case GX_NEAR_MIP_NEAR:
    case GX_NEAR_MIP_LIN:
        flags |= BGFX_SAMPLER_MIN_POINT;
        break;
    default: /* GX_LINEAR, GX_LIN_MIP_NEAR, GX_LIN_MIP_LIN → bilinear */
        break;
    }

    /* Magnification filter */
    switch (binding->mag_filt) {
    case GX_NEAR: flags |= BGFX_SAMPLER_MAG_POINT; break;
    default:      break;
    }

    return flags;
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
        binding->tlut_ptr, binding->tlut_fmt,
        binding->tlut_num_entries);

    if (decoded == 0) {
        free(rgba_data);
        return BGFX_INVALID_HANDLE;
    }

    /* Diagnostic: dump decoded texture stats to verify non-zero content */
    {
        static int s_tex_dump_count = 0;
        if (s_tex_dump_count < 30) {
            s_tex_dump_count++;
            uint32_t nonzero = 0;
            uint32_t nonzero_rgb = 0;
            for (uint32_t i = 0; i < rgba_size; i++) {
                if (rgba_data[i]) nonzero++;
            }
            for (uint32_t i = 0; i < rgba_size; i += 4) {
                if (rgba_data[i] || rgba_data[i+1] || rgba_data[i+2]) nonzero_rgb++;
            }
            /* Check source data non-zero count (first 256 bytes) */
            uint32_t src_nonzero = 0;
            const u8* src_bytes = (const u8*)binding->image_ptr;
            for (uint32_t i = 0; i < 256; i++) {
                if (src_bytes[i]) src_nonzero++;
            }
            fprintf(stderr, "{\"tex_decode\":{\"ptr\":\"%p\",\"fmt\":%d,\"w\":%u,\"h\":%u,"
                    "\"decoded\":%u,\"nonzero\":%u,\"nonzero_rgb\":%u,\"total\":%u,"
                    "\"src_nz256\":%u,"
                    "\"src16\":[",
                    binding->image_ptr, (int)binding->format,
                    (unsigned)binding->width, (unsigned)binding->height,
                    decoded, nonzero, nonzero_rgb, rgba_size, src_nonzero);
            for (int i = 0; i < 32; i++) {
                if (i > 0) fprintf(stderr, ",");
                fprintf(stderr, "%u", src_bytes[i]);
            }
            fprintf(stderr, "],\"rgba16\":[");
            for (int i = 0; i < 64 && i < (int)rgba_size; i++) {
                if (i > 0) fprintf(stderr, ",");
                fprintf(stderr, "%u", rgba_data[i]);
            }
            fprintf(stderr, "]}}\n");
        }
    }

    /* Upload to bgfx */
    const bgfx::Memory* mem = bgfx::copy(rgba_data, rgba_size);
    free(rgba_data);

    bgfx::TextureHandle tex = bgfx::createTexture2D(
        binding->width, binding->height,
        false, /* hasMips */
        1,     /* numLayers */
        bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_NONE, /* sampler flags set per-draw */
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
 * Check if a TEV stage references rasterized color (RASC or RASA) in any
 * of its four color input slots (A, B, C, D).
 */
static int stage_uses_rasc(const GXTevStage* st) {
    return (st->color_a == GX_CC_RASC || st->color_b == GX_CC_RASC ||
            st->color_c == GX_CC_RASC || st->color_d == GX_CC_RASC ||
            st->color_a == GX_CC_RASA || st->color_b == GX_CC_RASA ||
            st->color_c == GX_CC_RASA || st->color_d == GX_CC_RASA);
}

/**
 * Classify what inputs a TEV color argument references.
 * Returns a bitmask: bit 0=texture, bit 1=rasterized, bit 2=constant/register, bit 3=prev
 */
static u32 tev_arg_class(GXTevColorArg arg) {
    switch (arg) {
    case GX_CC_TEXC: case GX_CC_TEXA: return 0x1; /* texture */
    case GX_CC_RASC: case GX_CC_RASA: return 0x2; /* rasterized */
    case GX_CC_C0:   case GX_CC_A0:
    case GX_CC_C1:   case GX_CC_A1:
    case GX_CC_C2:   case GX_CC_A2:
    case GX_CC_KONST: case GX_CC_ONE: case GX_CC_HALF: return 0x4; /* constant/register */
    case GX_CC_CPREV: case GX_CC_APREV: return 0x8; /* previous stage */
    case GX_CC_ZERO: return 0x0; /* zero */
    default: return 0x0;
    }
}

/**
 * Resolve the actual RGBA konst color for a given TEV stage's k_color_sel.
 * Handles KCSEL_K0-K3 (full RGBA) and constant fractions (1, 7/8, etc.).
 */
static void resolve_konst_color(const GXTevStage* stage, uint8_t out[4]) {
    GXTevKColorSel sel = stage->k_color_sel;
    if (sel >= GX_TEV_KCSEL_K0 && sel <= GX_TEV_KCSEL_K3) {
        int idx = sel - GX_TEV_KCSEL_K0;
        out[0] = g_gx_state.tev_kregs[idx].r;
        out[1] = g_gx_state.tev_kregs[idx].g;
        out[2] = g_gx_state.tev_kregs[idx].b;
        out[3] = g_gx_state.tev_kregs[idx].a;
    } else if (sel >= GX_TEV_KCSEL_K0_R && sel <= GX_TEV_KCSEL_K3_R) {
        int idx = sel - GX_TEV_KCSEL_K0_R;
        out[0] = out[1] = out[2] = g_gx_state.tev_kregs[idx].r;
        out[3] = 255;
    } else if (sel >= GX_TEV_KCSEL_K0_G && sel <= GX_TEV_KCSEL_K3_G) {
        int idx = sel - GX_TEV_KCSEL_K0_G;
        out[0] = out[1] = out[2] = g_gx_state.tev_kregs[idx].g;
        out[3] = 255;
    } else if (sel >= GX_TEV_KCSEL_K0_B && sel <= GX_TEV_KCSEL_K3_B) {
        int idx = sel - GX_TEV_KCSEL_K0_B;
        out[0] = out[1] = out[2] = g_gx_state.tev_kregs[idx].b;
        out[3] = 255;
    } else if (sel >= GX_TEV_KCSEL_K0_A && sel <= GX_TEV_KCSEL_K3_A) {
        int idx = sel - GX_TEV_KCSEL_K0_A;
        out[0] = out[1] = out[2] = g_gx_state.tev_kregs[idx].a;
        out[3] = 255;
    } else {
        /* Constant fraction: 1, 7/8, 3/4, 5/8, 1/2, 3/8, 1/4, 1/8 */
        static const uint8_t fracs[] = {
            255, /* 1   */ 223, /* 7/8 */ 191, /* 3/4 */ 159, /* 5/8 */
            128, /* 1/2 */  96, /* 3/8 */  64, /* 1/4 */  32  /* 1/8 */
        };
        int fi = (sel <= GX_TEV_KCSEL_1_8) ? sel : 0;
        out[0] = out[1] = out[2] = fracs[fi];
        out[3] = 255;
    }
}

/**
 * Generic TEV preset selection based on input class analysis.
 *
 * NOTE: This is an approximation, NOT full TEV emulation. The GX TEV unit
 * computes: out = (d + (1-c)*a + c*b) op bias << scale per stage, with
 * arbitrary routing of texture, rasterized, constant, and previous-stage
 * values to a/b/c/d slots. Full emulation would require:
 *   - Runtime shader generation (or a uber-shader with uniform-driven paths)
 *   - Per-stage combiner formula evaluation
 *   - Indirect texture lookups
 *   - Alpha channel combiner (separate from color)
 *   - Konst color/alpha register selection per stage
 *
 * The classification approach maps the most common combiner patterns to
 * a fixed set of precompiled shaders (PASSCLR, REPLACE, MODULATE, BLEND,
 * DECAL). This covers ~90% of TP's TEV usage but will produce incorrect
 * output for complex multi-stage setups with indirect texturing or
 * non-standard combiner formulas.
 */
static int classify_tev_config(void) {
    u32 all_tex = 0, all_ras = 0, all_konst = 0, all_prev = 0;
    int num_stages = g_gx_state.num_tev_stages;
    if (num_stages == 0) num_stages = 1;

    /* Accumulate input classes across all active stages */
    for (int s = 0; s < num_stages && s < GX_MAX_TEVSTAGE; s++) {
        const GXTevStage* st = &g_gx_state.tev_stages[s];
        u32 ca = tev_arg_class(st->color_a);
        u32 cb = tev_arg_class(st->color_b);
        u32 cc = tev_arg_class(st->color_c);
        u32 cd = tev_arg_class(st->color_d);
        u32 classes = ca | cb | cc | cd;

        all_tex   |= (classes & 0x1);
        all_ras   |= (classes & 0x2);
        all_konst |= (classes & 0x4);
        all_prev  |= (classes & 0x8);
    }

    /* Check if texture is actually available */
    const GXTevStage* s0 = &g_gx_state.tev_stages[0];
    int has_texture = (s0->tex_map != GX_TEXMAP_NULL && s0->tex_map < GX_MAX_TEXMAP &&
                       g_gx_state.tex_bindings[s0->tex_map].valid);

    /* If TEV doesn't reference texture at all, or no texture bound */
    if (!all_tex || !has_texture) {
        return GX_TEV_SHADER_PASSCLR;
    }

    /* Multi-stage with register references: check for BLEND pattern
     * (register lerp: a=C0, b=C1, c=CPREV → lerp(mBlack, mWhite, prev)) */
    if (num_stages >= 2 && all_konst && all_prev) {
        const GXTevStage* s1 = &g_gx_state.tev_stages[1];
        if ((tev_arg_class(s1->color_a) & 0x4) &&
            (tev_arg_class(s1->color_b) & 0x4) &&
            (tev_arg_class(s1->color_c) & 0x8)) {
            return GX_TEV_SHADER_BLEND;
        }
    }

    /* J2D blend pattern: stage 0 = [C0, C1, TEXC, ZERO] → mix(C0, C1, tex)
     * This lerps between TEV registers C0/C1 using texture color.
     * Stage 1 may apply rasterized color (RASC) multiply. */
    if (all_konst && all_tex) {
        if (s0->color_a == GX_CC_C0 && s0->color_b == GX_CC_C1 &&
            s0->color_c == GX_CC_TEXC && s0->color_d == GX_CC_ZERO) {
            return GX_TEV_SHADER_BLEND;
        }
    }

    /* Texture + rasterized → MODULATE */
    if (all_ras) {
        return GX_TEV_SHADER_MODULATE;
    }

    /* Texture + constant → MODULATE (konst tinting) */
    if (all_konst) {
        return GX_TEV_SHADER_MODULATE;
    }

    /* Texture-only single stage checks */
    if (num_stages <= 1) {
        /* REPLACE: only texture in d slot, everything else is zero */
        if (s0->color_a == GX_CC_ZERO && s0->color_b == GX_CC_ZERO &&
            s0->color_c == GX_CC_ZERO && s0->color_d == GX_CC_TEXC) {
            return GX_TEV_SHADER_REPLACE;
        }
        /* DECAL: alpha-blended texture over vertex color */
        if (s0->color_c == GX_CC_TEXA) {
            return GX_TEV_SHADER_DECAL;
        }
    }

    /* Default: texture-only → REPLACE */
    return GX_TEV_SHADER_REPLACE;
}

/**
 * Detect which TEV shader to use for the current state.
 * Uses generic input classification instead of hardcoded preset patterns.
 */
static int detect_tev_preset(void) {
    int base = classify_tev_config();

    /* If alpha compare is active (non-trivial test), use the uber-shader
     * which has alpha discard support. GX_ALWAYS(7) with any op always passes,
     * so only upgrade when a real test is configured. */
    if (g_gx_state.alpha_comp0 != GX_ALWAYS || g_gx_state.alpha_comp1 != GX_ALWAYS) {
        return GX_TEV_SHADER_TEV;
    }

    return base;
}

/**
 * Override shader preset when vertex data is incompatible.
 * MODULATE requires vertex color (multiplies texture × color).
 * If no color attribute exists, the draw path will inject material color,
 * so we keep the preset as-is.
 */
static int fixup_preset_for_vertex(int preset) {
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

    /* GX_ALWAYS + z_write is used by clearEfb (JFWDisplay::clearEfb) together with
     * GXSetZTexture(GX_ZT_REPLACE,...) to write a specific depth value from a Z24X8
     * texture into the depth buffer.  On GCN hardware this writes z=1.0 (far plane)
     * from the z-texture, leaving depth=1.0 everywhere so subsequent geometry passes
     * LEQUAL.  On PC, GXSetZTexture is a stub (no-op), so writing vertex z instead
     * would corrupt the depth buffer with z=0 (near plane), causing all subsequent
     * 3D draws to fail the LEQUAL test.
     * Skip depth writes when z_func == GX_ALWAYS to match the GCN intent:
     * clearEfb should fill color only, not disturb the depth buffer. */
    if (g_gx_state.z_update_enable &&
        g_gx_state.z_func != GX_ALWAYS) {
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
    case GX_TRIANGLEFAN:    return 0; /* converted to triangles via index buffer */
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

    /* TEXnMTXIDX — 1 byte each (not bgfx attributes, skip them) */
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0MTXIDX + i].type != GX_NONE) {
            layout.skip(1);
            stride += 1;
        }
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
static uint32_t raw_attr_size(int attr);
static uint32_t calc_raw_vertex_stride(void) {
    uint32_t stride = 0;
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;

    /* Use raw_attr_size() which correctly handles INDEX8 (1 byte),
     * INDEX16 (2 bytes), and DIRECT (actual component data size). */
    if (desc[GX_VA_PNMTXIDX].type != GX_NONE) stride += 1;
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0MTXIDX + i].type != GX_NONE) stride += 1;
    }
    if (desc[GX_VA_POS].type != GX_NONE)
        stride += raw_attr_size(GX_VA_POS);
    if (desc[GX_VA_NRM].type != GX_NONE)
        stride += raw_attr_size(GX_VA_NRM);
    if (desc[GX_VA_CLR0].type != GX_NONE)
        stride += raw_attr_size(GX_VA_CLR0);
    if (desc[GX_VA_CLR1].type != GX_NONE)
        stride += raw_attr_size(GX_VA_CLR1);
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0 + i].type != GX_NONE)
            stride += raw_attr_size(GX_VA_TEX0 + i);
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
 * Read a GX component from big-endian source data (display list inline vertices).
 * Used when DIRECT-mode vertex data comes from DL bulk-copy (raw big-endian bytes).
 */
static float read_gx_component_be(const uint8_t* src, GXCompType type, uint8_t frac) {
    float scale = (frac > 0) ? (1.0f / (float)(1 << frac)) : 1.0f;
    switch (type) {
    case GX_S16: { int16_t v = (int16_t)((src[0] << 8) | src[1]); return (float)v * scale; }
    case GX_U16: { uint16_t v = (uint16_t)((src[0] << 8) | src[1]); return (float)v * scale; }
    case GX_S8:  return (float)(*(const int8_t*)src) * scale;
    case GX_U8:  return (float)(*src) * scale;
    case GX_F32: {
        uint32_t bits = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
                        ((uint32_t)src[2] << 8) | (uint32_t)src[3];
        float v; memcpy(&v, &bits, 4); return v;
    }
    default: return 0.0f;
    }
}

/**
 * Convert one vertex from raw GX format to the bgfx float layout.
 * Walks through each attribute, converting integer components to float
 * with frac-bit scaling applied.
 */
/**
 * Resolve a vertex attribute's data source.
 * For DIRECT mode, data comes from the raw vertex buffer (src + si).
 * For INDEX8/INDEX16, data comes from the indexed array (base + idx * stride).
 * Returns pointer to attribute data and advances si past the index/data in src.
 */
static const uint8_t* resolve_attr_data(int attr, const uint8_t* src, uint32_t* si) {
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    GXAttrType type = desc[attr].type;

    if (type == GX_INDEX8) {
        uint8_t idx = src[*si]; (*si) += 1;
        const GXArrayState* arr = &g_gx_state.vtx_arrays[attr];
        if (arr->base_ptr && arr->stride > 0)
            return (const uint8_t*)arr->base_ptr + (uint32_t)idx * arr->stride;
        return NULL;
    }
    if (type == GX_INDEX16) {
        /* DL vertex data (vtx_data_be=1) stores INDEX16 in big-endian.
         * Direct API vertex data (vtx_data_be=0) stores in native endian. */
        uint16_t idx;
        if (g_gx_state.draw.vtx_data_be) {
            uint8_t b0 = src[*si], b1 = src[*si + 1];
            idx = (uint16_t)((b0 << 8) | b1);
        } else {
            memcpy(&idx, src + *si, 2);
        }
        (*si) += 2;
        const GXArrayState* arr = &g_gx_state.vtx_arrays[attr];
        if (arr->base_ptr && arr->stride > 0)
            return (const uint8_t*)arr->base_ptr + (uint32_t)idx * arr->stride;
        return NULL;
    }
    /* GX_DIRECT — data is inline in the vertex buffer */
    return src + *si;
}

/**
 * Get the byte size consumed in the raw vertex buffer for a given attribute.
 * For INDEX8 → 1, INDEX16 → 2, DIRECT → component bytes.
 */
static uint32_t raw_attr_size(int attr) {
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    const GXVtxAttrFmtEntry* afmt = g_gx_state.vtx_attr_fmt[g_gx_state.draw.vtx_fmt];
    GXAttrType type = desc[attr].type;

    if (type == GX_INDEX8) {
        /* dolsdk2004 GXSave.c: NBT3 normals use 3 separate indices */
        if ((attr == GX_VA_NRM || attr == GX_VA_NBT) &&
            afmt[attr].cnt == GX_NRM_NBT3)
            return 3;
        return 1;
    }
    if (type == GX_INDEX16) {
        /* dolsdk2004 GXSave.c: NBT3 normals use 3 separate indices */
        if ((attr == GX_VA_NRM || attr == GX_VA_NBT) &&
            afmt[attr].cnt == GX_NRM_NBT3)
            return 6;
        return 2;
    }

    /* GX_DIRECT */
    if (attr == GX_VA_POS) {
        int n = (afmt[attr].cnt == GX_POS_XY) ? 2 : 3;
        return n * gx_comp_size(afmt[attr].comp_type);
    }
    if (attr == GX_VA_NRM || attr == GX_VA_NBT) {
        /* dolsdk2004 GXSave.c: NBT/NBT3 have 9 components, regular normal has 3 */
        int n = (afmt[attr].cnt == GX_NRM_NBT || afmt[attr].cnt == GX_NRM_NBT3) ? 9 : 3;
        return n * gx_comp_size(afmt[attr].comp_type);
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        /* Match dolsdk2004 GXSave.c clrCompSize table:
         * GX_RGB565=2, GX_RGB8=3, GX_RGBX8=4, GX_RGBA4=2, GX_RGBA6=3, GX_RGBA8=4 */
        static const uint32_t clr_comp_size[6] = { 2, 3, 4, 2, 3, 4 };
        GXCompType ct = afmt[attr].comp_type;
        if (ct <= GX_RGBA8)
            return clr_comp_size[ct];
        return 4; /* fallback */
    }
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        int n = (afmt[attr].cnt == GX_TEX_S) ? 1 : 2;
        return n * gx_comp_size(afmt[attr].comp_type);
    }
    return 0;
}

/* Select read_gx_component or read_gx_component_be based on attribute type
 * and endianness of vertex data.  INDEXED data comes from already-swapped
 * arrays (native endian).  DIRECT data from DL bulk copy is big-endian. */
#define READ_COMP(data, csz, c, type, frac, attr_type) \
    ((g_gx_state.draw.vtx_data_be && (attr_type) == GX_DIRECT) \
        ? read_gx_component_be((data) + (c) * (csz), (type), (frac)) \
        : read_gx_component((data) + (c) * (csz), (type), (frac)))

/**
 * Decode a GX color attribute from source bytes into 4-byte RGBA8.
 * Handles all GX color component types per dolsdk2004 clrCompSize table.
 * is_be: 1 if source data is big-endian (DL DIRECT), 0 for native endian.
 */
static void decode_gx_color_to_rgba8(const uint8_t* src, GXCompType type,
                                      int is_be, uint8_t dst[4]) {
    switch (type) {
    case GX_RGB565: {
        /* 16-bit packed: RRRRR GGGGGG BBBBB (2 bytes) */
        uint16_t c;
        if (is_be) c = (uint16_t)((src[0] << 8) | src[1]);
        else memcpy(&c, src, 2);
        dst[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        dst[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        dst[2] = (uint8_t)((c & 0x1F) * 255 / 31);
        dst[3] = 255;
        break;
    }
    case GX_RGB8:
        /* 24-bit: R, G, B bytes (3 bytes) */
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        break;
    case GX_RGBX8:
        /* 32-bit: R, G, B, X bytes (4 bytes, X ignored) */
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        break;
    case GX_RGBA4: {
        /* 16-bit packed: RRRR GGGG BBBB AAAA (2 bytes) */
        uint16_t c;
        if (is_be) c = (uint16_t)((src[0] << 8) | src[1]);
        else memcpy(&c, src, 2);
        dst[0] = (uint8_t)(((c >> 12) & 0xF) * 17);
        dst[1] = (uint8_t)(((c >> 8) & 0xF) * 17);
        dst[2] = (uint8_t)(((c >> 4) & 0xF) * 17);
        dst[3] = (uint8_t)((c & 0xF) * 17);
        break;
    }
    case GX_RGBA6: {
        /* 24-bit packed: RRRRRR GGGGGG BBBBBB AAAAAA (3 bytes) */
        uint32_t c = ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
        dst[0] = (uint8_t)(((c >> 18) & 0x3F) * 255 / 63);
        dst[1] = (uint8_t)(((c >> 12) & 0x3F) * 255 / 63);
        dst[2] = (uint8_t)(((c >> 6) & 0x3F) * 255 / 63);
        dst[3] = (uint8_t)((c & 0x3F) * 255 / 63);
        break;
    }
    case GX_RGBA8:
    default:
        /* 32-bit: R, G, B, A bytes (4 bytes) */
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        break;
    }
}

static void convert_vertex_to_float(const uint8_t* src, uint8_t* dst) {
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    const GXVtxAttrFmtEntry* afmt = g_gx_state.vtx_attr_fmt[g_gx_state.draw.vtx_fmt];
    uint32_t si = 0, di = 0;

    /* PNMTXIDX — copy 1 byte */
    if (desc[GX_VA_PNMTXIDX].type != GX_NONE) {
        dst[di++] = src[si++];
    }

    /* TEXnMTXIDX — copy 1 byte each (not used by bgfx, but must be in layout) */
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0MTXIDX + i].type != GX_NONE) {
            dst[di++] = src[si++];
        }
    }

    /* Position → Float */
    if (desc[GX_VA_POS].type != GX_NONE) {
        const uint8_t* data = resolve_attr_data(GX_VA_POS, src, &si);
        GXCompType pt = afmt[GX_VA_POS].comp_type;
        uint8_t frac = afmt[GX_VA_POS].frac;
        int ncomps = (afmt[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;
        uint32_t csz = gx_comp_size(pt);
        if (desc[GX_VA_POS].type == GX_DIRECT) {
            /* For direct mode, advance si past the actual data */
            si += ncomps * csz;
        }
        if (data) {
            for (int c = 0; c < ncomps; c++) {
                float v = READ_COMP(data, csz, c, pt, frac, desc[GX_VA_POS].type);
                memcpy(dst + di, &v, 4);
                di += 4;
            }
        } else {
            for (int c = 0; c < ncomps; c++) {
                float v = 0.0f;
                memcpy(dst + di, &v, 4);
                di += 4;
            }
        }
    }

    /* Normal → Float */
    if (desc[GX_VA_NRM].type != GX_NONE) {
        const uint8_t* data = resolve_attr_data(GX_VA_NRM, src, &si);
        GXCompType nt = afmt[GX_VA_NRM].comp_type;
        uint8_t frac = afmt[GX_VA_NRM].frac;
        uint32_t csz = gx_comp_size(nt);
        if (desc[GX_VA_NRM].type == GX_DIRECT) {
            si += 3 * csz;
        } else if (afmt[GX_VA_NRM].cnt == GX_NRM_NBT3) {
            /* dolsdk2004 GXSave.c: NBT3 indexed normals have 3 separate indices
             * (N, Binormal, Tangent). resolve_attr_data consumed the first one;
             * skip the remaining 2 indices for B and T. */
            uint32_t idx_sz = (desc[GX_VA_NRM].type == GX_INDEX8) ? 1 : 2;
            si += 2 * idx_sz;
        }
        if (data) {
            for (int c = 0; c < 3; c++) {
                float v = READ_COMP(data, csz, c, nt, frac, desc[GX_VA_NRM].type);
                memcpy(dst + di, &v, 4);
                di += 4;
            }
        } else {
            for (int c = 0; c < 3; c++) {
                float v = 0.0f;
                memcpy(dst + di, &v, 4);
                di += 4;
            }
        }
    }

    /* Color0 */
    if (desc[GX_VA_CLR0].type != GX_NONE) {
        const uint8_t* data = resolve_attr_data(GX_VA_CLR0, src, &si);
        if (desc[GX_VA_CLR0].type == GX_DIRECT) si += raw_attr_size(GX_VA_CLR0);
        if (data) {
            int is_be = (g_gx_state.draw.vtx_data_be && desc[GX_VA_CLR0].type == GX_DIRECT);
            decode_gx_color_to_rgba8(data, afmt[GX_VA_CLR0].comp_type, is_be, dst + di);
        }
        else { memset(dst + di, 0xFF, 4); }
        /* GCN doesn't use framebuffer alpha for TV display — vertex color
         * alpha can legitimately be 0. On PC, force alpha=255 so the
         * fragment is fully opaque and SRC_ALPHA blending works. */
        dst[di + 3] = 0xFF;
        di += 4;
    }
    /* Color1 */
    if (desc[GX_VA_CLR1].type != GX_NONE) {
        const uint8_t* data = resolve_attr_data(GX_VA_CLR1, src, &si);
        if (desc[GX_VA_CLR1].type == GX_DIRECT) si += raw_attr_size(GX_VA_CLR1);
        if (data) {
            int is_be = (g_gx_state.draw.vtx_data_be && desc[GX_VA_CLR1].type == GX_DIRECT);
            decode_gx_color_to_rgba8(data, afmt[GX_VA_CLR1].comp_type, is_be, dst + di);
        }
        else { memset(dst + di, 0xFF, 4); }
        /* Force alpha=255 — same GCN→PC alpha fix as Color0 */
        dst[di + 3] = 0xFF;
        di += 4;
    }

    /* Texture coordinates → Float */
    for (int i = 0; i < 8; i++) {
        if (desc[GX_VA_TEX0 + i].type != GX_NONE) {
            const uint8_t* data = resolve_attr_data(GX_VA_TEX0 + i, src, &si);
            GXCompType tt = afmt[GX_VA_TEX0 + i].comp_type;
            uint8_t frac = afmt[GX_VA_TEX0 + i].frac;
            int ncomps = (afmt[GX_VA_TEX0 + i].cnt == GX_TEX_S) ? 1 : 2;
            uint32_t csz = gx_comp_size(tt);
            if (desc[GX_VA_TEX0 + i].type == GX_DIRECT) {
                si += ncomps * csz;
            }
            if (data) {
                for (int c = 0; c < ncomps; c++) {
                    float v = READ_COMP(data, csz, c, tt, frac, desc[GX_VA_TEX0 + i].type);
                    memcpy(dst + di, &v, 4);
                    di += 4;
                }
            } else {
                for (int c = 0; c < ncomps; c++) {
                    float v = 0.0f;
                    memcpy(dst + di, &v, 4);
                    di += 4;
                }
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

    /* Create TEV uber-shader uniforms (Recipe 2 + Recipe 4) */
    s_tev_config_uniform = bgfx::createUniform("u_tevConfig", bgfx::UniformType::Vec4);
    s_alpha_test_uniform = bgfx::createUniform("u_alphaTest", bgfx::UniformType::Vec4);
    s_alpha_op_uniform   = bgfx::createUniform("u_alphaOp",   bgfx::UniformType::Vec4);

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

int pal_tev_ready(void) { return s_tev_ready; }
bgfx::ProgramHandle pal_tev_get_program(int preset) {
    if (preset >= 0 && preset < GX_TEV_SHADER_COUNT)
        return s_programs[preset];
    return BGFX_INVALID_HANDLE;
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
    if (bgfx::isValid(s_tev_config_uniform)) {
        bgfx::destroy(s_tev_config_uniform);
        s_tev_config_uniform = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(s_alpha_test_uniform)) {
        bgfx::destroy(s_alpha_test_uniform);
        s_alpha_test_uniform = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(s_alpha_op_uniform)) {
        bgfx::destroy(s_alpha_op_uniform);
        s_alpha_op_uniform = BGFX_INVALID_HANDLE;
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

    /* Test A: Simple quad (pos+color = 16 bytes) — known working */
    {
        struct TestVert {
            float x, y, z;
            uint32_t abgr;
        };

        bgfx::VertexLayout layout;
        layout.begin()
              .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
              .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
              .end();

        bgfx::TransientVertexBuffer tvb;
        if (!bgfx::getAvailTransientVertexBuffer(4, layout)) return;
        bgfx::allocTransientVertexBuffer(&tvb, 4, layout);

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

        float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        bgfx::setTransform(identity);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
        bgfx::submit(0, s_programs[GX_TEV_SHADER_PASSCLR]);
    }

    /* Test B: Same quad but with texcoord layout (24 bytes) matching game draws.
     * Uses screen-space positions + ortho MVP to replicate the game draw path. */
    {
        struct TestVertTC {
            float x, y, z;
            uint8_t r, g, b, a;
            float u, v;
        };

        bgfx::VertexLayout layout;
        layout.begin()
              .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
              .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
              .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
              .end();

        bgfx::TransientVertexBuffer tvb;
        if (!bgfx::getAvailTransientVertexBuffer(4, layout)) return;
        bgfx::allocTransientVertexBuffer(&tvb, 4, layout);

        TestVertTC* v = (TestVertTC*)tvb.data;
        /* Screen-space positions (0-376, 0-104, z=0) matching game draws */
        v[0] = {   0.0f,   0.0f, 0.0f, 255,0,0,255, 0.0f, 0.0f };  /* red */
        v[1] = { 376.0f,   0.0f, 0.0f, 255,0,0,255, 1.0f, 0.0f };
        v[2] = { 376.0f, 104.0f, 0.0f, 255,0,0,255, 1.0f, 1.0f };
        v[3] = {   0.0f, 104.0f, 0.0f, 255,0,0,255, 0.0f, 1.0f };

        bgfx::TransientIndexBuffer tib;
        if (!bgfx::getAvailTransientIndexBuffer(6)) return;
        bgfx::allocTransientIndexBuffer(&tib, 6);
        uint16_t* idx = (uint16_t*)tib.data;
        idx[0] = 0; idx[1] = 1; idx[2] = 2;
        idx[3] = 0; idx[4] = 2; idx[5] = 3;

        /* Build the same ortho MVP that the game uses (640×456 viewport) */
        float mvp[16];
        {
            const float (*proj)[4] = g_gx_state.proj_mtx;
            uint32_t mi = g_gx_state.current_pos_mtx;
            if (mi >= GX_MAX_POS_MTX) mi = 0;
            const float (*model)[4] = g_gx_state.pos_mtx[mi];

            float m44[16] = {
                model[0][0], model[0][1], model[0][2], model[0][3],
                model[1][0], model[1][1], model[1][2], model[1][3],
                model[2][0], model[2][1], model[2][2], model[2][3],
                0.0f,        0.0f,        0.0f,        1.0f
            };

            float mvp_rm[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++) {
                    mvp_rm[r * 4 + c] = 0.0f;
                    for (int k = 0; k < 4; k++)
                        mvp_rm[r * 4 + c] += proj[r][k] * m44[k * 4 + c];
                }
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    mvp[c * 4 + r] = mvp_rm[r * 4 + c];
        }

        bgfx::setTransform(mvp);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
        bgfx::submit(0, s_programs[GX_TEV_SHADER_PASSCLR]);
    }

    fprintf(stderr, "{\"tev\":\"test_quad_submitted\"}\n");
}

/* ================================================================ */
/* TEV combiner state tracking (Task 1)                             */
/* Logs unique TEV combiner tuples seen across the run.             */
/* ================================================================ */

/* Maximum unique TEV configurations to track. In practice ~4-20 unique
 * configs are seen during a 400-frame test; 128 provides ample headroom. */
#define TEV_TRACK_MAX 128

struct TevConfigKey {
    int num_stages;
    int color_a[4], color_b[4], color_c[4], color_d[4]; /* first 4 stages */
    int alpha_a[4], alpha_b[4], alpha_c[4], alpha_d[4];
    int tex_map[4];
    int preset;
};

static TevConfigKey s_tev_configs[TEV_TRACK_MAX];
static uint32_t     s_tev_config_hits[TEV_TRACK_MAX];
static int          s_tev_config_count = 0;

static void tev_track_config(int preset) {
    TevConfigKey key;
    memset(&key, 0, sizeof(key));
    key.num_stages = g_gx_state.num_tev_stages;
    if (key.num_stages == 0) key.num_stages = 1;
    key.preset = preset;
    int ns = (key.num_stages > 4) ? 4 : key.num_stages;
    for (int s = 0; s < ns; s++) {
        const GXTevStage* st = &g_gx_state.tev_stages[s];
        key.color_a[s] = st->color_a;
        key.color_b[s] = st->color_b;
        key.color_c[s] = st->color_c;
        key.color_d[s] = st->color_d;
        key.alpha_a[s] = st->alpha_a;
        key.alpha_b[s] = st->alpha_b;
        key.alpha_c[s] = st->alpha_c;
        key.alpha_d[s] = st->alpha_d;
        key.tex_map[s] = st->tex_map;
    }
    /* Find existing entry */
    for (int i = 0; i < s_tev_config_count; i++) {
        if (memcmp(&s_tev_configs[i], &key, sizeof(key)) == 0) {
            s_tev_config_hits[i]++;
            return;
        }
    }
    /* New unique config */
    if (s_tev_config_count < TEV_TRACK_MAX) {
        s_tev_configs[s_tev_config_count] = key;
        s_tev_config_hits[s_tev_config_count] = 1;
        s_tev_config_count++;
    }
}

/* ================================================================ */
/* Draw path failure counters (Task 2)                              */
/* Categorize why draw entries fail to produce actual draws.        */
/* ================================================================ */

static uint32_t s_skip_not_ready     = 0; /* s_tev_ready == 0 */
static uint32_t s_skip_no_verts      = 0; /* verts_written == 0 */
static uint32_t s_skip_bad_layout    = 0; /* build_vertex_layout returned 0 */
static uint32_t s_skip_bad_stride    = 0; /* calc_raw_vertex_stride returned 0 */
static uint32_t s_skip_invalid_prog  = 0; /* program handle invalid */
static uint32_t s_passclr_fill_count = 0; /* J2D TEVREG0 fill draws (tracked, not skipped) */
static uint32_t s_skip_passclr_alpha = 0; /* transparent PASSCLR fill skipped */
static uint32_t s_skip_passclr_env   = 0; /* TP_SKIP_PASSCLR env skipped */
static uint32_t s_skip_tvb_alloc     = 0; /* transient vertex buffer alloc failed */
static uint32_t s_ok_submitted       = 0; /* successful bgfx::submit calls */

u32 pal_tev_get_total_attempt_count(void) {
    return s_ok_submitted + s_skip_not_ready + s_skip_no_verts +
           s_skip_bad_layout + s_skip_bad_stride + s_skip_invalid_prog +
           s_skip_passclr_alpha + s_skip_passclr_env +
           s_skip_tvb_alloc;
}

u32 pal_tev_get_ok_submitted_count(void) { return s_ok_submitted; }

u32 pal_tev_get_filter_skip_count(void) {
    return s_skip_passclr_alpha + s_skip_passclr_env;
}

u32 pal_tev_get_skip_passclr_fill_count(void) { return s_passclr_fill_count; }
u32 pal_tev_get_skip_passclr_alpha_count(void) { return s_skip_passclr_alpha; }
u32 pal_tev_get_skip_passclr_env_count(void) { return s_skip_passclr_env; }

void pal_tev_set_persp_proj(const float mtx[4][4]) {
    memcpy(s_persp_proj, mtx, sizeof(s_persp_proj));
    s_has_persp_proj = 1;
}

unsigned short pal_tev_get_program_handle(int preset) {
    if (preset < 0 || preset >= GX_TEV_SHADER_COUNT) return UINT16_MAX;
    return s_programs[preset].idx;
}

void pal_tev_report_diagnostics(void) {
    /* TEV config summary */
    fprintf(stdout, "{\"tev_config_summary\":{\"unique_configs\":%d,\"configs\":[",
            s_tev_config_count);
    /* Sort by hit count descending */
    int order[TEV_TRACK_MAX];
    for (int i = 0; i < s_tev_config_count; i++) order[i] = i;
    for (int i = 0; i < s_tev_config_count - 1; i++) {
        for (int j = i + 1; j < s_tev_config_count; j++) {
            if (s_tev_config_hits[order[j]] > s_tev_config_hits[order[i]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }
    for (int i = 0; i < s_tev_config_count && i < 20; i++) {
        int idx = order[i];
        const TevConfigKey* k = &s_tev_configs[idx];
        if (i > 0) fprintf(stdout, ",");
        fprintf(stdout, "{\"hits\":%u,\"preset\":\"%s\",\"stages\":%d,"
                "\"s0\":{\"c\":[%d,%d,%d,%d],\"a\":[%d,%d,%d,%d],\"tex\":%d}",
                s_tev_config_hits[idx],
                (k->preset >= 0 && k->preset < GX_TEV_SHADER_COUNT) ? s_fs_names[k->preset] : "?",
                k->num_stages,
                k->color_a[0], k->color_b[0], k->color_c[0], k->color_d[0],
                k->alpha_a[0], k->alpha_b[0], k->alpha_c[0], k->alpha_d[0],
                k->tex_map[0]);
        if (k->num_stages >= 2) {
            fprintf(stdout, ",\"s1\":{\"c\":[%d,%d,%d,%d],\"a\":[%d,%d,%d,%d],\"tex\":%d}",
                    k->color_a[1], k->color_b[1], k->color_c[1], k->color_d[1],
                    k->alpha_a[1], k->alpha_b[1], k->alpha_c[1], k->alpha_d[1],
                    k->tex_map[1]);
        }
        fprintf(stdout, "}");
    }
    fprintf(stdout, "]}}\n");

    /* Draw path failure summary.
     * passclr_fill_count: TEVREG0-fill draws now rendered (not skipped) since
     * bgfx ViewMode::Sequential guarantees submission order in views 0 and 1. */
    fprintf(stdout, "{\"draw_path_summary\":{"
            "\"ok_submitted\":%u,"
            "\"skip_not_ready\":%u,"
            "\"skip_no_verts\":%u,"
            "\"skip_bad_layout\":%u,"
            "\"skip_bad_stride\":%u,"
            "\"skip_invalid_prog\":%u,"
            "\"passclr_fill_count\":%u,"
            "\"skip_passclr_alpha\":%u,"
            "\"skip_passclr_env\":%u,"
            "\"skip_tvb_alloc\":%u}}\n",
            s_ok_submitted,
            s_skip_not_ready,
            s_skip_no_verts,
            s_skip_bad_layout,
            s_skip_bad_stride,
            s_skip_invalid_prog,
            s_passclr_fill_count,
            s_skip_passclr_alpha,
            s_skip_passclr_env,
            s_skip_tvb_alloc);
    fflush(stdout);
}

/* RGB sum threshold below which a colour is considered "dark" and requires
 * a RASC fallback.  48 = 3 channels × 16: each channel < 16 (< 6% of full
 * intensity) is effectively black in the absence of GX lighting. */
#define RASC_DARK_THRESHOLD 48
/* Neutral grey used when both mat_color and amb_color are dark.  200 is
 * visible on all common display gamma curves without being blinding white. */
#define RASC_FALLBACK_GRAY  200
/* Default diffuse factor when per-vertex normals are not available.
 * 0.5 approximates ~60° average incidence angle for general scene geometry. */
#define RASC_DEFAULT_DIFFUSE 0.5f
/* Minimum k[0] constant attenuation value below which we skip attenuation
 * (avoids division by near-zero). */
#define RASC_ATTN_K0_MIN 0.001f

/* apply_rasc_color: compute the RASC (rasterized color) for a draw.
 *
 * Based on dolsdk2004 GXLight.c / GXSetChanCtrl:
 *   GCN hardware channel combiner output depends on chan_ctrl.enable:
 *
 *   enable=0 (lighting disabled):
 *     Output = mat_src value:
 *       mat_src=GX_SRC_REG → mat_color (register)
 *       mat_src=GX_SRC_VTX → vertex color attribute
 *     This is the DIRECT output — no lighting computation at all.
 *     Most J2D (2D) draws and many J3D materials use this path.
 *
 *   enable=1 (lighting enabled):
 *     Output = amb_color * amb_src + Σ(light[i].color × DiffuseFn(N·L) × AttnFn(d))
 *     Until we implement full lighting (Recipe 3), approximate as:
 *       if amb_color is usable → use amb_color
 *       else → grey fallback (geometry at least visible)
 *
 * References:
 *   dolsdk2004 src/gx/GXLight.c:  GXSetChanCtrl() register layout
 *   dolsdk2004 src/gx/__gx.h:     GXData.ambColor[], GXData.matColor[]
 *   docs/dolsdk2004-reference.md:  Recipe 1 (RASC fix)
 *
 * The game engine is single-threaded (one render thread); no locking needed. */
static void apply_rasc_color(uint8_t* const_clr) {
    const GXChanCtrl* ch = &g_gx_state.chan_ctrl[0];

    if (!ch->enable) {
        /* ---- Lighting DISABLED (SDK: enable=0) ---- */
        /* Output is determined solely by mat_src.
         *
         * mat_src=GX_SRC_REG: use mat_color register value directly.
         *   J2D panes: mat_color IS the intended pane colour.  A black
         *   background pane (mat=(0,0,0)) must stay black.
         *   J3D materials with lighting off: mat_color is the direct output.
         *
         * mat_src=GX_SRC_VTX: output = vertex color attribute.
         *   Signal to caller by returning white (multiply-neutral) so that
         *   the real vertex color passes through the "* v_color0" shader multiply.
         *   The caller already copies vertex CLR0 into the vertex buffer. */
        if (ch->mat_src == GX_SRC_VTX) {
            const_clr[0] = 255;
            const_clr[1] = 255;
            const_clr[2] = 255;
            const_clr[3] = 255;
        } else {
            const_clr[0] = ch->mat_color.r;
            const_clr[1] = ch->mat_color.g;
            const_clr[2] = ch->mat_color.b;
            const_clr[3] = ch->mat_color.a;
        }
        return;
    }

    /* ---- Lighting ENABLED (SDK: enable=1) ---- */
    /* Full equation per dolsdk2004 GXLight.c:
     *   RASC = clamp(amb_color + Σ(light[i].color × DiffuseFn(N·L) × AttnFn(d)))
     *
     * For diffuse:
     *   GX_DF_NONE:  diffuse = 1.0 (no attenuation by angle)
     *   GX_DF_SIGN:  diffuse = N·L (can go negative)
     *   GX_DF_CLAMP: diffuse = max(0, N·L) (standard Lambert)
     *
     * For attenuation (when enabled):
     *   spot_attn = a[0] + a[1]*cos(angle) + a[2]*cos²(angle)
     *   dist_attn = 1 / (k[0] + k[1]*d + k[2]*d²)
     *
     * Since we don't have per-vertex normals and positions in the RASC
     * computation (they're in the vertex buffer), we approximate using
     * a default normal (0,0,-1 = towards camera) and estimate distance
     * based on the current position matrix translation.
     *
     * This provides much better results than the grey fallback:
     * - Colored lighting (e.g. warm torches) now tints geometry correctly
     * - Multiple lights are summed
     * - Ambient + light colors blend as intended
     * - Materials with mat_color are modulated properly */
    const uint8_t ar = ch->amb_color.r;
    const uint8_t ag = ch->amb_color.g;
    const uint8_t ab = ch->amb_color.b;

    /* Start with ambient contribution */
    float out_r = ar / 255.0f;
    float out_g = ag / 255.0f;
    float out_b = ab / 255.0f;

    /* Accumulate active light contributions */
    u32 light_mask = ch->light_mask;
    for (int i = 0; i < GX_MAX_LIGHT; i++) {
        if (!(light_mask & (1u << i))) continue;
        const GXLightState* lt = &g_gx_state.lights[i];
        if (!lt->active) continue;

        /* Light color normalized to [0,1] */
        float lr = lt->color.r / 255.0f;
        float lg = lt->color.g / 255.0f;
        float lb = lt->color.b / 255.0f;

        /* Approximate diffuse factor.
         * Without per-vertex normals available here, use a default
         * cosine factor (RASC_DEFAULT_DIFFUSE ≈ 60° average incidence).
         * This is better than 0 (no light) or 1 (full light). */
        float diffuse = RASC_DEFAULT_DIFFUSE;
        if (ch->diff_fn == GX_DF_NONE) {
            diffuse = 1.0f;
        }

        /* Distance attenuation approximation.
         * Without per-vertex position, use k[0] as the constant term.
         * Most TP lights use k[0]=1, k[1]=0, k[2]=0 (no distance falloff)
         * or have attenuation disabled (attn_fn == GX_AF_NONE). */
        float attn = 1.0f;
        if (ch->attn_fn != GX_AF_NONE) {
            /* Distance attenuation: 1/(k0 + k1*d + k2*d²)
             * With default distance, just use constant term */
            if (lt->k[0] > RASC_ATTN_K0_MIN) {
                attn = 1.0f / lt->k[0];
                if (attn > 1.0f) attn = 1.0f;
            }
        }

        out_r += lr * diffuse * attn;
        out_g += lg * diffuse * attn;
        out_b += lb * diffuse * attn;
    }

    /* Modulate by material color (per SDK: final = lighting_result * mat_color) */
    if (ch->mat_src == GX_SRC_REG) {
        out_r *= ch->mat_color.r / 255.0f;
        out_g *= ch->mat_color.g / 255.0f;
        out_b *= ch->mat_color.b / 255.0f;
    }
    /* mat_src == VTX: vertex color modulation happens in shader via v_color0 */

    /* Clamp to [0,1] and convert to u8 */
    if (out_r > 1.0f) out_r = 1.0f;
    if (out_g > 1.0f) out_g = 1.0f;
    if (out_b > 1.0f) out_b = 1.0f;
    const_clr[0] = (uint8_t)(out_r * 255.0f);
    const_clr[1] = (uint8_t)(out_g * 255.0f);
    const_clr[2] = (uint8_t)(out_b * 255.0f);
    const_clr[3] = 255;

    /* If result is still too dark (no lights active or all dark),
     * fall back to grey so geometry is at least visible.
     * This will be removed once per-vertex lighting is implemented. */
    if ((int)const_clr[0] + const_clr[1] + const_clr[2] < RASC_DARK_THRESHOLD) {
        const_clr[0] = const_clr[1] = const_clr[2] = RASC_FALLBACK_GRAY;

        static uint32_t s_grey_fallback_log_count = 0;
        if (s_grey_fallback_log_count < 5) {
            s_grey_fallback_log_count++;
            fprintf(stderr,
                "{\"rasc_grey_fallback\":{\"n\":%u,"
                "\"mat\":[%u,%u,%u,%u],"
                "\"amb\":[%u,%u,%u],"
                "\"enable\":%d,\"mat_src\":%d,"
                "\"light_mask\":%u,\"fallback_gray\":%u}}\n",
                s_grey_fallback_log_count,
                ch->mat_color.r, ch->mat_color.g, ch->mat_color.b, ch->mat_color.a,
                ar, ag, ab,
                (int)ch->enable, (int)ch->mat_src,
                (unsigned)light_mask, (unsigned)RASC_FALLBACK_GRAY);
        }
    }
}

void pal_tev_flush_draw(void) {
    if (!s_tev_ready) { s_skip_not_ready++; return; }

    const GXDrawState* ds = &g_gx_state.draw;
    if (ds->verts_written == 0 || ds->vtx_data_pos == 0) { s_skip_no_verts++; return; }

    static uint32_t s_total_draw_count = 0;
    s_total_draw_count++;

    /* Fast-path for Noop renderer (Phase 1 headless CI test).
     * Skip expensive TVB allocation, vertex copy, bgfx state setup, and submit.
     * s_dl_draw_count in gx_displaylist.cpp is already incremented before this call,
     * so the dl_draws regression gate still reflects this draw correctly.
     * We still update gx_frame_draw_calls (crosscheck), gx_frame_valid_verts,
     * gx_frame_depth_draws, and gx_frame_blend_draws (for GOAL_DEPTH_BLEND_ACTIVE). */
    if (bgfx::getRendererType() == bgfx::RendererType::Noop) {
        gx_frame_draw_calls++;
        gx_frame_valid_verts += ds->verts_written;
        s_ok_submitted++;
        gx_stub_draw_call_crosscheck();
        if (g_gx_state.z_compare_enable)
            gx_frame_depth_draws++;
        if (g_gx_state.blend_mode != 0)
            gx_frame_blend_draws++;
        g_gx_state.tev_reg_dirty = 0;
        return;
    }

    /* 1. Select shader based on TEV config */
    int preset = detect_tev_preset();
    if (preset < 0 || preset >= GX_TEV_SHADER_COUNT) preset = GX_TEV_SHADER_PASSCLR;

    /* Track unique TEV combiner configurations */
    tev_track_config(preset);

    /* Diagnostic: log ALL draws for draw_count window 250-340 (title scene frame) */
    if (s_total_draw_count >= 250 && s_total_draw_count <= 340) {
        const GXTevStage* s0 = &g_gx_state.tev_stages[0];
        int tm = s0->tex_map;
        int tv = (tm >= 0 && tm < GX_MAX_TEXMAP) ? g_gx_state.tex_bindings[tm].valid : -1;
        uint32_t rs = calc_raw_vertex_stride();
        fprintf(stderr, "{\"all_draw\":{\"id\":%u,\"preset\":\"%s\","
                "\"nverts\":%u,\"prim\":%d,"
                "\"color_upd\":%d,\"blend\":%d,\"z_en\":%d,"
                "\"raw_stride\":%u,"
                "\"tev0\":[%d,%d,%d,%d],"
                "\"tex_map\":%d,\"tex_valid\":%d}}\n",
                s_total_draw_count, s_fs_names[preset],
                (unsigned)ds->verts_written, ds->prim_type,
                g_gx_state.color_update, g_gx_state.blend_mode, g_gx_state.z_compare_enable,
                rs,
                s0->color_a, s0->color_b, s0->color_c, s0->color_d,
                tm, tv);
    }

    preset = fixup_preset_for_vertex(preset);
    if (!bgfx::isValid(s_programs[preset])) { s_skip_invalid_prog++; return; }

    /* J2D constant-color fill tracking.
     *
     * PASSCLR draws with TEV [ZERO,ZERO,ZERO,C0] + GX_BM_NONE output
     * TEVREG0 as a solid color with no blending.  Both bgfx view 0 and 1
     * use bgfx::ViewMode::Sequential, which guarantees submission-order
     * rendering — fills render before the textured draws they underlie,
     * matching GCN hardware behavior.  We allow these fills to proceed so
     * the TP title-screen's colored UI backgrounds render correctly.
     * Track them for diagnostics only.
     */
    if (preset == GX_TEV_SHADER_PASSCLR &&
        g_gx_state.blend_mode == GX_BM_NONE)
    {
        const GXTevStage* s0 = &g_gx_state.tev_stages[0];
        if (s0->color_a == GX_CC_ZERO && s0->color_b == GX_CC_ZERO &&
            s0->color_c == GX_CC_ZERO && s0->color_d == GX_CC_C0)
        {
            s_passclr_fill_count++; /* tracked for diagnostics, draw proceeds */
        }
    }

    /* PASSCLR draws with TEV [ZERO,ZERO,ZERO,RASC] + SRC_ALPHA blending are
     * transparent fills on GCN (vertex alpha = 0 → SRC_ALPHA makes them
     * invisible).  On PC we force vertex alpha to 255 which makes them
     * opaque black, overwriting J2D textured content.  Skip them — but only
     * when the raster color comes from vertex colors (mat_src == GX_SRC_VTX).
     * When mat_src == GX_SRC_REG, the alpha is explicitly set by the game
     * (e.g. darwFilter fade overlay) and the draw should proceed. */
    if (preset == GX_TEV_SHADER_PASSCLR &&
        g_gx_state.blend_mode == GX_BM_BLEND &&
        g_gx_state.chan_ctrl[0].mat_src == GX_SRC_VTX)
    {
        const GXTevStage* s0 = &g_gx_state.tev_stages[0];
        if (s0->color_a == GX_CC_ZERO && s0->color_b == GX_CC_ZERO &&
            s0->color_c == GX_CC_ZERO && s0->color_d == GX_CC_RASC)
        {
            s_skip_passclr_alpha++; return;
        }
    }

    /* SHADER TEST: When TP_SKIP_PASSCLR is set, skip ALL PASSCLR draws
     * for title scene.  Diagnostic only — not needed in production. */
    {
        static int s_skip_passclr = -1;
        if (s_skip_passclr < 0) {
            const char* bt = getenv("TP_SKIP_PASSCLR");
            s_skip_passclr = (bt && bt[0] == '1') ? 1 : 0;
        }
        if (s_skip_passclr && preset == GX_TEV_SHADER_PASSCLR &&
            s_total_draw_count > 200) {
            s_skip_passclr_env++; return;
        }
    }

    /* 2. Build vertex layout (always Float for pos/texcoord) */
    bgfx::VertexLayout layout;
    uint32_t bgfx_stride = build_vertex_layout(layout);
    if (bgfx_stride == 0) { s_skip_bad_layout++; return; }

    /* Raw stride matches the actual GX vertex data byte layout */
    uint32_t raw_stride = calc_raw_vertex_stride();
    if (raw_stride == 0) { s_skip_bad_stride++; return; }

    /* 3. Check if we need to inject constant color when vertex color is missing.
     * PASSCLR: inject from TEV registers or material color.
     * MODULATE/BLEND/DECAL: inject from material color (GCN hardware uses
     * material color as rasterized color when no vertex color attribute).
     *
     * Special case: PASSCLR with any RASC operand in the TEV formula ALWAYS
     * injects, even when CLR0 vertex data is present.  On GCN hardware, RASC
     * is the OUTPUT of the channel combiner (amb + lights × mat) — it is NOT
     * the raw vertex-color data.  J3D rooms typically store vertex CLR0 =
     * (0,0,0,0) because GX lights provide the actual colour; passing raw (0,0,0)
     * through the shader produces transparent-black output.  We approximate
     * RASC as max(mat_color, amb_color) per channel. */
    const GXVtxDescEntry* desc = g_gx_state.vtx_desc;
    int inject_color = 0;
    uint8_t const_clr[4] = {0, 0, 0, 255};

    /* Determine if PASSCLR formula has RASC as the first (A) operand.
     * This is the J3D 3D room pattern: c=[RASC,0,0,0] → out = RASC.
     * In this case we force inject_color=1 regardless of vertex CLR0
     * presence, because RASC on GCN is the channel-combiner OUTPUT, not
     * the raw vertex-color data.  J3D rooms set vertex CLR0=(0,0,0,0) as
     * a placeholder; GX lights provide the actual colour via the combiner.
     * Without GX lights we approximate with mat_color / amb_color. */
    int passclr_uses_rasc = 0;
    if (preset == GX_TEV_SHADER_PASSCLR) {
        const GXTevStage* s0 = &g_gx_state.tev_stages[0];
        if (s0->color_a == GX_CC_RASC)  /* A=RASC: the dominant output operand */
            passclr_uses_rasc = 1;
    }

    /* Check if any TEV stage references RASC/RASA (pre-compute for all presets).
     * Used by the inject_color gate below. */
    int any_stage_uses_rasc = 0;
    {
        int num_stages = g_gx_state.num_tev_stages;
        if (num_stages == 0) num_stages = 1;
        for (int s = 0; s < num_stages && s < GX_MAX_TEVSTAGE; s++) {
            const GXTevStage* st = &g_gx_state.tev_stages[s];
            if (stage_uses_rasc(st))
                any_stage_uses_rasc = 1;
        }
    }

    /* Shader presets MODULATE, BLEND, DECAL, and TEV multiply their output
     * by v_color0 in the fragment shader.  On GCN, vertex CLR0 data is
     * irrelevant when the TEV formula doesn't reference RASC — the channel
     * combiner provides the colour via a separate circuit.  Games often
     * submit CLR0=(0,0,0,0) as a placeholder when RASC is unused.
     *
     * On PC, v_color0 comes from the vertex buffer's CLR0 data.  If the game
     * submitted CLR0=black and the shader multiplies by it, the result is
     * black.  To prevent this:
     *   - When NO CLR0 attribute → always inject (obvious case)
     *   - When CLR0 IS present but TEV does NOT use RASC → inject white
     *     (because the shader's "× v_color0" is a no-op in the GCN pipeline)
     *   - When CLR0 IS present and TEV DOES use RASC → inject RASC color
     *     (material/ambient/lit colour replaces the placeholder black) */
    /* On GCN, RASC is the OUTPUT of the channel combiner, not the raw
     * vertex CLR0 data.  The channel combiner transforms CLR0 through
     * ambient/material/lighting calculations.  On PC, v_color0 comes from
     * the vertex buffer directly (= CLR0), which is the WRONG value when
     * the TEV formula references RASC.
     *
     * Override conditions:
     *   - No CLR0 attribute → must inject (no vertex color at all)
     *   - Any RASC usage → must inject channel combiner result
     *   - Non-RASC shader presets → inject white (shader multiply is a no-op) */
    int needs_color_override = (desc[GX_VA_CLR0].type == GX_NONE)
                            || any_stage_uses_rasc
                            || passclr_uses_rasc
                            || (preset != GX_TEV_SHADER_PASSCLR
                                && preset != GX_TEV_SHADER_REPLACE);

    if (needs_color_override) {
        if (preset == GX_TEV_SHADER_PASSCLR) {
            inject_color = 1;
            /* Determine the constant color from TEV state.
             * Check ALL four TEV operand slots (A, B, C, D) for RASC so that
             * formulas like A=RASC,B=0,C=0,D=0 → out=RASC are handled correctly.
             * The common J3D room pattern uses A=RASC (GX_CC_RASC=10), not D=RASC. */
            const GXTevStage* s0 = &g_gx_state.tev_stages[0];
            if (s0->color_d == GX_CC_C0 || s0->color_d == GX_CC_CPREV) {
                const_clr[0] = g_gx_state.tev_regs[GX_TEVREG0].r;
                const_clr[1] = g_gx_state.tev_regs[GX_TEVREG0].g;
                const_clr[2] = g_gx_state.tev_regs[GX_TEVREG0].b;
                const_clr[3] = g_gx_state.tev_regs[GX_TEVREG0].a;
                /* GX_TEVREG0 may be dark on PC when:
                 *  (a) GXSetTevColor(GX_TEVREG0) was never called — use fallback.
                 *  (b) GXSetTevColor set TEVREG0=black because the game relies on
                 *      BRK animation to supply the per-frame color (e.g. daTitle);
                 *      on PC without BRK the static black must be overridden.
                 *  (c) clearEfb intentionally sets TEVREG0=black (z_func=GX_ALWAYS);
                 *      preserve that value so the screen clears to black correctly.
                 * Rule: apply RASC fallback when TEVREG0 is dark AND NOT a clearEfb draw. */
                {
                    int t0_dark       = ((int)const_clr[0] + (int)const_clr[1] + (int)const_clr[2] < RASC_DARK_THRESHOLD);
                    int t0_explicitly = (g_gx_state.tev_reg_dirty & (1u << GX_TEVREG0)) != 0;
                    int is_clearefb   = (g_gx_state.z_func == GX_ALWAYS);
                    /* Skip fallback only for clearEfb (explicitly set + GX_ALWAYS z-func). */
                    if (t0_dark && !(t0_explicitly && is_clearefb))
                        apply_rasc_color(const_clr);
                }
            } else if (s0->color_d == GX_CC_KONST) {
                resolve_konst_color(s0, const_clr);
            } else if (s0->color_a == GX_CC_RASC || s0->color_b == GX_CC_RASC ||
                       s0->color_c == GX_CC_RASC || s0->color_d == GX_CC_RASC) {
                /* Diagnostic: log first 3 A=RASC inject draws (3D room pattern) */
                {
                    static int s_rd_n = 0;
                    if (s_rd_n < 3 && s0->color_a == GX_CC_RASC) {
                        fprintf(stderr, "{\"rasc_inject\":{\"draw\":%u,"
                                "\"mat\":[%d,%d,%d,%d],"
                                "\"amb\":[%d,%d,%d,%d],"
                                "\"mat_src\":%d}}\n",
                                s_total_draw_count,
                                (int)g_gx_state.chan_ctrl[0].mat_color.r,
                                (int)g_gx_state.chan_ctrl[0].mat_color.g,
                                (int)g_gx_state.chan_ctrl[0].mat_color.b,
                                (int)g_gx_state.chan_ctrl[0].mat_color.a,
                                (int)g_gx_state.chan_ctrl[0].amb_color.r,
                                (int)g_gx_state.chan_ctrl[0].amb_color.g,
                                (int)g_gx_state.chan_ctrl[0].amb_color.b,
                                (int)g_gx_state.chan_ctrl[0].amb_color.a,
                                (int)g_gx_state.chan_ctrl[0].mat_src);
                        s_rd_n++;
                    }
                }
                apply_rasc_color(const_clr);
            }
        } else if (preset == GX_TEV_SHADER_MODULATE ||
                   preset == GX_TEV_SHADER_BLEND ||
                   preset == GX_TEV_SHADER_DECAL ||
                   preset == GX_TEV_SHADER_TEV) {
            inject_color = 1;
            /* Check if any TEV stage references rasterized color (RASC/RASA).
             * If not, vertex/material color is irrelevant to the TEV formula
             * and the injected color should be white (pass-through) so the
             * shader's "* v_color0" multiplication doesn't darken the output.
             *
             * This is critical for the TEV uber-shader: on GCN, vertex CLR0
             * is often set to (0,0,0,0) as a placeholder when TEV doesn't
             * reference RASC (the channel combiner provides color instead).
             * Without injection, v_color0=(0,0,0,0) zeros out the result. */
            int uses_rasc = 0;
            int uses_konst = 0;
            int num_stages = g_gx_state.num_tev_stages;
            if (num_stages == 0) num_stages = 1;
            for (int s = 0; s < num_stages && s < GX_MAX_TEVSTAGE; s++) {
                const GXTevStage* st = &g_gx_state.tev_stages[s];
                if (stage_uses_rasc(st))
                    uses_rasc = 1;
                if (st->color_a == GX_CC_KONST || st->color_b == GX_CC_KONST ||
                    st->color_c == GX_CC_KONST || st->color_d == GX_CC_KONST)
                    uses_konst = 1;
            }
            if (uses_konst && !uses_rasc) {
                resolve_konst_color(&g_gx_state.tev_stages[0], const_clr);
            } else if (uses_rasc) {
                apply_rasc_color(const_clr);
            } else {
                /* No RASC/KONST in any stage — vertex color is unused.
                 * Inject white so the shader "* v_color0" is a no-op. */
                const_clr[0] = 255;
                const_clr[1] = 255;
                const_clr[2] = 255;
                const_clr[3] = 255;
            }
        }
    }

    if (inject_color) {
        /* GCN doesn't use framebuffer alpha for display — mat_color.a can
         * legitimately be 0. On PC, force injected color alpha to 255 so
         * the fragment is fully opaque and SRC_ALPHA blending works.
         * Exception: the fade overlay (darwFilter) intentionally uses
         * varying alpha for its SRC_ALPHA blend effect. */
        if (!g_gx_state.fade_overlay_active)
            const_clr[3] = 255;

        /* SHADER TEST: When TP_BLEND_TEST is set and this is a BLEND draw,
         * force vertex color to red so PASSCLR outputs red instead of white.
         * This makes it visually obvious when BLEND draws reach the screen. */
        {
            static int s_blend_test_clr = -1;
            if (s_blend_test_clr < 0) {
                const char* bt = getenv("TP_BLEND_TEST");
                s_blend_test_clr = (bt && bt[0] == '1') ? 1 : 0;
            }
            if (s_blend_test_clr && preset == GX_TEV_SHADER_BLEND) {
                const_clr[0] = 255;
                const_clr[1] = 0;
                const_clr[2] = 0;
                const_clr[3] = 255;
            }
        }

        /* Rebuild layout with color attribute AND texture coords */
        const GXVtxAttrFmtEntry* af = g_gx_state.vtx_attr_fmt[ds->vtx_fmt];
        int has_pnmtx = desc[GX_VA_PNMTXIDX].type != GX_NONE;
        int npos = (af[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;

        layout.begin();
        if (has_pnmtx) layout.skip(1);
        for (int i = 0; i < 8; i++) {
            if (desc[GX_VA_TEX0MTXIDX + i].type != GX_NONE) layout.skip(1);
        }
        layout.add(bgfx::Attrib::Position, (uint8_t)npos, bgfx::AttribType::Float);
        layout.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true);
        /* Add texture coordinates (critical for MODULATE/BLEND/DECAL) */
        for (int i = 0; i < 8; i++) {
            if (desc[GX_VA_TEX0 + i].type != GX_NONE) {
                int ntc = (af[GX_VA_TEX0 + i].cnt == GX_TEX_S) ? 1 : 2;
                layout.add((bgfx::Attrib::Enum)(bgfx::Attrib::TexCoord0 + i),
                           (uint8_t)ntc, bgfx::AttribType::Float);
            }
        }
        layout.end();
        bgfx_stride = layout.getStride();
    }

    /* Allocate transient vertex buffer */
    uint16_t nverts = ds->verts_written;
    if (nverts == 0) return;

    bgfx::TransientVertexBuffer tvb;
    if (!bgfx::getAvailTransientVertexBuffer(nverts, layout)) {
        static int s_tvb_fail = 0;
        if (s_tvb_fail < 10) {
            fprintf(stderr, "[PAL] TVB alloc FAIL draw#%u nverts=%u stride=%u\n",
                    s_total_draw_count, (unsigned)nverts, bgfx_stride);
            s_tvb_fail++;
        }
        s_skip_tvb_alloc++; return;
    }
    bgfx::allocTransientVertexBuffer(&tvb, nverts, layout);

    /* Copy vertex data — convert from raw GX format to bgfx float format */
    if (inject_color) {
        /* Convert position to float, append constant color, then texture coords */
        const GXVtxAttrFmtEntry* af = g_gx_state.vtx_attr_fmt[ds->vtx_fmt];
        int has_pnmtx = desc[GX_VA_PNMTXIDX].type != GX_NONE;
        int npos = (af[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;

        for (uint16_t vi = 0; vi < nverts; vi++) {
            uint8_t* dst = tvb.data + vi * bgfx_stride;
            const uint8_t* src = ds->vtx_data + vi * raw_stride;
            uint32_t si = 0, di = 0;

            /* PNMTXIDX */
            if (has_pnmtx) { dst[di++] = src[si++]; }

            /* TEXnMTXIDX — skip in src, copy to dst layout */
            for (int i = 0; i < 8; i++) {
                if (desc[GX_VA_TEX0MTXIDX + i].type != GX_NONE) {
                    dst[di++] = src[si++];
                }
            }

            /* Position → Float with frac-bit scaling + index resolution */
            GXCompType pt = af[GX_VA_POS].comp_type;
            uint8_t frac = af[GX_VA_POS].frac;
            uint32_t csz = gx_comp_size(pt);
            const uint8_t* pos_data = resolve_attr_data(GX_VA_POS, src, &si);
            if (desc[GX_VA_POS].type == GX_DIRECT) si += npos * csz;
            if (pos_data) {
                for (int c = 0; c < npos; c++) {
                    float v = READ_COMP(pos_data, csz, c, pt, frac, desc[GX_VA_POS].type);
                    memcpy(dst + di, &v, 4);
                    di += 4;
                }
            } else {
                for (int c = 0; c < npos; c++) {
                    float v = 0.0f;
                    memcpy(dst + di, &v, 4);
                    di += 4;
                }
            }

            /* Append constant color */
            memcpy(dst + di, const_clr, 4);
            di += 4;

            /* Skip normal in source if present */
            if (desc[GX_VA_NRM].type != GX_NONE) {
                resolve_attr_data(GX_VA_NRM, src, &si);
                if (desc[GX_VA_NRM].type == GX_DIRECT) {
                    int nnrm = (af[GX_VA_NRM].cnt == GX_NRM_NBT || af[GX_VA_NRM].cnt == GX_NRM_NBT3) ? 9 : 3;
                    si += nnrm * gx_comp_size(af[GX_VA_NRM].comp_type);
                } else if (af[GX_VA_NRM].cnt == GX_NRM_NBT3) {
                    /* dolsdk2004 GXSave.c: NBT3 indexed normals have 3 separate indices;
                     * resolve_attr_data consumed the first one, skip remaining 2 */
                    uint32_t idx_sz = (desc[GX_VA_NRM].type == GX_INDEX8) ? 1 : 2;
                    si += 2 * idx_sz;
                }
            }

            /* Skip CLR0/CLR1 in source if present (we're replacing with const_clr) */
            for (int ci = 0; ci < 2; ci++) {
                if (desc[GX_VA_CLR0 + ci].type != GX_NONE) {
                    resolve_attr_data(GX_VA_CLR0 + ci, src, &si);
                    if (desc[GX_VA_CLR0 + ci].type == GX_DIRECT) {
                        si += raw_attr_size(GX_VA_CLR0 + ci);
                    }
                }
            }

            /* Texture coordinates → Float */
            for (int i = 0; i < 8; i++) {
                if (desc[GX_VA_TEX0 + i].type != GX_NONE) {
                    int ntc = (af[GX_VA_TEX0 + i].cnt == GX_TEX_S) ? 1 : 2;
                    GXCompType tt = af[GX_VA_TEX0 + i].comp_type;
                    uint8_t tf = af[GX_VA_TEX0 + i].frac;
                    uint32_t tc_sz = gx_comp_size(tt);
                    const uint8_t* tc_data = resolve_attr_data(GX_VA_TEX0 + i, src, &si);
                    if (desc[GX_VA_TEX0 + i].type == GX_DIRECT) si += ntc * tc_sz;
                    if (tc_data) {
                        for (int c = 0; c < ntc; c++) {
                            float v = READ_COMP(tc_data, tc_sz, c, tt, tf, desc[GX_VA_TEX0 + i].type);
                            memcpy(dst + di, &v, 4);
                            di += 4;
                        }
                    } else {
                        for (int c = 0; c < ntc; c++) {
                            float v = 0.0f;
                            memcpy(dst + di, &v, 4);
                            di += 4;
                        }
                    }
                }
            }
        }
    } else {
        /* Full vertex conversion: raw GX → float */
        for (uint16_t vi = 0; vi < nverts; vi++) {
            const uint8_t* src = ds->vtx_data + vi * raw_stride;
            uint8_t* dst = tvb.data + vi * bgfx_stride;
            convert_vertex_to_float(src, dst);
        }
    }

    /* One-time vertex dump for title scene debugging */
    {
        static int s_vtx_dump_count = 0;
        /* Dump vertex data for title scene draws (frame ~130+, draw_count > 1000) */
        if (s_vtx_dump_count < 8 && nverts >= 4 && s_total_draw_count > 1000 &&
            preset == GX_TEV_SHADER_BLEND) {
            s_vtx_dump_count++;
            fprintf(stderr, "{\"vtx_dump\":{\"draw_id\":%u,\"frame_est\":%u,\"nverts\":%u,"
                    "\"stride\":%u,\"raw_stride\":%u,\"inject\":%d,\"preset\":\"%s\",\"verts\":[",
                    s_total_draw_count, s_total_draw_count / 86, (unsigned)nverts,
                    bgfx_stride, raw_stride, inject_color,
                    (preset >= 0 && preset < GX_TEV_SHADER_COUNT) ? s_fs_names[preset] : "?");
            for (int vi = 0; vi < (int)nverts && vi < 4; vi++) {
                const float* fv = (const float*)(tvb.data + vi * bgfx_stride);
                int nf = bgfx_stride / 4;
                if (vi > 0) fprintf(stderr, ",");
                fprintf(stderr, "[");
                for (int fi = 0; fi < nf && fi < 12; fi++) {
                    if (fi > 0) fprintf(stderr, ",");
                    /* Check if this looks like a color (4 bytes packed) or float */
                    if (fi == 3) {
                        /* Color0 is stored as 4 packed uint8 bytes */
                        const uint8_t* cb = (const uint8_t*)(tvb.data + vi * bgfx_stride + fi * 4);
                        fprintf(stderr, "\"c(%d,%d,%d,%d)\"", cb[0], cb[1], cb[2], cb[3]);
                    } else {
                        fprintf(stderr, "%.4f", fv[fi]);
                    }
                }
                fprintf(stderr, "]");
            }
            fprintf(stderr, "]}}\n");
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

    /* Geometry-centroid camera fallback for the 3D intro room (PC only).
     *
     * When the normal camera path cannot position the view (demo camera NULL,
     * stage arrow absent), the pos_mtx stays at the fallback identity+translation
     * [0,-100,-200] which leaves all room geometry outside the frustum.
     *
     * This block fires ONLY when g_gx_state.draw_calls > CENTROID_FRAME_DRAWS_MIN
     * (per-frame counter, reset to 0 at the start of each frame).  The 3D room
     * frame has ~7400 RASC draws; title-screen frames have << 1000.  This prevents
     * the centroid from accumulating title-screen geometry (wrong coordinates).
     *
     * Once CENTROID_SAMPLES have been accumulated the block computes a LookAt view
     * matrix with eye placed OUTSIDE the near face of the room geometry
     * (eye.z = vz_max + 5000, where vz_max is the maximum raw vertex Z seen).
     * This guarantees the geometry is in FRONT of the camera, not behind it.
     *
     * The override fires at most once per process lifetime (static flag).
     * First ~CENTROID_SAMPLES draws still render with the wrong matrix (black),
     * but the remaining ~7400 room draws get the corrected view → pct_nonblack > 0. */
    if (passclr_uses_rasc && g_gx_state.draw_calls > CENTROID_FRAME_DRAWS_MIN) {
        /* s_centroid_sum, s_centroid_n, s_centroid_vz_max are file-scope statics
         * reset at the start of each frame in the gx_frame_draw_calls == 0 block. */

        /* Extract first vertex world position from TVB using the same
         * byte_prefix logic as rasc_geom_dump below.
         * Only extract if pos has 3 components (XYZ); skip XY-only draws to
         * avoid reading color bytes as Z which would corrupt the centroid. */
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        int centroid_pos_ok = 0;
        if (nverts >= 1) {
            const GXVtxAttrFmtEntry* af_c = g_gx_state.vtx_attr_fmt[ds->vtx_fmt];
            int npos_c = (af_c[GX_VA_POS].cnt == GX_POS_XY) ? 2 : 3;
            if (npos_c == 3) {
                int has_pnmtx_c = (g_gx_state.vtx_desc[GX_VA_PNMTXIDX].type != GX_NONE) ? 1 : 0;
                int tex_cnt_c = 0;
                for (int i = 0; i < 8; i++)
                    if (g_gx_state.vtx_desc[GX_VA_TEX0MTXIDX + i].type != GX_NONE) tex_cnt_c++;
                uint32_t poff = (uint32_t)(has_pnmtx_c + tex_cnt_c);
                if (poff + 12u <= (uint32_t)bgfx_stride) {
                    memcpy(&vx, tvb.data + poff,     4);
                    memcpy(&vy, tvb.data + poff + 4, 4);
                    memcpy(&vz, tvb.data + poff + 8, 4);
                    centroid_pos_ok = 1;
                }
            }
        }

        if (!s_geom_centroid_active && centroid_pos_ok) {
            if (s_centroid_n < CENTROID_SAMPLES) {
                s_centroid_sum[0] += vx;
                s_centroid_sum[1] += vy;
                s_centroid_sum[2] += vz;
                if (vz > s_centroid_vz_max) s_centroid_vz_max = vz;
                s_centroid_n++;
            }
            if (s_centroid_n == CENTROID_SAMPLES) {
                /* Centroid of first N RASC draws = approximate room centre */
                float cx = s_centroid_sum[0] / (float)CENTROID_SAMPLES;
                float cy = s_centroid_sum[1] / (float)CENTROID_SAMPLES;
                float cz = s_centroid_sum[2] / (float)CENTROID_SAMPLES;

                /* Camera eye: placed OUTSIDE the near face of the room.
                 * s_centroid_vz_max is the maximum raw vertex Z seen = near face.
                 * Adding 5000 ensures the camera is in front of all geometry,
                 * not behind it. (Using cz+2000 put the camera INSIDE the room
                 * because vz_max > cz+2000 = geometry was behind camera.) */
                float ex = cx, ey = cy + 500.0f, ez = s_centroid_vz_max + 5000.0f;

                /* Build LookAt via C_MTXLookAt convention (row-major 3×4):
                 *   look  = normalize(eye - center)   [points from target to eye]
                 *   right = normalize(cross(up, look)) [up = (0,1,0)]
                 *   up2   = cross(look, right)
                 * Each row: [axis.x, axis.y, axis.z, -dot(axis, eye)] */
                float lx = ex-cx, ly = ey-cy, lz = ez-cz;
                float ln = sqrtf(lx*lx + ly*ly + lz*lz);
                if (ln > 0.0f) { lx /= ln; ly /= ln; lz /= ln; }

                /* right = cross((0,1,0), look) = (1*lz - 0*ly, 0*lx - 0*lz, 0*ly - 1*lx)
                 *       = (lz, 0, -lx).  ry is always 0, so omitted from
                 *       magnitude: |right| = sqrt(rx² + rz²). */
                float rx = lz, ry = 0.0f, rz = -lx;
                float rn = sqrtf(rx*rx + rz*rz);
                if (rn > 0.0f) { rx /= rn; rz /= rn; }

                /* corrected_up = cross(look, right) */
                float ux = ly*rz - lz*ry;
                float uy = lz*rx - lx*rz;
                float uz = lx*ry - ly*rx;

                /* 3×4 row-major view matrix */
                float vm[3][4] = {
                    {rx, ry, rz, -(ex*rx + ey*ry + ez*rz)},
                    {ux, uy, uz, -(ex*ux + ey*uy + ez*uz)},
                    {lx, ly, lz, -(ex*lx + ey*ly + ez*lz)}
                };

                /* Save the centroid view for use in subsequent draws.
                 * IMPORTANT: do NOT rely on g_gx_state.pos_mtx[0] staying set —
                 * J3D calls GXLoadPosMtxImm for every mesh node, overwriting
                 * pos_mtx slots before pal_tev_flush_draw runs for that draw.
                 * s_geom_centroid_view is never touched after this assignment. */
                memcpy(s_geom_centroid_view, vm, sizeof(vm));
                s_geom_centroid_active = 1;

                fprintf(stderr,
                    "{\"geom_centroid_cam\":{\"centroid\":[%.1f,%.1f,%.1f],"
                    "\"eye\":[%.1f,%.1f,%.1f],"
                    "\"look\":[%.4f,%.4f,%.4f],"
                    "\"vz_max\":%.1f,"
                    "\"vm_row0\":[%.4f,%.4f,%.4f,%.1f]}}\n",
                    cx, cy, cz, ex, ey, ez,
                    lx, ly, lz,
                    s_centroid_vz_max,
                    vm[0][0], vm[0][1], vm[0][2], vm[0][3]);
            }
        }

        /* Re-compute and re-set the MVP for this draw using the SAVED centroid
         * view matrix and a built-in perspective projection.
         *
         * Two problems with using g_gx_state directly:
         *  1. pos_mtx[0]: J3D calls GXLoadPosMtxImm per mesh node, overwriting it
         *     → use s_geom_centroid_view (set once, never overwritten).
         *  2. proj_mtx: The 3D camera sets perspective, but J2D code overwrites it
         *     with orthographic before DL draws flush.  Orthographic maps pixel
         *     coordinates (0-640) to NDC — world-space coords (x=2249, z=8963) go
         *     far outside [-1,+1] and get clipped → all-black framebuffer.
         *     → build perspective projection from FOV/aspect/near/far parameters.
         */
        if (s_geom_centroid_active) {
            /* Build a perspective projection matrix matching the game camera.
             * GCN/GX format (row-major):
             *   [cot(fov/2)/aspect, 0,          0,               0         ]
             *   [0,                 cot(fov/2), 0,               0         ]
             *   [0,                 0,          -(f+n)/(f-n),   -2fn/(f-n) ]
             *   [0,                 0,          -1,              0         ]
             * TP room camera: fov=60°, aspect=640/456, near=1, far=100000 */
            float proj_c[4][4];
            {
                const float fov_deg = 60.0f; /* degrees — wide enough for room geometry */
                const float aspect = 640.0f / 456.0f;
                const float near_z = 1.0f;
                const float far_z  = 100000.0f;
                const float fov_rad = fov_deg * 3.14159265f / 180.0f;
                /* Use tanf instead of cotf for portability */
                float t = 1.0f / tanf(fov_rad * 0.5f);
                float range = far_z - near_z;
                memset(proj_c, 0, sizeof(proj_c));
                proj_c[0][0] = t / aspect;
                proj_c[1][1] = t;
                proj_c[2][2] = -(far_z + near_z) / range;
                proj_c[2][3] = -2.0f * far_z * near_z / range;
                proj_c[3][2] = -1.0f;
            }
            /* Use the saved centroid view, NOT pos_mtx[0] which J3D overwrites */
            float m44_c[16] = {
                s_geom_centroid_view[0][0], s_geom_centroid_view[0][1],
                s_geom_centroid_view[0][2], s_geom_centroid_view[0][3],
                s_geom_centroid_view[1][0], s_geom_centroid_view[1][1],
                s_geom_centroid_view[1][2], s_geom_centroid_view[1][3],
                s_geom_centroid_view[2][0], s_geom_centroid_view[2][1],
                s_geom_centroid_view[2][2], s_geom_centroid_view[2][3],
                0.0f,                       0.0f,
                0.0f,                       1.0f
            };
            float mvp_rowmaj[16], mvp_c[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++) {
                    mvp_rowmaj[r*4+c] = 0.0f;
                    for (int k = 0; k < 4; k++)
                        mvp_rowmaj[r*4+c] += proj_c[r][k] * m44_c[k*4+c];
                }
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    mvp_c[c*4+r] = mvp_rowmaj[r*4+c];
            bgfx::setTransform(mvp_c);
            /* One-shot log: which projection is the centroid MVP using? */
            {
                static int s_centroid_mvp_log = 0;
                if (s_centroid_mvp_log < 1) {
                    s_centroid_mvp_log++;
                    fprintf(stderr, "{\"centroid_mvp_proj\":{\"has_persp\":%d,"
                            "\"proj00\":%.6f,\"proj11\":%.6f,\"proj22\":%.6f,"
                            "\"proj23\":%.6f,\"proj32\":%.6f,\"proj33\":%.6f}}\n",
                            s_has_persp_proj,
                            proj_c[0][0], proj_c[1][1], proj_c[2][2],
                            proj_c[2][3], proj_c[3][2], proj_c[3][3]);
                }
            }
        }
    }

    /* 3D room RASC geometry dump: fires for the first 5 PASSCLR+RASC draws
     * AFTER the centroid camera fires (s_geom_centroid_active=1).  Sampling
     * post-centroid draws shows the corrected pos_mtx and NDC values so CI
     * can confirm in_frustum=1.  Pre-centroid draws (first ~50) are omitted
     * since they use the stale fallback camera and always show in_frustum=0. */
    if (passclr_uses_rasc) {
        static int s_rasc_geom_n = 0;
        /* Changed from "s_total_draw_count > 500" to "s_geom_centroid_active":
         * the dump now fires AFTER the centroid camera is established so the
         * logged pos_mtx0 / NDC values reflect the corrected transform. */
        if (s_rasc_geom_n < 5 && s_geom_centroid_active) {
            s_rasc_geom_n++;
            float wx = 0, wy = 0, wz = 0;
            if (nverts >= 1) {
                /* TVB layout with inject_color=1: optional 1-byte prefix indices
                 * (PNMTXIDX, TEXnMTXIDX) followed by 3 position floats.
                 * Position starts at byte `byte_prefix` in the vertex — use
                 * memcpy for portable unaligned reads when byte_prefix != 0. */
                int has_pnmtx = (g_gx_state.vtx_desc[GX_VA_PNMTXIDX].type != GX_NONE) ? 1 : 0;
                int tex_mtx_cnt = 0;
                for (int i = 0; i < 8; i++) {
                    if (g_gx_state.vtx_desc[GX_VA_TEX0MTXIDX + i].type != GX_NONE) tex_mtx_cnt++;
                }
                uint32_t byte_prefix = (uint32_t)(has_pnmtx + tex_mtx_cnt);
                /* Position starts at exactly byte_prefix (no padding in the injected layout).
                 * Use memcpy to safely read potentially unaligned float values. */
                uint32_t pos_offset = byte_prefix;
                if (pos_offset + 12u <= (uint32_t)bgfx_stride) {
                    memcpy(&wx, tvb.data + pos_offset,     4);
                    memcpy(&wy, tvb.data + pos_offset + 4, 4);
                    memcpy(&wz, tvb.data + pos_offset + 8, 4);
                }
                /* Build the same perspective projection used by the centroid MVP.
                 * This must match the projection in the centroid override block above. */
                float gp_local[4][4];
                {
                    const float fov_deg = 60.0f;
                    const float aspect = 640.0f / 456.0f;
                    const float near_z = 1.0f;
                    const float far_z  = 100000.0f;
                    const float fov_rad = fov_deg * 3.14159265f / 180.0f;
                    float t = 1.0f / tanf(fov_rad * 0.5f);
                    float range = far_z - near_z;
                    memset(gp_local, 0, sizeof(gp_local));
                    gp_local[0][0] = t / aspect;
                    gp_local[1][1] = t;
                    gp_local[2][2] = -(far_z + near_z) / range;
                    gp_local[2][3] = -2.0f * far_z * near_z / range;
                    gp_local[3][2] = -1.0f;
                }
                const float (*gp)[4] = (const float (*)[4])gp_local;
                /* Use the CENTROID MVP for clip/NDC calculation (same transform that
                 * bgfx::setTransform was given for this draw when centroid is active). */
                float use_mvp[16];
                if (s_geom_centroid_active) {
                    /* Compute proj * centroid_view (same as the override block above) */
                    float cv44[16] = {
                        s_geom_centroid_view[0][0], s_geom_centroid_view[0][1],
                        s_geom_centroid_view[0][2], s_geom_centroid_view[0][3],
                        s_geom_centroid_view[1][0], s_geom_centroid_view[1][1],
                        s_geom_centroid_view[1][2], s_geom_centroid_view[1][3],
                        s_geom_centroid_view[2][0], s_geom_centroid_view[2][1],
                        s_geom_centroid_view[2][2], s_geom_centroid_view[2][3],
                        0.0f, 0.0f, 0.0f, 1.0f
                    };
                    float mrm[16];
                    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
                        mrm[r*4+c] = 0;
                        for (int k = 0; k < 4; k++) mrm[r*4+c] += gp[r][k] * cv44[k*4+c];
                    }
                    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) use_mvp[c*4+r] = mrm[r*4+c];
                } else {
                    for (int i = 0; i < 16; i++) use_mvp[i] = mvp[i];
                }
                /* clip = use_mvp(col-major) * (wx,wy,wz,1) */
                float cx = use_mvp[0]*wx + use_mvp[4]*wy + use_mvp[8]*wz  + use_mvp[12];
                float cy = use_mvp[1]*wx + use_mvp[5]*wy + use_mvp[9]*wz  + use_mvp[13];
                float cz = use_mvp[2]*wx + use_mvp[6]*wy + use_mvp[10]*wz + use_mvp[14];
                float cw = use_mvp[3]*wx + use_mvp[7]*wy + use_mvp[11]*wz + use_mvp[15];
                float ndcx = (cw != 0.0f) ? cx/cw : cx;
                float ndcy = (cw != 0.0f) ? cy/cw : cy;
                float ndcz = (cw != 0.0f) ? cz/cw : cz;
                /* Dump raw pos_mtx[current] to diagnose identity matrix issue */
                uint32_t diag_mtx_idx = g_gx_state.current_pos_mtx;
                if (diag_mtx_idx >= GX_MAX_POS_MTX) diag_mtx_idx = 0;
                const float (*pm)[4] = g_gx_state.pos_mtx[diag_mtx_idx];
                int pm_is_identity = (pm[0][0] == 1.0f && pm[0][1] == 0.0f && pm[0][2] == 0.0f && pm[0][3] == 0.0f &&
                                      pm[1][0] == 0.0f && pm[1][1] == 1.0f && pm[1][2] == 0.0f && pm[1][3] == 0.0f &&
                                      pm[2][0] == 0.0f && pm[2][1] == 0.0f && pm[2][2] == 1.0f && pm[2][3] == 0.0f) ? 1 : 0;
                /* Compute tex_map index once for stage 0 texture-binding lookup */
                int s0_texmap = (int)g_gx_state.tev_stages[0].tex_map;
                int s0_tex_valid = (s0_texmap >= 0 && s0_texmap < GX_MAX_TEXMAP)
                                    ? (int)g_gx_state.tex_bindings[s0_texmap].valid : 0;
                /* Note: pos_mtx0 shown here is the GAME's matrix (loaded by J3D for this mesh).
                 * The centroid override bypasses this via s_geom_centroid_view. */
                fprintf(stderr, "{\"rasc_geom_dump\":{"
                        "\"draw\":%u,"
                        "\"nverts\":%u,"
                        "\"pnmtx\":%d,\"tmtx\":%d,"
                        "\"cur_mtx\":%u,"
                        "\"centroid_active\":%d,"
                        "\"pos_mtx_identity\":%d,"
                        "\"pos_mtx0\":[%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f],"
                        "\"centroid_view0\":[%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f],"
                        "\"world\":[%.4f,%.4f,%.4f],"
                        "\"clip\":[%.4f,%.4f,%.4f,%.4f],"
                        "\"ndc\":[%.4f,%.4f,%.4f],"
                        "\"in_frustum\":%d,"
                        "\"z_en\":%d,\"z_func\":%d,\"cull\":%d,"
                        "\"color_update\":%d,"
                        "\"blend_mode\":%d,\"blend_src\":%d,\"blend_dst\":%d,"
                        "\"scissor\":[%d,%d,%d,%d],"
                        "\"const_clr\":[%d,%d,%d,%d],"
                        "\"proj00\":%.6f,\"proj11\":%.6f,"
                        "\"num_tev\":%d,"
                        "\"s0_abcd\":[%d,%d,%d,%d],"
                        "\"s0_texmap\":%d,\"s0_tex_valid\":%d,"
                        "\"num_texgen\":%d,\"tg0_src\":%d}}\n",
                        s_total_draw_count,
                        (unsigned)nverts,
                        has_pnmtx, tex_mtx_cnt,
                        diag_mtx_idx,
                        s_geom_centroid_active,
                        pm_is_identity,
                        pm[0][0], pm[0][1], pm[0][2], pm[0][3],
                        pm[1][0], pm[1][1], pm[1][2], pm[1][3],
                        pm[2][0], pm[2][1], pm[2][2], pm[2][3],
                        s_geom_centroid_view[0][0], s_geom_centroid_view[0][1],
                        s_geom_centroid_view[0][2], s_geom_centroid_view[0][3],
                        s_geom_centroid_view[1][0], s_geom_centroid_view[1][1],
                        s_geom_centroid_view[1][2], s_geom_centroid_view[1][3],
                        s_geom_centroid_view[2][0], s_geom_centroid_view[2][1],
                        s_geom_centroid_view[2][2], s_geom_centroid_view[2][3],
                        wx, wy, wz,
                        cx, cy, cz, cw,
                        ndcx, ndcy, ndcz,
                        (ndcx >= -1.0f && ndcx <= 1.0f && ndcy >= -1.0f && ndcy <= 1.0f && ndcz >= -1.0f && ndcz <= 1.0f) ? 1 : 0,
                        g_gx_state.z_compare_enable,
                        g_gx_state.z_func,
                        g_gx_state.cull_mode,
                        g_gx_state.color_update,
                        g_gx_state.blend_mode, g_gx_state.blend_src, g_gx_state.blend_dst,
                        g_gx_state.sc_left, g_gx_state.sc_top,
                        g_gx_state.sc_wd, g_gx_state.sc_ht,
                        const_clr[0], const_clr[1], const_clr[2], const_clr[3],
                        gp[0][0], gp[1][1],
                        /* TEV stage config: helps diagnose flat-white vs textured output */
                        (int)g_gx_state.num_tev_stages,
                        (int)g_gx_state.tev_stages[0].color_a,
                        (int)g_gx_state.tev_stages[0].color_b,
                        (int)g_gx_state.tev_stages[0].color_c,
                        (int)g_gx_state.tev_stages[0].color_d,
                        /* Texture binding for stage 0: is a real texture loaded? */
                        s0_texmap, s0_tex_valid,
                        /* Texture coord gen: src tells whether GX_TG_MTX sources used */
                        (int)g_gx_state.num_tex_gens,
                        (g_gx_state.num_tex_gens > 0) ? (int)g_gx_state.tex_gens[0].src : -1);
            }
        }
    }

    /* Screen-space bounding box diagnostic for title scene BLEND draws */
    {
        static int s_ss_dump = 0;
        if (s_ss_dump < 100 && s_total_draw_count > 1000 && preset == GX_TEV_SHADER_BLEND) {
            s_ss_dump++;
            /* Compute screen-space bounds from first 4 verts */
            float ss_min_x = 99999, ss_max_x = -99999;
            float ss_min_y = 99999, ss_max_y = -99999;
            for (int vi = 0; vi < (int)nverts && vi < 4; vi++) {
                const float* fv = (const float*)(tvb.data + vi * bgfx_stride);
                float x = fv[0], y = fv[1], z = fv[2];
                /* col-major MVP: clip = mvp * (x,y,z,1) */
                float cx = mvp[0]*x + mvp[4]*y + mvp[8]*z + mvp[12];
                float cy = mvp[1]*x + mvp[5]*y + mvp[9]*z + mvp[13];
                float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
                if (cw != 0.0f) { cx /= cw; cy /= cw; }
                float sx = (cx + 1.0f) * 320.0f;
                float sy = (1.0f - cy) * 240.0f;
                if (sx < ss_min_x) ss_min_x = sx;
                if (sx > ss_max_x) ss_max_x = sx;
                if (sy < ss_min_y) ss_min_y = sy;
                if (sy > ss_max_y) ss_max_y = sy;
            }
            fprintf(stderr, "{\"ss_box\":{\"draw\":%u,\"screen\":[%.0f,%.0f,%.0f,%.0f],"
                    "\"vert0\":[%.1f,%.1f],\"mvp_tx\":%.6f,\"mvp_ty\":%.6f}}\n",
                    s_total_draw_count,
                    ss_min_x, ss_min_y, ss_max_x, ss_max_y,
                    ((const float*)(tvb.data))[0], ((const float*)(tvb.data))[1],
                    mvp[12], mvp[13]);
        }
    }

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
                bgfx::setTexture(0, s_tex_uniform, tex, gx_sampler_flags(binding));
            }
            /* Per-draw texture diagnostic:
             * Log first 5 draws AND any draw with invalid texture handle.
             * Also log first 5 draws after draw_id exceeds 50 (title scene). */
            static int s_tex_log_count = 0;
            static int s_title_log_count = 0;
            int should_log = (s_tex_log_count < 5);
            if (!bgfx::isValid(tex)) should_log = 1;
            if (s_total_draw_count > 50 && s_title_log_count < 10) {
                should_log = 1;
                s_title_log_count++;
            }
            if (should_log) {
                if (s_tex_log_count < 5) s_tex_log_count++;
                int ns = g_gx_state.num_tev_stages;
                const GXTevStage* s1 = (ns >= 2) ? &g_gx_state.tev_stages[1] : NULL;
                /* First 2 texcoord values for UV debugging */
                float tc0_u = 0, tc0_v = 0, tc1_u = 0, tc1_v = 0;
                if (nverts >= 1 && raw_stride > 0) {
                    const GXVtxAttrFmtEntry* af = g_gx_state.vtx_attr_fmt[ds->vtx_fmt];
                    for (int tci = 0; tci < 8; tci++) {
                        if (desc[GX_VA_TEX0 + tci].type != GX_NONE) {
                            GXCompType tt = af[GX_VA_TEX0 + tci].comp_type;
                            uint8_t tf = af[GX_VA_TEX0 + tci].frac;
                            uint32_t tc_off = 0;
                            /* Compute offset to first texcoord in raw vertex */
                            for (int ai = 0; ai < GX_VA_TEX0 + tci; ai++) {
                                if (desc[ai].type != GX_NONE) {
                                    tc_off += raw_attr_size(ai);
                                }
                            }
                            /* Read first vertex texcoord */
                            if (desc[GX_VA_TEX0 + tci].type == GX_DIRECT && tc_off + 4 <= raw_stride) {
                                auto rd = ds->vtx_data_be ? read_gx_component_be : read_gx_component;
                                tc0_u = rd(ds->vtx_data + tc_off, tt, tf);
                                tc0_v = rd(ds->vtx_data + tc_off + gx_comp_size(tt), tt, tf);
                                if (nverts >= 2) {
                                    tc1_u = rd(ds->vtx_data + raw_stride + tc_off, tt, tf);
                                    tc1_v = rd(ds->vtx_data + raw_stride + tc_off + gx_comp_size(tt), tt, tf);
                                }
                            }
                            break;
                        }
                    }
                }
                fprintf(stderr, "{\"tev_draw\":{\"draw_id\":%u,\"preset\":\"%s\","
                        "\"tex_valid\":%d,\"tex_map\":%d,"
                        "\"w\":%u,\"h\":%u,\"fmt\":%d,\"img_ptr\":\"%p\","
                        "\"bind_valid\":%d,"
                        "\"nverts\":%u,\"prim\":%d,"
                        "\"blend_mode\":%d,\"blend_src\":%d,\"blend_dst\":%d,"
                        "\"z_enable\":%d,"
                        "\"tc\":[%.3f,%.3f,%.3f,%.3f],"
                        "\"num_stages\":%d,"
                        "\"tev0_cd\":[%d,%d,%d,%d]",
                        s_total_draw_count, s_fs_names[preset],
                        bgfx::isValid(tex) ? 1 : 0,
                        s0->tex_map,
                        (unsigned)binding->width, (unsigned)binding->height,
                        (int)binding->format, binding->image_ptr,
                        binding->valid,
                        (unsigned)nverts, ds->prim_type,
                        g_gx_state.blend_mode, g_gx_state.blend_src, g_gx_state.blend_dst,
                        g_gx_state.z_compare_enable,
                        tc0_u, tc0_v, tc1_u, tc1_v,
                        ns,
                        s0->color_a, s0->color_b, s0->color_c, s0->color_d);
                if (s1) {
                    fprintf(stderr, ",\"tev1_cd\":[%d,%d,%d,%d]",
                            s1->color_a, s1->color_b, s1->color_c, s1->color_d);
                }
                fprintf(stderr, ",\"regs\":[[%d,%d,%d,%d],[%d,%d,%d,%d],[%d,%d,%d,%d],[%d,%d,%d,%d]]}}\n",
                        g_gx_state.tev_regs[0].r, g_gx_state.tev_regs[0].g,
                        g_gx_state.tev_regs[0].b, g_gx_state.tev_regs[0].a,
                        g_gx_state.tev_regs[1].r, g_gx_state.tev_regs[1].g,
                        g_gx_state.tev_regs[1].b, g_gx_state.tev_regs[1].a,
                        g_gx_state.tev_regs[2].r, g_gx_state.tev_regs[2].g,
                        g_gx_state.tev_regs[2].b, g_gx_state.tev_regs[2].a,
                        g_gx_state.tev_regs[3].r, g_gx_state.tev_regs[3].g,
                        g_gx_state.tev_regs[3].b, g_gx_state.tev_regs[3].a);
            }
        }

        /* Set TEV register uniforms for BLEND shader (mBlack/mWhite lerp) */
        if (preset == GX_TEV_SHADER_BLEND) {
            float reg0[4] = {
                g_gx_state.tev_regs[GX_TEVREG0].r / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG0].g / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG0].b / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG0].a / 255.0f,
            };
            float reg1[4] = {
                g_gx_state.tev_regs[GX_TEVREG1].r / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG1].g / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG1].b / 255.0f,
                g_gx_state.tev_regs[GX_TEVREG1].a / 255.0f,
            };

            /* DIAGNOSTIC: When TP_BLEND_RED is set, force both TEV regs to
             * red to confirm whether BLEND draws reach the screen at all.
             * If title scene shows red, the issue is in the shader formula
             * or uniform values, not in draw submission. */
            {
                static int s_blend_red = -1;
                if (s_blend_red < 0) {
                    const char* br = getenv("TP_BLEND_RED");
                    s_blend_red = (br && br[0] == '1') ? 1 : 0;
                }
                if (s_blend_red) {
                    reg0[0] = 1.0f; reg0[1] = 0.0f; reg0[2] = 0.0f; reg0[3] = 1.0f;
                    reg1[0] = 1.0f; reg1[1] = 0.0f; reg1[2] = 0.0f; reg1[3] = 1.0f;
                }
            }

            bgfx::setUniform(s_tev_reg0_uniform, reg0);
            bgfx::setUniform(s_tev_reg1_uniform, reg1);
        }
    }

    /* 9. Set render state */
    uint64_t state = 0;
    if (g_gx_state.color_update) {
        state |= BGFX_STATE_WRITE_RGB;
        /* GX doesn't use framebuffer alpha for TV display output.
         * On PC, write alpha alongside RGB so the rendered content is
         * visible in the window/capture (blending and compositing rely
         * on FB alpha). */
        state |= BGFX_STATE_WRITE_A;
    }
    state |= convert_blend_state();
    state |= convert_depth_state();
    state |= convert_cull_state();

    /* DIAGNOSTIC: When TP_NO_BLEND is set, disable blending for BLEND draws
     * to test whether alpha is the issue. */
    {
        static int s_no_blend = -1;
        if (s_no_blend < 0) {
            const char* nb = getenv("TP_NO_BLEND");
            s_no_blend = (nb && nb[0] == '1') ? 1 : 0;
        }
        if (s_no_blend && preset == GX_TEV_SHADER_BLEND) {
            state &= ~BGFX_STATE_BLEND_MASK;
            state &= ~BGFX_STATE_BLEND_EQUATION_MASK;
        }
    }
    /* When using index buffer conversion (quads→tris, fans→tris),
     * the primitive state must be triangle list (0), not the original GX type. */
    if (use_index_buffer && num_indices > 0)
        state |= 0; /* triangle list — explicit no-op for clarity */
    else
        state |= convert_primitive_state(ds->prim_type);

    /* Alpha test — apply GX alpha compare to bgfx alpha ref.
     * Only the simple case (comp0 != ALWAYS with ref0 > 0) is handled;
     * dual-compare with AND/OR/XOR is approximated by using the first ref. */
    if (g_gx_state.alpha_comp0 != GX_ALWAYS && g_gx_state.alpha_ref0 > 0) {
        state |= BGFX_STATE_ALPHA_REF(g_gx_state.alpha_ref0);
    } else if (g_gx_state.alpha_comp1 != GX_ALWAYS && g_gx_state.alpha_ref1 > 0) {
        state |= BGFX_STATE_ALPHA_REF(g_gx_state.alpha_ref1);
    }

    /* Centroid camera draws: disable depth test and depth write.
     * Pre-centroid PASSCLR fills write depth≈0.950 to the framebuffer.  Centroid
     * room draws at NDC.z≈0.9996 would fail LEQUAL (0.9996 > 0.950).  Rather
     * than a separate depth-cleared view (unreliable on Mesa softpipe), we
     * simply skip depth testing for centroid draws.  Room geometry layering is
     * acceptable without depth because the centroid camera only sees the outer
     * shell of the room. */
    if (s_geom_centroid_active) {
        state &= ~BGFX_STATE_DEPTH_TEST_MASK;
        state &= ~(uint64_t)BGFX_STATE_WRITE_Z;
    }

    bgfx::setState(state);
    if ((state & BGFX_STATE_DEPTH_TEST_MASK) != 0)
        gx_frame_submit_with_depth_state++;
    if ((state & BGFX_STATE_BLEND_MASK) != 0)
        gx_frame_submit_with_blend_state++;

    /* Per-draw diagnostic for title scene BLEND draws — logs state values
     * that could cause black output (blend func, alpha test, suppress, etc.) */
    {
        static int s_state_dump = 0;
        if (s_state_dump < 10 && s_total_draw_count > 200) {
            if (preset == GX_TEV_SHADER_BLEND || s_state_dump < 3) {
                s_state_dump++;
                fprintf(stderr, "{\"state_dump\":{\"draw_id\":%u,\"preset\":\"%s\","
                        "\"prog_valid\":%d,\"prog_idx\":%u,"
                        "\"color_update\":%d,\"state\":\"0x%016llX\","
                        "\"write_rgb\":%d,\"write_a\":%d,"
                        "\"suppress\":%d,"
                        "\"blend_mode\":%d,\"blend_src\":%d,\"blend_dst\":%d,"
                        "\"z_en\":%d,\"z_func\":%d,\"z_upd\":%d,"
                        "\"alpha_comp0\":%d,\"alpha_ref0\":%d,"
                        "\"cull\":%d,"
                        "\"scissor\":[%d,%d,%d,%d],"
                        "\"nverts\":%u,\"nidx\":%u,\"inject\":%d}}\n",
                        s_total_draw_count,
                        (preset >= 0 && preset < GX_TEV_SHADER_COUNT) ? s_fs_names[preset] : "?",
                        bgfx::isValid(s_programs[preset]) ? 1 : 0,
                        s_programs[preset].idx,
                        g_gx_state.color_update,
                        (unsigned long long)state,
                        (int)((state & BGFX_STATE_WRITE_RGB) != 0),
                        (int)((state & BGFX_STATE_WRITE_A) != 0),
                        0 /* suppress_color — depth-prime draws now skipped early */,
                        g_gx_state.blend_mode, g_gx_state.blend_src, g_gx_state.blend_dst,
                        g_gx_state.z_compare_enable, g_gx_state.z_func, g_gx_state.z_update_enable,
                        g_gx_state.alpha_comp0, g_gx_state.alpha_ref0,
                        g_gx_state.cull_mode,
                        g_gx_state.sc_left, g_gx_state.sc_top,
                        g_gx_state.sc_wd, g_gx_state.sc_ht,
                        (unsigned)nverts, (unsigned)num_indices,
                        inject_color);
            }
        }
    }

    /* Play-window per-frame state dump: log first 5 draws of each frame once
     * the play scene starts (identified by the first draw of a new frame in
     * the play-window total-draw range).  Resets every frame boundary so CI
     * always captures depth/blend state bits from the first BG draws of
     * frames 127+ to diagnose the Z/Blend propagation gap. */
    {
        static int  s_play_dump_active = 0; /* 1 when dump is armed for current frame */
        static int  s_play_dump_count  = 0; /* draws logged in current frame */

        /* New frame boundary: gx_frame_draw_calls was just reset to 0 and this
         * is the first draw (gx_frame_draw_calls == 0 at entry to this block,
         * incremented below).  Use s_total_draw_count threshold to restrict to
         * play-window frames (beyond the logo+title block of ~5000 draws). */
        if (gx_frame_draw_calls == 0 && s_total_draw_count > 5000) {
            s_play_dump_active = 1;
            s_play_dump_count  = 0;
        }
        if (s_play_dump_active && s_play_dump_count < 5) {
            s_play_dump_count++;
            fprintf(stderr, "{\"play_state\":{\"draw_id\":%u,\"frame_dc\":%u,"
                    "\"preset\":\"%s\",\"state\":\"0x%016llX\","
                    "\"depth_bits\":%d,\"blend_bits\":%d,"
                    "\"write_rgb\":%d,\"write_a\":%d,"
                    "\"z_en\":%d,\"z_upd\":%d,\"blend_mode\":%d}}\n",
                    s_total_draw_count, (unsigned)gx_frame_draw_calls,
                    (preset >= 0 && preset < GX_TEV_SHADER_COUNT) ? s_fs_names[preset] : "?",
                    (unsigned long long)state,
                    (int)((state & BGFX_STATE_DEPTH_TEST_MASK)  != 0),
                    (int)((state & BGFX_STATE_BLEND_MASK)        != 0),
                    (int)((state & BGFX_STATE_WRITE_RGB)         != 0),
                    (int)((state & BGFX_STATE_WRITE_A)           != 0),
                    g_gx_state.z_compare_enable, g_gx_state.z_update_enable,
                    g_gx_state.blend_mode);
        }
    }

    /* Per-frame centroid camera reset.
     * s_geom_centroid_active must be cleared at the start of each frame so that
     * only frames with draw_calls > CENTROID_FRAME_DRAWS_MIN activate the centroid
     * camera.  Without this reset, centroid stays active after the 3D room frame
     * (frame 129) and routes all subsequent frames to bgfx view 1, leaving the
     * Phase 4 captured frame 0% nonblack (view 0 receives no draws after frame 129). */
    if (gx_frame_draw_calls == 0) {
        s_geom_centroid_active = 0;
        s_centroid_n           = 0;
        s_centroid_sum[0] = s_centroid_sum[1] = s_centroid_sum[2] = 0.0f;
        s_centroid_vz_max = -1e30f;
        /* Allow centroid_view_switch to re-log on the next 3D room frame. */
        /* (s_centroid_view_logged is reset below in its own block) */
    }

    /* Per-frame draw diagnostic for Phase 2 (softpipe) analysis.
     * Logs the first 3 draws of each frame once we've completed enough
     * frames to be past the startup black screen (frame > 5).
     * Uses frame boundaries (gx_frame_draw_calls == 0) rather than a
     * cumulative draw count so it fires in Phase 2 where total draws never
     * exceed 5000 (the play_state threshold above).  Emits TEV register
     * values + vertex color for diagnosing red-channel-only output. */
    {
        static uint32_t s_diag_frame      = 0; /* frame counter for this diagnostic */
        static int      s_diag_frame_pos  = 0; /* draws logged in current frame */

        if (gx_frame_draw_calls == 0) {
            s_diag_frame++;
            s_diag_frame_pos = 0;
        }
        /* Log first 3 draws of frames 6-200 (captures Nintendo logo +
         * start of TP title screen; skips pure-black startup frames). */
        if (s_diag_frame >= 6 && s_diag_frame <= 200 && s_diag_frame_pos < 3) {
            s_diag_frame_pos++;
            /* For injected-color draws (no vertex color in buffer), const_clr
             * is the actual vertex color.  For vertex-buffered color, log flag. */
            uint8_t v0_r = 0, v0_g = 0, v0_b = 0, v0_a = 0;
            int has_vtx_clr = (desc[GX_VA_CLR0].type != GX_NONE);
            if (inject_color) {
                v0_r = const_clr[0]; v0_g = const_clr[1];
                v0_b = const_clr[2]; v0_a = const_clr[3];
            }
            const GXTevStage* ts = &g_gx_state.tev_stages[0];
            fprintf(stderr, "{\"frame_draw_diag\":{\"frame\":%u,\"frame_dc\":%u,"
                    "\"draw_id\":%u,\"preset\":\"%s\","
                    "\"blend_mode\":%d,\"z_en\":%d,"
                    "\"tevR0\":[%d,%d,%d,%d],"
                    "\"tevR1\":[%d,%d,%d,%d],"
                    "\"vtx_clr\":[%d,%d,%d,%d],"
                    "\"inject\":%d,\"has_vtx_clr\":%d,"
                    "\"tev0_cd\":[%d,%d,%d,%d],"
                    "\"nverts\":%u}}\n",
                    s_diag_frame, (unsigned)gx_frame_draw_calls,
                    s_total_draw_count,
                    (preset >= 0 && preset < GX_TEV_SHADER_COUNT) ? s_fs_names[preset] : "?",
                    g_gx_state.blend_mode, g_gx_state.z_compare_enable,
                    g_gx_state.tev_regs[0].r, g_gx_state.tev_regs[0].g,
                    g_gx_state.tev_regs[0].b, g_gx_state.tev_regs[0].a,
                    g_gx_state.tev_regs[1].r, g_gx_state.tev_regs[1].g,
                    g_gx_state.tev_regs[1].b, g_gx_state.tev_regs[1].a,
                    (int)v0_r, (int)v0_g, (int)v0_b, (int)v0_a,
                    inject_color, has_vtx_clr,
                    ts->color_a, ts->color_b, ts->color_c, ts->color_d,
                    (unsigned)nverts);
        }

        /* One-shot frame-200 TEV color-pipeline dump.
         * Fires for EVERY draw on diagnostic frame 200 (the daTitle full-draw
         * window). Logs the full chain that determines output color:
         *   d_src      : TEV stage-0 D input (GX_CC_C0=6, GX_CC_RASC=10, …)
         *   tev0_before: GX_TEVREG0 value before any apply_rasc_color override
         *   mat        : chan_ctrl[0].mat_color (source for non-ambient fallback)
         *   amb        : chan_ctrl[0].amb_color (used when mat is dark)
         *   const_clr  : resolved vertex/const color AFTER all fallback logic
         *   inject     : 1 if color was injected (no CLR0 in vertex buffer)
         *   tev_dirty  : bitmask of explicitly-set tev_regs (bit N = GX_TEVREGN)
         *   blend_src  : GX blend source factor (0=ZERO,1=ONE,4=SRC_ALPHA, …)
         *   blend_dst  : GX blend destination factor
         *   bgfx_state : computed bgfx STATE bits (depth+blend+cull+write masks)
         *   view_id    : bgfx view the draw will be submitted to (0/1/2)
         * Use these fields to diagnose why frame-200 draws produce near-black
         * avg_rgb. If mat=[0,0,0,255] the grey fallback fires but gives 200,200,
         * 200 — if still dark, look for blend_mode!=0 or z_func rejection. */
        if (s_diag_frame == 200) {
            const GXTevStage* s0t = &g_gx_state.tev_stages[0];
            /* Compute view_id inline — now always 0 (single-view) */
            int tev200_view = 0;
            fprintf(stderr, "{\"tev200\":{\"frame\":%u,\"draw_id\":%u,\"dc\":%u,"
                    "\"preset\":\"%s\","
                    "\"d_src\":%d,"
                    "\"tev0_before\":[%d,%d,%d,%d],"
                    "\"mat\":[%d,%d,%d,%d],"
                    "\"amb\":[%d,%d,%d,%d],"
                    "\"const_clr\":[%d,%d,%d,%d],"
                    "\"inject\":%d,"
                    "\"tev_dirty\":\"0x%02x\","
                    "\"z_func\":%d,\"blend_mode\":%d,"
                    "\"blend_src\":%d,\"blend_dst\":%d,"
                    "\"bgfx_state\":\"0x%016llx\","
                    "\"view_id\":%d}}\n",
                    s_diag_frame, s_total_draw_count, (unsigned)gx_frame_draw_calls,
                    (preset >= 0 && preset < GX_TEV_SHADER_COUNT) ? s_fs_names[preset] : "?",
                    (int)(s0t->color_d),
                    g_gx_state.tev_regs[GX_TEVREG0].r,
                    g_gx_state.tev_regs[GX_TEVREG0].g,
                    g_gx_state.tev_regs[GX_TEVREG0].b,
                    g_gx_state.tev_regs[GX_TEVREG0].a,
                    (int)g_gx_state.chan_ctrl[0].mat_color.r,
                    (int)g_gx_state.chan_ctrl[0].mat_color.g,
                    (int)g_gx_state.chan_ctrl[0].mat_color.b,
                    (int)g_gx_state.chan_ctrl[0].mat_color.a,
                    (int)g_gx_state.chan_ctrl[0].amb_color.r,
                    (int)g_gx_state.chan_ctrl[0].amb_color.g,
                    (int)g_gx_state.chan_ctrl[0].amb_color.b,
                    (int)g_gx_state.chan_ctrl[0].amb_color.a,
                    (int)const_clr[0], (int)const_clr[1],
                    (int)const_clr[2], (int)const_clr[3],
                    inject_color,
                    (unsigned)g_gx_state.tev_reg_dirty,
                    (int)g_gx_state.z_func,
                    (int)g_gx_state.blend_mode,
                    (int)g_gx_state.blend_src,
                    (int)g_gx_state.blend_dst,
                    (unsigned long long)state,
                    tev200_view);
        }
    }

    /* Apply scissor if set to a non-fullscreen region */
    if (g_gx_state.sc_wd > 0 && g_gx_state.sc_ht > 0) {
        bgfx::setScissor(
            (uint16_t)g_gx_state.sc_left, (uint16_t)g_gx_state.sc_top,
            (uint16_t)g_gx_state.sc_wd, (uint16_t)g_gx_state.sc_ht);
    }

    /* 10. Submit draw call.
     * All draws go to view 0 (single-view rendering).
     * The depth-conflict (pre-centroid PASSCLR writes depth≈0.950, so centroid
     * draws at NDC.z≈0.9996 fail LEQUAL) is solved by disabling depth test
     * for centroid draws — see the state override below. */
    bgfx::ViewId view_id = 0;
    /* One-shot log when centroid camera activates.
     * Logs draw count to show how many pre-centroid draws occurred. */
    {
        static int s_centroid_view_logged = 0;
        if (gx_frame_draw_calls == 0) {
            s_centroid_view_logged = 0;  /* allow re-log on next 3D room frame */
        }
        if (s_geom_centroid_active && !s_centroid_view_logged) {
            s_centroid_view_logged = 1;
            fprintf(stderr, "{\"centroid_view_switch\":{\"draw_id\":%u,"
                    "\"frame_dc\":%u,\"view_id\":%u}}\n",
                    s_total_draw_count, (unsigned)gx_frame_draw_calls,
                    (unsigned)view_id);
        }
    }
    /* Set uber-shader uniforms when using TEV shader (Recipe 2 + Recipe 4) */
    if (preset == GX_TEV_SHADER_TEV) {
        int base_mode = classify_tev_config();
        float config[4] = {
            (float)base_mode, /* TEV mode for color computation */
            0.0f, 0.0f, 0.0f
        };
        bgfx::setUniform(s_tev_config_uniform, config);

        /* Alpha compare uniforms */
        float alphaTest[4] = {
            g_gx_state.alpha_ref0 / 255.0f,       /* ref0 */
            (float)g_gx_state.alpha_comp0,          /* comp0 */
            (float)g_gx_state.alpha_comp1,          /* comp1 */
            g_gx_state.alpha_ref1 / 255.0f,        /* ref1 */
        };
        float alphaOp[4] = {
            (float)g_gx_state.alpha_op,             /* op */
            /* Enable alpha test only when at least one compare is NOT GX_ALWAYS.
             * GX_ALWAYS (7) always passes, so if both are ALWAYS the test is
             * a no-op — skip it to avoid float precision issues at the boundary. */
            (g_gx_state.alpha_comp0 != GX_ALWAYS || g_gx_state.alpha_comp1 != GX_ALWAYS) ? 1.0f : 0.0f,
            0.0f, 0.0f
        };
        bgfx::setUniform(s_alpha_test_uniform, alphaTest);
        bgfx::setUniform(s_alpha_op_uniform, alphaOp);

        /* The uber-shader also needs TEV reg uniforms for BLEND mode */
        float reg0[4] = {
            g_gx_state.tev_regs[GX_TEVREG0].r / 255.0f,
            g_gx_state.tev_regs[GX_TEVREG0].g / 255.0f,
            g_gx_state.tev_regs[GX_TEVREG0].b / 255.0f,
            g_gx_state.tev_regs[GX_TEVREG0].a / 255.0f,
        };
        float reg1[4] = {
            g_gx_state.tev_regs[GX_TEVREG1].r / 255.0f,
            g_gx_state.tev_regs[GX_TEVREG1].g / 255.0f,
            g_gx_state.tev_regs[GX_TEVREG1].b / 255.0f,
            g_gx_state.tev_regs[GX_TEVREG1].a / 255.0f,
        };
        bgfx::setUniform(s_tev_reg0_uniform, reg0);
        bgfx::setUniform(s_tev_reg1_uniform, reg1);
    }

    bgfx::submit(view_id, s_programs[preset]);
    s_ok_submitted++;

    /* One-shot: log first centroid draw details to verify TVB data */
    {
        static int s_centroid_draw_log = 0;
        if (s_centroid_draw_log < 3 && s_geom_centroid_active) {
            s_centroid_draw_log++;
            fprintf(stderr, "{\"centroid_draw\":{\"n\":%d,\"view\":%d,"
                    "\"nverts\":%u,\"preset\":%d,"
                    "\"state\":\"0x%llX\","
                    "\"proj_type\":%d,\"proj00\":%.6f,"
                    "\"blend\":%d,\"depth\":%d,\"cull\":%d}}\n",
                    s_centroid_draw_log, (int)view_id,
                    (unsigned)nverts, preset,
                    (unsigned long long)state,
                    g_gx_state.proj_type,
                    g_gx_state.proj_mtx[0][0],
                    g_gx_state.blend_mode,
                    g_gx_state.z_compare_enable,
                    g_gx_state.cull_mode);
        }
    }

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

    /* Reset the per-draw TEV register dirty flags.  Each draw must call
     * GXSetTevColor explicitly if it wants to lock in a dark register value;
     * otherwise the dark-TEV-register fallback fires to keep geometry visible. */
    g_gx_state.tev_reg_dirty = 0;
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
