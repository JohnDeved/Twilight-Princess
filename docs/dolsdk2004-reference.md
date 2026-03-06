# dolsdk2004 Reference Guide for TP Port

Reference: [doldecomp/dolsdk2004](https://github.com/doldecomp/dolsdk2004)

This document captures key findings from the dolsdk2004 (Dolphin SDK 2004) decompilation
that are directly useful for the Twilight Princess PC/NX port. The dolsdk2004 project
provides decompiled source code for the 4/20/2004 version of the Dolphin SDK library
files, giving us the **actual internal implementation** of the GX graphics API.

## Overview

dolsdk2004 is a decompilation of the official Nintendo Dolphin SDK libraries. It includes
complete or in-progress decompilations of:

| Library  | Status | Port Relevance |
|----------|--------|----------------|
| **gx**   | ⚠️ In Progress | **Critical** — TEV, lighting, textures, transforms |
| **os**   | ✔️ Complete | Moderate — OS timing, memory, threads |
| **vi**   | ⚠️ In Progress | Low — replaced by SDL3 |
| **ax**   | ✔️ Complete | Moderate — audio engine reference |
| **mtx**  | ✔️ Complete | Useful — matrix math reference |
| **dvd**  | ✔️ Complete | Low — replaced by filesystem |
| **pad**  | ✔️ Complete | Low — replaced by SDL3 input |
| **card** | ✔️ Complete | Low — replaced by filesystem save |
| Others   | ✔️ Complete | Minimal |

## Critical GX Implementation Details

### 1. Internal GX State Structure (`__gx.h`)

The **most valuable file** is `src/gx/__gx.h`, which reveals the complete internal `GXData`
structure that the SDK uses to track all GX state. Key fields:

```c
typedef struct __GXData_struct {
    // Vertex descriptor / format
    u32 vcdLo, vcdHi;          // Vertex component descriptor
    u32 vatA[8], vatB[8], vatC[8];  // Vertex attribute tables

    // Lighting
    u32 ambColor[2];           // Ambient colors (2 channels)
    u32 matColor[2];           // Material colors (2 channels)

    // Texture coordinates
    u32 suTs0[8], suTs1[8];    // Texture scale/offset registers

    // TEV stages
    u32 tref[8];               // TEV stage texture references
    u32 iref;                  // Indirect texture references
    u32 tevc[16];              // TEV color combiner registers
    u32 teva[16];              // TEV alpha combiner registers
    u32 tevKsel[8];            // TEV konst color selectors

    // Pixel pipeline
    u32 cmode0;                // Blend mode register
    u32 cmode1;                // Destination alpha register
    u32 zmode;                 // Z-buffer mode register
    u32 peCtrl;                // Pixel engine control

    // Projection
    u32 projType;              // GX_PERSPECTIVE or GX_ORTHOGRAPHIC
    f32 projMtx[6];           // Projection matrix (packed 6 floats)

    // Viewport
    f32 vpLeft, vpTop, vpWd, vpHt, vpNearz, vpFarz;
    f32 zOffset, zScale;

    // Texture state
    u32 tImage0[8], tMode0[8]; // Texture image/mode registers
    u32 texmapId[16];         // Which texture map each TEV stage uses

    // Generation mode
    u32 genMode;               // Number of TEV stages, tex gens, etc.

    // Dirty state tracking
    u32 dirtyState;            // Bitmask of what needs flushing
    u8 dirtyVAT;               // VAT needs update
} GXData;
```

**Port implications:**
- Our `gx_state.h` GXState structure should mirror these fields for accurate state tracking
- The `dirtyState` bitmask pattern (bit 0 = tex regs, bit 2 = genMode, bit 3 = VCD,
  bit 4 = VAT) matches what we need for efficient state management
- `projType` is stored as a u32 (0 = perspective, 1 = orthographic) — this matches our
  existing `proj_type` field

### 2. TEV Combiner Implementation (`GXTev.c`)

The TEV (Texture Environment) combiner is the heart of GX rendering. Key findings:

#### TEV Register Layout

The TEV color register (`tevc[stage]`) is a packed u32:
- Bits [3:0]   — Color input D
- Bits [7:4]   — Color input C
- Bits [11:8]  — Color input B
- Bits [15:12] — Color input A
- Bits [17:16] — Bias (0=zero, 1=+0.5, 2=-0.5, 3=compare mode)
- Bit [18]     — Operation (0=add, 1=sub)
- Bit [19]     — Clamp
- Bits [21:20] — Scale (0=×1, 1=×2, 2=×4, 3=÷2) or compare mode
- Bits [23:22] — Output register (TEVPREV, TEVREG0, TEVREG1, TEVREG2)
- Bits [31:24] — Register ID (0xC0 + stage*2)

The TEV alpha register (`teva[stage]`) is similar but uses 3-bit inputs:
- Bits [1:0]   — Swap table selectors (ras_sel, tex_sel)
- Bits [6:4]   — Alpha input D
- Bits [9:7]   — Alpha input C
- Bits [12:10]  — Alpha input B
- Bits [15:13]  — Alpha input A
- Bits [17:16] — Bias
- Bit [18]     — Operation
- Bit [19]     — Clamp
- Bits [21:20] — Scale
- Bits [23:22] — Output register

#### TEV Preset Tables

The SDK defines preset tables for `GXSetTevOp()`. For STAGE0 vs STAGE1+, inputs differ:

**Stage 0 Color (TEVCOpTableST0):**
| Mode     | A    | B    | C    | D    |
|----------|------|------|------|------|
| MODULATE | ZERO | TEXC | RASC | ZERO |
| DECAL    | RASC | TEXC | TEXA | ZERO |
| BLEND    | RASC | ONE  | TEXC | ZERO |
| REPLACE  | ZERO | ZERO | ZERO | TEXC |
| PASSCLR  | ZERO | ZERO | ZERO | RASC |

**Stage 1+ Color (TEVCOpTableST1):**
| Mode     | A    | B    | C    | D     |
|----------|------|------|------|-------|
| MODULATE | ZERO | TEXC | CPREV| ZERO  |
| DECAL    | CPREV| TEXC | TEXA | ZERO  |
| BLEND    | CPREV| ONE  | TEXC | ZERO  |
| REPLACE  | ZERO | ZERO | ZERO | TEXC  |
| PASSCLR  | ZERO | ZERO | ZERO | CPREV |

**Port implications:**
- Our TEV classification in `gx_tev.cpp` must distinguish Stage 0 from Stage 1+
- MODULATE Stage0 = `tex * rasterized_color`, Stage1+ = `tex * prev_stage`
- PASSCLR Stage0 = output rasterized color, Stage1+ = pass through previous stage output
- The actual TEV equation is: `output = (d + ((1-c)*a + c*b) + bias) * scale`

#### GXSetTevColor Implementation

`GXSetTevColor` packs RGBA into BP (Blitting Processor) register format:
- Register address = `0xE0 + id * 2` (RA pair) and `0xE1 + id * 2` (BG pair)
- RA register: bits [7:0] = R, bits [19:12] = A
- BG register: bits [7:0] = G (from byte 1), bits [19:12] = B (from byte 2)
- **Three identical BG writes** are sent (hardware workaround for register latching)

For `GXSetTevColorS10` (signed 10-bit):
- Uses 11-bit fields instead of 8-bit
- Range is -1024 to +1023

#### GXSetTevOrder

The `c2r[]` mapping from GXChannelID to tref channel indices:
```c
static int c2r[] = { 0, 1, 0, 1, 0, 1, 7, 5, 6 };
// GX_COLOR0=0, GX_COLOR1=1, GX_ALPHA0=0, GX_ALPHA1=1,
// GX_COLOR0A0=0, GX_COLOR1A1=1, GX_COLOR_ZERO=7,
// GX_ALPHA_BUMP=5, GX_ALPHA_BUMPN=6
```

**Port implication:** `GX_COLOR_NULL` (0xFF) maps to value 7 in the tref register.
Our port should handle this mapping correctly for channel selection.

### 3. Lighting System (`GXLight.c`)

#### GXLightObj Internal Layout

```c
typedef struct {
    u32 reserved[3];   // offset 0x00: unused padding
    u32 Color;         // offset 0x0C: packed RGBA8
    f32 a[3];          // offset 0x10: angle attenuation coefficients (spot light)
    f32 k[3];          // offset 0x1C: distance attenuation coefficients
    f32 lpos[3];       // offset 0x28: light position (or -infinity*dir for specular)
    f32 ldir[3];       // offset 0x34: light direction (negated internally!)
} __GXLightObjInt_struct;
```

**Key details:**
- Light direction is **stored negated** internally (`-nx, -ny, -nz`)
- For specular lights, position = `direction * -1e18` (large negative = pseudo-infinity)
- The half-angle vector for specular is computed as: `H = normalize(-dir + (0,0,1))`
- Light data is loaded to XF (Transform unit) at address `0x600 + lightIdx * 0x10`

#### GXSetChanCtrl Register Layout

```c
// XF register at offset idx + 14:
// Bit 0:  mat_src (0=register, 1=vertex)
// Bit 1:  enable (lighting enable)
// Bits 5:2: light_mask low 4 bits
// Bit 6:  amb_src (0=register, 1=vertex)
// Bits 8:7: diffuse function (clamped if attn_fn==0)
// Bit 9:  attn_enable (!= GX_AF_SPOT)
// Bit 10: attn_select (!= GX_AF_NONE)
// Bits 14:11: light_mask high 4 bits
```

**Port implications:**
- When `enable=0`, the output color = material color (if mat_src=REG) or vertex color (if mat_src=VTX)
- When `enable=1`, lighting computes: `clr = amb * amb_src + Σ(light_color * diffuse(N·L) * attn(d))`
- Our RASC grey fallback should consider mat_src: if VTX, use vertex color; if REG, use matColor

#### GXSetChanAmbColor / GXSetChanMatColor

These write to XF registers:
- Ambient: XF register `colIdx + 10` (offset 10 or 11)
- Material: XF register `colIdx + 12` (offset 12 or 13)
- Both store as packed RGBA32 (`r << 24 | g << 16 | b << 8 | a`)

For partial updates (GX_COLOR0 without alpha, or GX_ALPHA0 without color),
the existing register value is preserved and only the relevant bits are updated.

### 4. Pixel Pipeline (`GXPixel.c`)

#### GXSetBlendMode

The `cmode0` register layout:
- Bit 0:  Blend enable
- Bit 1:  Logic op enable
- Bit 2:  Dither enable
- Bit 3:  Color update enable
- Bit 4:  Alpha update enable
- Bits 7:5: Destination factor
- Bits 10:8: Source factor
- Bit 11: Subtract mode
- Bits 15:12: Logic operation

**Key insight:** `GX_BM_SUBTRACT` sets bit 11 AND bit 0 (blend enable). For debug builds,
blend_en is computed separately; in release builds, `type` value itself goes to bit 0
(so GX_BM_SUBTRACT=3 → bit 0 = 1).

#### GXSetZMode

The `zmode` register:
- Bit 0:  Compare enable
- Bits 3:1: Compare function
- Bit 4:  Update enable (Z write)

#### GXSetFog

Fog calculation is significantly different for perspective vs orthographic projection:
- **Perspective:** Computes A, B, C coefficients with log-scale B decomposition
  (mantissa + exponent). Uses `farz * nearz / ((farz - nearz) * (endz - startz))`.
- **Orthographic:** Simple linear: `A = 1/(endz - startz)`, `C = startz/(endz - startz)`

### 5. Transform Pipeline (`GXTransform.c`)

#### Projection Matrix Storage

GX stores projection as **6 packed floats**, not a full 4×4 matrix:
```c
projMtx[0] = mtx[0][0]
projMtx[2] = mtx[1][1]
projMtx[4] = mtx[2][2]
projMtx[5] = mtx[2][3]

// For orthographic:
projMtx[1] = mtx[0][3]  // X translation
projMtx[3] = mtx[1][3]  // Y translation

// For perspective:
projMtx[1] = mtx[0][2]  // X projection
projMtx[3] = mtx[1][2]  // Y projection
```

#### GXSetProjectionv

The first element `ptr[0]` determines type: `0.0f = perspective`, non-zero = orthographic.
This matches `GXGetProjectionv` which outputs `0.0f` for perspective, `1.0f` for ortho.

**Port implication:** When intercepting `GXSetProjectionv`, check `ptr[0]` to determine
projection type, then extract the 6 matrix elements from `ptr[1..6]`.

#### Viewport Calculation

```c
// Internal viewport transform:
sx = vpWd / 2.0f;
sy = -vpHt / 2.0f;      // NOTE: negated!
ox = 342.0f + vpLeft + vpWd/2;  // 342 = GX EFB guard band offset (hardware constant)
oy = 342.0f + vpTop + vpHt/2;  // same offset for Y; accounts for clipping guard band
zmin = vpNearz * zScale;
zmax = vpFarz * zScale;
sz = zmax - zmin;
oz = zmax + zOffset;
```

The 342.0f offset is a hardware constant for the GX scissor/viewport origin.

### 6. Display List System (`GXDisplayList.c`)

#### GXCallDisplayList Flow

1. Flush dirty state (`__GXSetDirtyState`)
2. If no vertices sent since last BP write, send flush primitive (`__GXSendFlushPrim`)
3. Write command `0x45` (GX_CMD_CALL_DL), address, and byte count

The flush primitive check (`*(u32*)&__GXData->vNumNot == 0`) simultaneously tests:
- `vNumNot` (upper 16 bits) — must be non-zero if vertices need flushing
- `bpSentNot` (lower 16 bits) — must be non-zero if BP register was sent

**Port implication:** Our display list replay must properly handle the flush mechanism.
When replaying DLs, we intercept GX commands and translate them to state changes.

### 7. Geometry / Primitive Submission (`GXGeometry.c`)

#### Dirty State Bitmask

```c
// dirtyState bit meanings:
// Bit 0: Texture SU registers need update (__GXSetSUTexRegs)
// Bit 1: BP mask needs update (__GXUpdateBPMask)
// Bit 2: Generation mode needs update (__GXSetGenMode)
// Bit 3: Vertex descriptor needs update (__GXSetVCD)
// Bit 4: Vertex attribute table needs update (__GXSetVAT)
```

#### GXSetCullMode Hardware Mapping

GX cull mode is **swapped** at the hardware level:
- `GX_CULL_FRONT` → hardware `GX_CULL_BACK` (bit swap)
- `GX_CULL_BACK` → hardware `GX_CULL_FRONT`

This is because the GX hardware uses opposite winding conventions from the API.

**Port implication:** Our bgfx cull mode mapping should match the API convention
(CW = front face), not the hardware convention.

### 8. Indirect Texturing (`GXBump.c`)

The indirect texturing system is used for:
- Normal map bump mapping (`GXSetTevIndBumpST`, `GXSetTevIndBumpXYZ`)
- Texture warping effects (`GXSetTevIndWarp`)
- Tile-based texturing (`GXSetTevIndTile`)

Each indirect texture stage adds UV offset to the direct texture lookup. The offset is
computed from an indirect texture sample multiplied by a 2×3 matrix.

**Port implication:** Indirect texturing is used sparingly in TP (water effects, heat
shimmer). It can be initially stubbed but will need implementation for visual accuracy.

## GX Init Default State

From `__GXInitGX()`, the complete default state after GXInit:

| Setting | Default Value |
|---------|---------------|
| Projection | Orthographic (identity-like) |
| Cull mode | GX_CULL_BACK |
| Clip mode | GX_CLIP_ENABLE |
| Num channels | 0 (no lighting) |
| Chan0 ctrl | Disabled, amb=REG, mat=VTX |
| Amb color 0 | Black (0,0,0,0) |
| Mat color 0 | White (255,255,255,255) |
| Num TEV stages | 1 |
| TEV stage 0 op | GX_REPLACE |
| Blend mode | GX_BM_NONE, srcalpha, invsrcalpha |
| Color update | Enabled |
| Alpha update | Enabled |
| Z mode | Compare=TRUE, func=LEQUAL, update=TRUE |
| Z comp loc | Before texture (TRUE) |
| Dither | Enabled |
| Pixel format | RGB8_Z24, linear Z |
| Fog | None |
| Alpha compare | Always pass |
| Swap table 0 | R,G,B,A (identity) |
| Swap table 1 | R,R,R,A (red broadcast) |
| Swap table 2 | G,G,G,A (green broadcast) |
| Swap table 3 | B,B,B,A (blue broadcast) |

## Current Port Gap Analysis

This section maps dolsdk2004 knowledge directly against our port's current implementation
to identify the highest-value improvements.

### What Our Port Has (Working)

| Feature | Port File | Status |
|---------|-----------|--------|
| TEV 5 presets (PASSCLR, REPLACE, MODULATE, BLEND, DECAL) | `gx_tev.cpp` | ✅ Working |
| Texture decoding (11 formats: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, CI4, CI8, CI14X2) | `gx_texture.cpp` | ✅ Working |
| Blend mode (NONE, BLEND, SUBTRACT + src/dst factors) | `gx_tev.cpp` | ✅ Working |
| Z-mode (compare + write enable) | `gx_tev.cpp` | ✅ Working |
| Projection matrix (perspective + orthographic) | `gx_tev.cpp` | ✅ Working |
| Viewport/scissor | `gx_tev.cpp` | ✅ Working |
| Display list replay (CP, XF, BP, DRAW commands) | `gx_displaylist.cpp` | ✅ Working |
| Konst color resolution (full K0-K3 + fractions) | `gx_tev.cpp` | ✅ Working |
| Vertex format parsing (8 VAT formats, all component types) | `gx_displaylist.cpp` | ✅ Working |
| Texture cache (256-entry LRU by ptr+format) | `gx_texture.cpp` | ✅ Working |

### What Our Port Is Missing (Gaps)

| Gap | Impact | SDK Source | Recipe |
|-----|--------|-----------|--------|
| **RASC ignores `mat_src`/`enable`** | Wrong color for VTX-sourced materials | `GXLight.c` ChanCtrl | Recipe 1 |
| **No generic TEV shader** | Complex materials fall back to REPLACE | `GXTev.c` combiner eq | Recipe 2 |
| **No GX lighting** | 3D geometry uses grey fallback | `GXLight.c` full pipeline | Recipe 3 |
| **Alpha compare not in shaders** | Foliage/grass renders as solid rectangles | `GXTev.c` alpha comp | Recipe 4 |
| **No mipmap decoding** | LOD 0 only, textures shimmer at distance | `GXTexture.c` mip layout | Recipe 5 |
| **GXLoadPosMtxIndx stubbed** | Skeletal animation uses wrong matrices | `GXTransform.c` mtx indx | Recipe 6 |
| **Konst alpha sel not resolved** | Alpha channel effects wrong | `GXTev.c` k_alpha_sel | — |
| **No fog** | No distance fade on 3D geometry | `GXPixel.c` fog formulas | — |
| **No indirect texturing** | Water/heat shimmer missing | `GXBump.c` indirect system | — |
| **Cull mode may be swapped** | Backfaces visible on some models | `GXGeometry.c` front↔back | — |

### Display List Command Coverage

From `GXSave.c` `__GXShadowDispList`, the complete FIFO command set:

| Cmd Op | Name | Our Support | Notes |
|--------|------|-------------|-------|
| 0x00 | NOP | ✅ Skip | |
| 0x08 | LOAD_CP | ✅ VCD/VAT | Regs 5-11 supported |
| 0x10 | LOAD_XF | ✅ Full | Position/normal/texture matrices |
| 0x20 | LOAD_INDX_A | ⚠️ Partial | Position matrix index — needs Recipe 6 |
| 0x28 | LOAD_INDX_B | ⚠️ Partial | Normal matrix index |
| 0x30 | LOAD_INDX_C | ⚠️ Partial | Texture matrix index |
| 0x38 | LOAD_INDX_D | ⚠️ Partial | Light index |
| 0x40 | CALL_DL | ❌ Skipped | Nested display lists not supported |
| 0x45 | CALL_DL (alt) | ❌ Skipped | Same |
| 0x61 | LOAD_BP | ✅ Full | Blitting processor registers |
| 0x80-0xB8 | DRAW | ✅ Full | All primitive types |

## Implementation Recipes

These are concrete implementation guides that map dolsdk2004 knowledge directly to
changes needed in our port code. Each recipe includes the SDK source, what our port
currently does, and exactly what to change.

---

### Recipe 1: Fix RASC Color — Use `mat_src` + `chan_ctrl.enable` (P0, ~30min)

**Current bug:** `apply_rasc_color()` in `gx_tev.cpp:1427` always reads
`chan_ctrl[0].mat_color` for ortho draws and uses amb+grey fallback for perspective.
It ignores `mat_src` (register vs vertex) and `enable` (lighting on/off).

**SDK truth** (from `GXLight.c` → `GXSetChanCtrl`):
```
When enable=0:
  if mat_src == GX_SRC_REG  → output = matColor (register)
  if mat_src == GX_SRC_VTX  → output = vertex color attribute
When enable=1:
  output = ambColor + Σ(light[i].color * diffuse(N·L) * attn(dist))
```

**What to change in `gx_tev.cpp`:**
```c
static void apply_rasc_color(uint8_t* const_clr) {
    const GXChanCtrl* ch = &g_gx_state.chan_ctrl[0];
    if (!ch->enable) {
        /* Lighting disabled: output depends on mat_src */
        if (ch->mat_src == GX_SRC_VTX) {
            /* Vertex color will be used — signal caller to use vertex attribute */
            const_clr[0] = const_clr[1] = const_clr[2] = const_clr[3] = 255;
            return;  /* white = multiply-neutral, vertex color comes through */
        }
        /* mat_src == GX_SRC_REG: use register color directly */
        const_clr[0] = ch->mat_color.r;
        const_clr[1] = ch->mat_color.g;
        const_clr[2] = ch->mat_color.b;
        const_clr[3] = ch->mat_color.a;
        return;
    }
    /* Lighting enabled: amb + lights (stub: use amb only for now) */
    const_clr[0] = ch->amb_color.r;
    const_clr[1] = ch->amb_color.g;
    const_clr[2] = ch->amb_color.b;
    const_clr[3] = 255;
    /* If amb is dark (<threshold), fall back to RASC_FALLBACK_GRAY.
     * Remove this fallback once Recipe 3 (full lighting) is done. */
    if ((int)const_clr[0] + const_clr[1] + const_clr[2] < RASC_DARK_THRESHOLD) {
        const_clr[0] = const_clr[1] = const_clr[2] = RASC_FALLBACK_GRAY;
    }
}
```

**Files:** `src/pal/gx/gx_tev.cpp` (apply_rasc_color)
**Test:** Frame 129 (3D room) should still show ~80% nonblack. Title screen unchanged.

---

### Recipe 2: Generic TEV Uber-Shader (P1, ~2-3 sessions)

**Current limitation:** `classify_tev_config()` maps all TEV configs to 5 fixed presets
(PASSCLR, REPLACE, MODULATE, BLEND, DECAL). Complex multi-stage setups produce wrong
output.

**SDK truth** (from `GXTev.c`): The TEV equation per stage is:
```
color_out = clamp((d + lerp(a, b, c) + bias) * scale)
          = clamp((d + ((1-c)*a + c*b) + bias) * scale)

alpha_out = clamp((d + lerp(a, b, c) + bias) * scale)  // separate alpha inputs
```

Where `a`, `b`, `c`, `d` are selected from: `{CPREV, APREV, C0, A0, C1, A1, C2, A2,
TEXC, TEXA, RASC, RASA, ONE, HALF, KONST, ZERO}` (color) or
`{APREV, A0, A1, A2, TEXA, RASA, KONST, ZERO}` (alpha).

**Implementation plan — uber-shader approach:**
1. Create a new shader program `fs_tev_generic` that evaluates the TEV formula
2. Pass per-stage config as uniforms:
   - `u_tevStage[N]` = vec4(a_sel, b_sel, c_sel, d_sel) packed as floats
   - `u_tevOp[N]` = vec4(bias, scale, op, clamp)
   - `u_tevKonst[N]` = vec4(konst_r, konst_g, konst_b, konst_a)
   - `u_numStages` = int
3. Fragment shader loop evaluates stages sequentially:
```glsl
vec4 prev = vec4(0.0);  // TEVPREV
vec4 reg0 = u_tevReg0;  // TEVREG0
// ... reg1, reg2

for (int s = 0; s < u_numStages; s++) {
    vec4 a = resolve_input(u_tevStage[s].x, tex, rasc, prev, reg0, ...);
    vec4 b = resolve_input(u_tevStage[s].y, ...);
    vec4 c = resolve_input(u_tevStage[s].z, ...);
    vec4 d = resolve_input(u_tevStage[s].w, ...);
    prev = clamp(d + mix(a, b, c) + bias) * scale;
}
```
4. `classify_tev_config()` returns `GX_TEV_SHADER_GENERIC` for unrecognized patterns
5. Falls through to existing presets when pattern matches (faster path)

**Key insight from SDK:** Stage 0 and Stage 1+ use the same formula, they just have
different default *input selections* set by `GXSetTevOp()`. The hardware always runs
the same combiner — our shader should too.

**Files:** `src/pal/gx/shaders/fs_tev_generic.sc` (new), `gx_tev.cpp` (add generic path)

---

### Recipe 3: Implement GX Lighting (P2, ~2 sessions)

**Current state:** `apply_rasc_color()` uses grey fallback for perspective draws with
dark ambient. No actual light computation.

**SDK truth** (from `GXLight.c`):

The **complete lighting equation** per channel:
```
RASC = amb_color * amb_src + Σ_i(light[i].color * DiffuseFn(N, L_i) * AttnFn(d_i))
```

Where:
- `amb_src` = ambient register color (GX_SRC_REG) or vertex color (GX_SRC_VTX)
- `DiffuseFn` = {NONE→1.0, SIGN→N·L, CLAMP→max(0, N·L)}
- `AttnFn` = {NONE→1.0, SPOT→spot_curve, SPEC→specular_curve}

**GXLightObj internal layout** (64 bytes, 16-word aligned):
```
Offset  Size  Field           Notes
0x00    12    reserved[3]     Padding (unused)
0x0C    4     Color           Packed RGBA8 (R<<24|G<<16|B<<8|A)
0x10    12    a[3]            Angle attenuation (spot): a0, a1, a2
0x1C    12    k[3]            Distance attenuation: k0, k1, k2
0x28    12    lpos[3]         World-space position (or -dir*1e18 for specular)
0x34    12    ldir[3]         Direction (STORED NEGATED: -nx, -ny, -nz)
```

**Attenuation formula** (from `GXInitLightAttnA`/`GXInitLightAttnK`):
```
spot_attn = max(0, a0 + a1*cos(angle) + a2*cos²(angle))
dist_attn = 1.0 / (k0 + k1*dist + k2*dist²)
```

**Implementation plan:**
1. Add `GXLightObj light_objects[8]` to `GXState` in `gx_state.h`
2. Intercept `GXLoadLightObjImm` → store light data (decode from packed format)
3. In `apply_rasc_color()`, when `enable=1`:
   - Loop through `light_mask` bits
   - For each active light: compute `N·L` (diffuse) and attenuation
   - Accumulate: `result += light_color * diffuse * attn`
4. For vertex shader lighting: pass light data as uniforms

**Files:** `include/pal/gx/gx_state.h` (add light struct), `src/pal/gx/gx_state.cpp`
(intercept GXLoadLightObjImm), `src/pal/gx/gx_tev.cpp` (apply_rasc_color lighting)

---

### Recipe 4: Alpha Compare / Discard (P1, ~1 session)

**Current state:** `alpha_comp0`/`alpha_comp1`/`alpha_op` tracked in gx_state.h but
NOT used in the fragment shader. All fragments pass regardless of alpha.

**SDK truth** (from `GXTev.c` → `GXSetAlphaCompare`):
```c
// BP register 0xF3:
// bits [7:0]   = ref0 (8-bit threshold 0, range 0-255)
// bits [10:8]  = comp0 (GX_NEVER..GX_ALWAYS)
// bits [23:16] = ref1 (8-bit threshold 1, range 0-255)
// bits [26:24] = comp1
// bits [28:27] = op (AND, OR, XOR, XNOR)
```

The alpha compare test is:
```
bool pass0 = compare(alpha, ref0, comp0);  // e.g. alpha >= ref0
bool pass1 = compare(alpha, ref1, comp1);
bool final = combine(pass0, pass1, op);    // AND, OR, XOR, XNOR
if (!final) discard;
```

**Implementation in fragment shader:**
```glsl
uniform vec4 u_alphaComp;  // x=ref0/255, y=ref1/255, z=comp0, w=comp1
uniform float u_alphaOp;

// After computing final color:
bool p0 = alpha_test(gl_FragColor.a, u_alphaComp.x, int(u_alphaComp.z));
bool p1 = alpha_test(gl_FragColor.a, u_alphaComp.y, int(u_alphaComp.w));
bool pass = combine_alpha(p0, p1, int(u_alphaOp));
if (!pass) discard;
```

**Impact:** Fixes tree/grass alpha cutout rendering (foliage uses alpha test to cut
transparent pixels), HUD element masking, and character outline effects.

**Files:** All fragment shaders (`fs_*.sc`), `gx_tev.cpp` (upload alpha compare uniforms)

---

### Recipe 5: Mipmap Texture Decoding (P2, ~1 session)

**Current state:** `gx_texture.cpp` decodes LOD 0 only. `hasMips=false` in bgfx upload.

**SDK truth** (from `GXTexture.c` → `GXGetTexBufferSize`):
```
Mipmap data is stored contiguously after the base level in GX texture memory.
Each mip level is half the dimensions of the previous (min 1×1).
Total size = Σ(tile_count(level) × tile_bytes) for level 0..max_lod

The tile dimensions depend on format:
  I4, CI4, CMPR:  8×8 tiles (rowShift=3, colShift=3), 32 bytes/tile
  I8, IA4, CI8:   8×4 tiles (rowShift=3, colShift=2), 32 bytes/tile
  IA8, RGB565, RGB5A3: 4×4 tiles (rowShift=2, colShift=2), 32 bytes/tile
  RGBA8:          4×4 tiles (rowShift=2, colShift=2), 64 bytes/tile (dual cache lines)
```

**Implementation plan:**
1. In `upload_gx_texture()`, check `tex_binding.min_lod` / `tex_binding.max_lod`
2. If `max_lod > 0`, compute mip chain:
   ```c
   offset = base_size;  // after LOD 0
   for (level = 1; level <= max_lod; level++) {
       w = max(1, width >> level);
       h = max(1, height >> level);
       decode_texture(data + offset, w, h, fmt, rgba_out);
       offset += GXGetTexBufferSize(w, h, fmt, FALSE, 1);
   }
   ```
3. Upload all mip levels to bgfx with `hasMips=true`

**Files:** `src/pal/gx/gx_texture.cpp` (decode mip chain), `src/pal/gx/gx_tev.cpp`
(bgfx create with mips)

---

### Recipe 6: Indexed Matrix Loads — GXLoadPosMtxIndx (P1, ~1 session)

**Current state:** `GXLoadPosMtxIndx` is stubbed. Display list vertex data references
matrix indices but they're ignored.

**SDK truth** (from `GXTransform.c`):
```c
void GXLoadPosMtxIndx(u16 mtx_indx, u32 id) {
    // mtx_indx = index into external matrix array
    // id = position matrix slot (0..9, multiply by 4 for XF address)
    u32 offset = id * 4;
    // Writes to XF at address offset, loading 12 floats (3×4 matrix)
    // from indexed array memory at mtx_indx * stride
}
```

**How it works:** The game pre-uploads a matrix array to GX memory. During display
list replay, `GXLoadPosMtxIndx` selects which matrix from that array to use for the
next batch of vertices. This is how **skeletal animation** works — each joint uses a
different matrix from the array.

**Implementation plan:**
1. Our display list parser already handles LOAD_INDX_A/B/C/D commands
2. When LOAD_INDX_A (position matrix) is encountered:
   - Read `mtx_indx` and `id` from the command
   - Look up the matrix data from `g_gx_state.array[GX_POS_MTX_ARRAY]`
   - Copy 12 floats into `g_gx_state.pos_mtx[id]`
3. `pal_tev_flush_draw()` already uses `g_gx_state.pos_mtx[g_gx_state.cur_pos_mtx]`

**Key detail from SDK `GXSave.c`:** The index base and stride are set via CP registers:
```c
// CP register 10 (0xA): index base address
// CP register 11 (0xB): index stride
// These are set per array (position=idx0, normal=idx1, texture=idx2, light=idx3)
```

**Note:** `g_gx_state.array[]` (GXArrayState) already exists in `gx_state.h` for
indexed vertex drawing. The position matrix array base/stride may need a separate
tracking field if it's not already covered by the existing `array[]` state.

**Files:** `src/pal/gx/gx_displaylist.cpp` (handle LOAD_INDX matrix copy),
`include/pal/gx/gx_state.h` (ensure matrix array tracking)

---

### Recipe 7: Vertex Attribute Size Computation from VAT (P0, reference)

**Current state:** Display list parser computes vertex stride from our state tracking.

**SDK truth** (from `GXSave.c` → `GetAttrSize`): The SDK computes vertex attribute
sizes from the packed VAT (Vertex Attribute Table) registers. This is the **authoritative
reference** for how to parse vertex data in display lists.

**Complete attribute size table:**

| Attr | VCD bits | VAT register | Size formula |
|------|----------|-------------|--------------|
| PNMTXIDX | vcdLo[0] | — | 1 byte if present |
| TEX0MTXIDX..TEX7MTXIDX | vcdLo[1..8] | — | 1 byte each if present |
| POS | vcdLo[10:9] | vatA[1:0]=cnt, vatA[4:2]=type | (cnt+2) × type_size |
| NRM | vcdLo[12:11] | vatA[9]=nbt, vatA[12:10]=type | 3 × type_size (or 9× for NBT) |
| CLR0 | vcdLo[14:13] | vatA[16:14]=type | clrCompSize[type] |
| CLR1 | vcdLo[16:15] | vatA[20:18]=type | clrCompSize[type] |
| TEX0 | vcdHi[1:0] | vatA[22:21]=cnt, vatA[25:22]=type | (cnt+1) × type_size |
| TEX1..TEX7 | vcdHi[2N+1:2N] | vatB/vatC | Similar pattern |

**VCD type values:** 0=NONE, 1=DIRECT, 2=INDEX8 (1 byte), 3=INDEX16 (2 bytes)

**Component type sizes:** `{U8:1, S8:1, U16:2, S16:2, F32:4}`
**Color component sizes:** `{RGB565:2, RGB8:3, RGBX8:4, RGBA4:2, RGBA6:3, RGBA8:4}`

**Files:** Reference only. Our `gx_displaylist.cpp` already handles this.

---

## Prioritized Actionable Items

### P0 — Immediate Impact (affects current rendering correctness)

| # | Item | Est. Time | Impact |
|---|------|-----------|--------|
| 1 | Fix RASC to use `mat_src`/`enable` flags (Recipe 1) | 30 min | Correct ortho+persp color |
| 2 | Cull mode: use API values not HW values | 15 min | Fix backface culling |
| 3 | Verify `GXSetProjectionv` `ptr[0]` check | 15 min | Correct proj type detection |

### P1 — Next Priority (unlocks visual quality)

| # | Item | Est. Time | Impact |
|---|------|-----------|--------|
| 4 | Alpha compare/discard in shaders (Recipe 4) | 1 session | Foliage/grass transparency |
| 5 | Indexed matrix loads (Recipe 6) | 1 session | Skeletal animation |
| 6 | Generic TEV uber-shader (Recipe 2) | 2-3 sessions | All material effects |

### P2 — Visual Polish

| # | Item | Est. Time | Impact |
|---|------|-----------|--------|
| 7 | GX lighting (Recipe 3) | 2 sessions | Correct 3D shading |
| 8 | Mipmap decoding (Recipe 5) | 1 session | Texture quality |
| 9 | Fog (perspective + ortho formulas) | 1 session | Distance fade effects |
| 10 | Konst alpha selectors (k_alpha_sel) | 30 min | Correct alpha blend effects |

### P3 — Completeness

| # | Item | Est. Time | Impact |
|---|------|-----------|--------|
| 11 | Indirect texturing | 2 sessions | Water, heat shimmer |
| 12 | Z-texture | 1 session | Special depth effects |
| 13 | TEV comparison modes (bias=3) | 1 session | Edge detection effects |

## Source File Cross-Reference

| dolsdk2004 file | TP Port equivalent | Status | Key Knowledge |
|-----------------|-------------------|--------|---------------|
| `src/gx/__gx.h` | `include/pal/gx/gx_state.h` | Partial | GXData struct → our GXState; missing light objects |
| `src/gx/GXTev.c` | `src/pal/gx/gx_tev.cpp` | 5 presets | TEV equation, stage 0 vs 1+ tables, k_alpha_sel |
| `src/gx/GXLight.c` | `src/pal/gx/gx_tev.cpp` (RASC) | Stub | Light obj layout, ChanCtrl bits, attn formulas |
| `src/gx/GXPixel.c` | `src/pal/gx/gx_tev.cpp` (blend) | Basic | cmode0 bits done; fog + alpha compare missing |
| `src/gx/GXTransform.c` | `src/pal/gx/gx_tev.cpp` (proj) | Done | Proj storage + viewport done; mtx indx stub |
| `src/gx/GXTexture.c` | `src/pal/gx/gx_texture.cpp` | LOD 0 | Decode done; mipmap layout for multi-LOD |
| `src/gx/GXDisplayList.c` | `src/pal/gx/gx_displaylist.cpp` | Done | DL replay working |
| `src/gx/GXSave.c` | `src/pal/gx/gx_displaylist.cpp` | Ref | Vertex size computation, DL shadow parser |
| `src/gx/GXInit.c` | Various PAL init | Partial | Default state table, GXInit flow |
| `src/gx/GXGeometry.c` | `src/pal/gx/gx_fifo.cpp` | Done | Cull mode swap detail |
| `src/gx/GXAttr.c` | `include/pal/gx/gx_state.h` | Done | VCD/VAT bit layout for vertex parsing |
| `src/gx/GXBump.c` | Not implemented | Stub | Indirect tex system (water/heat) |
| `src/gx/GXDraw.c` | Not needed | N/A | Debug shapes only |
| `src/gx/GXFifo.c` | `src/pal/gx/gx_fifo.cpp` | Done | FIFO management (not needed for PC) |
| `src/gx/GXMisc.c` | Various | Partial | GXSetMisc, DrawDone, timing |
| `src/gx/GXVert.c` | Inline vertex writes | Done | GXPosition/Normal/Color/TexCoord inline |
