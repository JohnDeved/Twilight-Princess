# Twilight Princess — Multiplatform Port Plan

## Strategy

Start from the **ShieldD build** (4,028 source files, 1.1M LOC) instead of bare GCN. The
Shield port already solves platform abstraction in 63+ files via `PLATFORM_SHIELD` conditionals.
We add `PLATFORM_PC` alongside `PLATFORM_SHIELD` in those same `#if` chains — mechanical, not
exploratory. **Total new code: ~7,800 LOC** to port a 1.1M LOC codebase.

**Rendering**: GX → **bgfx** shim (auto-selects D3D12/Metal/Vulkan/GLES per platform).
**Platform services**: **SDL3** (windowing, input, audio). bgfx + SDL3 connect at one point
(`bgfx::PlatformData::nwh` from SDL3 window).

| Platform | GPU (via bgfx) | Platform layer |
|---|---|---|
| Windows | D3D12 / D3D11 | SDL3 |
| Linux | Vulkan / OpenGL | SDL3 |
| macOS | Metal | SDL3 |
| NX Homebrew | GLES 3.1 | libnx + EGL |

## Codebase Inventory

| Layer | LOC | Files | Port action |
|---|---|---|---|
| Game logic (`src/d/actor/`) | 665K | 767 | **Unchanged** |
| Game systems (`src/d/` non-actor) | 169K | 169 | **Unchanged** (15 files do raw GX draws) |
| Framework (`src/f_pc/`, `src/f_op/`, `src/f_ap/`) | 9.5K | 56 | **Unchanged** |
| JSystem engine (`src/JSystem/`) | 67K | 311 | Extend Shield conditionals in ~10 files |
| Machine layer (`src/m_Do/`) | 11.7K | 17 | Extend conditionals + wire PAL (~400 LOC new) |
| SSystem math (`src/SSystem/`) | 6K | 40 | **Unchanged** — pure math, no PPC |
| Z2AudioLib (`src/Z2AudioLib/`) | 15K | 23 | Inherit Shield audio routing |
| Math libs (`src/c/`) | 1.1K | 6 | Inherit Shield errno guards |
| Dolphin SDK (`src/dolphin/`) | 66.5K | 180 | **Replaced** by PAL modules |
| Revolution SDK (`src/revolution/`) | 83K | 192 | **Replaced** by PAL + GX shim |
| Lingcod stubs (`src/lingcod/`) | 29 | 1 | **Replaced** by ImGui |
| **Total existing** | **1,116K** | **~1,800** | |
| **Total new code** | **~7,800** | **~25** | See LOC breakdown below |

## Architecture

```
Game Logic (src/d/, src/f_*)         ~1,050 files, 844K LOC — UNCHANGED
  → Machine Layer (src/m_Do/)        17 files, 11.7K LOC — extend Shield conditionals
    → PAL (src/pal/)                 NEW ~7,800 LOC — replaces src/dolphin/ + src/revolution/
       ├─ GX shim (gx/)             ~5,000 LOC — GX API → bgfx
       ├─ Platform (platform/)       ~1,250 LOC — SDL3 window/input/audio; libnx on NX
       └─ Debug UI (imgui/)          ~300 LOC — replaces lingcod stubs
```

## Shield Conditionals — What We Inherit

| What Shield does | Files | PC action |
|---|---|---|
| Widescreen/framebuffer (`PLATFORM_WII \|\| PLATFORM_SHIELD`) | `m_Do_graphic.cpp` + ~15 | Add `\|\| PLATFORM_PC` |
| Skip CARDInit, GXSaveCPUFifo (`!PLATFORM_SHIELD`) | `m_Do_MemCard.cpp`, `JUTGraphFifo.h` | Same exclusions |
| Dual heap (`sRootHeap2`) | JKernel, `m_Do_main.cpp` | Inherit — both map to host RAM |
| Fixed audio mode (`setOutputMode(1)`) | `JAUInitializer.cpp` | Inherit |
| GCN input path (JUTGamePad, not WPAD) | `m_Do_controller_pad.cpp` | Wire to SDL3 gamepad |
| Math errno guards | `e_acos.c`, `e_asin.c`, `e_fmod.c` | Inherit — prevents x86 crashes |
| Disabled REL dynamic linking (12 blocks) | `c_dylink.cpp` | Statically link everything |
| Larger sampling buffer (16 vs 10) | `m_Re_controller_pad.h` | Inherit |
| Disabled debug reporting | `JAISeMgr.cpp` | Inherit for release |
| Extra SE categories 10–11, optimized audio routing | Z2AudioLib (19 refs) | Inherit |
| Streamlined frame processor | `f_pc_manager.cpp` | Inherit |

**Replace with native code**: `NANDSimpleSafe*` → `pal_save` file I/O; `lingcod_*` stubs → ImGui;
GX FIFO pipe → RAM buffer; PPC assembly → standard math; dolphin/revolution SDK → PAL modules.

## Time Savers (Measured)

| Technique | Impact | Why it saves time |
|---|---|---|
| **J3D handles most rendering** | ~800 actor models render through J3DPacket/J3DShape display lists | Get J3D right → most of the game renders. Only 15 game files do raw `GXBegin` draws |
| **`GX_WRITE_*` macro redirect** | 4 `#define` changes capture ~2,500 GX call sites | Software equivalent of Shield's hardware FIFO intercept — no per-file changes |
| **`GXTexObj` opacity trick** | Store `bgfx::TextureHandle` in opaque `u32 dummy[8]` | Same as Shield's `NVGXNameTex` — zero game code changes |
| **Only 5 TEV preset modes used** | Game code uses GX_PASSCLR (23×), REPLACE (8×), MODULATE (7×), BLEND (4×), DECAL (2×) | J3D auto-generates complex TEV; game-side TEV is trivial to shim |
| **DCStore/DCFlush → no-ops** | 134 cache-coherency calls become `#define` no-ops | x86/ARM have coherent caches — 1 line in PAL header |
| **Yaz0 decompressor is portable** | JKRDecomp.cpp (292 LOC) has zero PPC dependencies | Reuse as-is — no rewrite needed |
| **SSystem math is portable** | 6K LOC — pure math, no PPC assembly | Collision, vectors, matrices compile unchanged |
| **JKernel heap is portable** | JKRHeap manages `malloc`'d blocks, no hardware dependency | Dual-heap inherits from Shield; just back with host RAM |
| **Static linking eliminates REL loader** | 12 `!PLATFORM_SHIELD` blocks disable `c_dylink` | Skip entire REL load/relocate/resolve pipeline (~1 week saved) |
| **Only 19 MWCC intrinsics outside SDK** | 6 are in `d_a_movie_player.cpp` (`__cntlzw` → `__builtin_clz`) | Rest in SDK (excluded). Trivial mechanical replacement |
| **ShieldD file list is battle-tested** | 4,028 files that compile for a non-Nintendo target | Zero guesswork on which files form a complete build |
| **Asset pre-conversion option** | Convert RARC/BMD/BTK from big-endian at build time | JSUInputStream byte-swap (87 LOC) or offline tool — either way, localized |
| **GX texture decode is well-documented** | 10 formats used; Dolphin's TextureDecoder is open-source reference | Untile + decode per-format: ~100 LOC each, proven algorithms |
| **Framework and collision are pure game logic** | f_pc/f_op (9.5K LOC), c_cc collision (3.1K LOC) — no hardware deps | Compile unchanged with no porting work |

## GX Shim — Shield's Blueprint (~5,000 LOC)

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

**GX → bgfx mapping** (Tier A — first playable):

| GX function | bgfx equivalent |
|---|---|
| `GXInit` | `bgfx::init()` |
| `GXBegin`/`GXEnd` + FIFO writes | `bgfx::TransientVertexBuffer` → `bgfx::submit` |
| `GXInitTexObj`/`GXLoadTexObj` | `bgfx::createTexture2D` → `TextureHandle` (injected into opaque `GXTexObj`) |
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

### GX shim LOC breakdown

| Component | LOC | Notes |
|---|---|---|
| State machine + bgfx flush | ~2,500 | `gx_state` struct captures all GX state; flush at draw time |
| TEV → bgfx shader generator | ~1,500 | Compile via `shaderc`, cache by TEV config hash |
| Texture decode (untile + format) | ~1,000 | 10 GX formats → linear RGBA8 for bgfx; ~100 LOC/format |
| Display list record/replay | ~400 | Record GX FIFO to RAM; replay through bgfx encoder |
| **Total GX shim** | **~5,000** | |

## Execution Plan

### Step 1 — Build (~250 LOC new: CMakeLists.txt)
- `config/ShieldD/splits.txt` → CMake source list (exclude dolphin/revolution/lingcod/PPC libs)
- Define `VERSION_PC = 13`, `#define PLATFORM_PC (VERSION == VERSION_PC)` in `global.h`
- Convert 19 MWCC intrinsics: `__cntlzw` → `__builtin_clz`; `__dcbf/__sync` → no-ops
- `DCStoreRange`/`DCFlushRange`/`ICInvalidateRange` → no-op macros (134 calls, 0 logic changes)

### Step 2 — Extend Shield Conditionals (~100 LOC changes across 63+ files)
- `PLATFORM_WII || PLATFORM_SHIELD` → add `|| PLATFORM_PC`
- `!PLATFORM_SHIELD` → `!PLATFORM_SHIELD && !PLATFORM_PC`
- Mechanical — no logic changes, ~1–2 lines per file

### Step 3 — PAL Bootstrap (~1,250 LOC new)

| PAL module | Replaces | Backend | LOC |
|---|---|---|---|
| `pal_window` | dolphin/vi (4K LOC) | SDL3 → bgfx `PlatformData` | ~150 |
| `pal_os` | dolphin/os (15K LOC) | `<chrono>` + `<thread>` | ~200 |
| `pal_fs` | dolphin/dvd (3.5K LOC) + NAND | `<filesystem>` | ~300 |
| `pal_input` | dolphin/pad (993 LOC) | SDL3 gamepad | ~200 |
| `pal_audio` | dolphin/dsp + ai (967 LOC) | SDL3 audio | ~250 (A) / ~800 (B) |
| `pal_save` | NANDSimpleSafe* | `<fstream>` | ~150 |

### Step 4 — DVD/ARAM Simplification (~200 LOC changes)
- DVD: `mDoDvdThd_command_c::create()` → sync read via `pal_fs`, mark done. 835+ actor files untouched.
- ARAM: `aramToMainRam`/`mainRamToAram` → `memcpy`. JKRAram* (1,394 LOC) → thin wrappers around host `malloc`.

### Step 5 — GX Shim Tier A (~5,000 LOC new)
- Redirect `GX_WRITE_*` macros (4 defines → ~2,500 sites captured)
- Implement GX → bgfx mapping table above
- `gx_state` struct → flush to bgfx at draw time
- TEV → bgfx shader: compile via `shaderc`, cache by TEV config hash
- Texture decode: untile 10 GX formats → linear RGBA8 for bgfx (~100 LOC/format)

### Step 6 — Audio (~100 LOC phase A / ~800 LOC phase B)
- **Phase A** (~1 day): `pal_audio` returns silence. Game loop runs unblocked.
- **Phase B**: Software mixer replaces DSP/ARAM mixing (JAudio2: 11.7K LOC) → SDL3 PCM output.

### Step 7 — Input + Save (~350 LOC new)
- Wire `JUTGamePad` → `pal_input` → SDL3 gamepad (Shield's GCN pad path)
- Replace NAND calls with `pal_save` file I/O (keep quest-log format)

### Step 8 — First Playable
- Target: title → Ordon → Faron → Forest Temple → save/load cycle
- ImGui toggles for problematic effects (~300 LOC)

### Step 9 — NX Homebrew (~500 LOC new)
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

| Work stream | LOC (new) | Estimate | Key acceleration |
|---|---|---|---|
| Build + conditionals (1–2) | ~350 | ~3–4 days | Static linking skips REL loader; 19 intrinsics trivial; mechanical `#if` extension |
| PAL + DVD/ARAM (3–4) | ~1,450 | ~1.5 weeks | Shield's disc-less/NAND paths carry over; replaces 150K LOC SDK with 1.4K |
| GX shim Tier A (5) | ~5,000 | ~3–4 weeks | Macro redirect captures 2,500 sites; J3D covers ~800 actors; only 5 TEV presets in game code; bgfx transient VBs match GX |
| Audio (6) | ~100–800 | A: ~1 day / B: ~2 weeks | Shield's fixed audio mode; stub-first unblocks gameplay |
| Input + Save (7) | ~350 | ~2–3 days | Shield's GCN pad path + NAND save path |
| NX bring-up (9) | ~500 | ~1–2 weeks | Same bgfx renderer; only PAL backends swap |

**Total new code: ~7,800 LOC** to port **1,116K LOC** codebase.
**Total to first playable: ~4–6 weeks** (parallel streams).

## Rules

- Never block on rare visual edge cases
- Unsupported GX → log once + safe fallback
- Game logic (`src/d/`, `src/f_*`) stays untouched — 844K LOC unchanged
- Prefer additive shims over invasive rewrites
- When in doubt, check what Shield does
- Track stub-hit counts to prioritize Tier B by frequency
- CI-driven: every commit triggers headless boot test — see [AI Agent Testing Plan](ai-agent-testing-plan.md)
- Agent workflow: step-by-step implementation guide — see [Agent Port Workflow](agent-port-workflow.md)

## Deferred

- Exact post-processing parity (bloom, DoF, motion blur)
- Mirror/movie special rendering paths
- Keyboard/mouse support, advanced input remapping
- Region selection (PAL/JPN) — ship USA first
- Wii-specific content/controls
