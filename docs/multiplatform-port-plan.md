# Twilight Princess — Multiplatform Port Plan

## Strategy

Start from the **ShieldD build** (4,028 source files) instead of bare GCN. The Shield port
already solves platform abstraction in 63+ files via `PLATFORM_SHIELD` conditionals. We add
`PLATFORM_PC` alongside `PLATFORM_SHIELD` in those same `#if` chains — mechanical, not
exploratory.

**Rendering**: GX → **bgfx** shim (auto-selects D3D12/Metal/Vulkan/GLES per platform).
**Platform services**: **SDL3** (windowing, input, audio). bgfx + SDL3 connect at one point
(`bgfx::PlatformData::nwh` from SDL3 window).

| Platform | GPU (via bgfx) | Platform layer |
|---|---|---|
| Windows | D3D12 / D3D11 | SDL3 |
| Linux | Vulkan / OpenGL | SDL3 |
| macOS | Metal | SDL3 |
| NX Homebrew | GLES 3.1 | libnx + EGL |

## Architecture

```
Game Logic (src/d/, src/f_*)         ~1,050 files — UNCHANGED
  → Machine Layer (src/m_Do/)        17 files — extend Shield conditionals
    → PAL (src/pal/)                 NEW — replaces src/dolphin/ + src/revolution/
       ├─ GX shim (gx/)             GX API → bgfx (via GX_WRITE_* macro redirect)
       ├─ Platform (platform/)       SDL3 window/input/audio; libnx on NX
       └─ Debug UI (imgui/)          Replaces lingcod stubs
```

**Excluded from port build** (replaced by PAL): `src/dolphin/` (180), `src/revolution/` (192),
`src/lingcod/` (1), PowerPC support libs.

## Shield Conditionals — What We Inherit

| What Shield does | Files | PC action |
|---|---|---|
| Widescreen/framebuffer (`PLATFORM_WII \|\| PLATFORM_SHIELD`) | `m_Do_graphic.cpp` + ~15 | Add `\|\| PLATFORM_PC` |
| Skip CARDInit, GXSaveCPUFifo (`!PLATFORM_SHIELD`) | `m_Do_MemCard.cpp`, `JUTGraphFifo.h` | Same exclusions |
| Dual heap (`sRootHeap2`) | JKernel, `m_Do_main.cpp` | Inherit — both map to host RAM |
| Fixed audio mode (`setOutputMode(1)`) | `JAUInitializer.cpp` | Inherit |
| GCN input path (JUTGamePad, not WPAD) | `m_Do_controller_pad.cpp` | Wire to SDL3 gamepad |
| Math errno guards | `e_acos.c`, `e_asin.c`, `e_fmod.c` | Inherit — prevents x86 crashes |
| Disabled REL dynamic linking (12 blocks in `c_dylink.cpp`) | `c_dylink.cpp` | Statically link everything |
| Larger sampling buffer (16 vs 10) | `m_Re_controller_pad.h` | Inherit |
| Disabled debug reporting | `JAISeMgr.cpp` | Inherit for release |
| Extra SE categories 10–11, optimized audio routing | Z2AudioLib (19 refs) | Inherit |
| Streamlined frame processor | `f_pc_manager.cpp` | Inherit |

**Replace with native code**: `NANDSimpleSafe*` → `pal_save` file I/O; `lingcod_*` stubs → ImGui;
GX FIFO pipe → RAM buffer; PPC assembly → standard math; dolphin/revolution SDK → PAL modules.

## GX Shim — Shield's Blueprint

Shield compiles all 20 `revolution/gx/*.c` files and runs them through NVIDIA's proprietary
binary translation layer. We can't reuse that layer, but it reveals the exact intercept points:

**`GX_WRITE_*` macro redirect** — The single most impactful change. Four macros in `GXRegs.h`
control all FIFO writes to `0xCC008000`. Redirecting them to a RAM buffer captures **~2,500 GX
call sites** with 4 `#define` changes:

```c
#if PLATFORM_PC
  #define GX_WRITE_U8(ub)   gx_fifo_write_u8(ub)
  #define GX_WRITE_U16(us)  gx_fifo_write_u16(us)
  #define GX_WRITE_U32(ui)  gx_fifo_write_u32(ui)
  #define GX_WRITE_F32(f)   gx_fifo_write_f32(f)
#endif
```

**`GXTexObj` opacity trick** — The struct is opaque (`u32 dummy[8]`). Shield's `NVGXNameTex`
injects Tegra handles inside it. We inject `bgfx::TextureHandle` the same way — zero game
code changes.

**GX → bgfx mapping** (Tier A — first playable):

| GX function | bgfx equivalent |
|---|---|
| `GXInit` | `bgfx::init()` |
| `GXBegin`/`GXEnd` + FIFO writes | `bgfx::TransientVertexBuffer` → `bgfx::submit` |
| `GXInitTexObj`/`GXLoadTexObj` | `bgfx::createTexture2D` → `TextureHandle` |
| `GXSetTevOp`/`GXSetTevOrder` | `bgfx::ProgramHandle` (TEV → shader via `shaderc`) |
| `GXSetBlendMode`/`GXSetZMode` | `BGFX_STATE_BLEND_*` / `BGFX_STATE_DEPTH_*` flags |
| `GXLoadPosMtxImm` | `bgfx::setUniform` (Mat4) |
| `GXCallDisplayList` | Record + replay through bgfx encoder |
| `GXDrawDone` | `bgfx::frame()` |

**Why bgfx over raw OpenGL**: Transient VBs match GX's immediate-mode draws (per-frame,
auto-freed). Per-draw state flags avoid OpenGL global state leaks. One shader source → all
backends via `shaderc`. Metal on macOS (Apple deprecated OpenGL in 2018). Built-in ImGui +
RenderDoc.

**Tier B** (expand on demand): remaining `GXSet*` variants, lighting, fog, indirect textures,
readback, perf counters. Gate heavy files behind ImGui toggles (`d_kankyo_rain.cpp` — 766 GX
calls, `d_a_movie_player.cpp` — 111, `d_a_mirror.cpp` — 73).

## Execution Plan

### Step 1 — Build (CMake from ShieldD file list)
- `config/ShieldD/splits.txt` → CMake source list (exclude dolphin/revolution/lingcod/PPC libs)
- Define `VERSION_PC = 13`, `#define PLATFORM_PC (VERSION == VERSION_PC)` in `global.h`
- Convert MWCC intrinsics: `__cntlzw` → `__builtin_clz`; `__dcbf/__sync` → no-ops

### Step 2 — Extend Shield Conditionals
- 63+ files: `PLATFORM_WII || PLATFORM_SHIELD` → add `|| PLATFORM_PC`
- `!PLATFORM_SHIELD` → `!PLATFORM_SHIELD && !PLATFORM_PC`
- Mechanical — no logic changes

### Step 3 — PAL Bootstrap

| PAL module | Replaces | Backend |
|---|---|---|
| `pal_window` | dolphin/vi | SDL3 → bgfx `PlatformData` |
| `pal_os` | dolphin/os | `<chrono>` + `<thread>` |
| `pal_fs` | dolphin/dvd + NAND | `<filesystem>` |
| `pal_input` | dolphin/pad | SDL3 gamepad |
| `pal_audio` | dolphin/dsp + ai | SDL3 audio |
| `pal_save` | NANDSimpleSafe* | `<fstream>` |

### Step 4 — DVD/ARAM Simplification
- DVD: `mDoDvdThd_command_c::create()` → sync read via `pal_fs`, mark done. 835+ actor files untouched.
- ARAM: `aramToMainRam`/`mainRamToAram` → `memcpy`. `JKRAramArchive` → `JKRMemArchive` wrapper.

### Step 5 — GX Shim (Tier A)
- Redirect `GX_WRITE_*` macros (4 defines → ~2,500 sites captured)
- Implement GX → bgfx mapping table above
- `gx_state` struct → flush to bgfx at draw time
- TEV → bgfx shader: compile via `shaderc`, cache by TEV config hash

### Step 6 — Audio
- **Phase A** (~1 day): `pal_audio` returns silence. Game loop runs unblocked.
- **Phase B**: Software mixer replaces DSP/ARAM mixing → SDL3 PCM output.

### Step 7 — Input + Save
- Wire `JUTGamePad` → `pal_input` → SDL3 gamepad (Shield's GCN pad path)
- Replace NAND calls with `pal_save` file I/O (keep 0xA94-byte quest-log format)

### Step 8 — First Playable
- Target: title → Ordon → Faron → Forest Temple → save/load cycle
- ImGui toggles for problematic effects

### Step 9 — NX Homebrew
- Same bgfx renderer (auto-selects GLES). Swap PAL backends: libnx/EGL, HID, audren, romfs.
- `VERSION_NX_HB = 14`, `PLATFORM_NX_HB` inherits same conditionals as `PLATFORM_PC`.

## Critical Path

```
Step 1 (CMake)  ──►  Step 2 (conditionals)
                       ├─► Steps 3–4 (PAL + DVD/ARAM)  ───────┐
                       ├─► Step 5 (GX shim Tier A)  ──────────┤──► Step 8 (first playable)
                       ├─► Step 6A (audio stubs)               │      └─► Step 10 (polish)
                       ├─► Step 7 (input + save)               │
                       └─► Step 9 (NX — after PC playable)     │
```

| Work stream | Estimate | Key acceleration |
|---|---|---|
| Build + conditionals (1–2) | ~3–4 days | Static linking skips REL loader; mechanical `#if` extension |
| PAL + DVD/ARAM (3–4) | ~1.5 weeks | Shield's disc-less and NAND error paths carry over |
| GX shim Tier A (5) | ~3–4 weeks | Macro redirect captures 2,500 sites; bgfx transient VBs match GX; one shader target |
| Audio (6) | Phase A: ~1 day | Shield's fixed audio mode — no hardware queries |
| Input + Save (7) | ~2–3 days | Shield's GCN pad path + NAND save path |

**Total to first playable: ~4–6 weeks** (parallel streams).

## Rules

- Never block on rare visual edge cases
- Unsupported GX → log once + safe fallback
- Game logic (`src/d/`, `src/f_*`) stays untouched
- Prefer additive shims over invasive rewrites
- When in doubt, check what Shield does
- Track stub-hit counts to prioritize Tier B by frequency

## Deferred

- Exact post-processing parity (bloom, DoF, motion blur)
- Mirror/movie special rendering paths
- Keyboard/mouse support, advanced input remapping
- Region selection (PAL/JPN) — ship USA first
- Wii-specific content/controls
