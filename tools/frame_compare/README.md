# Frame Comparison Tool

A tool for comparing rendered frames from the Twilight Princess PC port against
reference screenshots from the original GameCube build (via Dolphin emulator).

## Overview

`frame_compare.py` performs pixel-by-pixel and perceptual comparison between a
captured PC-port frame and a known-good GameCube reference frame. It outputs:

- **RMSE** (Root Mean Square Error, normalised to [0, 1])
- **SSIM** (Structural Similarity Index, [0, 1]) — requires `scikit-image`
- **Pct-diff** (percentage of pixels differing beyond a tolerance)
- A **side-by-side diff image** (captured | reference | amplified diff) — requires `Pillow`
- A **JSON result file** suitable for CI artifact ingestion

## Dependencies

```
# Required for PNG output and image resizing:
pip install Pillow

# Required for SSIM metric:
pip install scikit-image numpy
```

The tool works without these dependencies using only the Python standard library,
but is limited to BMP input, no PNG diff output, and no SSIM metric.

## Usage

### Single-frame comparison

```bash
python3 tools/frame_compare/frame_compare.py \
    --captured  verify_output/frame_0030.bmp \
    --reference tests/reference_frames/frame_0030.bmp \
    --diff-out  /tmp/diffs/diff_0030.png \
    --threshold 0.05
```

### Batch comparison

```bash
python3 tools/frame_compare/frame_compare.py \
    --captured-dir  verify_output/ \
    --reference-dir tests/reference_frames/ \
    --diff-dir      /tmp/diffs/ \
    --threshold     0.05 \
    --json-out      /tmp/frame_compare_results.json
```

### No reference files yet

If `tests/reference_frames/` is empty or does not exist, use
`--allow-missing-reference` to skip comparisons gracefully:

```bash
python3 tools/frame_compare/frame_compare.py \
    --captured-dir  verify_output/ \
    --reference-dir tests/reference_frames/ \
    --allow-missing-reference
```

This exits 0 and explains what to do next.

### Creating or updating reference frames

1. Run a headless capture to produce BMP frames:
   ```bash
   TP_HEADLESS=1 TP_TEST_FRAMES=200 TP_VERIFY=1 TP_VERIFY_DIR=verify_output \
       TP_VERIFY_CAPTURE_FRAMES=10,30,60,90,120 ./build/tp-pc
   ```

2. Copy the frames you accept as ground truth into the reference directory:
   ```bash
   python3 tools/frame_compare/frame_compare.py \
       --captured-dir  verify_output/ \
       --reference-dir tests/reference_frames/ \
       --update-reference
   ```
   Or, for Dolphin GCN screenshots, simply copy the BMP/PNG files manually
   into `tests/reference_frames/` with matching names (`frame_0030.bmp`, etc.).

3. Commit the reference frames:
   ```bash
   git add tests/reference_frames/
   git commit -m "tests: add reference frames for frame_compare CI gate"
   ```

## Threshold guidance

| Threshold | Meaning |
|-----------|---------|
| `0.01`    | Near-pixel-perfect — only minor numerical differences allowed |
| `0.05`    | Default — allows minor colour-space conversion differences |
| `0.15`    | Loose — allows noticeable visual differences (early development) |

The threshold applies to **normalised RMSE** (range 0–1 where 0 = identical and
1 = maximum possible difference). SSIM is reported but currently advisory only.

## CI integration

The CI workflow (`.github/workflows/port-test.yml`) runs `frame_compare.py` in
batch mode after every headless test run. See the `Frame comparison` step in that
workflow for details.

## Exit codes

| Code | Meaning |
|------|---------|
| `0`  | All comparisons passed (or no references found with `--allow-missing-reference`) |
| `1`  | At least one comparison exceeded the threshold |
| `2`  | Usage / input error |
