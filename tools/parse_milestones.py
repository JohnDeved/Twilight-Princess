#!/usr/bin/env python3
"""Parse milestone JSON lines from port test output and produce summary for AI agent.

The summary JSON is designed for machine consumption by AI agents:
- milestones_reached_count: number of distinct milestones reached
- milestones_reached: list of all milestone names reached
- stubs_hit: top unimplemented GX stubs by hit count (implement these first)
- crash: crash info if any
- frame_validation: per-frame rendering validation stats
- timing: milestone timing information
- pass: whether minimum milestone threshold was met
- integrity: whether milestone ordering and timing are physically valid
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

# Milestones that require specific prerequisites.
# If a milestone is claimed, all its prerequisites must also be present.
# This prevents agents from hardcoding high milestones without doing the work.
MILESTONE_PREREQUISITES = {
    1: [0],          # HEAP_INIT requires BOOT_START
    2: [0, 1],       # GFX_INIT requires BOOT_START, HEAP_INIT
    3: [0, 1, 2],    # PAD_INIT requires all prior init
    4: [0, 1, 2, 3], # FRAMEWORK_INIT requires all init
    5: [0, 1, 2, 4], # FIRST_FRAME requires init + framework
    10: [5, 15],     # FRAMES_60 requires FIRST_FRAME + RENDER_FRAME
    11: [5, 10, 15], # FRAMES_300 requires FRAMES_60
    12: [5, 10, 11, 15],  # FRAMES_1800 requires FRAMES_300
    15: [0, 1, 2],   # RENDER_FRAME requires GFX_INIT
}


def validate_integrity(milestones, frame_validation):
    """Validate that milestone sequence is physically possible.

    Checks:
    1. Timestamps are monotonically increasing for sequential milestones
    2. Prerequisites are satisfied (can't reach FRAMES_60 without FIRST_FRAME)
    3. RENDER_FRAME requires valid frame_validation data
    4. Frame milestones require plausible timing gaps
    5. No duplicate milestone IDs

    Returns (valid, issues, invalid_ids) tuple. invalid_ids is the set of
    milestone IDs that failed validation — these should NOT be counted.
    """
    issues = []
    invalid_ids = set()
    reached_ids = set()

    # Check for duplicates
    seen_ids = {}
    for m in milestones:
        mid = m.get("id", -999)
        if mid in seen_ids and 0 <= mid < 99:
            issues.append(f"Duplicate milestone ID {mid} ({MILESTONE_NAMES.get(mid, '?')})")
            invalid_ids.add(mid)
        seen_ids[mid] = m
        if 0 <= mid < 99:
            reached_ids.add(mid)

    # Check prerequisites
    for mid in reached_ids:
        prereqs = MILESTONE_PREREQUISITES.get(mid, [])
        for prereq in prereqs:
            if prereq not in reached_ids:
                prereq_name = MILESTONE_NAMES.get(prereq, f"ID_{prereq}")
                milestone_name = MILESTONE_NAMES.get(mid, f"ID_{mid}")
                issues.append(
                    f"Milestone {milestone_name} ({mid}) reached without "
                    f"prerequisite {prereq_name} ({prereq})"
                )
                invalid_ids.add(mid)

    # Check timing monotonicity for boot sequence (0-5)
    boot_milestones = sorted(
        [m for m in milestones if 0 <= m.get("id", -1) <= 5],
        key=lambda m: m["id"]
    )
    for i in range(1, len(boot_milestones)):
        prev_t = boot_milestones[i - 1].get("time_ms", 0)
        curr_t = boot_milestones[i].get("time_ms", 0)
        if curr_t < prev_t:
            mid = boot_milestones[i].get("id", -1)
            issues.append(
                f"Boot milestone {boot_milestones[i].get('milestone', '?')} "
                f"has earlier timestamp ({curr_t}ms) than "
                f"{boot_milestones[i-1].get('milestone', '?')} ({prev_t}ms)"
            )
            invalid_ids.add(mid)

    # RENDER_FRAME requires frame_validation with real data
    if 15 in reached_ids:
        if frame_validation is None:
            issues.append(
                "RENDER_FRAME claimed but no frame_validation data found"
            )
            invalid_ids.add(15)
        elif frame_validation.get("valid") != 1:
            issues.append(
                "RENDER_FRAME claimed but frame_validation.valid != 1"
            )
            invalid_ids.add(15)
        elif frame_validation.get("draw_calls", 0) == 0:
            issues.append(
                "RENDER_FRAME claimed but frame_validation.draw_calls == 0"
            )
            invalid_ids.add(15)

    # Frame count milestones need plausible timing
    if 10 in reached_ids:  # FRAMES_60
        first_frame_ms = seen_ids.get(5, {}).get("time_ms", 0)
        frames_60_ms = seen_ids.get(10, {}).get("time_ms", 0)
        # 60 frames at 60fps = 1000ms; use 500ms threshold to allow for
        # variable frame timing and uncapped headless mode
        if frames_60_ms > 0 and first_frame_ms > 0:
            gap = frames_60_ms - first_frame_ms
            if gap < 500:
                issues.append(
                    f"FRAMES_60 reached only {gap}ms after FIRST_FRAME "
                    f"(expected >=500ms for 60 frames)"
                )
                invalid_ids.add(10)

    if 12 in reached_ids:  # FRAMES_1800
        first_frame_ms = seen_ids.get(5, {}).get("time_ms", 0)
        frames_1800_ms = seen_ids.get(12, {}).get("time_ms", 0)
        # Integrity rule: 1800 frames reached in under 15s is implausible
        if frames_1800_ms > 0 and first_frame_ms > 0:
            gap = frames_1800_ms - first_frame_ms
            if gap < 15000:  # Less than 15s for 1800 frames is suspicious
                issues.append(
                    f"FRAMES_1800 reached only {gap}ms after FIRST_FRAME "
                    f"(expected >=15000ms for 1800 frames)"
                )
                invalid_ids.add(12)

    return len(issues) == 0, issues, invalid_ids


def main():
    parser = argparse.ArgumentParser(description="Parse port test milestones")
    parser.add_argument("logfile", help="Path to milestones.log")
    parser.add_argument("--output", default="milestone-summary.json",
                        help="Output summary JSON file")
    parser.add_argument("--min-milestone", type=int, default=0,
                        help="Fail if milestone count < this value")
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

    # Build list of all reached milestone names (exclude TEST_COMPLETE for clarity)
    reached_ids = sorted(set(m["id"] for m in milestones if 0 <= m["id"] < 99))
    milestones_reached = [MILESTONE_NAMES.get(mid, f"UNKNOWN_{mid}") for mid in reached_ids]
    milestone_count = len(reached_ids)

    # Build timing info
    timing = {}
    for m in milestones:
        if m["id"] >= 0 and "time_ms" in m:
            name = MILESTONE_NAMES.get(m["id"], f"UNKNOWN_{m['id']}")
            timing[name] = m["time_ms"]

    # Validate milestone integrity
    integrity_valid, integrity_issues, invalid_ids = validate_integrity(milestones, frame_validation)

    # Exclude integrity-failed milestones from the count
    valid_ids = sorted(set(rid for rid in reached_ids if rid not in invalid_ids))
    valid_milestones_reached = [MILESTONE_NAMES.get(mid, f"UNKNOWN_{mid}") for mid in valid_ids]
    valid_milestone_count = len(valid_ids)

    # Track which milestones were disqualified
    disqualified = sorted(invalid_ids)
    disqualified_names = [MILESTONE_NAMES.get(mid, f"UNKNOWN_{mid}") for mid in disqualified]

    summary = {
        "milestones_reached_count": valid_milestone_count,
        "milestones_reached": valid_milestones_reached,
        "last_milestone": milestones[-1] if milestones else None,
        "crash": crash,
        "total_milestones": valid_milestone_count,
        "stubs_hit": sorted(stubs, key=lambda s: -s.get("hits", 0))[:20],
        "frame_validation": frame_validation,
        "timing": timing,
        "integrity": {
            "valid": integrity_valid,
            "issues": integrity_issues,
            "disqualified_milestones": disqualified_names,
        },
        "pass": (valid_milestone_count >= args.min_milestone and crash is None),
    }

    with open(args.output, "w") as f:
        json.dump(summary, f, indent=2)

    # Print human-readable summary
    print(f"\n{'=' * 60}")
    print("PORT TEST SUMMARY")
    print(f"{'=' * 60}")
    print(f"Milestones reached: {valid_milestone_count}/{len(MILESTONE_NAMES) - 1}")
    print(f"Milestones: {', '.join(valid_milestones_reached) if valid_milestones_reached else 'NONE'}")
    if disqualified_names:
        print(f"Disqualified (integrity failed): {', '.join(disqualified_names)}")
    if timing:
        print(f"Timing: {json.dumps(timing)}")
    if crash:
        print(f"CRASH: signal {crash.get('signal', '?')}")
    if frame_validation:
        print(f"Frame validation: {json.dumps(frame_validation)}")
    if not integrity_valid:
        print(f"\n⚠️  INTEGRITY: FAILED ({len(integrity_issues)} issues)")
        for issue in integrity_issues:
            print(f"   ❌ {issue}")
    else:
        print(f"Integrity: ✅ PASSED")
    if stubs:
        print("Top unimplemented stubs:")
        for s in summary["stubs_hit"][:10]:
            print(f"  {s['stub']}: {s['hits']} hits")
    print(f"Result: {'PASS' if summary['pass'] else 'FAIL'}")
    print(f"{'=' * 60}\n")

    sys.exit(0 if summary["pass"] else 1)


if __name__ == "__main__":
    main()
