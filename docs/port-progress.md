# Port Progress Tracker

> **For AI Agents**: Read this file at the start of every session. Update it before ending
> your session. This is the single source of truth for what has been done and what to do next.

## Current Status

| Field | Value |
|---|---|
| **Highest CI Milestone** | `-1` (not yet building) |
| **Current Step** | Step 1 — Not started |
| **Last Updated** | 2026-02-27 |
| **Blocking Issue** | No port code exists yet — start at Step 1 |

## Step Checklist

Each step maps to the [Execution Plan](multiplatform-port-plan.md#execution-plan) and the
[Agent Port Workflow](agent-port-workflow.md). Mark items `[x]` when complete.

### Step 1 — CMake Build System (~250 LOC)
- [ ] Create root `CMakeLists.txt` with sources from `config/ShieldD/splits.txt`
- [ ] Exclude `src/dolphin/`, `src/revolution/`, `src/lingcod/`, PPC assembly
- [ ] Add `VERSION_PC = 13` and `PLATFORM_PC` to `include/global.h`
- [ ] Create `include/pal/pal_intrinsics.h` (MWCC → GCC/Clang, cache no-ops)
- [ ] Create `include/pal/pal_milestone.h` (boot milestone logging)
- [ ] Verify: `cmake -B build && cmake --build build` produces compiler errors (expected — missing PAL stubs)

### Step 2 — Extend Shield Conditionals (~100 LOC changes)
- [ ] `PLATFORM_WII || PLATFORM_SHIELD` → add `|| PLATFORM_PC` (~85 sites across ~42 files)
- [ ] `!PLATFORM_SHIELD` → `!PLATFORM_SHIELD && !PLATFORM_PC`
- [ ] Verify: rebuild shows fewer errors after conditional extension

### Step 3 — PAL Bootstrap (~1,250 LOC)
- [ ] `pal_os.cpp` — timers, threads, memory (~200 LOC)
- [ ] `pal_fs.cpp` — file I/O replacing DVD/NAND (~300 LOC)
- [ ] `pal_window.cpp` — SDL3 window + bgfx init (~150 LOC), headless mode support
- [ ] `pal_input.cpp` — SDL3 gamepad → JUTGamePad (~200 LOC)
- [ ] `pal_audio.cpp` — silence stubs for Phase A (~250 LOC)
- [ ] `pal_save.cpp` — fstream save/load (~150 LOC)
- [ ] Verify: `TP_HEADLESS=1 TP_TEST_FRAMES=10 ./build/tp-pc` → milestones 0–4

### Step 4 — DVD/ARAM Simplification (~200 LOC)
- [ ] DVD: `mDoDvdThd_command_c::create()` → sync `pal_fs_read()`, mark done
- [ ] ARAM: `aramToMainRam`/`mainRamToAram` → `memcpy`; JKRAram* → host malloc wrappers
- [ ] Verify: milestone reaches 5 (FIRST_FRAME)

### Step 5 — GX Shim Tier A (~5,000 LOC)
- [ ] 5a. `GX_WRITE_*` macro redirect in `GXRegs.h` (~100 LOC)
- [ ] 5b. GX state machine + bgfx flush in `src/pal/gx/gx_state.cpp` (~2,500 LOC)
- [ ] 5c. TEV → bgfx shader generator in `src/pal/gx/gx_tev.cpp` (~1,500 LOC)
- [ ] 5d. Texture decode (10 GX formats → RGBA8) in `src/pal/gx/gx_texture.cpp` (~1,000 LOC)
- [ ] 5e. Display list record/replay in `src/pal/gx/gx_displaylist.cpp` (~400 LOC)
- [ ] Verify: milestone reaches 6–8 (LOGO_SCENE through PLAY_SCENE)

### Step 6 — Audio (~100 LOC Phase A / ~800 LOC Phase B)
- [ ] Phase A: `pal_audio` returns silence — game loop runs unblocked
- [ ] Phase B: Software mixer replaces DSP/ARAM → SDL3 PCM output
- [ ] Verify: no audio-related hangs in headless mode

### Step 7 — Input + Save (~350 LOC)
- [ ] Wire `JUTGamePad` → `pal_input` → SDL3 gamepad
- [ ] Replace NAND calls with `pal_save` file I/O
- [ ] Verify: milestone reaches 10+ (FRAMES_60)

### Step 8 — First Playable
- [ ] Title → Ordon → Faron → Forest Temple gameplay loop
- [ ] ImGui toggles for problematic effects (~300 LOC)
- [ ] Verify: milestone reaches 12 (FRAMES_1800 — 30 seconds sustained)

### Step 9 — NX Homebrew (~500 LOC)
- [ ] Swap PAL backends: libnx/EGL, HID, audren, romfs
- [ ] `VERSION_NX_HB = 14`, `PLATFORM_NX_HB`
- [ ] Verify: builds and runs on NX homebrew hardware

## CI Infrastructure
- [ ] Create `.github/workflows/port-test.yml` (~80 LOC YAML)
- [ ] Create `tools/parse_milestones.py` (~100 LOC Python)
- [ ] Create `src/pal/pal_crash.cpp` — signal handler + backtrace (~20 LOC)
- [ ] Create GX stub coverage tracker `include/pal/gx/gx_stub_tracker.h` (~80 LOC)
- [ ] Auto-input injection for scene progression (~60 LOC)

## Milestone Reference

Use this table to diagnose where the port is stuck and decide what to work on.

| # | Milestone | Means | If stuck here, work on |
|---|---|---|---|
| -1 | (no output) | Build errors | Step 1: CMake + compile fixes |
| 0 | BOOT_START | Binary launches | Step 3: `mDoMch_Create` / heap init |
| 1 | HEAP_INIT | Heaps working | Step 3: `mDoGph_Create` / GFX init |
| 2 | GFX_INIT | bgfx ready | Step 3: input init / PAL bootstrap |
| 3 | PAD_INIT | Input ready | Step 3: framework init |
| 4 | FRAMEWORK_INIT | Process manager ready | Step 4: DVD/archive loading |
| 5 | FIRST_FRAME | Game loop running | Step 5: GX shim (scene can't render) |
| 6 | LOGO_SCENE | Logo scene loaded | Step 5: scene transition / assets |
| 7 | TITLE_SCENE | Title screen reached | Step 5: stage loading |
| 8 | PLAY_SCENE | Gameplay scene loaded | Step 5: GX stubs by frequency |
| 9 | STAGE_LOADED | Specific stage loaded | Step 5/7: rendering + input |
| 10 | FRAMES_60 | 1 second stable | Step 5: top stub hits |
| 11 | FRAMES_300 | 5 seconds stable | Step 6 Phase B and Step 8: polish |
| 12 | FRAMES_1800 | 30 seconds stable | Step 8: first playable achieved 🎉 |

## Session Log

> Agents: Add a brief entry here each time you complete meaningful work. Most recent first.
> Include: date, what you did, what milestone changed, what to do next.

| Date | Summary | Milestone Change | Next Action |
|---|---|---|---|
| 2026-02-27 | Created progress tracking system | N/A | Start Step 1: CMake build system |

## Files Created/Modified by the Port

> Agents: Update this list as you create or modify files for the port.

### New files (port code)
_None yet — start with Step 1_

### Modified files (conditional extensions)
_None yet — start with Step 2_

## How to Use This File

### At Session Start
1. Read this file first — check **Current Status** and **Step Checklist**
2. Check the **Session Log** for context on recent work
3. If CI is set up, check `milestone-summary.json` from the latest CI run
4. Use the **Milestone Reference** table to decide what to work on

### During Your Session
1. Work on the next unchecked item in the **Step Checklist**
2. Follow the detailed instructions in [Agent Port Workflow](agent-port-workflow.md)
3. Validate your work against the expected milestone for that step
4. If you need assets, ROM files, disc images, or other resources you cannot obtain
   yourself, **leave a comment on the PR** describing what you need and why

### Before Ending Your Session
1. Mark completed items `[x]` in the **Step Checklist**
2. Update **Current Status** table (milestone, current step, date, blocking issue)
3. Add an entry to the **Session Log** with what you did and what's next
4. Update **Files Created/Modified** if you added new port files
5. Commit this file alongside your code changes
