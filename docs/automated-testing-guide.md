# Automated Testing & Progress Tracking Guide

> **For AI Agents**: This is the authoritative guide for understanding and using the automated
> testing infrastructure. It consolidates all testing information into one place.

## Quick Reference

```bash
# Build the PC port
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC
ninja -C build tp-pc

# Run headless test (no GPU required)
TP_HEADLESS=1 TP_TEST_FRAMES=2000 ./build/tp-pc 2>&1 | tee milestones.log

# Parse results
python3 tools/parse_milestones.py milestones.log --output milestone-summary.json

# Check for regressions against baseline
python3 tools/check_milestone_regression.py milestone-summary.json \
    --baseline milestone-baseline.json --output regression-report.json
```

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│  AI Agent Session                                               │
│                                                                 │
│  1. Read milestone-baseline.json (current best milestone)       │
│  2. Read docs/port-progress.md (what to work on)                │
│  3. Make code changes                                           │
│  4. Push to PR                                                  │
│     ↓                                                           │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  CI Pipeline (.github/workflows/port-test.yml)           │   │
│  │                                                          │   │
│  │  Build (CMake+Ninja) → Run headless → Parse milestones   │   │
│  │       → Check regression → Post PR comment               │   │
│  └──────────────────────────────────────────────────────────┘   │
│     ↓                                                           │
│  5. Read PR comment (structured milestone results)              │
│  6. Follow "Recommended Next Action" from the report            │
│  7. Implement fix → goto 4                                      │
└─────────────────────────────────────────────────────────────────┘
```

## What the CI Tests

### Milestone System

The port uses a **milestone-based progress system** with 16 checkpoints that map to the
game's boot sequence. Each milestone is a JSON line emitted to stdout:

```json
{"milestone":"HEAP_INIT","id":1,"time_ms":42,"detail":"rootHeap=0x1000000"}
{"milestone":"FIRST_FRAME","id":5,"time_ms":230,"frame":1}
```

| # | Milestone | What It Proves | If Stuck Here |
|---|---|---|---|
| -1 | *(no output)* | Build errors | Fix CMake / compile errors |
| 0 | `BOOT_START` | Binary launched | Fix `mDoMch_Create` / heap init |
| 1 | `HEAP_INIT` | Memory system working | Fix `mDoGph_Create` / GFX init |
| 2 | `GFX_INIT` | bgfx initialized | Fix PAL bootstrap |
| 3 | `PAD_INIT` | Input subsystem ready | Fix framework init |
| 4 | `FRAMEWORK_INIT` | Process manager ready | Fix DVD/archive loading |
| 5 | `FIRST_FRAME` | Game loop running | Fix scene loading |
| 6 | `LOGO_SCENE` | Logo scene loaded from disc | Fix GX rendering |
| 7 | `TITLE_SCENE` | Title screen reached | Fix scene transitions |
| 8 | `PLAY_SCENE` | Gameplay scene loaded | Fix stage loading |
| 9 | `STAGE_LOADED` | Specific stage loaded | Fix rendering + input |
| 10 | `FRAMES_60` | 1s stable rendering | Fix top stub hits |
| 11 | `FRAMES_300` | 5s stable rendering | Polish + fix stubs |
| 12 | `FRAMES_1800` | 30s stable rendering | 🎉 First playable! |
| 13 | `DVD_READ_OK` | File I/O working | Fix DVD path mapping |
| 14 | `SCENE_CREATED` | Any process created | Fix profile list |
| 15 | `RENDER_FRAME` | Real GX rendering | Fix GX shim |

**Important constraints:**
- Milestones 6-8 require the scene's `create()` to return `cPhs_COMPLEATE_e` (real assets loaded)
- Milestones 10-12 require `RENDER_FRAME` (15) to fire first (no trivial frame counting)
- `RENDER_FRAME` requires: bgfx active + zero GX stubs hit + real draw calls + valid vertices

### GX Stub Tracker

Unimplemented GX functions log their hit counts. At test completion, a stub report is emitted:

```json
{"stub":"GXSetFog","hits":47}
{"stub":"GXSetLineWidth","hits":12}
```

**Agent strategy**: Implement the highest-hit stub first for maximum impact.

### Crash Detection

The PAL crash handler (`src/pal/pal_crash.cpp`) catches SIGSEGV/SIGABRT/SIGFPE and emits:

```json
{"milestone":"CRASH","id":-1,"signal":11}
```

Followed by a backtrace on stderr.

## CI Pipeline Details

### Trigger Conditions

The CI runs on every push or PR that touches:
- `src/**` — any source file
- `include/**` — any header
- `CMakeLists.txt` — build system
- `tools/parse_milestones.py` — milestone parser
- `tools/check_milestone_regression.py` — regression checker
- `milestone-baseline.json` — baseline milestone

### What CI Produces

| Artifact | Purpose | Agent Use |
|---|---|---|
| `milestone-summary.json` | Parsed milestone results | Check `highest_milestone` and `stubs_hit` |
| `regression-report.json` | Regression check result | Check `status` (improved/same/regressed) and `next_action` |
| `milestones.log` | Raw milestone log | Debug milestone timing |
| `logo_render.bmp` | Screenshot capture | Visual verification |

### PR Comment

On pull requests, the CI posts a structured comment with:
- Current milestone vs baseline
- Regression status (🎉 improved / ✅ same / 🚨 regressed)
- Top unimplemented stubs
- Frame validation stats
- **Recommended next action** with specific files to edit

The comment is updated on each push (not duplicated).

## Regression Detection

### How It Works

The file `milestone-baseline.json` stores the highest milestone ever achieved:

```json
{
  "highest_milestone": 6,
  "highest_milestone_name": "LOGO_SCENE",
  "updated": "2026-02-27",
  "note": "All rendering-critical stubs implemented"
}
```

After each CI run, `tools/check_milestone_regression.py` compares the current run against
this baseline:

- **Improved**: Current > baseline → 🎉 (update baseline automatically)
- **Same**: Current == baseline → ✅ (no change needed)
- **Regressed**: Current < baseline → 🚨 (investigate and fix before merging)

### Updating the Baseline

When your changes improve the milestone, the baseline should be updated. You can do this
manually by editing `milestone-baseline.json`, or the CI can do it automatically with:

```bash
python3 tools/check_milestone_regression.py milestone-summary.json \
    --baseline milestone-baseline.json --update-on-improvement
```

## Agent Development Workflow

### Starting a Session

1. **Read `milestone-baseline.json`** — know the current best milestone
2. **Read `docs/port-progress.md`** — check the step checklist and session log
3. **Check the latest CI run** — read the PR comment or download `milestone-summary.json`
4. **Decide what to work on** — use the decision table above or the `next_action` from the
   regression report

### Making Changes

1. Make minimal, focused changes
2. Build and test locally:
   ```bash
   ninja -C build tp-pc
   TP_HEADLESS=1 TP_TEST_FRAMES=100 ./build/tp-pc 2>&1 | tee /tmp/test.log
   python3 tools/parse_milestones.py /tmp/test.log --output /tmp/summary.json
   ```
3. Check that the milestone hasn't regressed:
   ```bash
   python3 tools/check_milestone_regression.py /tmp/summary.json \
       --baseline milestone-baseline.json
   ```
4. Commit and push — CI will run automatically

### Interpreting CI Results

The regression report's `next_action` field tells you exactly what to do:

```json
{
  "status": "same",
  "current_milestone": 6,
  "baseline_milestone": 6,
  "next_action": {
    "action": "Logo loads — implement GX stubs and rendering for title scene",
    "focus_files": ["src/pal/pal_gx_stubs.cpp", "src/pal/gx/gx_state.cpp"],
    "docs": "docs/agent-port-workflow.md#step-5--gx-shim-tier-a-5000-loc"
  }
}
```

### Before Ending a Session

1. Update `docs/port-progress.md`:
   - Mark completed checklist items
   - Update the session log
   - Update the current status table
2. If you improved the milestone, update `milestone-baseline.json`
3. Commit everything

## Environment Variables

| Variable | Purpose | Default |
|---|---|---|
| `TP_HEADLESS` | Set to `1` for no-GPU testing (bgfx Noop) | unset (windowed) |
| `TP_TEST_FRAMES` | Exit after N frames | unset (infinite loop) |
| `TP_SCREENSHOT` | Save screenshot to this path | unset (no screenshot) |
| `TP_TEST_INPUTS` | Enable synthetic input injection | unset (no injection) |
| `ROMS_TOKEN` | GitHub PAT for game data download | unset (skip download) |

## File Map

```
milestone-baseline.json              ← Current best milestone (regression baseline)
tools/parse_milestones.py            ← Parse milestone log → JSON summary
tools/check_milestone_regression.py  ← Compare against baseline, recommend next action
tools/setup_game_data.py             ← Download + extract game data
.github/workflows/port-test.yml      ← CI pipeline (build, test, comment on PR)
include/pal/pal_milestone.h          ← Milestone C API
src/pal/pal_milestone.cpp            ← Milestone state management
include/pal/gx/gx_stub_tracker.h    ← GX stub hit tracker
src/pal/gx/gx_stub_tracker.cpp      ← Stub tracker implementation
src/pal/pal_crash.cpp                ← Crash handler (signal → JSON)
docs/port-progress.md                ← Progress tracker (session log, checklist)
docs/multiplatform-port-plan.md      ← Full port strategy
docs/agent-port-workflow.md          ← Step-by-step implementation guide
docs/ai-agent-testing-plan.md        ← Original testing plan (superseded by this doc)
```
