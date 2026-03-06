#!/usr/bin/env python3
"""
frame_compare.py - Frame comparison tool for the Twilight Princess PC port.

Captures a rendered frame from the PC port at a deterministic game state and
compares it against a reference screenshot (e.g. from a Dolphin GC emulator
capture) using pixel-by-pixel RMSE and perceptual SSIM metrics.

Usage:
    # Compare a single captured BMP against a reference:
    python3 tools/frame_compare/frame_compare.py \\
        --captured verify_output/frame_0030.bmp \\
        --reference tests/reference_frames/frame_0030.bmp \\
        --diff-out /tmp/diff_0030.png \\
        --threshold 0.05

    # Batch-compare an entire directory of captures against references:
    python3 tools/frame_compare/frame_compare.py \\
        --captured-dir verify_output/ \\
        --reference-dir tests/reference_frames/ \\
        --diff-dir /tmp/diffs/ \\
        --threshold 0.05

    # Graceful-no-reference mode (no reference files exist yet):
    python3 tools/frame_compare/frame_compare.py \\
        --captured-dir verify_output/ \\
        --reference-dir tests/reference_frames/ \\
        --allow-missing-reference

Exit codes:
    0 = pass  (all comparisons within threshold, or no references found with
               --allow-missing-reference)
    1 = fail  (at least one comparison exceeded the threshold)
    2 = usage / input error
"""

import argparse
import json
import math
import os
import struct
import sys
from typing import Optional, Tuple

# ---------------------------------------------------------------------------
# Optional imports — graceful degradation when Pillow / scipy are absent
# ---------------------------------------------------------------------------
try:
    from PIL import Image  # type: ignore
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False

try:
    from skimage.metrics import structural_similarity as _ssim_fn  # type: ignore
    import numpy as _np
    _SSIM_AVAILABLE = True
except ImportError:
    _SSIM_AVAILABLE = False


# ---------------------------------------------------------------------------
# BMP reader (stdlib-only fallback — handles 24-bit and 32-bit BMP files)
# ---------------------------------------------------------------------------

def _read_bmp_stdlib(path: str) -> Tuple[int, int, list]:
    """Return (width, height, pixels) where pixels is a flat list of (R,G,B) tuples."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"Not a BMP file: {path}")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width        = struct.unpack_from("<i", data, 18)[0]
    height       = struct.unpack_from("<i", data, 22)[0]
    bit_count    = struct.unpack_from("<H", data, 28)[0]
    compression  = struct.unpack_from("<I", data, 30)[0]

    if compression != 0:
        raise ValueError(f"Compressed BMP not supported (compression={compression}): {path}")
    if bit_count not in (24, 32):
        raise ValueError(f"Only 24-bit and 32-bit BMP supported (got {bit_count}): {path}")

    flipped = height > 0          # positive height → bottom-up
    h = abs(height)
    bytes_per_pixel = bit_count // 8
    row_size = (width * bytes_per_pixel + 3) & ~3   # 4-byte aligned

    pixels = []
    for row in range(h):
        actual_row = (h - 1 - row) if flipped else row
        row_start = pixel_offset + actual_row * row_size
        for col in range(width):
            off = row_start + col * bytes_per_pixel
            b, g, r = data[off], data[off + 1], data[off + 2]
            pixels.append((r, g, b))

    return width, h, pixels


def load_image(path: str) -> Tuple[int, int, list]:
    """
    Load an image file and return (width, height, flat_pixel_list).
    flat_pixel_list contains (R, G, B) tuples in row-major order (top-left first).
    Supports BMP natively; PNG/JPEG require Pillow.
    """
    ext = os.path.splitext(path)[1].lower()
    if ext == ".bmp":
        try:
            return _read_bmp_stdlib(path)
        except Exception:
            pass  # fall through to Pillow if available

    if _PIL_AVAILABLE:
        img = Image.open(path).convert("RGB")
        w, h = img.size
        pixels = list(img.getdata())
        return w, h, pixels

    if ext == ".bmp":
        return _read_bmp_stdlib(path)   # re-raise

    raise RuntimeError(
        f"Cannot load {path!r}: Pillow is not installed. "
        "Install it with:  pip install Pillow"
    )


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def compute_rmse(pixels_a: list, pixels_b: list) -> float:
    """Root Mean Square Error across all channels in [0, 1] range."""
    if len(pixels_a) != len(pixels_b):
        raise ValueError(
            f"Image dimensions differ: {len(pixels_a)} vs {len(pixels_b)} pixels"
        )
    total = 0.0
    for (ra, ga, ba), (rb, gb, bb) in zip(pixels_a, pixels_b):
        total += (ra - rb) ** 2 + (ga - gb) ** 2 + (ba - bb) ** 2
    mse = total / (len(pixels_a) * 3)
    return math.sqrt(mse) / 255.0


def compute_ssim(
    w: int, h: int, pixels_a: list, pixels_b: list
) -> Optional[float]:
    """
    Structural Similarity Index (SSIM) in [0, 1].
    Returns None if scikit-image is not installed.
    """
    if not _SSIM_AVAILABLE:
        return None
    arr_a = _np.array(pixels_a, dtype=_np.uint8).reshape(h, w, 3)
    arr_b = _np.array(pixels_b, dtype=_np.uint8).reshape(h, w, 3)
    score = _ssim_fn(arr_a, arr_b, channel_axis=2, data_range=255)
    return float(score)


def compute_pct_diff(pixels_a: list, pixels_b: list, tolerance: int = 5) -> float:
    """Percentage of pixels that differ by more than `tolerance` in any channel."""
    differing = 0
    for (ra, ga, ba), (rb, gb, bb) in zip(pixels_a, pixels_b):
        if abs(ra - rb) > tolerance or abs(ga - gb) > tolerance or abs(ba - bb) > tolerance:
            differing += 1
    return 100.0 * differing / max(len(pixels_a), 1)


# ---------------------------------------------------------------------------
# Diff image generation
# ---------------------------------------------------------------------------

def save_diff_image(
    w: int, h: int, pixels_a: list, pixels_b: list, out_path: str
) -> None:
    """
    Save a side-by-side diff image: left=captured, middle=reference, right=amplified diff.
    Requires Pillow for PNG output; falls back to BMP.
    """
    diff_pixels = []
    for (ra, ga, ba), (rb, gb, bb) in zip(pixels_a, pixels_b):
        dr = min(255, abs(ra - rb) * 8)
        dg = min(255, abs(ga - gb) * 8)
        db = min(255, abs(ba - bb) * 8)
        diff_pixels.append((dr, dg, db))

    if _PIL_AVAILABLE:
        panel_w = w * 3
        out_img = Image.new("RGB", (panel_w, h))
        out_img.putdata(pixels_a)
        ref_img = Image.new("RGB", (w, h))
        ref_img.putdata(pixels_b)
        dif_img = Image.new("RGB", (w, h))
        dif_img.putdata(diff_pixels)
        composite = Image.new("RGB", (panel_w, h))
        composite.paste(out_img.crop((0, 0, w, h)), (0, 0))
        composite.paste(ref_img, (w, 0))
        composite.paste(dif_img, (w * 2, 0))
        composite.save(out_path)
    else:
        # BMP fallback (no Pillow required)
        bmp_path = os.path.splitext(out_path)[0] + ".bmp"
        panel_w = w * 3
        _save_bmp_raw(panel_w, h, pixels_a + pixels_b + diff_pixels, bmp_path)
        print(f"[frame_compare] Pillow not available; diff saved as {bmp_path}")


def _save_bmp_raw(w: int, h: int, pixels: list, path: str) -> None:
    """Write a minimal 24-bit BMP file without external dependencies."""
    row_size = (w * 3 + 3) & ~3
    pixel_data_size = row_size * h
    file_size = 54 + pixel_data_size

    header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII",
                      40, w, -h, 1, 24, 0, pixel_data_size, 2835, 2835, 0, 0)

    row_bytes_list = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = pixels[y * w + x]
            row += bytes([b, g, r])
        pad = row_size - w * 3
        row += bytes(pad)
        row_bytes_list.append(bytes(row))

    with open(path, "wb") as f:
        f.write(header + dib)
        for row in row_bytes_list:
            f.write(row)


# ---------------------------------------------------------------------------
# Single comparison
# ---------------------------------------------------------------------------

CompareResult = dict   # typed alias for readability


def compare_frames(
    captured_path: str,
    reference_path: str,
    diff_out: Optional[str],
    threshold: float,
) -> CompareResult:
    """
    Compare one captured frame against one reference frame.

    Returns a dict with keys:
        captured, reference, width, height,
        rmse, ssim (or None), pct_diff,
        passed, threshold, error (or None)
    """
    result: CompareResult = {
        "captured":   captured_path,
        "reference":  reference_path,
        "width":      None,
        "height":     None,
        "rmse":       None,
        "ssim":       None,
        "pct_diff":   None,
        "passed":     False,
        "threshold":  threshold,
        "error":      None,
    }
    try:
        wc, hc, pix_c = load_image(captured_path)
        wr, hr, pix_r = load_image(reference_path)

        if wc != wr or hc != hr:
            # Resize reference to captured dimensions if Pillow is available
            if _PIL_AVAILABLE:
                ref_img = Image.open(reference_path).convert("RGB").resize(
                    (wc, hc), Image.Resampling.LANCZOS if hasattr(Image, "Resampling") else Image.LANCZOS
                )
                pix_r = list(ref_img.getdata())
                wr, hr = wc, hc
            else:
                result["error"] = (
                    f"Size mismatch: captured={wc}x{hc} vs reference={wr}x{hr}. "
                    "Install Pillow to enable auto-resize."
                )
                return result

        result["width"]  = wc
        result["height"] = hc
        result["rmse"]   = compute_rmse(pix_c, pix_r)
        result["ssim"]   = compute_ssim(wc, hc, pix_c, pix_r)
        result["pct_diff"] = compute_pct_diff(pix_c, pix_r)

        # Primary metric: RMSE (normalised to [0,1])
        result["passed"] = result["rmse"] <= threshold

        if diff_out:
            os.makedirs(os.path.dirname(os.path.abspath(diff_out)), exist_ok=True)
            save_diff_image(wc, hc, pix_c, pix_r, diff_out)

    except FileNotFoundError as exc:
        result["error"] = str(exc)
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"

    return result


# ---------------------------------------------------------------------------
# Batch comparison
# ---------------------------------------------------------------------------

def batch_compare(
    captured_dir: str,
    reference_dir: str,
    diff_dir: Optional[str],
    threshold: float,
    allow_missing: bool,
) -> Tuple[list, bool]:
    """
    Compare all BMP/PNG files in captured_dir against matching files in reference_dir.

    Returns (results_list, overall_passed).
    """
    exts = {".bmp", ".png", ".jpg", ".jpeg"}
    captured_files = sorted(
        f for f in os.listdir(captured_dir) if os.path.splitext(f)[1].lower() in exts
    )

    results = []
    overall_passed = True

    if not captured_files:
        print(f"[frame_compare] No captured frames found in {captured_dir!r}")
        return results, True

    for fname in captured_files:
        cap_path = os.path.join(captured_dir, fname)
        ref_path = os.path.join(reference_dir, fname)

        if not os.path.exists(ref_path):
            if allow_missing:
                print(f"[frame_compare] SKIP  {fname} (no reference — run with "
                      "--update-reference to create one)")
                continue
            else:
                print(f"[frame_compare] FAIL  {fname} (reference not found: {ref_path})")
                results.append({
                    "captured":  cap_path,
                    "reference": ref_path,
                    "error":     "Reference file not found",
                    "passed":    False,
                    "threshold": threshold,
                })
                overall_passed = False
                continue

        diff_out = None
        if diff_dir:
            stem = os.path.splitext(fname)[0]
            diff_out = os.path.join(diff_dir, stem + "_diff.png")

        res = compare_frames(cap_path, ref_path, diff_out, threshold)
        results.append(res)

        if res.get("error"):
            print(f"[frame_compare] ERROR {fname}: {res['error']}")
            overall_passed = False
        else:
            ssim_str = f"{res['ssim']:.4f}" if res["ssim"] is not None else "n/a"
            if res["passed"]:
                print(
                    f"[frame_compare] PASS  {fname}  "
                    f"rmse={res['rmse']:.4f}  ssim={ssim_str}  "
                    f"pct_diff={res['pct_diff']:.2f}%"
                )
            else:
                print(
                    f"[frame_compare] FAIL  {fname}  "
                    f"rmse={res['rmse']:.4f} > threshold={threshold}  "
                    f"ssim={ssim_str}  pct_diff={res['pct_diff']:.2f}%"
                )
                overall_passed = False

    return results, overall_passed


# ---------------------------------------------------------------------------
# Reference management
# ---------------------------------------------------------------------------

def update_reference(captured_path: str, reference_dir: str) -> None:
    """Copy a captured frame into the reference directory."""
    os.makedirs(reference_dir, exist_ok=True)
    fname = os.path.basename(captured_path)
    dst = os.path.join(reference_dir, fname)
    import shutil
    shutil.copy2(captured_path, dst)
    print(f"[frame_compare] Updated reference: {dst}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Compare captured PC-port frames against GCN reference frames.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    # Single-frame mode
    single = p.add_argument_group("Single-frame mode")
    single.add_argument("--captured",   metavar="PATH", help="Captured BMP/PNG path")
    single.add_argument("--reference",  metavar="PATH", help="Reference BMP/PNG path")
    single.add_argument("--diff-out",   metavar="PATH", help="Path to write diff image")

    # Batch mode
    batch = p.add_argument_group("Batch mode")
    batch.add_argument("--captured-dir",   metavar="DIR", help="Directory of captured frames")
    batch.add_argument("--reference-dir",  metavar="DIR",
                       default="tests/reference_frames",
                       help="Directory of reference frames (default: tests/reference_frames)")
    batch.add_argument("--diff-dir",       metavar="DIR", help="Directory to write diff images")

    # Options
    p.add_argument("--threshold", type=float, default=0.05,
                   help="RMSE threshold for pass/fail in [0,1] (default: 0.05)")
    p.add_argument("--allow-missing-reference", action="store_true",
                   help="Skip frames with no reference instead of failing")
    p.add_argument("--update-reference", action="store_true",
                   help="Copy captured frames to reference-dir (create/update references)")
    p.add_argument("--json-out", metavar="PATH",
                   help="Write JSON results to this file")
    p.add_argument("--quiet", action="store_true",
                   help="Suppress per-frame output (only summary)")

    return p


def main(argv=None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    results = []
    overall_passed = True

    # --- Update-reference mode ---
    if args.update_reference:
        if args.captured_dir:
            exts = {".bmp", ".png", ".jpg", ".jpeg"}
            for fname in sorted(os.listdir(args.captured_dir)):
                if os.path.splitext(fname)[1].lower() in exts:
                    update_reference(
                        os.path.join(args.captured_dir, fname),
                        args.reference_dir,
                    )
        elif args.captured:
            update_reference(args.captured, args.reference_dir)
        else:
            parser.error("--update-reference requires --captured or --captured-dir")
        return 0

    # --- Single frame mode ---
    if args.captured and args.reference:
        if not os.path.exists(args.captured):
            print(f"ERROR: Captured file not found: {args.captured}", file=sys.stderr)
            return 2
        if not os.path.exists(args.reference):
            if args.allow_missing_reference:
                print(
                    f"[frame_compare] SKIP: no reference at {args.reference!r}\n"
                    "  To create a reference, run with --update-reference."
                )
                return 0
            print(f"ERROR: Reference file not found: {args.reference}", file=sys.stderr)
            return 2

        result = compare_frames(args.captured, args.reference, args.diff_out, args.threshold)
        results = [result]
        if result.get("error"):
            print(f"ERROR: {result['error']}", file=sys.stderr)
            overall_passed = False
        elif result["passed"]:
            if not args.quiet:
                ssim_str = f"{result['ssim']:.4f}" if result["ssim"] is not None else "n/a"
                print(
                    f"PASS  rmse={result['rmse']:.4f}  "
                    f"ssim={ssim_str}  "
                    f"pct_diff={result['pct_diff']:.2f}%"
                )
        else:
            ssim_str = f"{result['ssim']:.4f}" if result["ssim"] is not None else "n/a"
            print(
                f"FAIL  rmse={result['rmse']:.4f} > threshold={args.threshold}  "
                f"ssim={ssim_str}  "
                f"pct_diff={result['pct_diff']:.2f}%"
            )
            overall_passed = False

    # --- Batch mode ---
    elif args.captured_dir:
        if not os.path.isdir(args.captured_dir):
            print(f"ERROR: Captured directory not found: {args.captured_dir}", file=sys.stderr)
            return 2
        if not os.path.isdir(args.reference_dir):
            if args.allow_missing_reference:
                print(
                    f"[frame_compare] Reference directory {args.reference_dir!r} does not exist.\n"
                    "  No comparisons performed. Run with --update-reference to create references."
                )
                return 0
            print(
                f"ERROR: Reference directory not found: {args.reference_dir}\n"
                "  Run with --allow-missing-reference to skip missing references, or\n"
                "  run with --update-reference to create them from captured frames.",
                file=sys.stderr,
            )
            return 2

        results, overall_passed = batch_compare(
            args.captured_dir,
            args.reference_dir,
            args.diff_dir,
            args.threshold,
            args.allow_missing_reference,
        )
    else:
        parser.error(
            "Specify either --captured + --reference (single mode) "
            "or --captured-dir (batch mode)"
        )

    # --- Summary ---
    n_pass = sum(1 for r in results if r.get("passed"))
    n_fail = sum(1 for r in results if not r.get("passed"))
    n_err  = sum(1 for r in results if r.get("error"))

    if not args.quiet:
        print(
            f"\n[frame_compare] Summary: {n_pass} passed, {n_fail} failed, "
            f"{n_err} errors  (threshold={args.threshold})"
        )
        if not _PIL_AVAILABLE:
            print("  NOTE: Pillow not installed — PNG output and auto-resize disabled.")
            print("        Install with:  pip install Pillow")
        if not _SSIM_AVAILABLE:
            print("  NOTE: scikit-image not installed — SSIM metric unavailable.")
            print("        Install with:  pip install scikit-image")

    # --- JSON output ---
    if args.json_out:
        json_out_abs = os.path.abspath(args.json_out)
        json_out_dir = os.path.dirname(json_out_abs)
        if json_out_dir:
            os.makedirs(json_out_dir, exist_ok=True)
        with open(args.json_out, "w") as f:
            json.dump({"results": results, "passed": overall_passed}, f, indent=2)
        print(f"[frame_compare] Results written to {args.json_out}")

    return 0 if overall_passed else 1


if __name__ == "__main__":
    sys.exit(main())
