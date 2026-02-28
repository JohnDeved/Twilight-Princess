# Automated Testing & Progress Tracking Guide

> **For AI Agents**: This is the authoritative guide for understanding and using the automated
> testing infrastructure. It consolidates all testing information into one place.

## Quick Reference

```bash
# One-command self-test (build + run + parse + verify — same checks as CI)
tools/self-test.sh

# Quick smoke test (skip build, 100 frames)
tools/self-test.sh --quick

# Skip build, custom frame count
tools/self-test.sh --skip-build --frames 300
```

### Manual steps (if needed)

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
│  6. Implement fix based on stubs / milestones → goto 4          │
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
| `milestone-summary.json` | Parsed milestone results | Check `milestones_reached_count` and `stubs_hit` |
| `regression-report.json` | Regression check result | Check `status` (improved/same/regressed) |
| `milestones.log` | Raw milestone log | Debug milestone timing |
| `logo_render.bmp` | Screenshot capture | Visual verification |

### PR Comment

On pull requests, the CI posts a structured comment with:
- Current milestone count vs baseline
- Regression status (🎉 improved / ✅ same / 🚨 regressed)
- Top unimplemented stubs
- Frame validation stats

The comment is updated on each push (not duplicated).

## Regression Detection

### How It Works

The file `milestone-baseline.json` stores the milestone count ever achieved:

```json
{
  "milestones_reached_count": 8,
  "milestones_reached": ["BOOT_START", "HEAP_INIT", "GFX_INIT", "PAD_INIT", "FRAMEWORK_INIT", "FIRST_FRAME", "LOGO_SCENE", "SCENE_CREATED"],
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

When your changes improve the milestone, update the baseline manually by editing
`milestone-baseline.json` to reflect the new milestone count and milestones reached.
Then commit the updated baseline alongside your code changes so the improvement is
tracked in the PR diff.

## Agent Development Workflow

### Starting a Session

1. **Read `milestone-baseline.json`** — know the current best milestone
2. **Read `docs/port-progress.md`** — check the step checklist and session log
3. **Check the latest CI run** — read the PR comment or download `milestone-summary.json`
4. **Decide what to work on** — use the decision table above or the stubs list from
   the milestone summary

### Making Changes

1. Make minimal, focused changes
2. Run the self-test:
   ```bash
   tools/self-test.sh --quick          # fast smoke test after small changes
   tools/self-test.sh --skip-build     # full test with existing binary
   tools/self-test.sh                  # full rebuild + test
   ```
3. If the self-test passes, commit and push — CI will run the same checks automatically
4. If it fails, read the output to see which step failed and fix the issue

### Interpreting CI Results

The regression report tells you the current status:

```json
{
  "status": "same",
  "current_milestone_count": 8,
  "baseline_milestone_count": 8,
  "milestones_reached": ["BOOT_START", "HEAP_INIT", "GFX_INIT", "PAD_INIT", "FRAMEWORK_INIT", "FIRST_FRAME", "LOGO_SCENE", "SCENE_CREATED"]
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
| `TP_VERIFY` | Set to `1` to enable subsystem verification | unset (disabled) |
| `TP_VERIFY_DIR` | Directory for captured frame BMPs | `verify_output` |
| `TP_VERIFY_CAPTURE_FRAMES` | Comma-separated frame numbers to capture | `30,60,120,300,600,1200,1800` |
| `TP_TEST_INPUTS` | Enable synthetic input injection | unset (no injection) |
| `ROMS_TOKEN` | GitHub PAT for game data download | unset (skip download) |

## Subsystem Verification

The verification system (`TP_VERIFY=1`) provides automated health checks for three
subsystems: rendering, input, and audio. It works alongside the milestone system to
give agents concrete evidence of what's working.

### Rendering Verification

The rendering verification is **fully automated and dependable without human review**.
It uses five verification layers, any of which can independently detect regressions:

#### Layer 1: Per-Frame Metrics
Each frame reports draw calls, vertex count, stub count, and validity flag.
Zero draw calls or zero vertices = broken rendering pipeline.

#### Layer 2: Framebuffer Hashing (CRC32)
The CPU-side software framebuffer (640×480 RGBA) produces a deterministic CRC32 hash
per frame. Same input data + same rendering code = identical hash. If a code change
causes a different hash on a previously-stable frame, the rendering changed.

The hash is logged per frame:
```json
{"verify_frame":{"frame":30,"draw_calls":12,"verts":480,"fb_hash":"0xABCD1234","fb_has_draws":1}}
```

Expected hashes can be stored in `tests/render-baseline.json` — if the actual hash
differs, CI flags it as a regression.

#### Layer 3: Pixel Analysis
For each captured frame, the system computes:
- **Non-black pixel percentage** (0-100%): "Is anything rendering?"
- **Unique colors**: "Is there visual diversity, or just a flat color?"
- **Average color**: "Did the color palette change drastically?"

These thresholds are stored in `tests/render-baseline.json` and must not regress.

#### Layer 4: Golden Image Comparison (RMSE)
Captured frame BMPs are compared against golden reference images in `tests/golden/`:
- **RMSE** (Root Mean Square Error): 0 = identical, higher = more different
- **Pixel diff percentage**: How many pixels changed beyond a threshold
- **Threshold**: RMSE > 5.0 = regression

When no golden reference exists for a frame, the captured frame is reported as
"no golden reference" in the comparison results. To create golden references,
run `tools/verify_port.py --update-golden` locally and commit the resulting BMPs.

#### Layer 5: Render Baseline Thresholds
`tests/render-baseline.json` stores minimum expected values per frame:
```json
{
  "frames": {
    "30": {
      "min_draw_calls": 5,
      "min_pct_nonblack": 30,
      "expected_fb_hash": "0xABCD1234"
    }
  },
  "global": {
    "min_peak_draw_calls": 10,
    "min_render_health_pct": 50
  }
}
```

Any metric that falls below the baseline is flagged as a regression.

#### How It Works in Headless CI
The software framebuffer captures all GX shim draw calls at the CPU level,
**independent of bgfx and the GPU**. Even with bgfx's Noop renderer (no GPU),
every textured quad draw is rasterized into the 640×480 RGBA framebuffer.
This means:
- No GPU required in CI
- Deterministic output (no GPU driver differences)
- Frame captures are meaningful even in headless mode

```bash
# Run with full verification
TP_HEADLESS=1 TP_TEST_FRAMES=300 TP_VERIFY=1 TP_VERIFY_DIR=/tmp/verify \
    ./build/tp-pc 2>&1 | tee /tmp/test.log

# Analyze results — fully automated pass/fail
python3 tools/verify_port.py /tmp/test.log \
    --verify-dir /tmp/verify \
    --golden-dir tests/golden \
    --render-baseline tests/render-baseline.json \
    --check-rendering
```

### Input Verification

When input is implemented, the verification system logs:
- **Input events**: button presses, stick positions per frame
- **Game responses**: scene changes, menu actions triggered by input

This proves the input pipeline works end-to-end: SDL3 → PAL input → JUTGamePad → game logic.

### Audio Verification

When audio is implemented, the verification system logs:
- **Audio buffer status**: active/inactive, samples mixed, buffers queued
- **Silence detection**: whether audio buffers contain non-silent samples

This proves audio mixing produces actual sound data, not just silence stubs.

### Running Verification Locally

```bash
# Build and run with full verification
TP_HEADLESS=1 TP_TEST_FRAMES=300 TP_VERIFY=1 ./build/tp-pc 2>&1 | tee /tmp/test.log

# Check all subsystems
python3 tools/verify_port.py /tmp/test.log --check-all --output /tmp/verify-report.json

# Check rendering only
python3 tools/verify_port.py /tmp/test.log --check-rendering
```

### CI Integration

CI automatically runs verification and includes results in the PR comment:
- **Rendering**: ✅/❌ with frame counts, draw stats, pixel analysis
- **Input**: ✅/❌ with event counts and response tracking
- **Audio**: ✅/❌ with buffer activity and silence detection

Captured frame BMPs are uploaded as CI artifacts for visual inspection.

## File Map

```
milestone-baseline.json              ← Current best milestone (regression baseline)
tests/render-baseline.json           ← Rendering metrics baseline (draw calls, hashes)
tests/golden/                        ← Golden reference frame BMPs for image comparison
tools/self-test.sh                   ← One-command local test (build+run+parse+verify)
tools/parse_milestones.py            ← Parse milestone log → JSON summary
tools/check_milestone_regression.py  ← Compare against baseline, detect regressions
tools/verify_port.py                 ← Subsystem verification (rendering/input/audio)
tools/setup_game_data.py             ← Download + extract game data
.github/workflows/port-test.yml      ← CI pipeline (build, test, verify, comment on PR)
include/pal/pal_milestone.h          ← Milestone C API
src/pal/pal_milestone.cpp            ← Milestone state management
include/pal/pal_verify.h             ← Verification system C API
src/pal/pal_verify.cpp               ← Verification system implementation
include/pal/gx/gx_screenshot.h      ← Software framebuffer capture + hash
src/pal/gx/gx_screenshot.cpp        ← Software framebuffer implementation
include/pal/gx/gx_stub_tracker.h    ← GX stub hit tracker
src/pal/gx/gx_stub_tracker.cpp      ← Stub tracker implementation
src/pal/pal_crash.cpp                ← Crash handler (signal → JSON)
docs/port-progress.md                ← Progress tracker (session log, checklist)
docs/multiplatform-port-plan.md      ← Full port strategy
docs/agent-port-workflow.md          ← Step-by-step implementation guide
```
