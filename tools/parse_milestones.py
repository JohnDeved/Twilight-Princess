#!/usr/bin/env python3
"""Parse milestone JSON lines from port test output and produce summary for AI agent."""
import json
import sys
import argparse


def main():
    parser = argparse.ArgumentParser(description="Parse port test milestones")
    parser.add_argument("logfile", help="Path to milestones.log")
    parser.add_argument("--output", default="milestone-summary.json",
                        help="Output summary JSON file")
    parser.add_argument("--min-milestone", type=int, default=0,
                        help="Fail if highest milestone < this value")
    args = parser.parse_args()

    milestones = []
    stubs = []
    crash = None

    with open(args.logfile) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "milestone" in obj:
                milestones.append(obj)
                if obj["milestone"] == "CRASH":
                    crash = obj
            elif "stub" in obj:
                stubs.append(obj)

    highest = max((m["id"] for m in milestones if m["id"] >= 0), default=-1)
    last = milestones[-1] if milestones else None

    summary = {
        "highest_milestone": highest,
        "last_milestone": last,
        "crash": crash,
        "total_milestones": len([m for m in milestones if m["id"] >= 0]),
        "stubs_hit": sorted(stubs, key=lambda s: -s.get("hits", 0))[:20],
        "pass": highest >= args.min_milestone and crash is None,
    }

    with open(args.output, "w") as f:
        json.dump(summary, f, indent=2)

    # Print human-readable summary
    print(f"\n{'=' * 60}")
    print("PORT TEST SUMMARY")
    print(f"{'=' * 60}")
    if last:
        print(f"Highest milestone: {highest} ({last['milestone']})")
    else:
        print(f"Highest milestone: {highest} (NONE)")
    if crash:
        print(f"CRASH: signal {crash.get('signal', '?')}")
    if stubs:
        print("Top unimplemented stubs:")
        for s in summary["stubs_hit"][:10]:
            print(f"  {s['stub']}: {s['hits']} hits")
    print(f"Result: {'PASS' if summary['pass'] else 'FAIL'}")
    print(f"{'=' * 60}\n")

    sys.exit(0 if summary["pass"] else 1)


if __name__ == "__main__":
    main()
