# Reference Frames

This directory contains known-good reference screenshots from the original
GameCube build of Twilight Princess, captured via Dolphin emulator.

CI compares rendered frames from the PC port against these references using
`tools/frame_compare/frame_compare.py`.

## Status

No reference frames have been committed yet.  The CI frame-comparison step
currently runs with `--allow-missing-reference`, so it will **skip** comparisons
gracefully and exit 0 until reference frames are provided.

## How to add reference frames

1. Run the original game in [Dolphin](https://dolphin-emu.org/) at a known
   deterministic frame (e.g. frame 30, 60, 120 of the Nintendo logo scene).

2. Export a screenshot in BMP or PNG format from Dolphin using
   **Graphics → Save Screenshot** or the Dolphin CLI `--exec --movie` flags.

3. Name the file to match the PC-port capture convention:
   - `frame_0010.bmp` — frame 10
   - `frame_0030.bmp` — frame 30
   - `frame_0060.bmp` — frame 60
   - `frame_0090.bmp` — frame 90
   - `frame_0120.bmp` — frame 120
   - `frame_0300.bmp` — frame 300

4. Place the file in this directory and commit it:
   ```bash
   git add tests/reference_frames/
   git commit -m "tests: add GCN reference frames for frame_compare CI gate"
   ```

5. Once reference frames are present, lower the CI threshold gradually as
   rendering accuracy improves.  Start with `--threshold 0.15` (loose) and
   tighten to `0.05` (default) as the TEV pipeline matures.

## Updating references

If the PC port intentionally changes its rendering output (e.g. a TEV fix that
changes colours), update the references via:

```bash
python3 tools/frame_compare/frame_compare.py \
    --captured-dir  verify_output/ \
    --reference-dir tests/reference_frames/ \
    --update-reference
```

Always review the diff images before committing new references.
