#!/usr/bin/env python3
"""Parse milestone JSON lines from port test output and produce summary for AI agent.

The summary JSON is designed for machine consumption by AI agents:
- highest_milestone: numeric milestone level reached
- milestones_reached: list of all milestone names reached
- stubs_hit: top unimplemented GX stubs by hit count (implement these first)
- crash: crash info if any
- frame_validation: per-frame rendering validation stats
- timing: milestone timing information
- pass: whether minimum milestone threshold was met
"""
import json
import sys
import argparse


# Milestone ID to name mapping
MILESTONE_NAMES = {
    0: "BOOT_START",
    1: "HEAP_INIT",
    2: "GFX_INIT",
    3: "PAD_INIT",
    4: "FRAMEWORK_INIT",
    5: "FIRST_FRAME",
    6: "LOGO_SCENE",
    7: "TITLE_SCENE",
    8: "PLAY_SCENE",
    9: "STAGE_LOADED",
    10: "FRAMES_60",
    11: "FRAMES_300",
    12: "FRAMES_1800",
    13: "DVD_READ_OK",
    14: "SCENE_CREATED",
    15: "RENDER_FRAME",
    99: "TEST_COMPLETE",
}


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
    frame_validation = None

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
            elif "frame_validation" in obj:
                frame_validation = obj["frame_validation"]

    # Exclude TEST_COMPLETE (99) and CRASH (-1) from highest calculation
    real_ids = [m["id"] for m in milestones if 0 <= m["id"] < 99]
    highest = max(real_ids, default=-1)
    last = milestones[-1] if milestones else None

    # Build list of all reached milestone names (exclude TEST_COMPLETE for clarity)
    reached_ids = sorted(set(m["id"] for m in milestones if 0 <= m["id"] < 99))
    milestones_reached = [MILESTONE_NAMES.get(mid, f"UNKNOWN_{mid}") for mid in reached_ids]

    # Build timing info
    timing = {}
    for m in milestones:
        if m["id"] >= 0 and "time_ms" in m:
            name = MILESTONE_NAMES.get(m["id"], f"UNKNOWN_{m['id']}")
            timing[name] = m["time_ms"]

    summary = {
        "highest_milestone": highest,
        "highest_milestone_name": MILESTONE_NAMES.get(highest, "UNKNOWN"),
        "milestones_reached": milestones_reached,
        "last_milestone": last,
        "crash": crash,
        "total_milestones": len([m for m in milestones if 0 <= m["id"] < 99]),
        "stubs_hit": sorted(stubs, key=lambda s: -s.get("hits", 0))[:20],
        "frame_validation": frame_validation,
        "timing": timing,
        "pass": highest >= args.min_milestone and crash is None,
    }

    with open(args.output, "w") as f:
        json.dump(summary, f, indent=2)

    # Print human-readable summary
    print(f"\n{'=' * 60}")
    print("PORT TEST SUMMARY")
    print(f"{'=' * 60}")
    if last:
        print(f"Highest milestone: {highest} ({MILESTONE_NAMES.get(highest, 'UNKNOWN')})")
    else:
        print(f"Highest milestone: {highest} (NONE)")
    print(f"Milestones reached: {', '.join(milestones_reached)}")
    if timing:
        print(f"Timing: {json.dumps(timing)}")
    if crash:
        print(f"CRASH: signal {crash.get('signal', '?')}")
    if frame_validation:
        print(f"Frame validation: {json.dumps(frame_validation)}")
    if stubs:
        print("Top unimplemented stubs:")
        for s in summary["stubs_hit"][:10]:
            print(f"  {s['stub']}: {s['hits']} hits")
    print(f"Result: {'PASS' if summary['pass'] else 'FAIL'}")
    print(f"{'=' * 60}\n")

    sys.exit(0 if summary["pass"] else 1)


if __name__ == "__main__":
    main()
