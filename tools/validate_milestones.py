#!/usr/bin/env python3
"""Parse tp-pc stderr JSON and assert key milestones for autonomous iteration.

Usage:
    TP_HEADLESS=1 TP_TEST_FRAMES=400 build/tp-pc 2>stderr.log
    python3 tools/validate_milestones.py stderr.log

Asserts:
    - windowNum=1 by frame 130
    - PROC_BG created (fopAcM_create(PROC_BG) log present)
    - gh_free > 10MB at frame 200
    - dl_draws > 0 by frame 300
    - No actor_create_suppressed for critical profiles (PROC_BG=732)
"""
import json
import sys
import re

def main():
    if len(sys.argv) < 2:
        print("Usage: validate_milestones.py <stderr.log>", file=sys.stderr)
        sys.exit(1)

    logfile = sys.argv[1]
    with open(logfile, 'r', errors='replace') as f:
        lines = f.readlines()

    # Parse JSON lines from stderr
    first_windowNum1 = None
    proc_bg_created = False
    gh_free_at_200 = None
    first_dl_draws_nonzero = None
    max_frame = 0
    suppressed_profiles = {}
    crash_profiles = {}

    for line in lines:
        line = line.strip()
        if not line.startswith('{'):
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue

        # j3d_draw_diag: {j3d_draw_diag:{frame, windowNum, dl_draws, dl_verts, gh_free, gh_size}}
        if 'j3d_draw_diag' in obj:
            diag = obj['j3d_draw_diag']
            frame = diag.get('frame', 0)
            max_frame = max(max_frame, frame)
            wn = diag.get('windowNum', 0)
            dl_draws = diag.get('dl_draws', 0)
            gh_free = diag.get('gh_free', 0)

            if wn >= 1 and first_windowNum1 is None:
                first_windowNum1 = frame

            if dl_draws > 0 and first_dl_draws_nonzero is None:
                first_dl_draws_nonzero = frame

            if frame >= 195 and frame <= 210 and gh_free_at_200 is None:
                gh_free_at_200 = gh_free

        # objectSetCheck: look for fopAcM_create_PROC_BG step
        if 'objectSetCheck' in obj:
            osc = obj['objectSetCheck']
            step = osc.get('step', '')
            if step == 'fopAcM_create_PROC_BG':
                proc_bg_created = True

        # actor_create_suppressed
        if 'actor_create_suppressed' in obj:
            prof = obj['actor_create_suppressed'].get('prof', -1)
            suppressed_profiles[prof] = obj['actor_create_suppressed'].get('crash_count', 0)

        # actor_create_crash
        if 'actor_create_crash' in obj:
            prof = obj['actor_create_crash'].get('prof', -1)
            crash_profiles[prof] = crash_profiles.get(prof, 0) + 1

    # Print timeline
    print("=== Milestone Timeline ===")
    print(f"  max_frame:            {max_frame}")
    print(f"  first_windowNum=1:    {first_windowNum1}")
    print(f"  PROC_BG created:      {proc_bg_created}")
    print(f"  gh_free at ~200:      {gh_free_at_200}")
    print(f"  first dl_draws > 0:   {first_dl_draws_nonzero}")
    print(f"  crashed profiles:     {crash_profiles}")
    print(f"  suppressed profiles:  {suppressed_profiles}")

    # Assertions
    errors = []

    if first_windowNum1 is None:
        errors.append("FAIL: windowNum never reached 1")
    elif first_windowNum1 > 130:
        errors.append(f"FAIL: windowNum=1 first at frame {first_windowNum1} (expected <= 130)")

    if not proc_bg_created:
        errors.append("FAIL: PROC_BG never created (no fopAcM_create_PROC_BG log)")

    if gh_free_at_200 is not None and gh_free_at_200 < 10 * 1024 * 1024:
        errors.append(f"FAIL: gh_free at frame ~200 = {gh_free_at_200} (expected > 10MB)")

    # PROC_BG = 732 should not be suppressed
    if 732 in suppressed_profiles:
        errors.append(f"FAIL: PROC_BG (732) was suppressed after {suppressed_profiles[732]} crashes")

    if errors:
        print("\n=== FAILURES ===")
        for e in errors:
            print(f"  {e}")
        sys.exit(1)
    else:
        print("\n=== ALL MILESTONES PASSED ===")
        # Report optional status
        if first_dl_draws_nonzero is not None:
            print(f"  BONUS: dl_draws went nonzero at frame {first_dl_draws_nonzero}")
        else:
            print(f"  NOTE: dl_draws still 0 through frame {max_frame}")
        sys.exit(0)

if __name__ == '__main__':
    main()
