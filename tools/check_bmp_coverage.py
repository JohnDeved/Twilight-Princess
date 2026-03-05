#!/usr/bin/env python3
"""check_bmp_coverage.py - Report pixel coverage (pct_nonblack) for BMP files.

Usage:
    python3 tools/check_bmp_coverage.py verify_output_3d/frame_*.bmp

Emits one JSON line per file to stdout:
    {"phase3_bmp": "...", "pct_nonblack": N, "nonblack_pixels": M, "total_pixels": T, "avg_rgb": [R, G, B]}

Used by Phase 3 CI to verify that 3D gameplay frames contain visible geometry.
A pct_nonblack > 0 in frames 128-129 confirms the J3D 3D intro sequence rendered
through the full bgfx → Mesa softpipe pipeline.
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
        "phase3_bmp": str(path),
        "pct_nonblack": pct,
        "nonblack_pixels": nonblack,
        "total_pixels": total,
        "avg_rgb": [avg_r, avg_g, avg_b],
    }


def main():
    if len(sys.argv) < 2:
        print("Usage: check_bmp_coverage.py <file.bmp> [file.bmp ...]", file=sys.stderr)
        sys.exit(0)

    any_visible = False
    for path in sys.argv[1:]:
        p = Path(path)
        if not p.is_file():
            continue
        result = analyze_bmp(str(p))
        print(json.dumps(result))
        if result.get("pct_nonblack", 0) > 0:
            any_visible = True

    if any_visible:
        print('{"phase3_result":"visible_3d_frame_confirmed"}')
    else:
        print('{"phase3_result":"all_frames_black"}')


if __name__ == "__main__":
    main()
