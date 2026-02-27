# Port Progress Tracker

> **For AI Agents**: Read this file at the start of every session. Update it before ending
> your session. This is the single source of truth for what has been done and what to do next.

## Current Status

| Field | Value |
|---|---|
| **Highest CI Milestone** | `0` (binary links, runtime stubs functional but not yet tested) |
| **Current Step** | Step 3 in progress — runtime stubs functional, need asset loading |
| **Last Updated** | 2026-02-27 |
| **Blocking Issue** | No game assets available for DVD loading — headless test needed |

## Step Checklist

Each step maps to the [Execution Plan](multiplatform-port-plan.md#execution-plan) and the
[Agent Port Workflow](agent-port-workflow.md). Mark items `[x]` when complete.

### Step 1 — CMake Build System (~250 LOC) ✅
- [x] Create root `CMakeLists.txt` with sources from `config/Shield/splits.txt`
- [x] Exclude `src/dolphin/`, `src/revolution/`, `src/lingcod/`, PPC assembly
- [x] Add `VERSION_PC = 13` and `PLATFORM_PC` to `include/global.h`
- [x] Create `include/pal/pal_intrinsics.h` (MWCC → GCC/Clang, cache no-ops)
- [x] Create `include/pal/pal_milestone.h` (boot milestone logging)
- [x] Create `include/pal/pal_platform.h` (std lib, fabsf, isnan, stricmp compat)
- [x] Create `include/pal/gx/gx_stub_tracker.h` (GX stub hit tracking)
- [x] Add `GX_WRITE_*` macro redirect for `PLATFORM_PC` in `GXRegs.h`
- [x] Fix `.gitignore` to allow `CMakeLists.txt` (was blocked by `/*.txt`)
- [x] Fix `AT_ADDRESS()` globals in `OSExec.h` for GCC (extern instead of definition)
- [x] Verify: All 533 source files compile and link into `tp-pc` binary

### Step 2 — Extend Shield Conditionals (~206 LOC changes) ✅
- [x] `PLATFORM_WII || PLATFORM_SHIELD` → add `|| PLATFORM_PC` (~85 sites across ~42 files)
- [x] `!PLATFORM_SHIELD` → `!PLATFORM_SHIELD && !PLATFORM_PC` where needed
- [x] Add milestone instrumentation in `m_Do_main.cpp`
- [x] Verify: all 533 sources compile cleanly with conditionals extended

### SDK Stub Library (Full Link) ✅
- [x] `src/pal/pal_os_stubs.cpp` — OS functions (OSReport, cache, time, interrupts)
- [x] `src/pal/pal_gx_stubs.cpp` — 120 GX/GD graphics function stubs
- [x] `src/pal/pal_sdk_stubs.cpp` — 157 OS/hardware stubs (DVD, VI, PAD, AI, AR, DSP, etc.)
- [x] `src/pal/pal_math_stubs.cpp` — Real PSMTX/PSVEC/C_MTX math (not empty stubs)
- [x] `src/pal/pal_game_stubs.cpp` — Game-specific stubs (debug views, GF wrappers)
- [x] `src/pal/pal_remaining_stubs.cpp` — JStudio, JSU streams, JOR, J3D, misc
- [x] `src/pal/pal_crash.cpp` — Crash signal handler
- [x] `src/pal/gx/gx_stub_tracker.cpp` / `gx_fifo.cpp` — GX shim infrastructure
- [x] Verify: 0 undefined references — binary links successfully

### Step 3 — PAL Bootstrap (~1,250 LOC) ⬜ IN PROGRESS
- [x] Fix OSAllocFromArenaLo/Hi — use uintptr_t for 64-bit pointer safety
- [x] OSCreateThread/OSResumeThread — single-threaded dispatch (call main01 directly)
- [x] Fix SCCheckStatus → SC_STATUS_OK (was returning BUSY, caused infinite loop)
- [x] DVDDiskID — set proper game ID fields for retail version detection
- [x] ARAM emulation — 16MB malloc-backed buffer with ARAlloc/ARStartDMA
- [x] Message queue — synchronous implementation for single-threaded mode
- [x] OSThread struct — init stackBase/stackEnd/state for JKRThread compatibility
- [x] DVD thread — execute commands synchronously on PC (#if PLATFORM_PC in m_Do_dvd_thread.cpp)
- [x] Fake MEM1 — OSPhysicalToCached/OSCachedToPhysical redirect to valid host memory
- [x] VIGetRetraceCount — time-based simulation (~60 Hz) for frame counting
- [x] waitForTick — skip retrace-based vsync wait on PC (prevents deadlock)
- [x] **u32/s32 fix** — changed from `unsigned long`/`signed long` to `unsigned int`/`signed int` for 64-bit
- [x] **OS_BASE_CACHED** — redirect at definition site in dolphin/os.h + revolution/os.h (not pal_platform.h)
- [x] **OSPhysicalToCached** — use uintptr_t instead of u32 to prevent 64-bit address truncation
- [x] **__OSBusClock** — populate pal_fake_mem1[0xF8] via constructor(101) before static init
- [x] **Skip Wii arena reduction** — guard 0x1800000 MEM1 subtraction with #if !PLATFORM_PC
- [x] **Skip font init** — embedded font is big-endian PPC data, crashes on little-endian PC
- [x] **DVDReadAsyncPrio/DVDSetAutoInvalidation** — fix return types to match SDK headers
- [ ] `pal_window.cpp` — SDL3 window + bgfx init (~150 LOC), headless mode support
- [ ] `pal_input.cpp` — SDL3 gamepad → JUTGamePad (~200 LOC)
- [ ] `pal_audio.cpp` — silence stubs for Phase A (~250 LOC)
- [ ] `pal_save.cpp` — fstream save/load (~150 LOC)
- [ ] `pal_fs.cpp` — file I/O replacing DVD/NAND for host filesystem access (~300 LOC)
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
| 2026-02-27 | Step 3: Made OS stubs functional — 64-bit arena, threads, SCCheckStatus, DVDDiskID, ARAM emu, MEM1 fake, waitForTick skip, DVD sync exec | 0 → 0 (runtime ready) | Test headless boot, add pal_fs for assets |
| 2026-02-27 | Updated port-progress.md to reflect completed Steps 1+2, SDK stubs, full link | -1 → 0 | Step 3: PAL bootstrap (SDL3+bgfx) |
| 2026-02-27 | Achieved full link: fixed duplicate symbols, extern OSExec globals, 0 undefined refs | -1 → 0 | Update progress tracker |
| 2026-02-27 | Created 6 SDK stub files (GX, OS, math, game, remaining), fixed C++ linkage issues | -1 → -1 | Fix last linker errors |
| 2026-02-27 | Recovered lost CMakeLists.txt (gitignore fix), all 533 files compile + link | -1 → 0 | Create SDK stubs for linking |
| 2026-02-27 | Steps 1+2: CMake, global.h, PAL headers, GX redirect, Shield conditionals, milestones | N/A → -1 | Compile + link |
| 2026-02-27 | Created progress tracking system | N/A | Start Step 1: CMake build system |

## Files Created/Modified by the Port

> Agents: Update this list as you create or modify files for the port.

### New files (port code)
- `CMakeLists.txt` — Root CMake build system (parses Shield/splits.txt, 533 sources)
- `include/pal/pal_platform.h` — Compatibility header (std lib, fabsf, isnan, stricmp)
- `include/pal/pal_intrinsics.h` — MWCC → GCC/Clang intrinsic equivalents
- `include/pal/pal_milestone.h` — Boot milestone logging (12 milestones)
- `include/pal/gx/gx_stub_tracker.h` — GX stub hit coverage tracker
- `src/pal/pal_os_stubs.cpp` — OS function stubs (OSReport, cache, time, interrupts)
- `src/pal/pal_gx_stubs.cpp` — 120 GX/GD graphics function stubs
- `src/pal/pal_sdk_stubs.cpp` — 157 OS/hardware SDK stubs (DVD, VI, PAD, AI, AR, DSP, etc.)
- `src/pal/pal_math_stubs.cpp` — Real PSMTX/PSVEC/C_MTX math implementations
- `src/pal/pal_game_stubs.cpp` — Game-specific stubs (debug views, GF wrappers)
- `src/pal/pal_remaining_stubs.cpp` — JStudio, JSU streams, JOR, J3D, misc SDK
- `src/pal/pal_crash.cpp` — Crash signal handler
- `src/pal/gx/gx_stub_tracker.cpp` — GX stub tracker implementation
- `src/pal/gx/gx_fifo.cpp` — GX FIFO RAM buffer infrastructure
- `.github/workflows/port-test.yml` — CI workflow for port testing
- `tools/parse_milestones.py` — CI milestone parser
- `assets/` — Placeholder asset headers for compilation

### Modified files (conditional extensions)
- `include/global.h` — Added VERSION_PC=13, PLATFORM_PC macro
- `include/revolution/private/GXRegs.h` — GX_WRITE_* redirect for PLATFORM_PC
- `include/revolution/os/OSExec.h` — AT_ADDRESS extern fix for GCC
- `include/pal/pal_platform.h` — OS_BASE_CACHED/OSPhysicalToCached override for PC
- `.gitignore` — Allow CMakeLists.txt (!CMakeLists.txt exception)
- `.github/copilot-instructions.md` — Added "Commit Early and Often" rule
- `src/m_Do/m_Do_main.cpp` — Milestone instrumentation
- `src/m_Do/m_Do_dvd_thread.cpp` — Synchronous DVD command execution on PC
- `src/JSystem/JFramework/JFWDisplay.cpp` — Skip retrace-based wait on PC
- 77 files — Extended `PLATFORM_SHIELD` conditionals to include `PLATFORM_PC`

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
