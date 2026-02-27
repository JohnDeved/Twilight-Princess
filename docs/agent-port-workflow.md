# Agent Port Workflow

Step-by-step guide for AI agents implementing the Twilight Princess PC port.
Read this alongside the [port plan](multiplatform-port-plan.md) and the
[testing plan](ai-agent-testing-plan.md).

## Prerequisites

Before starting any step, the agent should be familiar with:

- `include/global.h` — platform macros (`PLATFORM_GCN`, `PLATFORM_WII`, `PLATFORM_SHIELD`)
- `config/ShieldD/splits.txt` — the 4,028-file list for a non-Nintendo build
- `src/m_Do/m_Do_main.cpp` — game entry point and main loop
- `include/revolution/private/GXRegs.h` — the 4 `GX_WRITE_*` macros to redirect

## Step 1 — CMake Build System (~250 LOC)

**Goal**: Get the codebase compiling on a host x86/ARM compiler.

### What to create

1. **`CMakeLists.txt`** (root) — top-level CMake project
2. **`src/pal/CMakeLists.txt`** — PAL module library

### Source file list

Parse `config/ShieldD/splits.txt` to extract source file paths. Exclude:
- `src/dolphin/**` — replaced by PAL modules
- `src/revolution/**` — replaced by PAL + GX shim
- `src/lingcod/**` — replaced by ImGui integration
- Any PPC assembly files (`.s`, `.S`)

### Platform defines

Add to `include/global.h` after line 17:

```c
#define VERSION_PC           13
#define VERSION_NX_HB        14

// Update platform macros (after existing PLATFORM_SHIELD line):
#define PLATFORM_PC     (VERSION == VERSION_PC)
#define PLATFORM_NX_HB  (VERSION == VERSION_NX_HB)
```

### Compiler intrinsic replacements

Create `include/pal/pal_intrinsics.h`:

```c
#pragma once

#if PLATFORM_PC || PLATFORM_NX_HB

// MWCC → GCC/Clang equivalents (19 occurrences outside SDK)
#define __cntlzw(x)  __builtin_clz(x)
#define __dcbf(a, b) ((void)0)
#define __sync()     ((void)0)

// Cache coherency — no-ops on x86/ARM (134 calls)
#define DCStoreRange(addr, len)       ((void)0)
#define DCFlushRange(addr, len)       ((void)0)
#define DCStoreRangeNoSync(addr, len) ((void)0)
#define ICInvalidateRange(addr, len)  ((void)0)

#endif
```

### Milestone instrumentation

Include the milestone system from the start. Create `include/pal/pal_milestone.h`
(see [testing plan §2](ai-agent-testing-plan.md#2-boot-milestone-instrumentation-200-loc)).

### Validation

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC
ninja -C build 2>&1 | head -50  # expect compiler errors from missing PAL stubs
```

**Expected CI milestone after this step**: `-1` (build errors — not yet linking).

## Step 2 — Extend Shield Conditionals (~100 LOC changes)

**Goal**: Add `PLATFORM_PC` to existing Shield conditional blocks.

### Pattern 1: Shared non-GCN path
```c
// Before:
#if PLATFORM_WII || PLATFORM_SHIELD
// After:
#if PLATFORM_WII || PLATFORM_SHIELD || PLATFORM_PC
```

### Pattern 2: Shield exclusion
```c
// Before:
#if !PLATFORM_SHIELD
// After:
#if !PLATFORM_SHIELD && !PLATFORM_PC
```

### How to find all sites

```bash
grep -rn "PLATFORM_SHIELD" src/ include/ --include="*.cpp" --include="*.h" --include="*.c"
```

This returns ~85 conditional blocks across ~42 files. Each change is 1-2 lines —
mechanical, no logic changes.

### Validation

After extending conditionals, rebuild. The error count should drop significantly.
Files that previously errored on Shield-only paths should now compile.

**Expected CI milestone after this step**: `-1` or `0` (may compile but crash immediately
without PAL implementations).

## Step 3 — PAL Bootstrap (~1,250 LOC)

**Goal**: Implement the PAL modules that replace the Dolphin/Revolution SDK.

Create these files in `src/pal/` and `include/pal/`:

| Module | File | What it replaces | LOC |
|---|---|---|---|
| Window | `pal_window.cpp` | `dolphin/vi` — SDL3 window + bgfx init | ~150 |
| OS | `pal_os.cpp` | `dolphin/os` — timers, threads | ~200 |
| Filesystem | `pal_fs.cpp` | `dolphin/dvd` + NAND — file I/O | ~300 |
| Input | `pal_input.cpp` | `dolphin/pad` — SDL3 gamepad → JUTGamePad | ~200 |
| Audio | `pal_audio.cpp` | `dolphin/dsp` + `dolphin/ai` — SDL3 audio | ~250 |
| Save | `pal_save.cpp` | NANDSimpleSafe* — fstream save/load | ~150 |

### Implementation order

1. `pal_os` — needed first (timers, memory)
2. `pal_fs` — DVD/archive loading
3. `pal_window` — creates SDL3 window, inits bgfx
4. `pal_input` — gamepad input
5. `pal_audio` — start with silence stubs (Phase A)
6. `pal_save` — file-based saves

### Headless mode

`pal_window.cpp` must support `TP_HEADLESS=1`:

```c
bgfx::Init init;
if (getenv("TP_HEADLESS")) {
    init.type = bgfx::RendererType::Noop;
} else {
    init.type = bgfx::RendererType::Count; // auto-detect
}
```

### Validation

```bash
TP_HEADLESS=1 TP_TEST_FRAMES=10 ./build/tp-pc
# Should print milestone JSON for BOOT_START, HEAP_INIT, GFX_INIT, PAD_INIT
```

**Expected CI milestone after this step**: `4` (FRAMEWORK_INIT).

## Step 4 — DVD/ARAM Simplification (~200 LOC)

**Goal**: Replace hardware-specific DVD and ARAM with host equivalents.

### DVD

In `mDoDvdThd_command_c::create()` (or via PAL shim):
- Replace async DVD reads with sync `pal_fs_read()`
- Mark command as done immediately
- 835+ actor files that call `dComIfG_getObjectRes()` remain untouched

### ARAM

- `aramToMainRam` / `mainRamToAram` → `memcpy`
- JKRAram* (1,394 LOC) → thin wrappers around `malloc`/`free`

### Validation

The game should now get past asset loading. Milestones should advance past
FRAMEWORK_INIT to FIRST_FRAME.

**Expected CI milestone after this step**: `5` (FIRST_FRAME).

## Step 5 — GX Shim Tier A (~5,000 LOC)

**Goal**: Implement the core GX → bgfx translation layer.

This is the largest step. Break it into sub-tasks:

### 5a. GX_WRITE_* macro redirect (~100 LOC)

In `include/revolution/private/GXRegs.h`:

```c
#if PLATFORM_PC
  #define GX_WRITE_U8(ub)   gx_fifo_write_u8(ub)
  #define GX_WRITE_U16(us)  gx_fifo_write_u16(us)
  #define GX_WRITE_U32(ui)  gx_fifo_write_u32(ui)
  #define GX_WRITE_F32(f)   gx_fifo_write_f32(f)
#endif
```

### 5b. GX state machine (~2,500 LOC)

Create `src/pal/gx/gx_state.cpp`:
- `gx_state` struct captures all GX state (blend mode, depth mode, cull mode, etc.)
- Flush to bgfx at draw time via `bgfx::submit()`
- Implement the GX → bgfx mapping table from the port plan

### 5c. TEV → bgfx shader generator (~1,500 LOC)

Create `src/pal/gx/gx_tev.cpp`:
- Convert TEV combiner configurations to bgfx shader programs
- Cache shaders by TEV config hash
- Only 5 preset modes used in game code — start with those

### 5d. Texture decode (~1,000 LOC)

Create `src/pal/gx/gx_texture.cpp`:
- Decode 10 GX tile formats to linear RGBA8 for bgfx
- ~100 LOC per format
- Reference: Dolphin's `TextureDecoder` (open source)

### 5e. Display list record/replay (~400 LOC)

Create `src/pal/gx/gx_displaylist.cpp`:
- Record GX FIFO commands to RAM buffer
- Replay through bgfx encoder on `GXCallDisplayList`

### Stub tracking

For any GX function not yet implemented, use the stub tracker:

```c
void GXSetFog(/* ... */) {
    gx_stub_hit(GX_STUB_SET_FOG, "GXSetFog");
    // safe no-op fallback
}
```

### Validation

```bash
TP_HEADLESS=1 TP_TEST_FRAMES=300 ./build/tp-pc
# Check milestone-summary.json for LOGO_SCENE (6) or higher
# Check stub hit report for next functions to implement
```

**Expected CI milestone after this step**: `6-8` (LOGO_SCENE through PLAY_SCENE).

## Step 6 — Audio (~100 LOC Phase A)

**Goal**: Unblock the game loop by providing silent audio.

### Phase A — Silence stubs

`pal_audio.cpp` returns silence buffers. The game loop runs without blocking
on DSP/ARAM audio mixing.

### Phase B — Software mixer (~800 LOC, later)

Replace JAudio2's DSP/ARAM mixing with software mixer → SDL3 PCM output.

**Expected CI milestone after this step**: No change (audio doesn't affect milestones).

## Step 7 — Input + Save (~350 LOC)

**Goal**: Wire real gamepad input and save/load.

### Input

Wire `JUTGamePad` → `pal_input` → SDL3 gamepad. Shield already uses the GCN
pad path (not WPAD), so the mapping is straightforward.

### Save

Replace NAND calls with `pal_save` file I/O. Keep the quest-log data format
unchanged.

**Expected CI milestone after this step**: `10+` (FRAMES_60 — sustained execution
with input working).

## Step 8 — First Playable

**Target**: Title → Ordon → Faron → Forest Temple → save/load cycle.

- Add ImGui toggles for problematic effects (~300 LOC)
- Gate heavy rendering files behind toggles
- Focus on stability over visual fidelity

**Expected CI milestone**: `12` (FRAMES_1800 — 30 seconds sustained gameplay).

## CI-Driven Development Loop

After each step, the agent should:

1. Push changes to the PR
2. Wait for CI to run `.github/workflows/port-test.yml`
3. Read `milestone-summary.json` from CI artifacts
4. Check the decision table in [testing plan §8](ai-agent-testing-plan.md#8-ai-agent-workflow):

| Highest milestone | Action |
|---|---|
| -1 (no output) | Fix build errors |
| 0 BOOT_START | Fix heap init (`mDoMch_Create`) |
| 1 HEAP_INIT | Fix GFX init (`mDoGph_Create`) |
| 2 GFX_INIT | Fix PAL bootstrap |
| 3-4 PAD/FRAMEWORK | Fix process manager setup |
| 5 FIRST_FRAME | Fix DVD/archive loading |
| 6 LOGO_SCENE | Fix scene transition |
| 7 TITLE_SCENE | Fix stage loading |
| 8 PLAY_SCENE | Implement GX stubs (by frequency) |
| 10+ FRAMES_60+ | Fix top stub hits from coverage report |

5. Implement the fix indicated by the decision table
6. Repeat from step 1

## File Organization

```
src/pal/
  ├── CMakeLists.txt
  ├── pal_window.cpp      # SDL3 window + bgfx init
  ├── pal_os.cpp           # timers, threads, memory
  ├── pal_fs.cpp           # file I/O (replaces DVD/NAND)
  ├── pal_input.cpp        # SDL3 gamepad → JUTGamePad
  ├── pal_audio.cpp        # SDL3 audio (Phase A: silence)
  ├── pal_save.cpp         # file-based save/load
  ├── pal_crash.cpp        # signal handler + backtrace
  └── gx/
      ├── gx_state.cpp     # GX state machine → bgfx flush
      ├── gx_tev.cpp       # TEV → bgfx shader generator
      ├── gx_texture.cpp   # GX tile decode → linear RGBA8
      └── gx_displaylist.cpp  # display list record/replay

include/pal/
  ├── pal_window.h
  ├── pal_os.h
  ├── pal_fs.h
  ├── pal_input.h
  ├── pal_audio.h
  ├── pal_save.h
  ├── pal_milestone.h     # boot milestone logging
  ├── pal_intrinsics.h    # MWCC → GCC/Clang intrinsics
  └── gx/
      ├── gx_state.h
      ├── gx_tev.h
      ├── gx_texture.h
      └── gx_stub_tracker.h

tools/
  └── parse_milestones.py  # CI milestone parser
```

## Rules

- **Never modify game logic** without `#if PLATFORM_PC` guards
- **Additive only** — new code in `src/pal/`, shims in `include/pal/`
- **Stub-first** — implement stubs that log + return safe defaults, then fill in
- **Frequency-driven** — implement the most-called stubs first (check CI reports)
- **Headless always works** — every commit must pass `TP_HEADLESS=1` mode
- **When in doubt, check Shield** — `grep -rn PLATFORM_SHIELD` shows the patterns to follow
