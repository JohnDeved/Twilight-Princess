#!/usr/bin/env python3
"""
render_dev.py - Developer inner-loop tool for the Twilight Princess PC port.

Automates the day-to-day render development cycle:
  1. Build the PC port (CMake + Ninja)
  2. Launch it headlessly to a specific scene / frame count
  3. Capture a screenshot
  4. Run frame_compare against the reference
  5. Open the diff image (optional)

Usage:
    python3 tools/render_dev.py [options]

Examples:
    # Full cycle: build → run → compare → open diff
    python3 tools/render_dev.py --frames 120

    # Skip build (fast iteration):
    python3 tools/render_dev.py --skip-build --frames 30

    # Custom capture frames:
    python3 tools/render_dev.py --skip-build --capture-frames 10,30,60,120

    # Use a specific binary:
    python3 tools/render_dev.py --binary build/tp-pc --frames 200

    # Only build (no run):
    python3 tools/render_dev.py --build-only

    # Only compare existing captures:
    python3 tools/render_dev.py --compare-only --captured-dir verify_output

See tools/frame_compare/README.md for reference frame management.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
DEFAULT_BINARY = BUILD_DIR / "tp-pc"
DEFAULT_VERIFY_DIR = REPO_ROOT / "verify_output_dev"
DEFAULT_DIFF_DIR = REPO_ROOT / "verify_diff_dev"
DEFAULT_REFERENCE_DIR = REPO_ROOT / "tests" / "reference_frames"
FRAME_COMPARE_TOOL = REPO_ROOT / "tools" / "frame_compare" / "frame_compare.py"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run(cmd, **kwargs):
    """Run a subprocess command; raise on failure."""
    print(f"  $ {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)
    return result


def cmake_build(build_type: str = "RelWithDebInfo") -> None:
    """Configure and build the PC port."""
    print("\n=== Build ===")
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    run([
        "cmake", "-B", str(BUILD_DIR), "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DTP_VERSION=PC",
    ], cwd=REPO_ROOT)
    run(["ninja", "-C", str(BUILD_DIR), "tp-pc"], cwd=REPO_ROOT)
    print("  Build succeeded.")


def run_headless(
    binary: Path,
    frames: int,
    capture_frames: str,
    verify_dir: Path,
    extra_env: dict,
) -> None:
    """Launch tp-pc in headless mode to capture frames."""
    print(f"\n=== Run headless (frames={frames}, capture={capture_frames}) ===")
    verify_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env.update({
        "TP_HEADLESS":              "1",
        "TP_TEST_FRAMES":           str(frames),
        "TP_VERIFY":                "1",
        "TP_VERIFY_DIR":            str(verify_dir),
        "TP_VERIFY_CAPTURE_FRAMES": capture_frames,
        "TP_BMP_INTERVAL":          "9999",
    })
    env.update(extra_env)

    # Wrap in Xvfb on Linux if no display is available
    if platform.system() == "Linux" and not os.environ.get("DISPLAY"):
        xvfb = shutil.which("xvfb-run")
        if xvfb:
            cmd = [xvfb, "-a", str(binary)]
        else:
            cmd = [str(binary)]
            print("  WARNING: No DISPLAY and xvfb-run not found — may fail on Linux.")
    else:
        cmd = [str(binary)]

    result = subprocess.run(cmd, env=env, cwd=REPO_ROOT)
    if result.returncode != 0:
        print(f"  WARNING: tp-pc exited with code {result.returncode} (may be expected in headless mode)")

    bmp_files = sorted(verify_dir.glob("*.bmp"))
    print(f"  Captured {len(bmp_files)} frame(s) in {verify_dir}")
    for f in bmp_files:
        size_kb = f.stat().st_size // 1024
        print(f"    {f.name}  ({size_kb} KB)")


def run_frame_compare(
    captured_dir: Path,
    reference_dir: Path,
    diff_dir: Path,
    threshold: float,
    allow_missing: bool,
    json_out: Path,
) -> int:
    """Run frame_compare.py and return its exit code."""
    print(f"\n=== Frame comparison (threshold={threshold}) ===")
    diff_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable, str(FRAME_COMPARE_TOOL),
        "--captured-dir",   str(captured_dir),
        "--reference-dir",  str(reference_dir),
        "--diff-dir",       str(diff_dir),
        "--threshold",      str(threshold),
        "--json-out",       str(json_out),
    ]
    if allow_missing:
        cmd.append("--allow-missing-reference")

    result = subprocess.run(cmd, cwd=REPO_ROOT)
    return result.returncode


def open_diff_images(diff_dir: Path) -> None:
    """Open diff images in the system image viewer."""
    diff_files = sorted(diff_dir.glob("*_diff.*"))
    if not diff_files:
        print("  No diff images found.")
        return

    system = platform.system()
    for f in diff_files:
        print(f"  Opening {f.name} …")
        try:
            if system == "Darwin":
                subprocess.Popen(["open", str(f)])
            elif system == "Windows" and hasattr(os, "startfile"):
                os.startfile(str(f))  # type: ignore[attr-defined]
            else:
                viewer = shutil.which("eog") or shutil.which("feh") or shutil.which("display")
                if viewer:
                    subprocess.Popen([viewer, str(f)])
                else:
                    print(f"    (no image viewer found — view manually: {f})")
        except Exception as exc:
            print(f"    Failed to open: {exc}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Twilight Princess PC port — render development inner loop.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    # Build options
    p.add_argument("--skip-build", action="store_true",
                   help="Skip CMake build (use existing binary)")
    p.add_argument("--build-only", action="store_true",
                   help="Only build, do not run or compare")
    p.add_argument("--build-type", default="RelWithDebInfo",
                   help="CMake build type (default: RelWithDebInfo)")

    # Run options
    p.add_argument("--binary", default=str(DEFAULT_BINARY), metavar="PATH",
                   help=f"tp-pc binary path (default: {DEFAULT_BINARY})")
    p.add_argument("--frames", type=int, default=200,
                   help="Number of frames to run (default: 200)")
    p.add_argument("--capture-frames", default="10,30,60,90,120",
                   help="Comma-separated frame numbers to capture (default: 10,30,60,90,120)")
    p.add_argument("--verify-dir", default=str(DEFAULT_VERIFY_DIR), metavar="DIR",
                   help=f"Directory for captured BMP frames (default: {DEFAULT_VERIFY_DIR})")

    # Compare options
    p.add_argument("--compare-only", action="store_true",
                   help="Skip build and run; only compare existing captures")
    p.add_argument("--captured-dir", metavar="DIR",
                   help="Override captured frames directory for comparison")
    p.add_argument("--reference-dir", default=str(DEFAULT_REFERENCE_DIR), metavar="DIR",
                   help=f"Reference frames directory (default: {DEFAULT_REFERENCE_DIR})")
    p.add_argument("--diff-dir", default=str(DEFAULT_DIFF_DIR), metavar="DIR",
                   help=f"Diff image output directory (default: {DEFAULT_DIFF_DIR})")
    p.add_argument("--threshold", type=float, default=0.05,
                   help="RMSE threshold for pass/fail (default: 0.05)")
    p.add_argument("--allow-missing-reference", action="store_true", default=True,
                   help="Skip frames with no reference (default: True)")
    p.add_argument("--no-allow-missing-reference", dest="allow_missing_reference",
                   action="store_false",
                   help="Fail if any captured frame has no reference")

    # Misc
    p.add_argument("--open-diff", action="store_true",
                   help="Open diff images in system viewer after comparison")
    p.add_argument("--no-compare", action="store_true",
                   help="Skip frame comparison (only build + run)")

    args = p.parse_args(argv)

    binary = Path(args.binary)
    verify_dir = Path(args.verify_dir)
    diff_dir = Path(args.diff_dir)
    reference_dir = Path(args.reference_dir)
    json_out = diff_dir / "frame_compare_results.json"

    # --- Build ---
    if not args.skip_build and not args.compare_only:
        cmake_build(args.build_type)

    if args.build_only:
        print("\nBuild complete. Exiting (--build-only).")
        return 0

    # --- Run ---
    if not args.compare_only:
        if not binary.exists():
            print(f"ERROR: Binary not found: {binary}", file=sys.stderr)
            print("  Run without --skip-build to build it first, or pass --binary PATH.",
                  file=sys.stderr)
            return 2

        run_headless(
            binary=binary,
            frames=args.frames,
            capture_frames=args.capture_frames,
            verify_dir=verify_dir,
            extra_env={},
        )

    # --- Compare ---
    if not args.no_compare:
        cap_dir = Path(args.captured_dir) if args.captured_dir else verify_dir
        rc = run_frame_compare(
            captured_dir=cap_dir,
            reference_dir=reference_dir,
            diff_dir=diff_dir,
            threshold=args.threshold,
            allow_missing=args.allow_missing_reference,
            json_out=json_out,
        )

        if args.open_diff:
            open_diff_images(diff_dir)

        if rc != 0:
            print(f"\nFrame comparison FAILED (exit {rc})", file=sys.stderr)
            return rc

    print("\nrender_dev.py completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
