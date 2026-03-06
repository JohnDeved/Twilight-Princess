# PROGRESS.md — PC Port: GX Render Backend, Frame-Comparison Testing, and CI Automation

## Session Summary

**Date**: 2026-03-06
**Branch**: `copilot/advance-pc-port-render-engine`
**Base branch**: `copilot/sub-pr-14`
**Milestone baseline**: 17/17 milestones passing

---

## What Was Accomplished

### 1. Frame-Comparison Testing Tool (`tools/frame_compare/`)

Created a standalone Python tool that performs pixel-accurate and perceptual comparison between PC-port frame captures and GameCube reference screenshots:

- **`tools/frame_compare/frame_compare.py`** — Full-featured CLI tool with:
  - RMSE metric (primary pass/fail, normalised 0–1)
  - SSIM metric (structural similarity, advisory — requires `scikit-image`)
  - Pct-diff metric (percentage of differing pixels)
  - Side-by-side diff image output (captured | reference | amplified diff ×8)
  - Single-frame and batch modes
  - `--allow-missing-reference` for graceful handling when no references exist
  - `--update-reference` to seed reference frames from captured output
  - JSON output for CI artifact ingestion
  - Pure stdlib BMP I/O (no Pillow dependency for basic operation)
- **`tools/frame_compare/README.md`** — Comprehensive usage documentation including threshold guidance, Dolphin screenshot workflow, and CI integration notes

### 2. Reference Frame Infrastructure (`tests/reference_frames/`)

- Created **`tests/reference_frames/README.md`** explaining:
  - How to capture reference frames from Dolphin (GameCube emulator)
  - Naming convention (`frame_0030.bmp`, etc.)
  - How to commit and update references
  - Threshold guidance for early development vs. production

> **Note**: No actual reference BMP/PNG files are committed yet — the Dolphin
> emulator and GCN disc image are required to generate them. The CI step runs
> with `--allow-missing-reference` and exits 0 until references are provided.
> See `tests/reference_frames/README.md` for setup instructions.

### 3. CI Pipeline Extension (`.github/workflows/port-test.yml`)

Extended the existing `port-test.yml` workflow with:

- **Frame comparison step** — Runs `frame_compare.py` in batch mode after all
  headless test phases. Uses `--allow-missing-reference` so CI does not fail
  when no references are present.
- **Frame diff artifact** — `frame_compare_diffs/` directory added to the
  `port-test-results` artifact upload.
- **Frame compare results JSON** — `frame-compare-results.json` added to artifact
  upload and parsed in the PR comment step.
- **PR comment section** — New "🎯 Frame Comparison (GCN Reference)" section in the
  PR comment showing per-frame RMSE, SSIM, and pass/fail status.

### 4. Automated Review Helpers (`.github/workflows/render-review.yml`)

New workflow that runs on every PR touching render backend files:

- **clang-format check** — Checks changed C/C++ files in `src/pal/` and
  `include/pal/` against `.clang-format`. Posts a suggestion comment with the
  diff if formatting differs.
- **cppcheck static analysis** — Runs `cppcheck` with
  `--enable=warning,performance,portability` on the GX render backend
  (`src/pal/gx/`). Emits GitHub check annotations and posts a summary comment.
- **Frame diff image posting** — Downloads frame diff images from the latest
  `port-test` run's artifact and posts a comment linking reviewers to them.

### 5. Developer Inner-Loop Script (`tools/render_dev.py`)

Created `tools/render_dev.py` — a single command that automates the full
render development cycle:

```
python3 tools/render_dev.py --frames 120
```

Steps performed:
1. **Build** — `cmake -B build -G Ninja && ninja tp-pc`
2. **Run** — Launch `tp-pc` headlessly, capture frames at specified frame numbers
3. **Compare** — Run `frame_compare.py` against `tests/reference_frames/`
4. **Open diff** (optional with `--open-diff`) — Opens diff images in system viewer

Options: `--skip-build`, `--build-only`, `--compare-only`, `--capture-frames`,
`--threshold`, `--allow-missing-reference`, `--no-compare`, `--open-diff`.

---

## What Was Not Done and Why

### GX render backend extensions (TEV combiner improvements)

**Status**: Not implemented in this session.

The existing `src/pal/gx/gx_tev.cpp` already has a working TEV preset shader
pipeline (PASSCLR / REPLACE / MODULATE / BLEND / DECAL) that achieves 17/17
milestones. Extending it to support arbitrary TEV combiners requires:

1. A dynamic shader generation system (the current approach uses fixed presets
   compiled to GLSL/SPIR-V).
2. Implementation of TEV color combiner formula: `d + lerp(a, b, c) * scale ± bias`
   for all `GXTevColorArg` inputs.
3. Alpha compare (`GXSetAlphaCompare`).
4. Texture sampling improvements (currently only basic REPLACE/MODULATE).

This is blocked by: (a) the bgfx `BGFX_BUILD_TOOLS=OFF` constraint means
precompiled shader binaries are required; (b) runtime shader generation would
require `shaderc` at runtime or a much larger set of precompiled variants.

**Next steps for TEV combiner work**:
- Generate precompiled variants for common TEV combiner combinations used by
  Twilight Princess (see `rasc_geom_dump` CI logs for which stage configs appear).
- Alternatively, implement a single "uber-shader" that encodes all combiner
  inputs as uniforms.
- Priority TEV inputs: `GX_CC_TEXC`, `GX_CC_RASC`, `GX_CC_KONST`, `GX_CC_ZERO`
  with `GX_TEV_ADD` and `GX_TEV_SUB` ops.

---

## What Is Blocked

### Reference frames
- Requires Dolphin emulator + GCN disc image to generate ground-truth captures.
- These are not available in CI (CI uses headless Mesa llvmpipe, not Dolphin).
- **Action needed**: A developer with access to the GCN disc must run Dolphin,
  capture frames at the same deterministic state as the PC port, and commit them
  to `tests/reference_frames/`.

### YAML validation with `actionlint`
- `actionlint` is not installed in the sandboxed build environment.
- YAML syntax was manually validated. Both workflows are well-formed YAML.
- Run `actionlint .github/workflows/render-review.yml .github/workflows/port-test.yml`
  locally to verify.

---

## Architectural Decisions

| Decision | Rationale |
|---|---|
| Pure stdlib BMP I/O in `frame_compare.py` | Avoids hard Pillow dependency; CI installs it as a best-effort `pip install` |
| `--allow-missing-reference` as default in CI | Prevents blocking all CI runs until someone provides Dolphin captures |
| RMSE as primary metric (not SSIM) | SSIM requires `scikit-image`; RMSE is deterministic and stdlib-computable |
| Separate `render-review.yml` workflow | Keeps format/static-analysis concerns separate from the headless test run |
| `tools/render_dev.py` as Python, not shell | Cross-platform (macOS/Windows/Linux); cleaner argument parsing |

---

## Files Created / Modified

### New files
- `tools/frame_compare/frame_compare.py` — Frame comparison CLI tool
- `tools/frame_compare/README.md` — Usage documentation
- `tests/reference_frames/README.md` — Reference frame setup guide
- `tools/render_dev.py` — Developer inner-loop script
- `.github/workflows/render-review.yml` — Automated review helpers workflow
- `PROGRESS.md` — This file

### Modified files
- `.github/workflows/port-test.yml` — Added frame comparison step, frame diff
  artifact upload, and PR comment section

---

## Next Session Checklist

- [ ] Generate reference frames with Dolphin and commit to `tests/reference_frames/`
- [ ] Lower CI frame-compare threshold from 0.15 → 0.05 once references exist
- [ ] Implement TEV uber-shader or additional precompiled TEV combiner variants
- [ ] Implement `GXSetAlphaCompare` in the bgfx pipeline
- [ ] Add texture coordinate generation (GXSetTexCoordGen) for textured geometry
- [ ] Review `rasc_geom_dump` logs from CI to identify which TEV stage combinations
  appear most frequently and prioritize those for precompiled variants
- [ ] Validate `render-review.yml` with `actionlint`
- [ ] Remove `--allow-missing-reference` from CI once first reference frames are committed

---

## Session Log

### 2026-03-06
- Added `tools/frame_compare/` tool with stdlib BMP I/O and optional Pillow/scikit-image
- Added `tests/reference_frames/README.md`
- Extended `port-test.yml` with frame comparison step and PR comment section
- Created `render-review.yml` with clang-format, cppcheck, and frame diff posting
- Created `tools/render_dev.py` developer inner-loop script
- No regressions: milestone baseline remains 17/17
