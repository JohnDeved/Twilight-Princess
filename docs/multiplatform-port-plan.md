# Twilight Princess Multiplatform Port Plan (GCN-First, Time-to-Ship Optimized)

## Scope

- Canonical game behavior/content: **GameCube version**.
- Target platforms: **Windows, Linux, macOS, NX Switch (homebrew)**.
- Primary rendering backend for first ship: **OpenGL**.
- Tooling direction: modern C++ build with **CMake** (replaces the current Ninja/configure.py decomp toolchain).
- Development tooling: **ImGui** integrated from the start.

## Codebase Snapshot (Measured)

| Area | Files | Notes |
|---|---|---|
| `src/d/` (game logic) | ~159 .cpp | Core game systems (camera, save, stage, etc.) |
| `src/d/actor/` | ~835 .cpp | All actors — largest directory by far |
| `src/f_pc/` (process framework) | 32 .cpp | Generic process management |
| `src/f_op/` (operation framework) | 23 .cpp | Actor/scene/camera operations |
| `src/f_ap/` (application) | 1 .cpp | Top-level game mode |
| `src/m_Do/` (machine layer) | 17 .cpp | **Primary hardware abstraction — key porting surface** |
| `src/JSystem/` | ~311 .cpp | J3D rendering, JKernel, JAudio2, J2D, JParticle |
| `src/dolphin/` | ~180 .cpp/.c | GameCube SDK (DVD, OS, GX, PAD, DSP, etc.) |
| `src/revolution/` | ~192 .cpp/.c | Wii SDK (WPAD, KPAD, homebuttonLib, etc.) |
| `src/Z2AudioLib/` | 23 .cpp | TP-specific audio (wraps JAudio2) |
| `src/SSystem/` | 40 .cpp | Math, collision, component utilities |
| GX headers | 21 in `include/dolphin/gx/`, 21 in `include/revolution/gx/` | Identical API surface for GCN and Wii |

**Total GX function call sites across `src/`**: ~2,500+ (dominated by `GXSet*`, `GXInit*`, `GXLoad*`, FIFO writes).

## Platform Matrix

| Platform | Graphics | Platform Layer |
|---|---|---|
| Windows | OpenGL 4.5 | SDL3 |
| Linux | OpenGL 4.5 | SDL3 |
| macOS | OpenGL 4.1 | SDL3 |
| NX Homebrew | OpenGL 4.3 (Mesa/Nouveau) | libnx + EGL |

## Core Principles

- **GCN-first semantics**: preserve GameCube gameplay feel, camera behavior, and content assumptions.
- **One renderer first**: OpenGL everywhere for initial ship.
- **Boundary-only porting**: convert at PAL and GX boundaries; avoid rewriting gameplay systems.
- **Fail-soft, not fail-hard**: unknown GX paths render with safe defaults + log once.
- **Playable-first**: prioritize title → field → combat → menu/save flow over perfect visual parity.

## What We Intentionally Skip Early

- Wii-specific systems (WPAD/KPAD in 45+ files, Wii speaker, `src/revolution/` SDK paths).
- Exact parity for heavy post-processing (DoF/bloom/motion blur) in first playable.
- Mirror-heavy and movie paths in first pass (see measured heavy areas below).
- Full async disc emulation (threaded DVD behavior via `mDoDvdThd_*`).
- Full GX API upfront (~2,500 call sites; implement hot path first, long tail on demand).
- Broad settings UI and advanced remapping in v1 (basic stable mapping first).
- Shield-specific code paths (`src/lingcod/`, `PLATFORM_SHIELD` branches).

## Architecture

```
Game Logic (src/d/, src/d/actor/)       ~994 files — DO NOT TOUCH unless blocking
  → Framework (src/f_pc/, src/f_op/, src/f_ap/)   56 files — DO NOT TOUCH
    → Machine Layer (src/m_Do/)          17 files — THIN ADAPTATION LAYER
      → PAL (src/pal/)                   NEW — platform services + GX shim
         → GX Shim (src/pal/gx/)        NEW — GX → OpenGL translation
         → Platform (src/pal/platform/)  NEW — SDL3 / libnx backends
```

**Key insight — `src/m_Do/` is already the hardware abstraction layer.** Files like
`m_Do_graphic.cpp`, `m_Do_controller_pad.cpp`, `m_Do_audio.cpp`, `m_Do_MemCard.cpp`,
`m_Do_dvd_thread.cpp`, and `m_Do_machine.cpp` sit between gameplay and hardware.
The PAL layer slots in directly beneath `m_Do/`, keeping changes above it minimal.

## Existing Conditional Compilation System (Major Time Saver)

The codebase already has a mature version/platform conditional system in `include/global.h`:

```c
#define PLATFORM_GCN    (VERSION >= VERSION_GCN_USA && VERSION <= VERSION_GCN_JPN)
#define PLATFORM_WII    (VERSION >= VERSION_WII_USA_R0 && VERSION <= VERSION_WII_PAL_KIOSK)
#define PLATFORM_SHIELD (VERSION >= VERSION_SHIELD && VERSION <= VERSION_SHIELD_DEBUG)
```

**~220+ files** already use `PLATFORM_*` conditionals. The `PLATFORM_SHIELD` path (63+ files)
is a precedent for a non-Nintendo target:
- `PLATFORM_WII || PLATFORM_SHIELD` blocks often share code, showing how a PC target can
  piggyback on Wii paths where hardware semantics align (widescreen, framebuffer setup).
- Shield-specific tweaks (sampling buffer sizes, save system via `lingcod`) demonstrate
  the pattern for platform-specific overrides.

**Action**: Add `PLATFORM_PC` and (later) `PLATFORM_NX_HB` macros. Many conditionals become
`#if PLATFORM_GCN || PLATFORM_PC` or fall through to platform-agnostic defaults.
This avoids mass-editing — new platform code is additive.

## Separation of Concerns

| Layer | Directories | Rules |
|---|---|---|
| Game logic | `src/d/`, `src/d/actor/`, `src/f_*` | No platform headers, no OpenGL. Compile unchanged. |
| Machine layer | `src/m_Do/` | Calls PAL interfaces. Thin edits to swap HW calls → PAL calls. |
| GX shim | `src/pal/gx/` | Owns GX → OpenGL translation and render state. |
| Platform services | `src/pal/platform/` | SDL3 / libnx backends for window, input, audio, fs, save. |
| Debug UI | `src/pal/imgui/` | ImGui overlays; no gameplay ownership. |

## Step-by-Step Execution Plan

### Step 1: Build Modernization

**Goal**: Compile the GCN-canonical C++ source with a modern host compiler.

- Introduce root `CMakeLists.txt` targeting the host platform (not PowerPC cross-compile).
- The current build (`configure.py` → Ninja → MWCC cross-compiler) stays for decomp validation;
  the CMake build is a **parallel** target for the port.
- Exclude `src/dolphin/`, `src/revolution/`, `src/PowerPC_EABI_Support/`, `src/TRK_MINNOW_DOLPHIN/`,
  `src/NdevExi2A/`, `src/odemuexi2/`, `src/odenotstub/`, `src/lingcod/` from the port build
  (these are SDK/hardware libraries replaced by PAL).
- Convert MWCC constructs that block compilation:
  - `#pragma pack` → `__attribute__((packed))` or already portable (check `include/types.h`).
  - `__declspec(weak)` → `__attribute__((weak))` (already handled by `DECL_WEAK` macro in `types.h`).
  - MWCC intrinsics (`__cntlzw`, `__dcbf`, `__sync` in `global.h`) → GCC/Clang builtins or no-ops.
- **Time saver**: `DECL_WEAK`, `ATTRIBUTE_ALIGN`, `NO_INLINE` macros in `types.h`/`global.h`
  already abstract most compiler differences. Only MWCC intrinsics need new shims.

### Step 2: Flatten Version Branching Toward GCN Behavior

**Goal**: Resolve the ~220+ conditional-compilation sites to GCN-canonical behavior.

- Define `VERSION = VERSION_GCN_USA` (or a new `VERSION_PC` that makes `PLATFORM_PC` true and
  `PLATFORM_GCN`-like for gameplay defaults).
- For `#if PLATFORM_WII`-only blocks: exclude (dead code for the port).
- For `#if PLATFORM_GCN`-only blocks: keep as-is (already canonical).
- For `#if PLATFORM_WII || PLATFORM_SHIELD` blocks: evaluate case-by-case; some (widescreen,
  framebuffer management) may be useful under `PLATFORM_PC`.
- For `REGION_*` blocks: default to `REGION_USA`; region selection is a post-ship feature.
- **Time saver**: Don't mass-delete conditionals. Add `PLATFORM_PC` to relevant `#if` chains
  and let the preprocessor do the work. Keeps the tree mergeable with upstream decomp.

### Step 3: PAL Bootstrap (Minimal Surface)

**Goal**: Boot to a window with main loop and ImGui overlay — no rendering yet.

Create `src/pal/` with this minimal surface:

| PAL module | Wraps | PC backend | NX backend |
|---|---|---|---|
| `pal_window` | Window + GL context | SDL3 | libnx + EGL |
| `pal_os` | Timers, sleep, assertions | SDL3 / `<chrono>` | libnx |
| `pal_fs` | File I/O, path resolution | `<filesystem>` | libnx romfs |
| `pal_input` | Gamepad + keyboard | SDL3 gamepad | libnx HID |
| `pal_audio` | PCM output stream | SDL3 audio | libnx audren |
| `pal_save` | Save file read/write | `<fstream>` | libnx account/save |

**Concrete m_Do touch points** (files that call into hardware and need PAL wiring):

| m_Do file | Hardware dependency | PAL replacement |
|---|---|---|
| `m_Do_main.cpp` | `JKRAram`, OS init, heap setup | `pal_os`, RAM-only heaps |
| `m_Do_graphic.cpp` | GX init, framebuffer, video mode | `pal_window` + GX shim |
| `m_Do_controller_pad.cpp` | `JUTGamePad` → PAD SDK | `pal_input` |
| `m_Do_audio.cpp` | `JKRSolidHeap`, DVD thread for audio archives | `pal_audio` + `pal_fs` |
| `m_Do_MemCard.cpp` | Memory card thread, mutex/condvar | `pal_save` |
| `m_Do_dvd_thread.cpp` | DVD async command queue | `pal_fs` (sync read) |
| `m_Do_machine.cpp` | Exception handler, video mode | `pal_os` |
| `m_Do_DVDError.cpp` | DVD error overlay | Remove or stub |

### Step 4: Replace DVD/Disc I/O With Direct File Access

**Goal**: Eliminate hardware DVD threading without changing any caller.

- `mDoDvdThd_command_c` and subclasses (`mDoDvdThd_mountArchive_c`,
  `mDoDvdThd_mountXArchive_c`, `mDoDvdThd_toMainRam_c`) — keep the class interfaces.
- Replace `create(...)` internals: read file from `pal_fs`, copy to heap, mark done immediately.
- `sync()` returns true on first call (already done).
- Callers in `src/d/`, `src/JSystem/JKernel/` continue to allocate commands and poll — no edits.
- **Time saver**: This is a pure internal swap — zero callsite changes across 835+ actor files.

### Step 5: ARAM-to-RAM Collapse

**Goal**: Eliminate ARAM DMA without changing JKR/JAudio mount/load contracts.

Affected files (from `include/JSystem/JKernel/`):
- `JKRAram.h`, `JKRAramArchive.h`, `JKRAramHeap.h`, `JKRAramPiece.h`,
  `JKRAramStream.h`, `JKRAramBlock.h`, `JKRDvdAramRipper.h`

Action:
- `JKRAram::aramToMainRam()` / `mainRamToAram()` → `memcpy` with matching signatures.
- `JKRAramArchive` → backed by a `JKRMemArchive` internally.
- `JKRAramHeap` / `JKRAramBlock` → thin wrappers around host `malloc`.
- Audio ARAM streams (`JASAramStream.h`, `JAUStreamAramMgr.h`) → RAM ring buffer.
- **Time saver**: Keep every public method signature; only gut the DMA internals.

### Step 6: GX Shim Bring-Up in Priority Tiers

**Scope**: ~2,500 GX call sites, 21 GX headers, ~50+ distinct GX functions.

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

#### Tier B — Expand by Demand

- Remaining `GXSet*` variants (lighting, fog, indirect textures, line/point width).
- `GXDraw*` geometric primitives (`GXDrawCylinder`, `GXDrawSphere`, etc.).
- `GXGet*` readback functions.
- `GXPerf*` performance counters (stub or remove).
- Rare TEV/texture filter modes encountered only in edge scenes.

**Implementation strategy**:
- Single `gx_state` struct tracks all GX state; flush to OpenGL at draw time.
- Vertex data captured in RAM buffer during `GXBegin`/`GXEnd` or display list replay;
  uploaded as GL VBO on flush.
- TEV stages → generated GLSL fragment shader (cache by TEV config hash).
- **Time saver**: J3D is the dominant draw path (~311 JSystem files). Getting J3D materials
  correct covers the vast majority of in-game rendering. Implement the TEV combiner for
  J3D's common patterns first; exotic TEV setups can fall back to a solid-color shader.

### Step 7: Visual Scope for First Playable

- Prioritize correctness in: title screen, Ordon Village, Faron Woods, first dungeon.
- Defer exact parity in expensive edge paths (see heavy areas below).
- Use ImGui feature toggles to disable problematic effects at runtime.

### Step 8: Audio Bring-Up

**Phase A — Stub (unblock gameplay)**:
- `pal_audio` returns silence; all `Z2AudioLib` (23 files) and `JAudio2` (92 files) calls succeed but produce no output.
- Game loop runs at full speed because audio never blocks.

**Phase B — Software playback**:
- Replace DSP/ARAM-coupled `JAudio2` mixing with a software mixer writing PCM to `pal_audio`.
- Keep `Z2AudioMgr`, `Z2SoundMgr`, `Z2SeqMgr`, `Z2SeMgr` wrappers unchanged —
  they call into JAudio2, which calls into the new software backend.
- ADPCM decode in software (DSPADPCM format is well-documented).
- MIDI sequence playback via the existing `JASSeqParser` with software instrument bank.
- **Time saver**: Phase A costs almost nothing and lets the entire gameplay loop ship without
  audio blocking. Phase B can be incremental — add sound effects first, then music, then streaming.

### Step 9: Input and Save Stability

**Input** (touch point: `m_Do_controller_pad.cpp` → `JUTGamePad` → `PAD` SDK):
- `pal_input` provides a `PadStatus`-compatible struct from SDL3 gamepad or libnx HID.
- `mDoCPd_c::read()` calls `pal_input` instead of `PADRead()`.
- Start with one GCN-style mapping (A/B/X/Y/Z/L/R/Start + analog stick + C-stick + D-pad).
- Keyboard/mouse: post-ship (add as second `pal_input` backend).

**Save** (touch point: `m_Do_MemCard.cpp`):
- Replace memory card thread + mutex/condvar machinery with synchronous `pal_save` file I/O.
- Keep `mDoMemCd_Ctrl_c` command interface (Restore/Store/Format) and 0xA94-byte quest-log format.
- Platform-specific save directory via `pal_save` (`~/.local/share/` on Linux, `%APPDATA%` on Windows, etc.).

### Step 10: NX Homebrew Bring-Up

- Same GX → OpenGL renderer (no changes).
- Swap PAL backends: `pal_window` → libnx/EGL, `pal_input` → HID, `pal_audio` → audren,
  `pal_fs` → romfs, `pal_save` → libnx account/save.
- Known Mesa/Nouveau driver quirks → local workarounds in `src/pal/gx/` (not renderer redesign).
- **Time saver**: Because PAL is a thin interface, NX bring-up is primarily a backend swap,
  not a new port. Budget ~1–2 weeks after PC is playable.

### Step 11: Polish for Shipment

- Validate core progression: title → Ordon → Faron → Forest Temple → save/load cycle.
- Fill GX long-tail only when blocking content (tracked via runtime stub-hit counters).
- Package: PC → single executable + `data/` folder; NX → NRO + romfs.
- Defer advanced visual parity and non-critical systems to post-ship.

## Critical Path and Dependencies

```
Step 1 (CMake build compiles)
  └─► Step 2 (GCN-canonical source compiles on host)
        ├─► Step 3 (PAL bootstrap — window + main loop)
        │     ├─► Step 4 (DVD → pal_fs) ─────────────────────┐
        │     ├─► Step 5 (ARAM → RAM) ───────────────────────┤
        │     └─► Step 6 Tier A (GX shim — first draw) ─────┤
        │           └─► Step 7 (first playable scene) ◄──────┘
        │                 └─► Step 11 (polish + package)
        ├─► Step 8A (audio stubs — can start with Step 3) ──► Step 8B (real audio)
        ├─► Step 9 (input + save — can start with Step 3)
        └─► Step 10 (NX bring-up — after PC playable)
```

**Parallelizable work streams** (assign to separate contributors):
1. **Build + conditionals** (Steps 1–2): one person, ~1–2 weeks.
2. **PAL + DVD + ARAM** (Steps 3–5): one person, ~2–3 weeks.
3. **GX shim** (Step 6): one person (ideally with GX/GL experience), ~3–5 weeks for Tier A.
4. **Audio** (Step 8): one person, Phase A in ~2 days, Phase B ~2–4 weeks.
5. **Input + Save** (Step 9): one person, ~1 week.

## High-Impact Time Savers (Verified Against Codebase)

| Technique | Why it works | Measured impact |
|---|---|---|
| Extend `PLATFORM_*` conditionals instead of mass-editing | 220+ files already use the system; additive `PLATFORM_PC` avoids churn | Zero callsite edits in game logic |
| Keep `mDoDvdThd_*` / JKR interfaces, replace internals | DVD/ARAM consumers never see the change | ~835 actor files untouched |
| Implement J3D material path first in GX shim | J3D is the dominant draw path (311 JSystem files) | Covers majority of in-game rendering |
| Stub audio (Phase A) before real playback | Game loop never blocks on audio | Playable build weeks earlier |
| `m_Do/` is already the hardware boundary | Only 17 files sit between gameplay and hardware | PAL wiring is surgical, not systemic |
| `types.h` / `global.h` already abstract compiler differences | `DECL_WEAK`, `ATTRIBUTE_ALIGN`, `NO_INLINE` macros exist | MWCC → GCC/Clang friction is minimal |
| Use `PLATFORM_SHIELD` patterns as reference | 63+ files show how a non-Nintendo target was integrated | Copy-paste patterns for `PLATFORM_PC` |

## Known Heavy Areas (Measured)

| File | Lines | GX calls | Reason to defer |
|---|---|---|---|
| `src/d/d_kankyo_rain.cpp` | 6,353 | ~766 | Extreme GX density — weather/rain effects |
| `src/d/actor/d_a_movie_player.cpp` | 4,215 | ~111 | Video playback + display pipeline |
| `src/d/actor/d_a_mirror.cpp` | 635 | ~73 | Special multi-pass rendering |
| `src/d/d_home_button.cpp` | — | — | Wii-only overlay, not needed on PC/NX |

**Strategy**: Gate these behind ImGui toggles. Implement stubs that skip rendering and log once.
Revisit after the core playable loop is stable.

## Operational Rules During Port

- Never block progress on rare visual edge cases.
- Any unsupported GX function gets: `static bool warned = false; if (!warned) { log(...); warned = true; }` + safe fallback.
- Keep gameplay logic (`src/d/`, `src/d/actor/`, `src/f_*`) untouched unless a blocker demands targeted edits.
- Prefer additive shims over invasive rewrites.
- Keep first ship focused on stable gameplay over perfect parity.
- Track stub-hit counts at runtime to prioritize Tier B GX work by actual frequency.

## Post-Ship Backlog (Intentionally Deferred)

- Exact post-processing parity (bloom, DoF, motion blur).
- Full feature parity for all niche overlays and scenes.
- Wider input remapping UX and keyboard/mouse support.
- Additional graphics backends beyond OpenGL (Vulkan, Metal).
- Deep visual/performance tuning beyond required playability.
- Region selection (PAL/JPN) — default to USA for first ship.
- Wii-specific content or control schemes.
