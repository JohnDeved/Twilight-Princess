# Copilot Custom Instructions — Twilight Princess Port

## Project Overview

This is a decompilation of **The Legend of Zelda: Twilight Princess** being ported to
PC and NX Homebrew. The codebase is 1.1M LOC of C/C++ originally targeting GameCube/Wii.
We use the **NVIDIA Shield port** (PLATFORM_SHIELD, 63+ files) as our starting point —
it already solved most platform-abstraction problems.

## Port Architecture

- **Rendering**: GX (GameCube GPU API) → **bgfx** shim (~5,000 LOC). bgfx auto-selects
  D3D12 on Windows, Metal on macOS, Vulkan on Linux, GLES on NX.
- **Platform**: **SDL3** handles windowing, input, and audio.
- **New code lives in `src/pal/`** — replaces `src/dolphin/` + `src/revolution/` SDK.
- **Game logic is unchanged** — `src/d/`, `src/f_pc/`, `src/f_op/`, `src/SSystem/` stay as-is.

## Key Files

| File | Purpose |
|---|---|
| `include/global.h` | Platform macros: `PLATFORM_GCN`, `PLATFORM_WII`, `PLATFORM_SHIELD`, `PLATFORM_PC` |
| `include/revolution/private/GXRegs.h` | `GX_WRITE_*` macros — redirect for PC port |
| `src/m_Do/m_Do_main.cpp` | Main entry + game loop (`main01()`) |
| `config/ShieldD/splits.txt` | Battle-tested file list (4,028 files for non-Nintendo build) |
| `docs/multiplatform-port-plan.md` | Full port strategy and execution plan |
| `docs/ai-agent-testing-plan.md` | Headless CI testing with boot milestones |
| `docs/agent-port-workflow.md` | Step-by-step agent workflow for implementing the port |

## Coding Conventions

- **C style**: This is a decompilation — match the original code style (C99/C++03, no modern C++).
- **No game logic changes**: Never modify files in `src/d/`, `src/f_pc/`, `src/f_op/`,
  `src/f_ap/`, `src/SSystem/`, `src/c/` unless adding a `#if PLATFORM_PC` conditional.
- **Additive shims only**: Port code goes in `src/pal/` and `include/pal/`. Don't rewrite
  existing SDK files — replace them entirely via CMake exclusion.
- **Platform conditionals**: Follow the Shield pattern:
  ```c
  #if PLATFORM_WII || PLATFORM_SHIELD || PLATFORM_PC
      // shared non-GCN path
  #endif
  ```
- **Naming**: PAL module functions use `pal_` prefix (e.g., `pal_window_init()`, `pal_fs_read()`).
- **Types**: Use the existing project types (`u8`, `u16`, `u32`, `s32`, `f32`, etc.) from
  `include/dolphin/types.h`.

## Build System

- **Original build**: `python3 configure.py && ninja` (requires game disc images in `orig/`)
- **Port build** (planned): CMake-based, sources from `config/ShieldD/splits.txt` minus
  `dolphin/`, `revolution/`, `lingcod/`, plus `src/pal/`.
- **Headless testing**: Set `TP_HEADLESS=1` and `TP_TEST_FRAMES=1800` env vars.
- **CI workflow**: `.github/workflows/port-test.yml` builds and runs headless tests.

## When Working on the Port

1. **Check `docs/multiplatform-port-plan.md`** for the current execution plan and step details.
2. **Check `docs/ai-agent-testing-plan.md`** for the CI testing infrastructure.
3. **Check `docs/agent-port-workflow.md`** for the step-by-step implementation guide.
4. **Use the milestone system**: After changes, verify that the highest boot milestone
   hasn't regressed. Milestones are logged as JSON to stdout.
5. **GX stubs**: When adding a GX function stub, use the stub tracker so CI reports hit counts:
   ```c
   void GXSetFog(...) { gx_stub_hit(GX_STUB_SET_FOG, "GXSetFog"); }
   ```
6. **Test headless**: Always ensure `TP_HEADLESS=1` mode works (bgfx Noop backend, no GPU).
7. **Prioritize by frequency**: Implement the most-called GX stubs first (check `milestone-summary.json`).

## Version Numbers

| Version | Value | Platform macro |
|---|---|---|
| `VERSION_PC` | 13 | `PLATFORM_PC` |
| `VERSION_NX_HB` | 14 | `PLATFORM_NX_HB` |

These follow after `VERSION_SHIELD_DEBUG = 12` in `global.h`.

## What NOT to Do

- Don't modify game logic files without `#if PLATFORM_PC` guards
- Don't use C++11/14/17 features in game code (decompilation targets C++03)
- Don't add SDL3 or bgfx calls directly in game code — always go through PAL/shim layers
- Don't remove or modify the original build system (`configure.py` / `ninja`)
- Don't change `src/dolphin/` or `src/revolution/` — these are replaced, not modified
