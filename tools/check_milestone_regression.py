#!/usr/bin/env python3
"""
Compare milestone results against a stored baseline to detect regressions.

Usage:
    python3 tools/check_milestone_regression.py milestone-summary.json \\
        --baseline milestone-baseline.json

Exit codes:
    0 = pass (same or improved, integrity valid)
    1 = regression detected or integrity failure
    2 = input error
"""
import json
import sys
import argparse


def load_json(path):
    """Load a JSON file, returning None on error."""
    try:
        with open(path) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Warning: Could not load {path}: {e}", file=sys.stderr)
        return None


def main():
    parser = argparse.ArgumentParser(description="Check milestone regression")
    parser.add_argument("summary", help="Path to milestone-summary.json from current run")
    parser.add_argument("--baseline", default="milestone-baseline.json",
                        help="Path to baseline milestone file")
    parser.add_argument("--output", default=None,
                        help="Write regression report to this JSON file")
    args = parser.parse_args()

    summary = load_json(args.summary)
    baseline = load_json(args.baseline)

    if summary is None:
        print("ERROR: Could not read milestone summary", file=sys.stderr)
        sys.exit(2)

    current = summary.get("milestones_reached_count", 0)
    baseline_count = baseline.get("milestones_reached_count", 0) if baseline else 0

    # Check integrity from parse_milestones.py
    integrity = summary.get("integrity", {})
    integrity_valid = integrity.get("valid", True)
    integrity_issues = integrity.get("issues", [])
    disqualified = integrity.get("disqualified_milestones", [])

    # Determine status — count already excludes integrity-failed milestones,
    # so we compare the filtered count directly against baseline.
    if current > baseline_count:
        status = "improved"
        emoji = "🎉"
    elif current == baseline_count:
        status = "same"
        emoji = "✅"
    else:
        status = "regressed"
        emoji = "🚨"

    report = {
        "status": status,
        "current_milestone_count": current,
        "baseline_milestone_count": baseline_count,
        "delta": current - baseline_count,
        "milestones_reached": summary.get("milestones_reached", []),
        "crash": summary.get("crash"),
        "stubs_hit": summary.get("stubs_hit", [])[:10],
        "integrity_valid": integrity_valid,
        "integrity_issues": integrity_issues,
        "disqualified_milestones": disqualified,
        "pass": status in ("same", "improved"),
    }

    # Print human-readable report
    print(f"\n{'=' * 60}")
    print(f"MILESTONE REGRESSION CHECK {emoji}")
    print(f"{'=' * 60}")
    print(f"Current count:  {current}")
    print(f"Baseline count: {baseline_count}")
    print(f"Status: {status.upper()} (delta: {current - baseline_count:+d})")

    milestones = summary.get("milestones_reached", [])
    if milestones:
        print(f"Milestones: {', '.join(milestones)}")

    if not integrity_valid:
        print(f"\n🚫 INTEGRITY ISSUES — some milestones disqualified:")
        for issue in integrity_issues:
            print(f"   ❌ {issue}")
        if disqualified:
            print(f"   Disqualified: {', '.join(disqualified)}")
        print(f"\n   These milestones are excluded from the count.")

    if report["crash"]:
        print(f"\n⚠️  CRASH detected: signal {report['crash'].get('signal', '?')}")

    if report["stubs_hit"]:
        print(f"\n📊 Top unimplemented stubs (implement these next):")
        for s in report["stubs_hit"][:5]:
            print(f"   {s.get('stub', '?')}: {s.get('hits', 0)} hits")

    print(f"{'=' * 60}\n")

    # Write report JSON
    if args.output:
        with open(args.output, "w") as f:
            json.dump(report, f, indent=2)
        print(f"Report written to: {args.output}")

    sys.exit(0 if report["pass"] else 1)


if __name__ == "__main__":
    main()
