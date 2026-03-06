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

## Actionable Items for the Port

### High Priority (affects rendering correctness)

1. **TEV Stage 0 vs 1+ distinction**: Our TEV classifier must handle the different
   input mappings for stage 0 (uses RASC/TEXC) vs stage 1+ (uses CPREV/TEXC).

2. **Lighting channel state**: When `chan_ctrl.enable=0`, output = material/vertex color
   directly. When `enable=1`, full lighting equation applies. Our RASC color fallback
   should use `matColor` when `mat_src=GX_SRC_REG`.

3. **GXSetTevColor triple-write**: The SDK writes the BG register THREE times as a
   hardware workaround. Our interceptor should only capture the value once.

4. **Projection type detection**: `GXSetProjectionv` uses `ptr[0]==0.0f` for perspective.
   Verify our code checks this correctly.

5. **Cull mode swap**: GX_CULL_FRONT/BACK are swapped at hardware level. Our bgfx
   translation should use the API values, not hardware values.

### Medium Priority (affects visual quality)

6. **Fog implementation**: Perspective vs ortho fog use completely different formulas.
   The perspective path involves log-scale decomposition of the B coefficient.

7. **Alpha compare**: `GXSetAlphaCompare` uses dual-threshold comparison with AND/OR/XOR
   logic. This maps to fragment shader `discard` conditions.

8. **Konst color selectors**: The full set of TEV_KCSEL values (1, 7/8, 3/4, 5/8, 1/2,
   3/8, 1/4, 1/8, K0-K3 RGB/A channels) must be correctly resolved to actual color values.

### Low Priority (nice to have)

9. **Indirect texturing**: Used for water/heat effects. Can be stubbed initially.

10. **Z-texture**: Used for special depth effects. Rarely used in TP.

11. **TEV comparison modes**: When `bias=3`, the TEV operates in comparison mode rather
    than arithmetic mode. Used for special effects.

## Source File Cross-Reference

| dolsdk2004 file | TP Port equivalent | Status |
|-----------------|-------------------|--------|
| `src/gx/__gx.h` | `include/pal/gx/gx_state.h` | Partial match |
| `src/gx/GXTev.c` | `src/pal/gx/gx_tev.cpp` | Core implemented, presets only |
| `src/gx/GXLight.c` | `src/pal/gx/gx_tev.cpp` (RASC handling) | Stubbed/fallback |
| `src/gx/GXPixel.c` | `src/pal/gx/gx_tev.cpp` (blend/depth) | Basic blend/depth done |
| `src/gx/GXTransform.c` | `src/pal/gx/gx_tev.cpp` (proj/viewport) | Implemented |
| `src/gx/GXTexture.c` | `src/pal/gx/gx_texture.cpp` | Decoding done |
| `src/gx/GXDisplayList.c` | `src/pal/gx/gx_displaylist.cpp` | Replay implemented |
| `src/gx/GXInit.c` | Various PAL init code | Partial |
| `src/gx/GXGeometry.c` | `src/pal/gx/gx_fifo.cpp` | Primitive capture done |
| `src/gx/GXBump.c` | Not implemented | Stubbed |
| `src/gx/GXAttr.c` | `include/pal/gx/gx_state.h` (vtx desc) | Format parsing done |
| `src/gx/GXDraw.c` | Not needed | Debug shapes only |
