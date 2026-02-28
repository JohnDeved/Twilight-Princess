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
| `docs/port-progress.md` | **Read first** — tracks what's done, what's next, session log |
| `docs/automated-testing-guide.md` | **Authoritative testing guide** — CI, milestones, regression detection |
| `milestone-baseline.json` | Current best milestone (regression baseline) |
| `include/global.h` | Platform macros: `PLATFORM_GCN`, `PLATFORM_WII`, `PLATFORM_SHIELD` (add `PLATFORM_PC` in Step 1) |
| `include/revolution/private/GXRegs.h` | `GX_WRITE_*` macros — redirect for PC port |
| `src/m_Do/m_Do_main.cpp` | Main entry + game loop (`main01()`) |
| `config/ShieldD/splits.txt` | Battle-tested file list (4,028 files for non-Nintendo build) |
| `docs/multiplatform-port-plan.md` | Full port strategy and execution plan |
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

1. **Read `docs/port-progress.md` first** — check current status, step checklist, and session log.
2. **Read `docs/automated-testing-guide.md`** — the authoritative guide for testing and CI.
3. **Check `milestone-baseline.json`** — know the current best milestone before making changes.
4. **Check the latest CI PR comment** — it contains the milestone summary, regression status,
   top unimplemented stubs, and a recommended next action.
5. **Use the milestone system**: After changes, verify that the highest boot milestone
   hasn't regressed. Milestones are logged as JSON to stdout.
6. **Always check for regressions** before pushing:
   ```bash
   TP_HEADLESS=1 TP_TEST_FRAMES=100 ./build/tp-pc 2>&1 | tee /tmp/test.log
   python3 tools/parse_milestones.py /tmp/test.log --output /tmp/summary.json
   python3 tools/check_milestone_regression.py /tmp/summary.json --baseline milestone-baseline.json
   ```
7. **GX stubs**: When adding a GX function stub, use the stub tracker so CI reports hit counts:
   ```c
   void GXSetFog(...) { gx_stub_hit(GX_STUB_SET_FOG, "GXSetFog"); }
   ```
8. **Test headless**: Always ensure `TP_HEADLESS=1` mode works (bgfx Noop backend, no GPU).
9. **Prioritize by frequency**: Implement the most-called GX stubs first (check `milestone-summary.json`).
10. **Update `docs/port-progress.md` before ending your session** — mark completed items,
    update the current status, and add a session log entry so the next agent knows where to continue.
11. **Update `milestone-baseline.json`** if you improved the milestone.
12. **Ask for what you need**: If you require assets, ROM files, disc images, tools, or any
    resources you cannot obtain yourself, **leave a comment on the PR** describing exactly what
    you need and why. Do not silently skip work that depends on missing resources.

## CI Integration

- **CI posts a PR comment** with structured test results on every push.
- The comment includes: milestone reached, regression status, top stubs, and recommended action.
- **Read the PR comment** — it's the fastest way to understand what to do next.
- **Regression detection**: CI compares against `milestone-baseline.json`. Regressions are
  flagged with 🚨. Do not merge PRs that regress the milestone.
- **Artifacts**: `milestone-summary.json`, `regression-report.json`, and `milestones.log`
  are uploaded as CI artifacts for detailed analysis.

## Version Numbers

| Version | Value | Platform macro |
|---|---|---|
| `VERSION_PC` | 13 | `PLATFORM_PC` |
| `VERSION_NX_HB` | 14 | `PLATFORM_NX_HB` |

These follow after `VERSION_SHIELD_DEBUG = 12` in `global.h`.

## Commit Early and Often

Your session can be terminated at any time — crashes, tool errors, timeouts, or context
limits will cause you to lose all uncommitted work. **Commit frequently so progress is
never lost.**

- **Commit immediately after every meaningful edit** (a file compiles, a stub is wired up,
  a build-system change works, a header is ported, etc.). Do not batch multiple unrelated
  changes into one large commit.
- **Commit before any risky operation** (large refactors, build-system changes, merge
  conflict resolution). If the operation fails and the session dies, the prior progress
  is safe.
- **Commit before ending any logical unit of work.** If you have been working for several
  minutes and have touched more than one or two files, stop and commit now.
- **Use concise, descriptive commit messages** that explain *what* changed and *why*
  (e.g., `"pal: add SDL3 window init stub"`, `"cmake: exclude dolphin/ sources for PC"`).
- **Never leave uncommitted changes while waiting for a build or test.** Stage and commit
  first, then run the build. If the build fails you can fix-and-commit again.
- **If in doubt, commit.** An extra small commit is always better than lost work.

## What NOT to Do

- Don't modify game logic files without `#if PLATFORM_PC` guards
- Don't use C++11/14/17 features in game code (decompilation targets C++03)
- Don't add SDL3 or bgfx calls directly in game code — always go through PAL/shim layers
- Don't remove or modify the original build system (`configure.py` / `ninja`)
- Don't change `src/dolphin/` or `src/revolution/` — these are replaced, not modified
