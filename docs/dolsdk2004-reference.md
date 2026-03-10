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

### 9. Vertex Attribute Size Computation (`GXSave.c`)

The authoritative vertex size computation used by the SDK display list parser. This is
critical for our DL parser to compute vertex stride correctly.

**Component size tables:**
```c
static u32 vtxCompSize[5] = { 1, 1, 2, 2, 4 };  // U8, S8, U16, S16, F32
static int clrCompSize[6] = { 2, 3, 4, 2, 3, 4 }; // RGB565, RGB888, RGBX8, RGBA4, RGBA6, RGBA8
```

**Per-attribute GetAttrSize() logic:**
- **PNMTXIDX..TEX7MTXIDX (0-8):** 1 byte if present, 0 if not (from vcdLo bits 0-8)
- **POS (9):** Direct=`(cnt+2) * vtxCompSize[type]`, Index8=1, Index16=2
  - cnt from vatA bit 0, type from vatA bits 1-3
- **NRM (10):** Direct=`3 * vtxCompSize[type]` (or 9× if NBT), Index8=1/3, Index16=2/6
  - NBT flag from vatA bit 9, type from vatA bits 10-12
- **CLR0 (11):** Direct=`clrCompSize[fmt]`, Index8=1, Index16=2
  - fmt from vatA bits 14-16
- **CLR1 (12):** Direct=`clrCompSize[fmt]`, Index8=1, Index16=2
  - fmt from vatA bits 18-20
- **TEX0 (13):** Direct=`(cnt+1) * vtxCompSize[type]`, Index8=1, Index16=2
  - cnt from vatA bit 21, type from vatA bits 22-24
- **TEX1 (14):** From vatB bits 0-3
- **TEX2 (15):** From vatB bits 9-12
- **TEX3 (16):** From vatB bits 18-21
- **TEX4 (17):** From vatB bits 27-30
- **TEX5 (18):** From vatC bits 5-8
- **TEX6 (19):** From vatC bits 14-17
- **TEX7 (20):** From vatC bits 23-26

**VCD (Vertex Component Descriptor) register layout:**
- `vcdLo` bits: [0]=PNMTXIDX, [1-8]=TEXnMTXIDX, [9:10]=POS(2bit), [11:12]=NRM(2bit),
  [13:14]=CLR0(2bit), [15:16]=CLR1(2bit)
- `vcdHi` bits: [0:1]=TEX0, [2:3]=TEX1, ..., [14:15]=TEX7

**Port implication:** Our `raw_attr_size()` function in gx_tev.cpp should match this
table exactly. Any mismatch causes incorrect vertex stride → corrupted geometry.

### 10. cmode0 / zmode Register Bit Layouts (`GXPixel.c`)

**cmode0 (blend register):**
```
bit  0: blend enable (GX_BM_BLEND or GX_BM_SUBTRACT set this)
bit  1: logic op enable (GX_BM_LOGIC)
bit  2: dither enable
bit  3: color update enable
bit  4: alpha update enable
bits 5-7: dst_factor (3 bits)
bits 8-10: src_factor (3 bits)
bit  11: subtract mode (GX_BM_SUBTRACT)
bits 12-15: logic op (4 bits)
```

**zmode (depth register):**
```
bit 0: compare_enable
bits 1-3: func (GXCompare)
bit 4: update_enable
```

**peCtrl register:**
```
bits 0-2: pixel format
bits 3-5: z format
bit 6: z comp location (before=1 or after=0 texture)
```

**Port implication:** These bit layouts are critical for DL parsing that writes BP
registers directly. Our BP command handler at `0x61` must decode these fields correctly.

### 11. Display List Command Format (`GXSave.c` `__GXShadowDispList`)

Full command byte format for GX display lists:

```
cmdOp = (cmd >> 3) & 0x1F   (5 bits: command opcode)
vatIdx = cmd & 0x7           (3 bits: vertex format index)
```

| cmdOp | Byte | Description | Data |
|-------|------|-------------|------|
| 0 | 0x00 | NOP | none |
| 1 | 0x08 | CP register write | u8 addr + u32 data |
| 2 | 0x10 | XF register write | u32 header + N×u32 data |
| 4 | 0x20 | XF pos mtx index load | u32 reg |
| 5 | 0x28 | XF nrm mtx index load | u32 reg |
| 6 | 0x30 | XF tex mtx index load | u32 reg |
| 7 | 0x38 | XF light index load | u32 reg |
| 8 | 0x40 | Call display list | (nested — error) |
| 12 | 0x61 | BP register write | u32 data |
| 16-23 | 0x80-0xBF | Draw primitives | u16 count + vertex data |

**CP register addresses (from cmdOp=1):**
```
cpAddr = (reg8 >> 4) & 0xF
vatIdx = reg8 & 0xF
0x05: VCD low (vcdLo)
0x06: VCD high (vcdHi)
0x07: VAT A[vatIdx]
0x08: VAT B[vatIdx]
0x09: VAT C[vatIdx]
0x0A: Index base (vatIdx - 0x15 maps to indexBase[0..3])
0x0B: Index stride (vatIdx - 0x15 maps to indexStride[0..3])
```

**XF indexed load register format (cmdOp 4-7):**
```
bits 0-11:  XF address offset
bits 12-15: count - 1
bits 16-31: array index
```
- Pos matrix (0x20): offset = id*4, count = 12
- Nrm matrix (0x28): offset = id*3 + 0x400, count = 9
- Tex matrix (0x30): offset = id*4 or (id-GX_PTTEXMTX0)*4+0x500
- Light (0x38): offset = 0x600 + id*16

**Port implication:** Our DL parser handles 0x10, 0x61, and 0x80-0xBF. Missing 0x08 (CP
writes — VCD/VAT changes mid-DL) and 0x20/0x28/0x30 (indexed matrix loads). The 0x08
command is particularly important because J3D materials can change vertex format mid-DL.

### 12. Projection and Viewport Internal Formulas (`GXTransform.c`)

**GXSetProjection packed format (6 floats):**
```c
// Common to both types:
projMtx[0] = mtx[0][0];  // sx (x scale)
projMtx[2] = mtx[1][1];  // sy (y scale)
projMtx[4] = mtx[2][2];  // sz (z scale)
projMtx[5] = mtx[2][3];  // tz (z offset)

// Perspective only (type == GX_PERSPECTIVE):
projMtx[1] = mtx[0][2];  // mtx[0][2] (usually 0 for symmetric frustum)
projMtx[3] = mtx[1][2];  // mtx[1][2] (usually 0 for symmetric frustum)

// Orthographic only (type == GX_ORTHOGRAPHIC):
projMtx[1] = mtx[0][3];  // tx (x offset)
projMtx[3] = mtx[1][3];  // ty (y offset)
```

**GXSetProjectionv type detection:**
```c
// ptr[0] == 0.0f → GX_PERSPECTIVE
// ptr[0] != 0.0f → GX_ORTHOGRAPHIC
```

**Viewport hardware transform:**
```c
sx = vpWd / 2.0f;
sy = -vpHt / 2.0f;                      // Y IS NEGATED
ox = 342.0f + (vpLeft + vpWd / 2.0f);   // 342.0 guard-band offset
oy = 342.0f + (vpTop + vpHt / 2.0f);
zmin = vpNearz * zScale;
zmax = vpFarz * zScale;
sz = zmax - zmin;
oz = zmax + zOffset;
```

**GXProject screen-space formula:**
```c
// Perspective (pm[0] == 0):
xc = peye.x * pm[1] + peye.z * pm[2];
yc = peye.y * pm[3] + peye.z * pm[4];
zc = pm[6] + peye.z * pm[5];
wc = 1.0f / -peye.z;

// Orthographic (pm[0] != 0):
xc = pm[2] + peye.x * pm[1];
yc = pm[4] + peye.y * pm[3];
zc = pm[6] + peye.z * pm[5];
wc = 1.0f;

// Screen coords:
*sx = (vp[2]/2) + (vp[0] + wc * (xc * vp[2]/2));
*sy = (vp[3]/2) + (vp[1] + wc * (-yc * vp[3]/2));  // Y negated
*sz = vp[5] + wc * (zc * (vp[5] - vp[4]));
```

**Port implication:** Our GXProject implementation is already correct. The viewport Y
negation and 342.0 guard-band offset are important for scissor rect conversion.

### 13. Fog Implementation (`GXPixel.c`)

**Perspective fog (type bit 3 = 0):**
```c
A = (farz * nearz) / ((farz - nearz) * (endz - startz));
B = farz / (farz - nearz);
C = startz / (endz - startz);
// B is decomposed into mantissa + exponent for hardware
```

**Orthographic fog (type bit 3 = 1):**
```c
A = 1.0f / (endz - startz);
a = A * (farz - nearz);
c = A * (startz - nearz);
```

**Fog type (3 bits):** 0=none, 2=linear, 4=exp, 5=exp2, 6=reverse_exp, 7=reverse_exp2

**BP registers:** 0xEE=fog0(a), 0xEF=fog1(b_m), 0xF0=fog2(b_s), 0xF1=fog3(c+proj+fsel), 0xF2=fogclr

**Port implication:** Fog requires shader support. For bgfx, pass fog params as uniforms
and implement fog in fragment shader: `finalColor = mix(color, fogColor, fogFactor)`.
Not critical for initial rendering but needed for atmosphere in gameplay areas.

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
| **Cull mode may be swapped** | Backfaces visible on some models | `GXGeometry.c` front↔back | ✅ Fixed |

### Bugs Found and Fixed via SDK Verification

These bugs were discovered by comparing our DL parser bit layouts against the actual
dolsdk2004 source code:

| Bug | Impact | SDK Source | Status |
|-----|--------|-----------|--------|
| **BP alpha compare bit layout wrong** | Alpha test misreads ref1/comp0/comp1/op — foliage alpha broken for DL materials | `GXTev.c` GXSetAlphaCompare | ✅ Fixed |
| **BP konst selector at wrong address** | Konst color/alpha selection broken for all DL materials using KCOLOR | `GXInit.c` tevKsel[0-7] at 0xF6-0xFD | ✅ Fixed |
| **BP TEV color reg didn't distinguish konst** | GXSetTevKColor via DL wrote to tev_regs instead of tev_kregs | `GXTev.c` type=8 flag at bits [23:20] | ✅ Fixed |
| **Channel control defaults wrong** | mat_src=GX_SRC_REG (should be VTX), num_chans=1 (should be 0) | `GXInit.c` __GXInitGX | ✅ Fixed |
| **XF channel control not parsed** | J3D materials that set lighting via DL had no effect | `GXLight.c` GXSetChanCtrl → XF 0x100E-0x1011 | ✅ Fixed |
| **XF light memory not parsed** | Lights loaded via DL (GXLoadLightObjImm in DL) had no effect | `GXLight.c` light addr = idx*0x10+0x600 | ✅ Fixed |
| **GEN_MODE cull mode not extracted** | Per-material cull mode from DL ignored | `GXGeometry.c` genMode bits [15:14] + FRONT↔BACK swap | ✅ Fixed |
| **TEV order ALPHA_BUMP/BUMPN unmapped** | Bump-alpha channels mapped to NULL instead of correct enum | `GXTev.c` c2r[] = {0,1,0,1,0,1,7,5,6} | ✅ Fixed |
| **CP MatIdxA/B not handled** | Position matrix selection via DL didn't work | `GXAttr.c` CP reg 0x30/0x40 | ✅ Fixed |
| **GXSetFogColor was no-op** | Fog color always default | `GXPixel.c` GXSetFogColor | ✅ Fixed |

### Display List Command Coverage

From `GXSave.c` `__GXShadowDispList`, the complete FIFO command set:

| Cmd Op | Name | Our Support | Notes |
|--------|------|-------------|-------|
| 0x00 | NOP | ✅ Skip | |
| 0x08 | LOAD_CP | ✅ Full | VCD/VAT/MatIdx/array base+stride + unknown logging |
| 0x10 | LOAD_XF | ✅ Full | Matrices, colors, channels, tex gens, lights |
| 0x20 | LOAD_INDX_A | ✅ Done | Position matrix index via pal_gx_fifo_load_indx |
| 0x28 | LOAD_INDX_B | ✅ Done | Normal matrix index |
| 0x30 | LOAD_INDX_C | ✅ Done | Texture matrix index |
| 0x38 | LOAD_INDX_D | ✅ Done | Light index |
| 0x40 | CALL_DL | ❌ Skipped | Nested display lists (GCN address, not resolvable) |
| 0x48 | INVAL_VTX | ✅ No-op | Vertex cache invalidation (not needed on PC) |
| 0x61 | LOAD_BP | ✅ Full | TEV, alpha compare, konst sel, blend, z-mode, cull, textures |
| 0x80-0xB8 | DRAW | ✅ Full | All primitive types |

## Implementation Recipes

These are concrete implementation guides that map dolsdk2004 knowledge directly to
changes needed in our port code. Each recipe includes the SDK source, what our port
currently does, and exactly what to change.

---

### Recipe 1: Fix RASC Color — Use `mat_src` + `chan_ctrl.enable` (P0, ~30min) ✅ DONE

**Status:** Implemented in commit 7e1dea75.

**Changes made in `src/pal/gx/gx_tev.cpp` (apply_rasc_color):**
- Removed projection-type heuristic (ortho vs perspective)
- Now uses `chan_ctrl.enable` and `mat_src` flags per SDK
- `enable=0`: uses `mat_color` (mat_src=REG) or white pass-through (mat_src=VTX)
- `enable=1`: uses amb_color approximation with grey fallback for dark amb

**Additional fixes in same session:**
- `GX_COLOR0A0` now replicates channel ctrl to alpha channel per SDK (gx_state.cpp)
- `GXInitLight*` functions write to GXLightObj memory at SDK-correct offsets (pal_gx_stubs.cpp)
- `GXLoadLightObjImm` captures light params into `g_gx_state.lights[]` (foundation for Recipe 3)

---

### Recipe 2: Generic TEV Uber-Shader (P1, ~2-3 sessions) ✅ DONE

**Status:** Implemented in commit cd32bae7.

**Changes made:**
- New `fs_gx_tev.sc` uber-shader with uniform-driven mode selection
- All 5 original TEV modes (PASSCLR, REPLACE, MODULATE, BLEND, DECAL) in one shader
- `u_tevConfig.x` selects mode as float (0-5), compatible with GLSL 1.20
- Mode 5 = GENERIC fallback (currently tex * vtx, to be extended)
- Shader pre-compiled for GLSL, ESSL, and SPIR-V backends
- `GX_TEV_SHADER_TEV` (index 5) added to shader program array
- Foundation for per-stage TEV formula evaluation in future iterations

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

### Recipe 3: Implement GX Lighting (P2, ~2 sessions) ✅ DONE

**Status:** Implemented in commit e291f32b.

**Changes made in `src/pal/gx/gx_tev.cpp` (apply_rasc_color):**
- Replaced grey fallback with actual lighting computation
- Accumulates ambient + Σ(light[i].color × diffuse × attenuation)
- Diffuse: GX_DF_NONE=1.0, GX_DF_SIGN/CLAMP=0.5 (average incidence)
- Attenuation: k[0] constant term when attn_fn != GX_AF_NONE
- Modulates by mat_color when mat_src=REG
- Grey fallback only for truly unlit geometry (no active lights)

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

### Recipe 4: Alpha Compare / Discard (P1, ~1 session) ✅ DONE

**Status:** Implemented in commit cd32bae7 as part of TEV uber-shader.

**Changes made:**
- New `fs_gx_tev.sc` uber-shader with `u_alphaTest` and `u_alphaOp` uniforms
- Alpha compare function supports all 8 GXCompare modes
- Logic ops AND/OR/XOR/XNOR implemented via float comparisons (GLSL 1.20 compatible)
- Activated automatically when `alpha_comp0 != GX_ALWAYS || alpha_comp1 != GX_ALWAYS`
- Default alpha state initialized to GX_ALWAYS (per __GXInitGX)

---

### Recipe 5: Mipmap Texture Decoding (P2, ~1 session) ✅ FOUNDATION DONE

**Status:** Mipmap utility functions added in commit 46fdac5d.

**Changes made:**
- `pal_gx_tex_mipmap_chain_size()` — total bytes for mip chain up to max_lod
- `pal_gx_tex_mip_offset()` — byte offset of a specific mip level
- Both functions use `pal_gx_tex_size()` with halved dimensions per level

**Remaining work:** Wire into `upload_gx_texture()` in gx_tev.cpp:
- Check `tex_binding.max_lod > 0`
- Decode each mip level using offset + dimensions
- Upload with `bgfx::createTexture2D(..., hasMips=true, ...)`

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

### Recipe 6: Indexed Matrix Loads — GXLoadPosMtxIndx (P1, ~1 session) ✅ DONE

**Status:** DL parser forwarding implemented in commit 46fdac5d.

**Changes made:**
- DL parser LOAD_INDX_A/B/C/D commands now call `pal_gx_fifo_load_indx()`
- Previously just discarded the index and address values
- Position matrix resolution from vertex arrays enables skeletal animation
- `pal_gx_fifo_load_indx()` already existed for the FIFO path

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

### Recipe 7: Vertex Attribute Size Computation from VAT (P0, reference) ✅ DONE

**Status:** Color attribute size fix applied in commit 46fdac5d.

**Changes made in `src/pal/gx/gx_tev.cpp` (raw_attr_size):**
- Color attributes now use `clrCompSize[6]` table from dolsdk2004 GXSave.c
- RGB565=2, RGB8=3, RGBX8=4, RGBA4=2, RGBA6=3, RGBA8=4
- Previously hardcoded all colors to 4 bytes (only correct for RGBA8/RGBX8)
- Other attribute sizes (POS, NRM, TEX) already match SDK

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
| `src/gx/__gx.h` | `include/pal/gx/gx_state.h` | Done | GXData struct → our GXState |
| `src/gx/GXTev.c` | `src/pal/gx/gx_tev.cpp` | Done | TEV equation + uber-shader + alpha compare |
| `src/gx/GXLight.c` | `src/pal/gx/gx_tev.cpp` (RASC) | Done | Light obj layout, ChanCtrl, CPU lighting |
| `src/gx/GXPixel.c` | `src/pal/gx/gx_tev.cpp` (blend) | Done | Blend, z-mode, fog; cmode0 bits verified |
| `src/gx/GXTransform.c` | `src/pal/gx/gx_tev.cpp` (proj) | Done | Proj storage + viewport done |
| `src/gx/GXTexture.c` | `src/pal/gx/gx_texture.cpp` | LOD 0 | Decode done; mipmap utilities added |
| `src/gx/GXDisplayList.c` | `src/pal/gx/gx_displaylist.cpp` | Done | DL replay working |
| `src/gx/GXSave.c` | `src/pal/gx/gx_displaylist.cpp` | Done | Vertex size + konst bit layouts verified |
| `src/gx/GXInit.c` | Various PAL init | Done | Default state, konst reg addresses verified |
| `src/gx/GXGeometry.c` | `src/pal/gx/gx_displaylist.cpp` | Done | Cull mode FRONT↔BACK swap verified |
| `src/gx/GXAttr.c` | `include/pal/gx/gx_state.h` | Done | VCD/VAT bit layout + MatIdx CP regs |
| `src/gx/GXBump.c` | Not implemented | Stub | Indirect tex system (water/heat) |
| `src/gx/GXDraw.c` | Not needed | N/A | Debug shapes only |
| `src/gx/GXFifo.c` | `src/pal/gx/gx_fifo.cpp` | Done | FIFO management (not needed for PC) |
| `src/gx/GXMisc.c` | Various | Partial | GXSetMisc, DrawDone, timing |
| `src/gx/GXVert.c` | Inline vertex writes | Done | GXPosition/Normal/Color/TexCoord inline |
