#!/usr/bin/env python3
"""
Analyze port verification results — rendering, input, and audio health.

Parses the structured JSON output from pal_verify.cpp and produces a
human-readable report with pass/fail verdicts for each subsystem.

Usage:
    python3 tools/verify_port.py milestones.log \\
        --output verify-report.json \\
        --verify-dir verify_output \\
        --check-rendering \\
        --check-input \\
        --check-audio

Exit codes:
    0 = all requested checks passed
    1 = one or more checks failed
    2 = input error
"""
import json
import sys
import argparse
import struct
from pathlib import Path


def parse_log(logfile):
    """Parse verification JSON lines from the log file."""
    frames = []
    captures = []
    fb_analyses = []
    inputs = []
    input_responses = []
    audio_reports = []
    summary = None
    milestones = []
    stubs = []

    with open(logfile) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            if "verify_frame" in obj:
                frames.append(obj["verify_frame"])
            elif "verify_capture" in obj:
                captures.append(obj)
            elif "verify_fb" in obj:
                fb_analyses.append(obj["verify_fb"])
            elif "verify_input" in obj:
                inputs.append(obj["verify_input"])
            elif "verify_input_response" in obj:
                input_responses.append(obj["verify_input_response"])
            elif "verify_audio" in obj:
                audio_reports.append(obj["verify_audio"])
            elif "verify_summary" in obj:
                summary = obj["verify_summary"]
            elif "milestone" in obj:
                milestones.append(obj)
            elif "stub" in obj:
                stubs.append(obj)

    return {
        "frames": frames,
        "captures": captures,
        "fb_analyses": fb_analyses,
        "inputs": inputs,
        "input_responses": input_responses,
        "audio_reports": audio_reports,
        "summary": summary,
        "milestones": milestones,
        "stubs": stubs,
    }


def analyze_bmp(path):
    """Analyze a BMP file and return basic pixel statistics."""
    try:
        with open(path, "rb") as f:
            header = f.read(54)
            if len(header) < 54 or header[0:2] != b"BM":
                return None

            width = struct.unpack_from("<i", header, 18)[0]
            height = struct.unpack_from("<i", header, 22)[0]
            bpp = struct.unpack_from("<H", header, 28)[0]

            if bpp != 24:
                return None

            row_bytes = width * 3
            row_padding = (4 - (row_bytes % 4)) % 4

            total_pixels = width * abs(height)
            nonblack = 0
            total_r = 0
            total_g = 0
            total_b = 0

            for _y in range(abs(height)):
                row = f.read(row_bytes)
                if len(row) < row_bytes:
                    break
                for x in range(width):
                    b_val = row[x * 3]
                    g_val = row[x * 3 + 1]
                    r_val = row[x * 3 + 2]
                    if r_val > 2 or g_val > 2 or b_val > 2:
                        nonblack += 1
                    total_r += r_val
                    total_g += g_val
                    total_b += b_val
                f.read(row_padding)  # skip padding

            return {
                "width": width,
                "height": abs(height),
                "total_pixels": total_pixels,
                "nonblack_pixels": nonblack,
                "pct_nonblack": (nonblack * 100) // total_pixels if total_pixels > 0 else 0,
                "avg_color": [
                    total_r // total_pixels if total_pixels > 0 else 0,
                    total_g // total_pixels if total_pixels > 0 else 0,
                    total_b // total_pixels if total_pixels > 0 else 0,
                ],
            }
    except (IOError, struct.error):
        return None


def check_rendering(data, verify_dir):
    """Check rendering health. Returns (passed, details)."""
    result = {
        "checked": True,
        "passed": False,
        "details": {},
        "issues": [],
    }

    summary = data.get("summary")
    if not summary:
        result["issues"].append("No verify_summary found — is TP_VERIFY=1 set?")
        return result

    total_frames = summary.get("total_frames", 0)
    frames_with_draws = summary.get("frames_with_draws", 0)
    frames_nonblack = summary.get("frames_nonblack", 0)
    render_health = summary.get("render_health_pct", 0)

    result["details"] = {
        "total_frames": total_frames,
        "frames_with_draws": frames_with_draws,
        "frames_nonblack": frames_nonblack,
        "render_health_pct": render_health,
        "peak_draw_calls": summary.get("peak_draw_calls", 0),
        "peak_verts": summary.get("peak_verts", 0),
    }

    # Check captured frames
    if verify_dir:
        bmp_files = sorted(Path(verify_dir).glob("frame_*.bmp"))
        frame_analyses = []
        for bmp in bmp_files:
            analysis = analyze_bmp(str(bmp))
            if analysis:
                analysis["file"] = bmp.name
                frame_analyses.append(analysis)
        result["details"]["captured_frames"] = frame_analyses

    # Verdict
    if total_frames == 0:
        result["issues"].append("No frames were rendered")
    elif frames_with_draws == 0:
        result["issues"].append("No frames had draw calls — GX shim may not be working")
    elif frames_nonblack == 0:
        result["issues"].append("All frames were black — rendering pipeline may be broken")
    elif render_health < 10:
        result["issues"].append(
            f"Render health is very low ({render_health}%) — most frames are empty"
        )

    # Also check framebuffer analyses from log
    fb_data = data.get("fb_analyses", [])
    if fb_data:
        nonblack_counts = [fb.get("pct_nonblack", 0) for fb in fb_data]
        max_nonblack = max(nonblack_counts) if nonblack_counts else 0
        result["details"]["max_pct_nonblack"] = max_nonblack
        result["details"]["fb_analyses_count"] = len(fb_data)

        if max_nonblack > 0:
            result["details"]["rendering_produces_pixels"] = True
        else:
            result["details"]["rendering_produces_pixels"] = False

    result["passed"] = len(result["issues"]) == 0
    return result


def check_input(data):
    """Check input health. Returns (passed, details)."""
    result = {
        "checked": True,
        "passed": False,
        "details": {},
        "issues": [],
    }

    summary = data.get("summary")
    inputs = data.get("inputs", [])
    responses = data.get("input_responses", [])

    result["details"] = {
        "input_events": len(inputs),
        "input_responses": len(responses),
    }

    if summary:
        result["details"]["input_events_total"] = summary.get("input_events", 0)
        result["details"]["input_responses_total"] = summary.get("input_responses", 0)
        result["details"]["input_health_pct"] = summary.get("input_health_pct", 0)

    # Input is "passing" if either:
    # 1. No input was injected (nothing to verify)
    # 2. Input was injected and game responded
    if len(inputs) == 0:
        result["passed"] = True  # no input to verify
        result["details"]["note"] = "No input events injected — input system not yet tested"
    elif len(responses) > 0:
        result["passed"] = True
    else:
        result["issues"].append(
            f"Input events were sent ({len(inputs)}) but no game responses detected"
        )

    return result


def check_audio(data):
    """Check audio health. Returns (passed, details)."""
    result = {
        "checked": True,
        "passed": False,
        "details": {},
        "issues": [],
    }

    summary = data.get("summary")
    audio_reports = data.get("audio_reports", [])

    result["details"]["audio_reports"] = len(audio_reports)

    if summary:
        result["details"]["audio_frames_active"] = summary.get("audio_frames_active", 0)
        result["details"]["audio_frames_nonsilent"] = summary.get("audio_frames_nonsilent", 0)
        result["details"]["audio_health_pct"] = summary.get("audio_health_pct", 0)

    # Audio is "passing" if either:
    # 1. Audio system not yet implemented (no reports) — expected during early port
    # 2. Audio is active and producing non-silent output
    if len(audio_reports) == 0:
        result["passed"] = True  # audio not yet implemented
        result["details"]["note"] = "No audio reports — audio system not yet implemented"
    else:
        active_reports = [r for r in audio_reports if r.get("active", False)]
        nonsilent_reports = [r for r in audio_reports if r.get("has_nonsilence", False)]

        if len(active_reports) == 0:
            result["issues"].append("Audio system exists but is never active")
        elif len(nonsilent_reports) == 0:
            result["issues"].append(
                "Audio is active but all buffers are silent — "
                "audio mixing may not be working"
            )

    if len(result["issues"]) == 0:
        result["passed"] = True

    return result


def main():
    parser = argparse.ArgumentParser(description="Verify port subsystem health")
    parser.add_argument("logfile", help="Path to milestones/verification log")
    parser.add_argument("--output", default="verify-report.json",
                        help="Output report JSON file")
    parser.add_argument("--verify-dir", default="verify_output",
                        help="Directory with captured frame BMPs")
    parser.add_argument("--check-rendering", action="store_true",
                        help="Check rendering health")
    parser.add_argument("--check-input", action="store_true",
                        help="Check input health")
    parser.add_argument("--check-audio", action="store_true",
                        help="Check audio health")
    parser.add_argument("--check-all", action="store_true",
                        help="Check all subsystems")
    args = parser.parse_args()

    if args.check_all:
        args.check_rendering = True
        args.check_input = True
        args.check_audio = True

    # If no checks specified, default to rendering
    if not args.check_rendering and not args.check_input and not args.check_audio:
        args.check_rendering = True

    data = parse_log(args.logfile)

    report = {
        "checks": {},
        "overall_pass": True,
    }

    if args.check_rendering:
        verify_dir = args.verify_dir if Path(args.verify_dir).is_dir() else None
        rendering = check_rendering(data, verify_dir)
        report["checks"]["rendering"] = rendering
        if not rendering["passed"]:
            report["overall_pass"] = False

    if args.check_input:
        input_result = check_input(data)
        report["checks"]["input"] = input_result
        if not input_result["passed"]:
            report["overall_pass"] = False

    if args.check_audio:
        audio_result = check_audio(data)
        report["checks"]["audio"] = audio_result
        if not audio_result["passed"]:
            report["overall_pass"] = False

    # Write report
    with open(args.output, "w") as f:
        json.dump(report, f, indent=2)

    # Print human-readable summary
    print(f"\n{'=' * 60}")
    print("PORT VERIFICATION REPORT")
    print(f"{'=' * 60}")

    for name, check in report["checks"].items():
        emoji = "✅" if check["passed"] else "❌"
        print(f"\n{emoji} {name.upper()}")
        for key, val in check.get("details", {}).items():
            if key == "captured_frames":
                print(f"  Captured frames: {len(val)}")
                for fr in val:
                    print(f"    {fr['file']}: {fr['pct_nonblack']}% non-black, "
                          f"avg color ({fr['avg_color'][0]},{fr['avg_color'][1]},{fr['avg_color'][2]})")
            elif key == "note":
                print(f"  ℹ️  {val}")
            else:
                print(f"  {key}: {val}")
        for issue in check.get("issues", []):
            print(f"  ⚠️  {issue}")

    overall_emoji = "✅" if report["overall_pass"] else "❌"
    print(f"\n{overall_emoji} Overall: {'PASS' if report['overall_pass'] else 'FAIL'}")
    print(f"{'=' * 60}\n")

    sys.exit(0 if report["overall_pass"] else 1)


if __name__ == "__main__":
    main()
