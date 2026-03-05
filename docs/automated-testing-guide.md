# Automated Testing & Progress Tracking Guide

> **For AI Agents**: This is the authoritative guide for understanding and using the automated
> testing infrastructure. It consolidates all testing information into one place.

## Quick Reference

```bash
# ⚡ Fast single-frame render test (~10-20s, title screen)
tools/quick-test.sh

# ⚡ Fast 3D render test (~60-90s, gameplay room)
tools/quick-test.sh --3d

# ⚡ Skip build, render specific frame
tools/quick-test.sh --skip-build --frame 60

# Full self-test (build + run + parse + verify — same checks as CI)
tools/self-test.sh

# Quick smoke test (skip build, 100 frames)
tools/self-test.sh --quick
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
│  4. Run tools/quick-test.sh locally (~10-90s)                   │
│     → See rendered frame BMP + milestone validation             │
│     → Iterate until satisfied                                   │
│  5. Push to PR                                                  │
│     ↓                                                           │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  CI Pipeline (.github/workflows/port-test.yml)           │   │
│  │                                                          │   │
│  │  Build (CMake+Ninja) → Run headless → Parse milestones   │   │
│  │       → Check regression → Post PR comment               │   │
│  └──────────────────────────────────────────────────────────┘   │
│     ↓                                                           │
│  6. Read PR comment (structured milestone results)              │
│  7. Implement fix based on stubs / milestones → goto 3          │
└─────────────────────────────────────────────────────────────────┘
```

## Quick Render Test (`tools/quick-test.sh`)

The quick render test is designed for **fast local iteration** and is also used by
CI for its render phase. Instead of running the full CI pipeline from scratch, it
renders target frame(s) from the intro sequence and validates milestones in **10-90 seconds**.

### How It Works

The test treats rendering like a **video render** — each frame can take its time
to rasterize via Mesa softpipe (no GPU required). Instead of rendering hundreds
of frames, it targets just the meaningful frames:

| Mode | Target Frame | Game Time | Content | Time |
|------|-------------|-----------|---------|------|
| Default | Frame 10 | ~0.17s | Title screen (2D) | ~10-20s |
| `--3d` | Frame 129 | ~2.15s | 3D gameplay room (7587 draws) | ~60-90s |
| `--frame N` | Frame N | N/60s | Whatever is on screen at that point | varies |
| `--frame 10 --3d` | 10 + 129 | ~2.15s | Both title + 3D in one pass | ~60-90s |

### Usage

```bash
# Fast title screen test (default, ~10-20s)
tools/quick-test.sh

# Skip build (if already built)
tools/quick-test.sh --skip-build

# 3D gameplay room (more thorough, ~60-90s)
tools/quick-test.sh --3d

# Both title + 3D in one pass
tools/quick-test.sh --frame 10 --3d

# Specific frame
tools/quick-test.sh --frame 60

# CI usage (render-only mode, CI handles post-processing):
tools/quick-test.sh --skip-build --render-only \
  --output-dir verify_output --frame 10 --3d --frame-delay 120000
```

### Options

| Flag | Description |
|------|-------------|
| `--skip-build` | Skip build step (use existing binary) |
| `--3d` | Add frame 129 (3D room) to capture list |
| `--frame N` | Add frame N to capture list (can specify multiple) |
| `--output-dir DIR` | Output directory (default: `quick-test-output/`) |
| `--render-only` | Only render + capture, skip milestone parse/regression |
| `--frame-delay MS` | Override frame delay for 3D frames (default: 90000ms) |

### Output

Results are saved to `quick-test-output/` (gitignored, or `--output-dir` override):
- `frame_NNNN.bmp` — the rendered frame(s) (view to see render progress)
- `test.log` — full game output (milestones, telemetry, errors)
- `milestone-summary.json` — parsed milestone results (unless `--render-only`)
- `regression-report.json` — comparison against baseline (unless `--render-only`)

### When To Use Which Test

| Scenario | Command | Time |
|----------|---------|------|
| Quick visual check after code change | `tools/quick-test.sh --skip-build` | ~10s |
| Verify 3D rendering pipeline | `tools/quick-test.sh --skip-build --3d` | ~60-90s |
| Full regression check (same as CI) | `tools/self-test.sh` | ~5-10min |
| Minimal logic check (no GPU) | `tools/self-test.sh --quick` | ~5-10s |

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
- **Milestones that fail integrity checks are excluded from the count** — they don't count toward
  `milestones_reached_count`. This includes milestones with missing prerequisites, implausible
  timing, or missing corroborating data.

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
- `tools/**` — test and build tools
- `milestone-baseline.json` — baseline milestone
- `tests/**` — test fixtures

### CI Architecture (2-Phase Pipeline)

| Phase | Renderer | Frames | Timeout | What It Tests |
|-------|----------|--------|---------|---------------|
| **Phase 1: Logic** | Noop (no GPU) | 400 | 120s | Boot milestones, game logic, GX stubs, FRAMES_300 |
| **Phase 2: Render** | softpipe (via `quick-test.sh`) | 131 | 240s | Frame 10 (title screen) + Frame 129 (3D room, 7587 draws) |

**Total CI test time: ~5-8 minutes** (down from ~20+ minutes with the old 3-phase pipeline).

Phase 2 calls `tools/quick-test.sh --render-only` — the same script agents use
locally. CI passes `--frame-delay 120000` (120s, 2x safety margin for softpipe
on CI hardware) vs the default 90s for local use.

### What CI Produces

| Artifact | Purpose | Agent Use |
|---|---|---|
| `milestone-summary.json` | Parsed milestone results | Check `milestones_reached_count` and `stubs_hit` |
| `regression-report.json` | Regression check result | Check `status` (improved/same/regressed) |
| `milestones.log` | Merged milestone log (logic + render) | Debug milestone timing |
| `milestones_logic.log` | Phase 1 raw output | Debug logic-only milestones |
| `milestones_render.log` | Phase 2 raw output | Debug render milestones |
| `telemetry-validation.txt` | Draw-call telemetry validation | Check rendering pipeline health |
| `verify_output/frame_*.bmp` | Captured frame screenshots | Visual verification |

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

- **Improved**: Current > baseline → 🎉 (baseline auto-updated and committed by CI)
- **Same**: Current == baseline → ✅ (no change needed)
- **Regressed**: Current < baseline → 🚨 (investigate and fix before merging)

### Updating the Baseline

When milestones improve, CI automatically updates `milestone-baseline.json` and commits
it to the PR branch. The auto-update only happens when:
- The milestone count increased (improvement, not regression)
- All milestones pass integrity checks (no disqualified milestones)
- No crash was detected

The updated baseline appears in the PR diff, so it's always reviewable. If you need to
update the baseline manually (e.g., after a legitimate regression that was intentional),
edit `milestone-baseline.json` directly and commit it alongside your code changes.

## Agent Development Workflow

### Starting a Session

1. **Read `milestone-baseline.json`** — know the current best milestone
2. **Read `docs/port-progress.md`** — check the step checklist and session log
3. **Check the latest CI run** — read the PR comment or download `milestone-summary.json`
4. **Decide what to work on** — use the decision table above or the stubs list from
   the milestone summary

### Making Changes

1. Make minimal, focused changes
2. Run the quick render test for fast visual feedback:
   ```bash
   tools/quick-test.sh --skip-build          # title screen in ~10s
   tools/quick-test.sh --skip-build --3d     # 3D room in ~60-90s
   tools/quick-test.sh --skip-build --frame 10 --3d  # both in one pass
   ```
3. If satisfied, run the full self-test to verify regression status:
   ```bash
   tools/self-test.sh --quick          # fast: skip build, 100 logic frames + render test
   tools/self-test.sh --skip-build     # full: skip build, 400 logic frames + render test + telemetry
   tools/self-test.sh                  # full: rebuild + logic + render + telemetry
   ```
   The self-test mirrors CI exactly: Phase 1 (Noop logic), Phase 2 (softpipe render via
   `quick-test.sh`), milestone parsing with `--goal-log`, and telemetry validation.
4. If the self-test passes, commit and push — CI runs the same `quick-test.sh`
   in render-only mode with a 120s frame delay for CI hardware
5. If it fails, read the output to see which step failed and fix the issue

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
| `TP_VERIFY` | Set to `1` to enable subsystem verification | unset (disabled) |
| `TP_VERIFY_DIR` | Directory for captured frame BMPs | `verify_output` |
| `TP_VERIFY_CAPTURE_FRAMES` | Comma-separated frame numbers to capture | `10,129` (CI default; custom lists supported) |
| `TP_FRAME_DELAY_MS` | Sleep N ms before 3D frame capture (softpipe time) | unset (no delay) |
| `TP_FRAME_DELAY_START` | Frame number to start the delay at | `129` |
| `TP_TEST_INPUTS` | Enable synthetic input injection | unset (no injection) |
| `ROMS_TOKEN` | GitHub PAT for game data download | unset (skip download) |

## Subsystem Verification

The verification system (`TP_VERIFY=1`) provides automated health checks for three
subsystems: rendering, input, and audio. It works alongside the milestone system to
give agents concrete, actionable evidence of what's working and what to fix next.

### Rendering Verification

The rendering verification is **fully automated and dependable without human review**.
It uses seven verification layers, any of which can independently detect regressions:

#### Layer 1: Per-Frame Metrics
Each frame reports draw calls, vertex count, stub count, and validity flag.
Zero draw calls or zero vertices = broken rendering pipeline.

#### Layer 2: Framebuffer Hashing (CRC32)
The CPU-side software framebuffer (640×480 RGBA) produces a deterministic CRC32 hash
per frame. Same input data + same rendering code = identical hash. If a code change
causes a different hash on a previously-stable frame, the rendering changed.

The hash is logged per frame:
```json
{"verify_frame":{"frame":30,"draw_calls":12,"verts":480,"fb_hash":"0xABCD1234","fb_has_draws":1,
  "textured_draws":10,"untextured_draws":2,"unique_textures":5,"shader_mask":7,
  "depth_draws":11,"blend_draws":3,"prim_mask":400}}
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
    "10": {
      "min_draw_calls": 0,
      "min_pct_nonblack": 0,
      "note": "Frame 10: title screen (2D)"
    },
    "129": {
      "min_draw_calls": 0,
      "min_pct_nonblack": 0,
      "note": "Frame 129: 3D gameplay room"
    }
  },
  "global": {
    "min_peak_draw_calls": 1,
    "min_render_health_pct": 0
  }
}
```

Any metric that falls below the baseline is flagged as a regression.

#### Layer 6: Render Pipeline Stage Health
Checks each stage of the GX → bgfx rendering pipeline individually. If a stage
is broken, the agent gets **actionable guidance** on exactly what to fix:

| Stage | What It Checks | If Broken |
|---|---|---|
| **Geometry** | Are vertices submitted? | Fix GXBegin/GXPosition/GXEnd → pal_tev_flush_draw() |
| **Textures** | Are GX textures decoded? | Fix GXLoadTexObj → pal_gx_decode_texture() |
| **Shaders** | Are textured shaders used? | Fix TEV preset detection (REPLACE/MODULATE) |
| **Depth** | Is z-buffering active? | Ensure GXSetZMode sets z_compare_enable |
| **Blending** | Is alpha blending working? | Check GXSetBlendMode state |
| **Primitives** | Are multiple prim types used? | Check GXBegin prim type conversion |

Each stage can be gated in `tests/render-baseline.json` via the `pipeline` section:
```json
{
  "pipeline": {
    "require_geometry": true,
    "require_textures": false,
    "require_textured_shaders": false,
    "require_depth": false
  }
}
```

As rendering progresses, enable requirements to prevent regressions. Once textures
work, set `require_textures: true` so it can never regress.

A **visual complexity score** (0-100) summarizes overall pipeline quality:
- 0-25: Only basic geometry (no textures, no shaders)
- 25-50: Textures loading, basic shaders
- 50-75: Multiple shaders, depth testing, blending
- 75-100: Full pipeline — multiple textures, shaders, depth, blend, primitive types

#### Layer 7: Frame Progression
Detects if the game is **stuck** or **frozen** by tracking framebuffer hash changes:
- If the hash never changes across 60+ frames → game may be stuck in a loop
- If all captured frames have identical hashes → scene content isn't progressing

This catches bugs where the game renders but never advances past the clear color.

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
tools/quick-test.sh                  ← Fast render test (~10-90s, used by both local + CI)
tools/self-test.sh                   ← Full local test (build+logic+render+parse+verify+telemetry)
tools/parse_milestones.py            ← Parse milestone log → JSON summary
tools/check_milestone_regression.py  ← Compare against baseline, detect regressions
tools/verify_port.py                 ← Subsystem verification (rendering/input/audio)
tools/validate_telemetry.py          ← Draw-call telemetry validation (TEV, J3D, Z/Blend)
tools/check_bmp_coverage.py          ← Pixel coverage analysis for captured frames
tools/convert_frames.py              ← BMP→PNG conversion + MP4 video generation
tools/crash-report.sh                ← GDB crash report (stack traces, deduplication)
tools/setup_game_data.py             ← Download + extract game data
.github/workflows/port-test.yml      ← CI pipeline (build, test, verify, comment on PR)
include/pal/pal_milestone.h          ← Milestone C API
src/pal/pal_milestone.cpp            ← Milestone state management
include/pal/pal_verify.h             ← Verification system C API
src/pal/pal_verify.cpp               ← Verification system implementation
include/pal/gx/gx_capture.h        ← bgfx frame capture buffer + hash
src/pal/gx/gx_capture.cpp          ← bgfx frame capture implementation
include/pal/gx/gx_stub_tracker.h    ← GX stub hit tracker
src/pal/gx/gx_stub_tracker.cpp      ← Stub tracker implementation
src/pal/pal_crash.cpp                ← Crash handler (signal → JSON)
docs/port-progress.md                ← Progress tracker (session log, checklist)
docs/multiplatform-port-plan.md      ← Full port strategy
docs/agent-port-workflow.md          ← Step-by-step implementation guide
```
