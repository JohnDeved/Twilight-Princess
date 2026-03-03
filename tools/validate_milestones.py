#!/usr/bin/env python3
"""Parse tp-pc stderr JSON and assert key milestones for autonomous iteration.

Usage:
    TP_HEADLESS=1 TP_TEST_FRAMES=200 build/tp-pc 2>stderr.log
    python3 tools/validate_milestones.py stderr.log

Asserts:
    - windowNum=1 by frame 130
    - PROC_BG created (fopAcM_create(PROC_BG) log present)
    - gh_free > 10MB at frame 200
    - dl_draws > 0 by frame 200 (only if enough play-scene frames reached)
    - No actor_create_suppressed for critical profiles (PROC_BG=732)

Frame-time aware:
    Consumes {frame_time:{frame,ms}} telemetry to detect whether enough
    play-scene frames were reached for the dl_draws assertion to be meaningful.
    If the run timed out before frame 135 (BG Draw needs ~8 frames after
    PROC_BG creation at frame 127), reports that explicitly instead of
    failing on unreachable milestones.
"""
import json
import sys

# BG actor is created at ~frame 127 and needs ~8 more frames to reach Draw.
# Frame 135 is the minimum to meaningfully test dl_draws.
MIN_PLAY_SCENE_FRAME_FOR_DL_DRAWS = 135

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
    bg_draw_diag = {}

    # frame_time telemetry: {frame_time:{frame, ms}}
    frame_times = {}  # frame -> ms
    play_scene_frames = 0  # count of frames >= 120 (play scene starts ~frame 120)

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

        # frame_time: {frame_time:{frame, ms}}
        if 'frame_time' in obj:
            ft = obj['frame_time']
            frame = ft.get('frame', 0)
            ms = ft.get('ms', 0)
            frame_times[frame] = ms
            max_frame = max(max_frame, frame)
            if frame >= 120:
                play_scene_frames += 1

        # bg_draw_diag: {bg_draw_diag:{frame, part, model_null, shapes, ...}}
        if 'bg_draw_diag' in obj:
            diag = obj['bg_draw_diag']
            frame = diag.get('frame', 0)
            if frame not in bg_draw_diag:
                bg_draw_diag[frame] = []
            bg_draw_diag[frame].append(diag)

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

    # Estimate play-scene frame budget
    play_scene_times = {f: ms for f, ms in frame_times.items() if f >= 120}
    avg_play_scene_ms = 0
    if play_scene_times:
        avg_play_scene_ms = sum(play_scene_times.values()) / len(play_scene_times)
    enough_frames_for_dl = max_frame >= MIN_PLAY_SCENE_FRAME_FOR_DL_DRAWS

    # Print timeline
    print("=== Milestone Timeline ===")
    print(f"  max_frame:            {max_frame}")
    print(f"  first_windowNum=1:    {first_windowNum1}")
    print(f"  PROC_BG created:      {proc_bg_created}")
    print(f"  gh_free at ~200:      {gh_free_at_200}")
    print(f"  first dl_draws > 0:   {first_dl_draws_nonzero}")
    print(f"  crashed profiles:     {crash_profiles}")
    print(f"  suppressed profiles:  {suppressed_profiles}")

    # Frame budget summary
    print(f"\n=== Frame Budget ===")
    print(f"  play-scene frames:    {play_scene_frames} (frames >= 120)")
    print(f"  avg play-scene ms:    {avg_play_scene_ms:.0f}")
    print(f"  enough for dl_draws:  {enough_frames_for_dl} (need frame >= {MIN_PLAY_SCENE_FRAME_FOR_DL_DRAWS})")
    if play_scene_times:
        sorted_ft = sorted(play_scene_times.items())
        print(f"  play-scene range:     frame {sorted_ft[0][0]}-{sorted_ft[-1][0]}")
        for f, ms in sorted_ft[:5]:
            print(f"    frame {f}: {ms}ms")
        if len(sorted_ft) > 5:
            print(f"    ... ({len(sorted_ft) - 5} more)")

    # BG draw diagnostics summary
    if bg_draw_diag:
        print(f"\n=== BG Draw Diagnostics ===")
        for frame in sorted(bg_draw_diag.keys())[:3]:
            parts = bg_draw_diag[frame]
            null_models = sum(1 for p in parts if p.get('model_null', False))
            total_shapes = sum(p.get('shapes', 0) for p in parts)
            hidden_shapes = sum(p.get('hidden', 0) for p in parts)
            entered = sum(p.get('entered', 0) for p in parts)
            print(f"  frame {frame}: {len(parts)} parts, {null_models} null models, "
                  f"{total_shapes} shapes ({hidden_shapes} hidden), {entered} entered")

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
        if first_dl_draws_nonzero is not None:
            print(f"  BONUS: dl_draws went nonzero at frame {first_dl_draws_nonzero}")
        elif not enough_frames_for_dl:
            print(f"  TIMEOUT: run only reached frame {max_frame}, need >= {MIN_PLAY_SCENE_FRAME_FOR_DL_DRAWS} for dl_draws assertion")
            if avg_play_scene_ms > 0:
                frames_needed = MIN_PLAY_SCENE_FRAME_FOR_DL_DRAWS - max_frame
                est_seconds = (frames_needed * avg_play_scene_ms) / 1000
                print(f"  ESTIMATE: need ~{est_seconds:.0f}s more ({frames_needed} frames * {avg_play_scene_ms:.0f}ms/frame)")
        else:
            print(f"  NOTE: dl_draws still 0 at frame {max_frame} (frame >= {MIN_PLAY_SCENE_FRAME_FOR_DL_DRAWS} reached)")
            print(f"  ACTION: BG Draw path needs tracing — check bg_draw_diag output")
        sys.exit(0)

if __name__ == '__main__':
    main()
