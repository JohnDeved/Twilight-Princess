#!/usr/bin/env python3
"""check_bmp_coverage.py - Report pixel coverage (pct_nonblack) for BMP files.

Usage:
    python3 tools/check_bmp_coverage.py verify_output_3d/frame_*.bmp
    python3 tools/check_bmp_coverage.py verify_output/frame_0120.bmp --require frame_0120:80

Emits one JSON line per file to stdout:
    {"bmp": "...", "pct_nonblack": N, "nonblack_pixels": M, "total_pixels": T, "avg_rgb": [R, G, B]}

Options:
    --require PATTERN:MIN_PCT
        Assert that any file whose path contains PATTERN has pct_nonblack >= MIN_PCT.
        Can be specified multiple times.  Exits with code 1 if any requirement fails.
        Example: --require frame_0120:80

Used by Phase 3 CI to verify that 3D gameplay frames contain visible geometry.
A pct_nonblack > 0 in frame 129 confirms the J3D 3D intro sequence rendered
through the full bgfx → Mesa softpipe pipeline.
Used by Phase 2/4 CI as a RASC regression gate: frame_0120 must stay >= 80% nonblack
to confirm the GX_CC_RASC → material-color injection path is still working.
"""

import json
import struct
import sys
from pathlib import Path


NONBLACK_THRESHOLD = 4  # pixel channel value > 4 counts as nonblack


def analyze_bmp(path: str) -> dict:
    """Read a BMP file and compute pixel coverage statistics."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 54 or data[0:2] != b"BM":
        return {"error": "not a valid BMP", "file": path}

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    pixel_data = data[pixel_offset:]
    # BMP pixel rows are BGRA (32-bit) or BGR (24-bit); detect from bits-per-pixel
    bpp = struct.unpack_from("<H", data, 28)[0]
    stride = bpp // 8  # bytes per pixel
    total = len(pixel_data) // stride

    if total == 0:
        return {"error": "empty pixel data", "file": path}

    nonblack = 0
    r_sum = g_sum = b_sum = 0
    for i in range(total):
        off = i * stride
        b = pixel_data[off]
        g = pixel_data[off + 1]
        r = pixel_data[off + 2]
        if r > NONBLACK_THRESHOLD or g > NONBLACK_THRESHOLD or b > NONBLACK_THRESHOLD:
            nonblack += 1
        r_sum += r
        g_sum += g
        b_sum += b

    pct = nonblack * 100 // total
    avg_r = r_sum // total
    avg_g = g_sum // total
    avg_b = b_sum // total

    return {
        "bmp": str(path),
        "pct_nonblack": pct,
        "nonblack_pixels": nonblack,
        "total_pixels": total,
        "avg_rgb": [avg_r, avg_g, avg_b],
    }


def _parse_args(argv):
    """Parse argv into (bmp_paths, require_specs).

    require_specs is a list of (pattern, min_pct) tuples from --require PATTERN:MIN_PCT.
    """
    bmp_paths = []
    require_specs = []
    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg == "--require" and i + 1 < len(argv):
            spec = argv[i + 1]
            i += 2
            if ":" not in spec:
                print(f"[check_bmp_coverage] --require must be PATTERN:MIN_PCT, got: {spec!r}",
                      file=sys.stderr)
                continue
            pattern, min_pct_str = spec.rsplit(":", 1)
            try:
                min_pct = int(min_pct_str)
            except ValueError:
                print(f"[check_bmp_coverage] invalid MIN_PCT in --require {spec!r}", file=sys.stderr)
                continue
            require_specs.append((pattern, min_pct))
        else:
            bmp_paths.append(arg)
            i += 1
    return bmp_paths, require_specs


def main():
    if len(sys.argv) < 2:
        print("Usage: check_bmp_coverage.py <file.bmp> [file.bmp ...] [--require PATTERN:MIN_PCT]",
              file=sys.stderr)
        sys.exit(0)

    bmp_paths, require_specs = _parse_args(sys.argv)

    any_visible = False
    frame_129_visible = False
    results = []
    for path in bmp_paths:
        p = Path(path)
        if not p.is_file():
            continue
        result = analyze_bmp(str(p))
        print(json.dumps(result))
        results.append(result)
        if result.get("pct_nonblack", 0) > 0:
            any_visible = True
            # Check specifically for frame 129 (3D room frame, captured via
            # TP_VERIFY_CAPTURE_FRAMES="129" with 1-frame async lag = frame 128 pixels)
            if "frame_0129" in str(p):
                frame_129_visible = True

    if frame_129_visible:
        print('{"phase3_result":"visible_3d_frame_confirmed"}')
    elif any_visible:
        print('{"phase3_result":"title_screen_only_nonblack"}')
    else:
        print('{"phase3_result":"all_frames_black"}')

    # Evaluate --require assertions
    failures = []
    for pattern, min_pct in require_specs:
        matched = [r for r in results if pattern in r.get("bmp", "")]
        if not matched:
            msg = f"RASC gate MISSING: no BMP matching {pattern!r} (pattern required >= {min_pct}%)"
            print(f'{{"rasc_gate":"missing","pattern":{json.dumps(pattern)},"min_pct":{min_pct}}}')
            failures.append(msg)
            continue
        for r in matched:
            pct = r.get("pct_nonblack", 0)
            avg = r.get("avg_rgb", [0, 0, 0])
            file_name = r.get("bmp", pattern)
            if pct < min_pct:
                msg = (f"RASC gate FAIL: {file_name} pct_nonblack={pct}% "
                       f"< required {min_pct}% (avg_rgb={avg})")
                print(f'{{"rasc_gate":"fail","file":{json.dumps(file_name)},'
                      f'"pct_nonblack":{pct},"min_pct":{min_pct},"avg_rgb":{avg}}}')
                failures.append(msg)
            else:
                print(f'{{"rasc_gate":"pass","file":{json.dumps(file_name)},'
                      f'"pct_nonblack":{pct},"min_pct":{min_pct},"avg_rgb":{avg}}}')

    if failures:
        for f in failures:
            print(f"[check_bmp_coverage] FAIL: {f}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
