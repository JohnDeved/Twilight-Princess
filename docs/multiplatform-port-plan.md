# Twilight Princess Multiplatform Port Plan (Shield-Derived, Time-to-Ship Optimized)

## Strategy: Leverage the Shield Port

The Nvidia Shield version of Twilight Princess is already a non-Nintendo platform target that
compiles from the **same source tree**. While the Shield build still targets PowerPC and runs
under a translation layer on the Shield TV hardware, it has **already solved most of the
platform-abstraction problems** we need for a native PC/NX port:

| Problem | Shield's solution | What we reuse |
|---|---|---|
| Platform conditionals | `PLATFORM_SHIELD` in 63+ files, 220+ files use `PLATFORM_*` | Add `PLATFORM_PC` to the same `#if` chains — game logic unchanged, only preprocessor directives updated |
| Dual memory heaps | Shield shares `JKRHeap::sRootHeap2` (MEM2) with Wii | Keep dual-heap; map both to host RAM |
| Simplified saves | `NANDSimpleSafeOpen/Close` instead of physical memory cards | Replace NAND calls with `pal_save` file I/O |
| No physical media | Shield skips `CARDInit()` (`#if !PLATFORM_SHIELD`) | Same — skip card I/O, use `pal_fs` |
| Fixed audio output | `JASDriver::setOutputMode(1)` (no hardware query) | Same pattern — set fixed output mode for host audio |
| FIFO state bypass | `GXSaveCPUFifo()` disabled (`#if !PLATFORM_SHIELD`) | Same — our GX shim owns state, no FIFO save needed |
| Math robustness | Shield adds `errno = EDOM` guards in `e_acos.c`, `e_asin.c`, `e_fmod.c` | Keep these fixes — they prevent crashes on x86 too |
| Debug reporting | Shield disables some `JASReport` calls | Keep disabled for release builds |
| Input model | Shield reuses `JUTGamePad` (GCN path, not WPAD/KPAD) | Same — map SDL3 gamepad to GCN `PadStatus` |
| Widescreen / framebuffer | `#if PLATFORM_WII \|\| PLATFORM_SHIELD` blocks | Add `PLATFORM_PC` to these — get widescreen free |
| Post-processing scales | `lingcod_getBloomScale()` / `lingcod_getDoFScale()` return 1.0f | Replace `lingcod` stubs with ImGui-controllable values |
| Sampling buffer | `RECPD_SAMPLING_BUF_COUNT = 16` (vs GCN's 10) | Use Shield's larger buffer for PC too |
| Dynamic linking disabled | 12 `#if !PLATFORM_SHIELD` blocks in `c_dylink.cpp` | Skip REL dynamic linker — statically link everything |
| Audio categories | Shield/Wii share extra SE categories 10–11 | Inherit expanded audio routing (Z2AudioMgr) |
| Actor management | Shield-specific frame processor path (`f_pc_manager.cpp`) | Inherit optimized process scheduling |

**Bottom line**: Instead of porting from GCN and re-discovering every platform boundary,
we start from the ShieldD (debug) build configuration. The ShieldD config compiles **4,028
source files** — the full game — and its `PLATFORM_SHIELD` conditionals mark exactly where
hardware assumptions live. Our port adds `PLATFORM_PC` alongside `PLATFORM_SHIELD` in most
cases, then replaces the remaining PowerPC/GX hardware layer with native implementations.

## Shield's GX Graphics Path — What We Can and Cannot Reuse

The Shield port's approach to GX graphics has important implications for our porting strategy.

### How Shield handles GX (the translation layer)

Shield compiles **all 20 `revolution/gx/*.c` files** unchanged and runs the resulting PowerPC
GX code through NVIDIA's binary translation layer on the Tegra hardware. This means:

- The game writes GX commands to `0xCC008000` (the hardware FIFO address) via `GXWGFifo`
  volatile writes — Shield's translation layer intercepts these memory-mapped I/O writes.
- `GXInit()` calls `EnableWriteGatherPipe()` which sets up the PPC Write Gather Buffer register
  (`PPCMtwpar(0xCC008000)`, HID2 bit 30) — translated to Tegra equivalents at runtime.
- Frame submission uses `GXDrawDone()` → `GXSetDrawDone()` → PE register interrupt — again
  intercepted by the translation layer.
- NVIDIA adds a thin runtime hook layer (**NVGX**) for texture naming and render group management:
  `NVGXNameTex`, `NVGXNameTexCI`, `NVGXDestroyTex`, `NVGXCreateGroup`, `NVGXCreateSubGroup`,
  `NVGXReleaseGroup`. These are **runtime-linked symbols** (in `config/Shield/symbols.txt`)
  that map to the proprietary Tegra GPU driver — not available to us.

### What this means for us

We **cannot** reuse Shield's GX translation layer directly — it's NVIDIA's proprietary binary
translator. However, Shield's architecture gives us a **precise blueprint** for where to
intercept GX calls:

| Shield intercept point | What Shield does | What we do instead |
|---|---|---|
| `GXWGFifo` at `0xCC008000` | Binary translation intercepts volatile writes | Redirect `GX_WRITE_*` macros to RAM command buffer |
| `GXInit()` / `EnableWriteGatherPipe()` | Translation layer hooks PPC register setup | Initialize OpenGL context + GX state tracker |
| `GXDrawDone()` / `GXCopyDisp()` | Translation layer intercepts PE register writes | Flush command buffer → OpenGL draw calls |
| `GXSaveCPUFifo()` | **Disabled** (`#if !PLATFORM_SHIELD`) | Same — our shim owns FIFO state, no save needed |
| `NVGXNameTex` / `NVGXDestroyTex` | Maps GX textures to Tegra GPU handles | Map `GXTexObj` to OpenGL texture objects |
| `NVGXCreateGroup` / `NVGXReleaseGroup` | Groups draw calls for GPU scheduling | Not needed — OpenGL handles draw ordering |
| `J3D_sqrtf()` PPC assembly | **Disabled** (`#if !PLATFORM_SHIELD`) | Same — use standard `sqrtf()` |

### The GX API contract is our shim surface

The key insight is that **GX already has a clean API boundary**. The public GX functions
(`GXInit`, `GXSetViewport`, `GXBegin`, `GXLoadTexObj`, etc.) and the `GXTexObj` / `GXTlutObj`
opaque structs (defined as `u32 dummy[8]` — intentionally hiding implementation) form a
well-defined contract. Our GX shim provides the same function signatures backed by OpenGL:

```
Game/J3D code                    Shield (translation layer)       PC port (native shim)
─────────────                    ────────────────────────         ─────────────────────
GXInitTexObj(&obj, img, ...)  →  PPC binary → Tegra GPU driver   →  glGenTextures + glTexImage2D
GXBegin(GX_TRIANGLES, ...)    →  PPC binary → Tegra FIFO         →  begin RAM vertex buffer
GXPosition3f32(x, y, z)      →  PPC binary → Tegra FIFO write   →  write to RAM vertex buffer
GXEnd()                       →  PPC binary → Tegra FIFO flush   →  glBufferData + glDrawArrays
GXSetTevOp(stage, mode)       →  PPC binary → Tegra TEV          →  generate GLSL fragment shader
```

This is exactly the approach used by Dolphin Emulator's GPU backend and other GX → GL
projects. Shield proves the GX API surface is stable enough for a non-GCN GPU to render
correctly — our shim just does the translation in C++ instead of binary translation.

## Scope

- Port baseline: **ShieldD build configuration** (4,028 source files, 804 REL modules).
- Canonical gameplay: **GCN behavior** (Shield already preserves GCN input model and content).
- Target platforms: **Windows, Linux, macOS, NX Switch (homebrew)**.
- Primary rendering backend: **OpenGL**.
- Build system: **CMake** (parallel to existing Ninja/configure.py decomp toolchain).
- Development tooling: **ImGui** integrated from the start (replaces `lingcod` debug hooks).

## Codebase Snapshot (Measured)

| Area | Files | Notes |
|---|---|---|
| `src/d/` (game logic, excl. actor/) | ~159 .cpp | Core game systems (camera, save, stage, etc.) |
| `src/d/actor/` | ~835 .cpp | All actors — largest directory (~994 total under `src/d/`) |
| `src/f_pc/` (process framework) | 32 .cpp | Generic process management |
| `src/f_op/` (operation framework) | 23 .cpp | Actor/scene/camera operations |
| `src/f_ap/` (application) | 1 .cpp | Top-level game mode |
| `src/m_Do/` (machine layer) | 17 .cpp | **Primary hardware abstraction — key porting surface** |
| `src/JSystem/` | ~311 .cpp | J3D rendering, JKernel, JAudio2, J2D, JParticle |
| `src/dolphin/` | ~180 .cpp/.c | GameCube SDK (DVD, OS, GX, PAD, DSP, etc.) |
| `src/revolution/` | ~192 .cpp/.c | Wii SDK — Shield compiles all of these too |
| `src/Z2AudioLib/` | 23 .cpp | TP-specific audio (wraps JAudio2) |
| `src/SSystem/` | 40 .cpp | Math, collision, component utilities |
| `src/lingcod/` | 1 .c | Nvidia stub layer — replace with ImGui/PAL hooks |
| GX headers | 21 in `include/dolphin/gx/`, 21 in `include/revolution/gx/` | Shield compiles these too — GX shim replaces them |

**Total GX function call sites across `src/`**: ~2,500+ (dominated by `GXSet*`, `GXInit*`, `GXLoad*`, FIFO writes).

## Platform Matrix

| Platform | Graphics | Platform Layer |
|---|---|---|
| Windows | OpenGL 4.5 | SDL3 |
| Linux | OpenGL 4.5 | SDL3 |
| macOS | OpenGL 4.1 | SDL3 |
| NX Homebrew | OpenGL 4.3 (Mesa/Nouveau) | libnx + EGL |

## Core Principles

- **Shield-derived, not from-scratch**: start from the ShieldD configuration as the closest
  existing non-Nintendo target. Inherit its platform conditionals and behavioral choices.
- **GCN-first semantics**: preserve GameCube gameplay feel and content (Shield already does this).
- **One renderer first**: OpenGL everywhere for initial ship.
- **Boundary-only porting**: replace `src/dolphin/`, `src/revolution/` SDK layers with PAL +
  GX shim; keep everything above `src/m_Do/` untouched.
- **Fail-soft, not fail-hard**: unknown GX paths render with safe defaults + log once.
- **Playable-first**: title → field → combat → menu/save before visual perfection.

## What We Intentionally Skip Early

- Wii-specific systems (WPAD/KPAD in 45+ files, Wii speaker — Shield already skips most of these).
- Exact parity for heavy post-processing (DoF/bloom/motion blur) in first playable.
- Mirror-heavy and movie paths in first pass (see measured heavy areas below).
- Full GX API upfront (~2,500 call sites; implement hot path first, long tail on demand).
- Broad settings UI and advanced remapping in v1 (basic stable mapping first).

## Architecture

```
Game Logic (src/d/, src/d/actor/)       ~994 files — DO NOT TOUCH (same as Shield)
  → Framework (src/f_pc/, src/f_op/, src/f_ap/)   56 files — DO NOT TOUCH
    → Machine Layer (src/m_Do/)          17 files — THIN ADAPTATION (reuse Shield conditionals)
      → PAL (src/pal/)                   NEW — replaces src/dolphin/ + src/revolution/
         → GX Shim (src/pal/gx/)        NEW — replaces revolution/gx/ (GX → OpenGL)
         │                                Uses GX_WRITE_* macro redirect (same intercept
         │                                point as Shield's binary translation layer)
         → Platform (src/pal/platform/)  NEW — replaces dolphin/os,dvd,pad,dsp,vi + lingcod
```

**Key insight — the Shield port already marks the exact boundaries.** Every
`#if PLATFORM_SHIELD` or `#if !PLATFORM_SHIELD` in the codebase is a signpost showing where
hardware assumptions live. Our job is to add `PLATFORM_PC` to those same decision points and
provide native implementations underneath.

## Shield Conditional Audit (What We Inherit vs Replace)

The 63+ files with `PLATFORM_SHIELD` conditionals fall into clear categories:

### Inherit directly (add `PLATFORM_PC` alongside `PLATFORM_SHIELD`)

| Pattern | Files | Action |
|---|---|---|
| `PLATFORM_WII \|\| PLATFORM_SHIELD` (widescreen, framebuffer) | `m_Do_graphic.cpp` + ~15 others | Change to `PLATFORM_WII \|\| PLATFORM_SHIELD \|\| PLATFORM_PC` |
| Dual heap (`sRootHeap2`) | JKernel, m_Do_main.cpp | Inherit — PC has plenty of RAM |
| `!PLATFORM_SHIELD` guards (skip `CARDInit`, `GXSaveCPUFifo`) | `m_Do_MemCard.cpp`, `JUTGraphFifo.h` | Same exclusions apply to PC |
| Fixed audio output mode | `JAUInitializer.cpp` | Inherit `setOutputMode(1)` |
| Larger sampling buffer (16 vs 10) | `m_Re_controller_pad.h` | Inherit — no downside on PC |
| Math `errno` guards | `e_acos.c`, `e_asin.c`, `e_fmod.c` | Inherit — prevents x86 crashes |
| Disabled debug reporting | `JAISeMgr.cpp` | Inherit for release, toggle for debug |

### Replace with native implementations

| Pattern | Files | Action |
|---|---|---|
| `lingcod_*` stubs (bloom/DoF scales, events) | `src/lingcod/LingcodPatch.c` | Replace with ImGui-controllable post-processing params |
| `NANDSimpleSafeOpen/Close` | `m_Do_MemCard.cpp` | Replace with `pal_save` file I/O |
| GX FIFO pipe (`0xCC008000`) | `include/dolphin/gx/GXVert.h` | Redirect writes to RAM buffer in GX shim |
| PowerPC inline assembly | `J3DTransform.cpp` (Shield already disables) | Use standard `sqrtf`/math — Shield shows the way |
| Revolution SDK (`src/revolution/`) | 192 files | Replace with PAL modules |
| Dolphin SDK (`src/dolphin/`) | 180 files | Replace with PAL modules |

## Separation of Concerns

| Layer | Directories | Rules |
|---|---|---|
| Game logic | `src/d/`, `src/d/actor/`, `src/f_*` | No platform headers, no OpenGL. Compile unchanged. |
| Machine layer | `src/m_Do/` | Extend existing `PLATFORM_SHIELD` conditionals with `PLATFORM_PC`. |
| GX shim | `src/pal/gx/` | Owns GX → OpenGL translation. Replaces `revolution/gx/` + `dolphin/gx/`. |
| Platform services | `src/pal/platform/` | SDL3 / libnx backends. Replaces `dolphin/os,dvd,pad,dsp,vi` + `lingcod`. |
| Debug UI | `src/pal/imgui/` | ImGui overlays; replaces `lingcod` debug hooks. |

## Step-by-Step Execution Plan

### Step 1: Build Modernization (Shield-Derived)

**Goal**: Compile the ShieldD source set with a modern host compiler.

- Use `config/ShieldD/splits.txt` (4,028 files) as the definitive source list — this is the
  proven set of files that compile together for a non-GCN target.
- Introduce root `CMakeLists.txt` targeting the host platform.
- The current build (`configure.py` → Ninja → MWCC) stays untouched for decomp validation.
- **Exclude from port build** (replaced by PAL):
  - `src/dolphin/` (180 files) — replaced by PAL platform services
  - `src/revolution/` (192 files) — replaced by PAL platform services
  - `src/PowerPC_EABI_Support/`, `src/TRK_MINNOW_DOLPHIN/`, `src/NdevExi2A/`
  - `src/odemuexi2/`, `src/odenotstub/`
  - `src/lingcod/` (1 file) — replaced by ImGui/PAL hooks
- **Include in port build** (compile with `PLATFORM_PC` defined):
  - `src/d/`, `src/d/actor/` (~994 files) — unchanged
  - `src/f_pc/`, `src/f_op/`, `src/f_ap/` (56 files) — unchanged
  - `src/m_Do/` (17 files) — extend Shield conditionals
  - `src/JSystem/` (~311 files) — extend Shield conditionals where present
  - `src/Z2AudioLib/` (23 files), `src/SSystem/` (40 files) — unchanged
- Define `VERSION_PC = 13` and add to `include/global.h`:
  ```c
  #define PLATFORM_PC (VERSION == VERSION_PC)
  ```
  This makes `PLATFORM_PC` a peer of `PLATFORM_SHIELD`, inheriting the same conditional
  architecture.
- Convert MWCC constructs (most already handled by existing macros):
  - `__cntlzw` → `__builtin_clz`; `__dcbf`/`__dcbst`/`__icbi` → no-ops on x86;
    `__sync` → no-op (x86 is strongly ordered).
  - `DECL_WEAK`, `ATTRIBUTE_ALIGN`, `NO_INLINE` in `types.h`/`global.h` already portable.
- **Time saver**: ShieldD's `splits.txt` is a battle-tested file list — no guesswork about
  which files form a complete build. Shield's existing `#if !PLATFORM_SHIELD` exclusions
  (e.g., `CARDInit`, `GXSaveCPUFifo`) carry over to `PLATFORM_PC` for free.

### Step 2: Extend Shield Conditionals to PC

**Goal**: Make all `PLATFORM_SHIELD` code paths available to `PLATFORM_PC`.

- Audit the 63+ files with `PLATFORM_SHIELD` conditionals:
  - `#if PLATFORM_WII || PLATFORM_SHIELD` → `#if PLATFORM_WII || PLATFORM_SHIELD || PLATFORM_PC`
  - `#if !PLATFORM_SHIELD` → `#if !PLATFORM_SHIELD && !PLATFORM_PC`
  - `#if PLATFORM_SHIELD`-only → evaluate: inherit or replace with PC-specific code
- For `REGION_*` blocks: define `REGION_USA` for PC (same as GCN USA).
- Shield's `REGION_CHN` code (Chinese text, lingcod integration) → skip for PC.
- **Time saver**: This is mechanical — extend existing `#if` chains. No logic changes.
  The Shield port already proved these code paths work on a non-Nintendo target.
  Estimated ~63 files to touch, most are one-line `#if` chain extensions.

### Step 3: PAL Bootstrap (Replace SDK Layers)

**Goal**: Provide native implementations for the SDK functions that Shield ran via translation.

The Shield build still compiles `src/dolphin/` and `src/revolution/` because it runs them
through a PowerPC translation layer. Our port replaces those 372 files with ~6 thin PAL modules:

| PAL module | Replaces | PC backend | NX backend |
|---|---|---|---|
| `pal_window` | `dolphin/vi`, `revolution/vi` (video init) | SDL3 | libnx + EGL |
| `pal_os` | `dolphin/os` (timers, threads, exceptions) | `<chrono>` + `<thread>` | libnx |
| `pal_fs` | `dolphin/dvd` + NAND filesystem | `<filesystem>` | libnx romfs |
| `pal_input` | `dolphin/pad` (Shield reuses GCN pad path) | SDL3 gamepad | libnx HID |
| `pal_audio` | `dolphin/dsp` + `dolphin/ai` | SDL3 audio | libnx audren |
| `pal_save` | `NANDSimpleSafe*` (Shield's save path) | `<fstream>` | libnx account/save |

**m_Do touch points** — same files Shield already modifies:

| m_Do file | Shield's current behavior | PC PAL replacement |
|---|---|---|
| `m_Do_main.cpp` | Uses `sRootHeap2` (dual heap, `PLATFORM_WII \|\| PLATFORM_SHIELD`) | Inherit dual heap → both backed by host RAM |
| `m_Do_graphic.cpp` | 15+ Shield conditionals (widescreen, framebuffer, cursor) | Inherit widescreen path, wire to `pal_window` + GX shim |
| `m_Do_controller_pad.cpp` | Uses GCN `JUTGamePad` path (not WPAD) | Wire `JUTGamePad` to `pal_input` (SDL3 gamepad) |
| `m_Do_audio.cpp` | Loads `Z2CSRes.arc`, inits `Z2AudioCS` | Keep audio archive loading via `pal_fs` |
| `m_Do_MemCard.cpp` | Skips `CARDInit`, uses `NANDSimpleSafe*` for saves | Replace NAND with `pal_save` file I/O |
| `m_Do_dvd_thread.cpp` | DVD async commands (unchanged from GCN on Shield) | Make `create()` do sync read via `pal_fs`, mark done |
| `m_Do_machine.cpp` | Exception handler, video mode | Replace with `pal_os` signal handler |
| `m_Do_DVDError.cpp` | DVD error overlay | Stub (no disc errors on PC) |

### Step 4: DVD/ARAM Simplification

**Goal**: Collapse hardware I/O to direct memory operations.

**DVD** — Shield doesn't use physical media either, but still runs the DVD thread code through
PowerPC translation. We simplify further:
- `mDoDvdThd_command_c` subclasses: `create()` → sync read via `pal_fs`, mark done.
- `sync()` → returns true immediately.
- Zero changes to callers (835+ actor files untouched).

**ARAM** — Shield runs ARAM code through the translation layer too. We collapse it:
- `JKRAram::aramToMainRam()` / `mainRamToAram()` → `memcpy`.
- `JKRAramArchive` → backed by `JKRMemArchive` internally.
- `JKRAramHeap` / `JKRAramBlock` → thin wrappers around host `malloc`.
- Audio ARAM streams → RAM ring buffer.

**Time saver**: The Shield port proves these subsystems work when ARAM is just memory —
the translation layer was already treating ARAM addresses as regular RAM. We formalize that.

### Step 5: GX Shim (Replace Shield's Translated GX With Native OpenGL)

**Scope**: The Shield build compiles all 20 `revolution/gx/*.c` files and runs them through
NVIDIA's proprietary PowerPC binary translation layer. We replace that entire layer with a
native C++ GX → OpenGL shim. See "Shield's GX Graphics Path" above for the full analysis of
Shield's intercept points and how our shim maps to each one.

**Key Shield insight**: The GX API surface is a clean, well-defined contract. `GXTexObj` and
other state objects use opaque structs (`u32 dummy[8]`), which means we can redefine internals
to hold OpenGL handles without any game code knowing. Shield's NVGX texture naming functions
(`NVGXNameTex`, `NVGXDestroyTex`) prove this approach works — they map GX texture objects to
a different GPU's handle space at runtime.

**GX FIFO replacement strategy** (informed by Shield's translation model):

The hardware FIFO at `0xCC008000` is accessed via `GX_WRITE_*` macros in
`include/revolution/private/GXRegs.h`:
```c
#define GX_WRITE_U8(ub)    GXWGFifo.u8 = (u8)(ub)
#define GX_WRITE_U16(us)   GXWGFifo.u16 = (u16)(us)
#define GX_WRITE_U32(ui)   GXWGFifo.u32 = (u32)(ui)
#define GX_WRITE_F32(f)    GXWGFifo.f32 = (f32)(f)
```

For `PLATFORM_PC`, redirect these to a RAM command buffer (original macros remain active
for GCN/Wii/Shield builds):
```c
#if PLATFORM_PC
  #define GX_WRITE_U8(ub)    gx_fifo_write_u8(ub)
  #define GX_WRITE_U16(us)   gx_fifo_write_u16(us)
  #define GX_WRITE_U32(ui)   gx_fifo_write_u32(ui)
  #define GX_WRITE_F32(f)    gx_fifo_write_f32(f)
#endif
```

This is the **single most impactful substitution** in the port — it redirects ~2,500 GX call
sites through our shim without touching any game code. Shield's translation layer does the
equivalent interception in hardware; we do it in software at the macro level.

#### Tier A — First Playable (title → field → combat)

| GX area | Key functions | Primary consumers | Shield behavior |
|---|---|---|---|
| State init | `GXInit`, `GXSetViewport`, `GXSetScissor` | `m_Do_graphic.cpp` | Translation layer initializes Tegra GPU |
| Vertex format | `GXSetVtxDesc`, `GXSetVtxAttrFmt`, `GXClearVtxDesc` | J3D material setup | Translated to Tegra vertex state |
| Matrix | `GXLoadPosMtxImm`, `GXLoadNrmMtxImm`, `GXSetCurrentMtx` | J3D draw, actors | Translated to Tegra matrix registers |
| Draw | `GXBegin`, `GXEnd`, FIFO writes (`GXPosition*`, `GXColor*`, `GXTexCoord*`) | J3D shape draw, 2D UI | FIFO writes intercepted by translation layer |
| Texture | `GXInitTexObj`, `GXLoadTexObj`, `GXInitTlutObj`, `GXLoadTlut` | J3D, particle, UI | Mapped via `NVGXNameTex` to Tegra handles |
| TEV | `GXSetTevOp`, `GXSetTevOrder`, `GXSetNumTevStages`, `GXSetTevColor` | 41+ files — dominant material path | Translated to Tegra shader pipeline |
| Blend/Z | `GXSetBlendMode`, `GXSetZMode`, `GXSetAlphaCompare` | J3D pixel setup | Translated to Tegra blend state |
| Display list | `GXCallDisplayList` | 10 files: `J3DShapeDraw`, `J3DPacket`, `d_a_player`, etc. | Replayed through translation layer |

**Shield confirms**:
- `GXSaveCPUFifo()` disabled — our shim owns state, no hardware FIFO save needed.
- `J3D_sqrtf()` PPC assembly disabled — standard `sqrtf()` works correctly.
- `NVGXNameTex`/`NVGXDestroyTex` exist — proves GX texture objects can be mapped to any GPU.
- `GXDrawDone()` triggers PE interrupt 0x13 — we trigger `glFinish()` + swap instead.

#### Tier B — Expand by Demand

- Remaining `GXSet*` variants (lighting, fog, indirect textures).
- `GXDraw*` geometric primitives, `GXGet*` readback, `GXPerf*` counters.
- Rare TEV/texture filter modes.

**Implementation strategy**:
- `gx_state` struct captures all GX state; flush to OpenGL at draw time.
- Redefine `GXTexObj` internals to hold `GLuint` texture handle (opaque `u32 dummy[8]` allows
  this — same approach as Shield's `NVGXNameTex` mapping to Tegra handles).
- FIFO writes (`GXPosition*`, `GXColor*`, `GXTexCoord*`) → RAM vertex buffer, uploaded as GL VBO.
- TEV stages → generated GLSL fragment shader (cache by TEV config hash).
- **Time saver**: J3D is the dominant draw path. Getting J3D materials correct covers the
  majority of in-game rendering. Focus TEV combiner on J3D's common patterns first.

### Step 6: Audio Bring-Up

Shield's audio architecture tells us exactly what to do:

**Phase A — Stub (unblock gameplay)**:
- `pal_audio` returns silence. Shield already sets `JASDriver::setOutputMode(1)` (fixed mode) —
  we inherit this and route the output to a silent buffer.
- Game loop runs unblocked.

**Phase B — Software playback**:
- Replace DSP/ARAM mixing in `JAudio2` with software mixer → `pal_audio` PCM output.
- Keep `Z2AudioMgr`, `Z2SoundMgr`, `Z2SeqMgr`, `Z2SeMgr` unchanged.
- Shield already loads `Z2Sound.baa`, `Z2SoundSeqs.arc`, `Z2CSRes.arc` — same archives, load via `pal_fs`.
- ADPCM decode in software; MIDI via existing `JASSeqParser`.
- **Time saver**: Phase A is ~2 days. Shield's fixed audio mode means no hardware-query complexity.

### Step 7: Input and Save

**Input** — Shield already solves this:
- Shield uses the **GCN input path** (`JUTGamePad`, port 1), not WPAD/KPAD.
- `m_Do_controller_pad.cpp` creates `JUTGamePad` for `PLATFORM_GCN || PLATFORM_SHIELD`.
- Add `PLATFORM_PC`: wire `JUTGamePad` → `pal_input` → SDL3 gamepad.
- GCN button mapping (A/B/X/Y/Z/L/R/Start + sticks + D-pad) maps naturally to modern gamepads.

**Save** — Shield already simplifies this:
- Shield skips `CARDInit()` and all physical memory card operations.
- Shield uses `NANDSimpleSafeOpen/Close` for file-based saves.
- PC: replace NAND calls with `pal_save` (`<fstream>` to `~/.local/share/` or `%APPDATA%`).
- Keep `mDoMemCd_Ctrl_c` command interface and 0xA94-byte quest-log format.
- **Time saver**: Shield already has the "no physical media" code path. We replace the NAND
  layer underneath, not the save logic above.

### Step 8: Visual Scope for First Playable

- Prioritize: title screen, Ordon Village, Faron Woods, first dungeon.
- Shield's `lingcod_getBloomScale()` / `lingcod_getDoFScale()` → ImGui sliders.
- Use ImGui feature toggles to disable problematic effects at runtime.

### Step 9: NX Homebrew Bring-Up

- Same GX → OpenGL renderer (no changes).
- Swap PAL backends: `pal_window` → libnx/EGL, `pal_input` → HID, `pal_audio` → audren,
  `pal_fs` → romfs, `pal_save` → libnx account/save.
- Define `VERSION_NX_HB = 14` and `#define PLATFORM_NX_HB (VERSION == VERSION_NX_HB)` in `global.h`,
  inheriting the same Shield-derived conditionals as `PLATFORM_PC`.
- **Time saver**: ~1–2 weeks after PC is playable. PAL interface is identical, only backend swaps.

### Step 10: Polish for Shipment

- Validate: title → Ordon → Faron → Forest Temple → save/load cycle.
- Fill GX long-tail only when blocking content (tracked via runtime stub-hit counters).
- Package: PC → executable + `data/`; NX → NRO + romfs.

## Critical Path and Dependencies

```
Step 1 (CMake build from ShieldD file list)
  └─► Step 2 (extend PLATFORM_SHIELD → PLATFORM_PC in 63+ files)
        ├─► Step 3 (PAL bootstrap — window + main loop)
        │     ├─► Step 4 (DVD/ARAM simplification) ─────────────┐
        │     └─► Step 5 Tier A (GX shim — first draw) ────────┤
        │           └─► Step 8 (first playable scene) ◄─────────┘
        │                 └─► Step 10 (polish + package)
        ├─► Step 6A (audio stubs — can start with Step 3) ──► Step 6B (real audio)
        ├─► Step 7 (input + save — can start with Step 3)
        └─► Step 9 (NX bring-up — after PC playable)
```

**Parallelizable work streams**:
1. **Build + conditionals** (Steps 1–2): one person, ~3–4 days.
   *(Shield's static linking eliminates REL loader; conditional extension is mechanical.)*
2. **PAL + DVD/ARAM** (Steps 3–4): one person, ~1.5 weeks.
   *(Shield's NAND error handling and disc-less code path carry over directly.)*
3. **GX shim** (Step 5): one person (GX/GL experience), ~3–4 weeks for Tier A.
   *(Faster — `GX_WRITE_*` macro redirect captures 2,500 call sites; `GXTexObj` opacity trick
   avoids texture plumbing; Shield's NVGX proves the GX API surface supports GPU handle mapping.)*
4. **Audio** (Step 6): one person, Phase A in ~1 day, Phase B ~2–4 weeks.
   *(Faster — Shield's audio routing is pre-mapped; fixed output mode; no hardware queries.)*
5. **Input + Save** (Step 7): one person, ~2–3 days.
   *(Faster — Shield's GCN pad path and NAND save path are the exact patterns we need.)*

**Estimated total time to first playable (with parallel streams)**: ~4–6 weeks
*(Down from ~7–9 weeks without Shield leverage. Key savings: GX macro redirect eliminates
per-callsite work, static linking skips REL loader, Shield conditionals are mechanical.)*

## High-Impact Time Savers (Verified Against Shield Port)

| Technique | Why it works | Measured impact |
|---|---|---|
| Start from ShieldD config (4,028 files) | Battle-tested file list for a non-Nintendo target | No file-list guesswork; build works or fails immediately |
| Extend `PLATFORM_SHIELD` conditionals | 63+ files already have the decision points marked | Mechanical `#if` extension — ~63 files, ~1 line each |
| Inherit Shield's hardware exclusions | `!PLATFORM_SHIELD` guards skip CARDInit, GXSaveCPUFifo, etc. | Same exclusions apply to PC — zero new analysis needed |
| Redirect `GX_WRITE_*` macros | 4 macros in `GXRegs.h` control all FIFO writes | Captures ~2,500 GX call sites with 4 `#define` changes |
| Redefine `GXTexObj` internals | Opaque `u32 dummy[8]` struct hides implementation | Store `GLuint` handle inside — same trick as Shield's `NVGXNameTex` |
| Reuse Shield's GCN input path | Shield uses `JUTGamePad`, not WPAD/KPAD | Map SDL3 gamepad → GCN PadStatus; skip Wii input entirely |
| Reuse Shield's save simplification | Shield uses `NANDSimpleSafe*` instead of memory cards | Replace one NAND layer instead of reimplementing card I/O |
| Inherit Shield's dual-heap architecture | `sRootHeap2` already works on Shield | Both heaps map to host RAM — no heap redesign needed |
| Skip REL dynamic linking | 12 `!PLATFORM_SHIELD` blocks disable `c_dylink` on Shield | Statically link all code — eliminates REL loader entirely |
| Replace `lingcod` stubs with ImGui | `lingcod` functions return constants (bloom=1.0, DoF=1.0) | ImGui sliders give runtime control for free |
| Keep `mDoDvdThd_*` / JKR interfaces | DVD/ARAM consumers never see the change | ~835 actor files untouched |
| Shield's math fixes carry over | `errno` guards in `e_acos/asin/fmod` prevent x86 NaN crashes | Prevents runtime crashes without new debugging |

## Additional Shield-Derived Time Savings (Deep Research)

Beyond the primary conditional patterns, the Shield port reveals additional acceleration
opportunities found during deep codebase analysis:

### 1. Static Linking Eliminates REL Loader (~1 week saved)

Shield disables the dynamic linker (`c_dylink.cpp` has **12 `#if !PLATFORM_SHIELD` blocks**).
On GCN/Wii, actor code lives in REL modules (804 `.rel` files) loaded dynamically by
`c_dylink`. Shield statically links everything — the PC port inherits this.

**Impact**: Eliminates the need to implement or emulate REL loading, relocation, and symbol
resolution. The CMake build compiles all 4,028 source files into a single executable.

### 2. Audio System Pre-Configured (~2 days saved)

Shield's audio path (`Z2AudioLib/`, 19 `PLATFORM_SHIELD` references) reveals:
- Extra sound effect categories 10–11 already enabled (`Z2AudioMgr.cpp`)
- Scene wave loading path optimized for non-disc I/O (`Z2SceneMgr.cpp`)
- SE manager callbacks simplified (`JAISeMgr.cpp` — `#if !PLATFORM_SHIELD` disables reporting)
- Sequence player skips hardware-specific features (`Z2SeqMgr.cpp`)

**Impact**: The audio stub (Phase A) and full implementation (Phase B) can reuse Shield's
simplified audio routing. No need to discover which audio paths depend on hardware — Shield
already marks them.

### 3. Frame Processor Optimized Path (~1 day saved)

`f_pc_manager.cpp` has `#elif PLATFORM_SHIELD && !DEBUG` — Shield uses a streamlined frame
processor that skips debug overhead. We inherit this for release builds and add ImGui hooks
for debug builds separately.

### 4. Actor Management Shortcut (~1 day saved)

`f_op_actor_mng.cpp` has Shield-specific actor management. Combined with static linking
(no REL loading), actor creation becomes a direct function call instead of dynamic symbol
lookup → load → relocate → call.

### 5. Struct Padding Already Portable

Shield-specific struct padding in `m_Re_controller_pad.h`, `d_vibration.h`, and other headers
means the data layout is already tested on a non-GCN memory model. PC inherits these padded
structs with no alignment surprises.

### 6. NAND Error Handling Already Simplified

Shield's save path already handles NAND error states (`NAND_STATE_*` in `m_Do_MemCard.cpp`)
for a non-card-based target. The PC port replaces `NANDSimpleSafe*` with file I/O but keeps
the same error state machine — no redesign needed.

### 7. Texture Object Opacity Enables GPU Handle Injection

The `GXTexObj` struct is intentionally opaque (`typedef struct { u32 dummy[8]; } GXTexObj`)
with internal fields (`__GXTexObjInt`) hidden from public API. Shield's `NVGXNameTex` /
`NVGXDestroyTex` functions exploit this opacity to map GX textures to Tegra GPU handles.
We do the same: redefine `__GXTexObjInt` internals to include a `GLuint` texture ID, keeping
the 32-byte public struct size but storing OpenGL handles inside.

### 8. Display List Replay Already Works Through Translation

Shield replays `GXCallDisplayList` through its translation layer — proving display lists
(used in 10+ files including `J3DShapeDraw`, `J3DPacket`, `d_a_player`) contain standard
GX commands that can be intercepted and re-executed. Our shim records display lists as
command buffers and replays them through the OpenGL backend.

### Summary: Revised Time Estimate With All Shield Optimizations

| Work stream | Previous estimate | With Shield savings | Key savings source |
|---|---|---|---|
| Build + conditionals | ~1 week | ~3–4 days | Static linking eliminates REL loader |
| PAL + DVD/ARAM | ~2 weeks | ~1.5 weeks | Shield's NAND error handling carries over |
| GX shim Tier A | ~3–5 weeks | ~3–4 weeks | `GX_WRITE_*` macro redirect + `GXTexObj` opacity trick |
| Audio | Phase A: ~2 days | Phase A: ~1 day | Audio routing pre-mapped by Shield conditionals |
| Input + Save | ~3–5 days | ~2–3 days | GCN pad path + NAND save path are exact patterns |

**Revised total estimate**: ~4–6 weeks to first playable (down from ~7–9 weeks in original plan).

## Known Heavy Areas (Measured)

| File | Lines | GX calls | Reason to defer |
|---|---|---|---|
| `src/d/d_kankyo_rain.cpp` | 6,353 | ~766 state calls | Extreme GX density — weather/rain effects |
| `src/d/actor/d_a_movie_player.cpp` | 4,215 | ~111 state calls | Video playback + display pipeline |
| `src/d/actor/d_a_mirror.cpp` | 635 | ~73 state calls | Special multi-pass rendering |
| `src/d/d_home_button.cpp` | — | — | Wii-only overlay, not needed on PC/NX |

**Strategy**: Gate these behind ImGui toggles. Implement stubs that skip rendering and log once.
Revisit after the core playable loop is stable.

## Operational Rules During Port

- Never block progress on rare visual edge cases.
- Any unsupported GX function gets: `static bool warned = false; if (!warned) { log(...); warned = true; }` + safe fallback.
- Keep gameplay logic (`src/d/`, `src/d/actor/`, `src/f_*`) untouched unless a blocker demands targeted edits.
- Prefer additive shims over invasive rewrites.
- When in doubt, check what Shield does — it's the closest prior art.
- Track stub-hit counts at runtime to prioritize Tier B GX work by actual frequency.

## Post-Ship Backlog (Intentionally Deferred)

- Exact post-processing parity (bloom, DoF, motion blur — `lingcod` scale values are the starting point).
- Full feature parity for all niche overlays and scenes.
- Wider input remapping UX and keyboard/mouse support.
- Additional graphics backends beyond OpenGL (Vulkan, Metal).
- Deep visual/performance tuning beyond required playability.
- Region selection (PAL/JPN) — default to USA for first ship.
- Wii-specific content or control schemes.
- Shield-specific features (`REGION_CHN`, `Z2AudioCS` Chinese sound resources).
