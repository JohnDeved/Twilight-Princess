#!/usr/bin/env python3
"""
Compare milestone results against a stored baseline to detect regressions.

Usage:
    python3 tools/check_milestone_regression.py milestone-summary.json \\
        --baseline milestone-baseline.json \\
        --update-on-improvement

Exit codes:
    0 = pass (same or improved)
    1 = regression detected
    2 = input error
"""
import json
import sys
import argparse
from pathlib import Path


# Decision table: given a highest milestone, what should an agent do next?
# This is the machine-readable version of the table in port-progress.md.
NEXT_ACTIONS = {
    -1: {
        "action": "Fix build errors — code does not compile",
        "focus_files": ["CMakeLists.txt", "src/pal/"],
        "docs": "docs/agent-port-workflow.md#step-1--cmake-build-system-250-loc",
    },
    0: {
        "action": "Fix heap initialization crash in mDoMch_Create",
        "focus_files": ["src/m_Do/m_Do_machine.cpp", "src/pal/pal_os_stubs.cpp"],
        "docs": "docs/agent-port-workflow.md#step-3--pal-bootstrap-1250-loc",
    },
    1: {
        "action": "Fix GFX initialization (mDoGph_Create / bgfx init)",
        "focus_files": ["src/m_Do/m_Do_graphic.cpp", "src/pal/gx/gx_bgfx.cpp"],
        "docs": "docs/agent-port-workflow.md#step-3--pal-bootstrap-1250-loc",
    },
    2: {
        "action": "Fix input init or PAL bootstrap",
        "focus_files": ["src/m_Do/m_Do_controller_pad.cpp", "src/pal/pal_sdk_stubs.cpp"],
        "docs": "docs/agent-port-workflow.md#step-3--pal-bootstrap-1250-loc",
    },
    3: {
        "action": "Fix process manager setup (fapGm_Create)",
        "focus_files": ["src/f_pc/f_pc_manager.cpp", "src/pal/pal_game_stubs.cpp"],
        "docs": "docs/agent-port-workflow.md#step-3--pal-bootstrap-1250-loc",
    },
    4: {
        "action": "Fix process manager setup (fapGm_Create)",
        "focus_files": ["src/f_pc/f_pc_manager.cpp", "src/pal/pal_game_stubs.cpp"],
        "docs": "docs/agent-port-workflow.md#step-3--pal-bootstrap-1250-loc",
    },
    5: {
        "action": "Game loop runs but scene fails to load — fix DVD/archive loading",
        "focus_files": ["src/m_Do/m_Do_dvd_thread.cpp", "src/pal/pal_endian.cpp",
                        "src/JSystem/JKernel/"],
        "docs": "docs/agent-port-workflow.md#step-4--dvdaram-simplification-200-loc",
    },
    6: {
        "action": "Logo loads — implement GX stubs and rendering for title scene",
        "focus_files": ["src/pal/pal_gx_stubs.cpp", "src/pal/gx/gx_state.cpp",
                        "src/pal/gx/gx_tev.cpp"],
        "docs": "docs/agent-port-workflow.md#step-5--gx-shim-tier-a-5000-loc",
    },
    7: {
        "action": "Title works — fix dScnPly_c or stage loading for gameplay scene",
        "focus_files": ["src/d/d_s_play.cpp", "src/pal/pal_gx_stubs.cpp"],
        "docs": "docs/agent-port-workflow.md#step-5--gx-shim-tier-a-5000-loc",
    },
    8: {
        "action": "Gameplay scene loaded — focus on GX stubs and rendering fidelity",
        "focus_files": ["src/pal/pal_gx_stubs.cpp", "src/pal/gx/gx_tev.cpp",
                        "src/pal/gx/gx_displaylist.cpp"],
        "docs": "docs/agent-port-workflow.md#step-5--gx-shim-tier-a-5000-loc",
    },
    9: {
        "action": "Stage loaded — implement input and remaining GX stubs",
        "focus_files": ["src/pal/pal_gx_stubs.cpp", "src/pal/gx/gx_state.cpp"],
        "docs": "docs/agent-port-workflow.md#step-7--input--save-350-loc",
    },
    10: {
        "action": "1s stable — fix top stub hits from coverage report",
        "focus_files": ["src/pal/pal_gx_stubs.cpp"],
        "docs": "docs/agent-port-workflow.md#step-5--gx-shim-tier-a-5000-loc",
    },
    11: {
        "action": "5s stable — polish rendering and fix remaining stubs",
        "focus_files": ["src/pal/pal_gx_stubs.cpp", "src/pal/gx/gx_tev.cpp"],
        "docs": "docs/agent-port-workflow.md#step-8--first-playable",
    },
    12: {
        "action": "30s stable — first playable achieved! Focus on input, save, audio",
        "focus_files": ["src/pal/pal_input.cpp", "src/pal/pal_save.cpp",
                        "src/pal/pal_audio.cpp"],
        "docs": "docs/agent-port-workflow.md#step-8--first-playable",
    },
}


def load_json(path):
    """Load a JSON file, returning None on error."""
    try:
        with open(path) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Warning: Could not load {path}: {e}", file=sys.stderr)
        return None


def get_next_action(milestone_id):
    """Get the recommended next action for a given milestone level."""
    if milestone_id in NEXT_ACTIONS:
        return NEXT_ACTIONS[milestone_id]
    # For milestones above 12, use the milestone 12 action
    if milestone_id > 12:
        return NEXT_ACTIONS[12]
    return NEXT_ACTIONS.get(-1)


def main():
    parser = argparse.ArgumentParser(description="Check milestone regression")
    parser.add_argument("summary", help="Path to milestone-summary.json from current run")
    parser.add_argument("--baseline", default="milestone-baseline.json",
                        help="Path to baseline milestone file")
    parser.add_argument("--update-on-improvement", action="store_true",
                        help="Update baseline file when milestone improves")
    parser.add_argument("--output", default=None,
                        help="Write regression report to this JSON file")
    args = parser.parse_args()

    summary = load_json(args.summary)
    baseline = load_json(args.baseline)

    if summary is None:
        print("ERROR: Could not read milestone summary", file=sys.stderr)
        sys.exit(2)

    current = summary.get("highest_milestone", -1)
    baseline_milestone = baseline.get("highest_milestone", -1) if baseline else -1

    # Determine status
    if current > baseline_milestone:
        status = "improved"
        emoji = "🎉"
    elif current == baseline_milestone:
        status = "same"
        emoji = "✅"
    else:
        status = "regressed"
        emoji = "🚨"

    next_action = get_next_action(current)

    report = {
        "status": status,
        "current_milestone": current,
        "baseline_milestone": baseline_milestone,
        "delta": current - baseline_milestone,
        "crash": summary.get("crash"),
        "stubs_hit": summary.get("stubs_hit", [])[:10],
        "next_action": next_action,
        "pass": status != "regressed",
    }

    # Print human-readable report
    print(f"\n{'=' * 60}")
    print(f"MILESTONE REGRESSION CHECK {emoji}")
    print(f"{'=' * 60}")
    print(f"Current milestone:  {current}")
    print(f"Baseline milestone: {baseline_milestone}")
    print(f"Status: {status.upper()} (delta: {current - baseline_milestone:+d})")

    if report["crash"]:
        print(f"\n⚠️  CRASH detected: signal {report['crash'].get('signal', '?')}")

    if report["stubs_hit"]:
        print(f"\n📊 Top unimplemented stubs (implement these next):")
        for s in report["stubs_hit"][:5]:
            print(f"   {s.get('stub', '?')}: {s.get('hits', 0)} hits")

    if next_action:
        print(f"\n🎯 Recommended next action:")
        print(f"   {next_action['action']}")
        print(f"   Focus files: {', '.join(next_action['focus_files'])}")
        print(f"   Guide: {next_action['docs']}")

    print(f"{'=' * 60}\n")

    # Write report JSON
    if args.output:
        with open(args.output, "w") as f:
            json.dump(report, f, indent=2)
        print(f"Report written to: {args.output}")

    # Update baseline on improvement
    if args.update_on_improvement and status == "improved":
        last = summary.get("last_milestone", {})
        new_baseline = {
            "highest_milestone": current,
            "highest_milestone_name": last.get("milestone", "UNKNOWN"),
            "updated": "auto",
            "note": f"Auto-updated from {baseline_milestone} to {current}",
        }
        with open(args.baseline, "w") as f:
            json.dump(new_baseline, f, indent=2)
            f.write("\n")
        print(f"✅ Baseline updated: {baseline_milestone} → {current}")

    sys.exit(0 if report["pass"] else 1)


if __name__ == "__main__":
    main()
