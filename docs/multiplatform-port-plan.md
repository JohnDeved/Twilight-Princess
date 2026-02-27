# Twilight Princess Multiplatform Port Plan (Shield-Derived, Time-to-Ship Optimized)

## Strategy: Leverage the Shield Port

The Nvidia Shield version of Twilight Princess is already a non-Nintendo platform target that
compiles from the **same source tree**. While the Shield build still targets PowerPC and runs
under a translation layer on the Shield TV hardware, it has **already solved most of the
platform-abstraction problems** we need for a native PC/NX port:

| Problem | Shield's solution | What we reuse |
|---|---|---|
| Platform conditionals | `PLATFORM_SHIELD` in 63+ files, 220+ files use `PLATFORM_*` | Add `PLATFORM_PC` to the same `#if` chains — zero game logic edits |
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

**Bottom line**: Instead of porting from GCN and re-discovering every platform boundary,
we start from the ShieldD (debug) build configuration. The ShieldD config compiles **4,028
source files** — the full game — and its `PLATFORM_SHIELD` conditionals mark exactly where
hardware assumptions live. Our port adds `PLATFORM_PC` alongside `PLATFORM_SHIELD` in most
cases, then replaces the remaining PowerPC/GX hardware layer with native implementations.

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

### Step 5: GX Shim (Replace Shield's Translated GX)

**Scope**: The Shield build compiles all 20 `revolution/gx/*.c` files and runs them through
PowerPC translation. We replace that entire layer with a native GX → OpenGL shim.

#### Tier A — First Playable (title → field → combat)

| GX area | Key functions | Primary consumers |
|---|---|---|
| State init | `GXInit`, `GXSetViewport`, `GXSetScissor` | `m_Do_graphic.cpp` |
| Vertex format | `GXSetVtxDesc`, `GXSetVtxAttrFmt`, `GXClearVtxDesc` | J3D material setup |
| Matrix | `GXLoadPosMtxImm`, `GXLoadNrmMtxImm`, `GXSetCurrentMtx` | J3D draw, actors |
| Draw | `GXBegin`, `GXEnd`, FIFO writes (`GXPosition*`, `GXColor*`, `GXTexCoord*`) | J3D shape draw, 2D UI |
| Texture | `GXInitTexObj`, `GXLoadTexObj`, `GXInitTlutObj`, `GXLoadTlut` | J3D, particle, UI |
| TEV | `GXSetTevOp`, `GXSetTevOrder`, `GXSetNumTevStages`, `GXSetTevColor` | 41+ files — dominant material path |
| Blend/Z | `GXSetBlendMode`, `GXSetZMode`, `GXSetAlphaCompare` | J3D pixel setup |
| Display list | `GXCallDisplayList` | 10 files: `J3DShapeDraw`, `J3DPacket`, `d_a_player`, etc. |

**Note**: Shield already disables `GXSaveCPUFifo()` — our shim doesn't need it either.
Shield also disables PowerPC-specific `J3D_sqrtf()` assembly — we inherit standard `sqrtf()`.

#### Tier B — Expand by Demand

- Remaining `GXSet*` variants (lighting, fog, indirect textures).
- `GXDraw*` geometric primitives, `GXGet*` readback, `GXPerf*` counters.
- Rare TEV/texture filter modes.

**Implementation strategy**:
- `gx_state` struct captures all GX state; flush to OpenGL at draw time.
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
- Add `PLATFORM_NX_HB` macro, inheriting the same Shield-derived conditionals as `PLATFORM_PC`.
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
1. **Build + conditionals** (Steps 1–2): one person, ~1 week.
   *(Faster than before — ShieldD file list eliminates guesswork, conditional extension is mechanical.)*
2. **PAL + DVD/ARAM** (Steps 3–4): one person, ~2 weeks.
   *(Faster — Shield already proves these subsystems work without real hardware.)*
3. **GX shim** (Step 5): one person (GX/GL experience), ~3–5 weeks for Tier A.
   *(Same scope — Shield didn't help here since it ran GX through translation.)*
4. **Audio** (Step 6): one person, Phase A in ~2 days, Phase B ~2–4 weeks.
   *(Faster — Shield's fixed output mode and known archive list reduce exploration.)*
5. **Input + Save** (Step 7): one person, ~3–5 days.
   *(Faster — Shield's GCN pad path and NAND save path are the exact patterns we need.)*

**Estimated total time to first playable (with parallel streams)**: ~5–7 weeks
*(Down from ~7–9 weeks without Shield leverage — saves ~2 weeks of exploration and dead ends.)*

## High-Impact Time Savers (Verified Against Shield Port)

| Technique | Why it works | Measured impact |
|---|---|---|
| Start from ShieldD config (4,028 files) | Battle-tested file list for a non-Nintendo target | No file-list guesswork; build works or fails immediately |
| Extend `PLATFORM_SHIELD` conditionals | 63+ files already have the decision points marked | Mechanical `#if` extension — ~63 files, ~1 line each |
| Inherit Shield's hardware exclusions | `!PLATFORM_SHIELD` guards skip CARDInit, GXSaveCPUFifo, etc. | Same exclusions apply to PC — zero new analysis needed |
| Reuse Shield's GCN input path | Shield uses `JUTGamePad`, not WPAD/KPAD | Map SDL3 gamepad → GCN PadStatus; skip Wii input entirely |
| Reuse Shield's save simplification | Shield uses `NANDSimpleSafe*` instead of memory cards | Replace one NAND layer instead of reimplementing card I/O |
| Inherit Shield's dual-heap architecture | `sRootHeap2` already works on Shield | Both heaps map to host RAM — no heap redesign needed |
| Replace `lingcod` stubs with ImGui | `lingcod` functions return constants (bloom=1.0, DoF=1.0) | ImGui sliders give runtime control for free |
| Keep `mDoDvdThd_*` / JKR interfaces | DVD/ARAM consumers never see the change | ~835 actor files untouched |
| Shield's math fixes carry over | `errno` guards in `e_acos/asin/fmod` prevent x86 NaN crashes | Prevents runtime crashes without new debugging |

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
