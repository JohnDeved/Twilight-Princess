#!/usr/bin/env python3
"""Parse Phase 1 JSON telemetry and flag regressions in rendering metrics.

Usage:
    TP_HEADLESS=1 TP_TEST_FRAMES=400 build/tp-pc 2>stderr.log 1>stdout.log
    python3 tools/validate_telemetry.py stdout.log stderr.log

Parses stdout for:
    - tev_config_summary: unique TEV combiner configurations
    - draw_path_summary: categorized draw skip/success counts
    - collision_stub_calls: call frequency for collision stubs
    - dl_validation: DL vertex stride/index/bulk-copy validation

Parses stderr for:
    - j3d_draw_diag: per-frame dl_draws, dl_verts, j3d_entries
    - bg_draw_entry / bg_kankyo_stats: BG actor draw lifecycle
    - render frame_stats: periodic draw_calls/verts

Compares against baseline thresholds and flags regressions.
Can be used in CI to detect rendering pipeline breakage.
"""
import json
import sys
import os


def parse_json_lines(filepath):
    """Parse all JSON objects from a log file (one per line)."""
    objs = []
    try:
        with open(filepath, 'r', errors='replace') as f:
            for line in f:
                line = line.strip()
                if not line.startswith('{'):
                    continue
                try:
                    objs.append(json.loads(line))
                except json.JSONDecodeError:
                    continue
    except FileNotFoundError:
        pass
    return objs


def main():
    if len(sys.argv) < 2:
        print("Usage: validate_telemetry.py <stdout.log> [stderr.log]", file=sys.stderr)
        sys.exit(1)

    stdout_log = sys.argv[1]
    stderr_log = sys.argv[2] if len(sys.argv) > 2 else None

    stdout_objs = parse_json_lines(stdout_log)
    stderr_objs = parse_json_lines(stderr_log) if stderr_log else []

    # ================================================================
    # Extract telemetry
    # ================================================================

    tev_config = None
    draw_path = None
    collision = None
    dl_validation = None
    milestones = []

    for obj in stdout_objs:
        if 'tev_config_summary' in obj:
            tev_config = obj['tev_config_summary']
        if 'draw_path_summary' in obj:
            draw_path = obj['draw_path_summary']
        if 'collision_stub_calls' in obj:
            collision = obj['collision_stub_calls']
        if 'dl_validation' in obj:
            dl_validation = obj['dl_validation']
        if 'milestone' in obj:
            milestones.append(obj['milestone'])

    # j3d_draw_diag from stderr
    j3d_diag_frames = {}
    frame_stats = {}
    bg_draw_entries = []
    bg_kankyo_stats = []
    kankyo_crash_count = 0
    for obj in stderr_objs:
        if 'j3d_draw_diag' in obj:
            d = obj['j3d_draw_diag']
            frame = d.get('frame', 0)
            j3d_diag_frames[frame] = d
        if 'render' in obj and obj.get('render') == 'frame_stats':
            frame = obj.get('frame', 0)
            frame_stats[frame] = obj
        if 'bg_draw_entry' in obj:
            bg_draw_entries.append(obj['bg_draw_entry'])
        if 'bg_kankyo_stats' in obj:
            bg_kankyo_stats.append(obj['bg_kankyo_stats'])
        if 'milestone' in obj:
            milestones.append(obj['milestone'])

    # Count kankyo crash lines in stderr
    if stderr_log:
        try:
            with open(stderr_log, 'r', errors='replace') as f:
                for line in f:
                    if 'BG kankyo crash' in line or 'BG draw crash' in line:
                        kankyo_crash_count += 1
        except FileNotFoundError:
            pass

    # ================================================================
    # Report
    # ================================================================

    warnings = []
    errors = []

    print("=== TEV Config Summary ===")
    if tev_config:
        print(f"  Unique TEV configurations: {tev_config.get('unique_configs', 0)}")
        configs = tev_config.get('configs', [])
        for i, cfg in enumerate(configs[:10]):
            print(f"  [{i}] preset={cfg.get('preset','?')} hits={cfg.get('hits',0)} "
                  f"stages={cfg.get('stages',0)} "
                  f"s0.c={cfg.get('s0',{}).get('c','?')} "
                  f"s0.a={cfg.get('s0',{}).get('a','?')}")
            if cfg.get('stages', 0) >= 2 and 's1' in cfg:
                print(f"       s1.c={cfg['s1'].get('c','?')} s1.a={cfg['s1'].get('a','?')}")
        if tev_config.get('unique_configs', 0) == 0:
            errors.append("REGRESSION: No TEV configurations seen (was 4+)")
    else:
        warnings.append("No tev_config_summary found in stdout")

    print("\n=== Draw Path Summary ===")
    if draw_path:
        ok = draw_path.get('ok_submitted', 0)
        total_skips = sum(v for k, v in draw_path.items() if k.startswith('skip_'))
        total = ok + total_skips
        print(f"  Total draw attempts: {total}")
        print(f"  Successfully submitted: {ok} ({100*ok/max(total,1):.1f}%)")
        for k, v in sorted(draw_path.items()):
            if k.startswith('skip_') and v > 0:
                print(f"  {k}: {v} ({100*v/max(total,1):.1f}%)")

        if ok == 0 and total > 0:
            errors.append("REGRESSION: Zero draws submitted (was 186+)")
        elif ok < 50:
            warnings.append(f"Low draw count: {ok} (baseline ~186)")

        # Check for unexpected failure categories
        for bad_key in ['skip_bad_layout', 'skip_bad_stride', 'skip_invalid_prog', 'skip_tvb_alloc']:
            v = draw_path.get(bad_key, 0)
            if v > 0:
                warnings.append(f"Unexpected draw failures: {bad_key}={v}")
    else:
        warnings.append("No draw_path_summary found in stdout")

    print("\n=== DL Validation ===")
    if dl_validation:
        print(f"  Total DL draws: {dl_validation.get('total_draws', dl_validation.get('draws', 0))}")
        print(f"  Total DL verts: {dl_validation.get('total_verts', dl_validation.get('verts', 0))}")
        print(f"  DL calls: {dl_validation.get('calls', 0)}")
        print(f"  Stride-zero skips: {dl_validation.get('stride_zero', 0)}")
        print(f"  Overflow skips: {dl_validation.get('overflow', 0)}")
        print(f"  Bulk copy OK: {dl_validation.get('bulk_copy_ok', 0)}")
        print(f"  Bulk copy fail: {dl_validation.get('bulk_copy_fail', 0)}")
        if dl_validation.get('stride_zero', 0) > 100:
            warnings.append(f"High stride-zero skips: {dl_validation['stride_zero']}")
        if dl_validation.get('bulk_copy_fail', 0) > 0:
            warnings.append(f"Bulk copy failures: {dl_validation['bulk_copy_fail']}")
    else:
        warnings.append("No dl_validation found in stdout")

    print("\n=== Collision Stub Calls ===")
    if collision:
        total_calls = sum(collision.values())
        print(f"  Total collision stub calls: {total_calls}")
        for k, v in sorted(collision.items()):
            if v > 0:
                print(f"  {k}: {v}")
        if total_calls == 0:
            print("  (All collision stubs are dead code during noop test)")
    else:
        warnings.append("No collision_stub_calls found in stdout")

    print("\n=== 3D Rendering Stability ===")
    # Check BG draw lifecycle
    bg_draw_count = len(bg_draw_entries)
    print(f"  BG draw frames: {bg_draw_count}")
    if bg_draw_count > 0:
        bg_rooms = set(e.get('roomNo', -1) for e in bg_draw_entries)
        print(f"  BG rooms drawn: {sorted(bg_rooms)}")

    # Check kankyo crash dependency
    total_kankyo_faults = sum(s.get('faults', 0) for s in bg_kankyo_stats)
    total_kankyo_successes = sum(s.get('successes', 0) for s in bg_kankyo_stats)
    print(f"  Kankyo faults: {total_kankyo_faults}")
    print(f"  Kankyo successes: {total_kankyo_successes}")
    print(f"  Kankyo crash lines: {kankyo_crash_count}")
    if kankyo_crash_count > 0:
        errors.append(f"REGRESSION: {kankyo_crash_count} kankyo crash-recovery events (should be 0 with clean bypass)")

    # Check dl_draws across sustained frame range
    if j3d_diag_frames:
        play_frames = {f: d for f, d in j3d_diag_frames.items() if 130 <= f <= 400}
        if play_frames:
            dl_draws_list = [(f, d.get('dl_draws', 0)) for f, d in sorted(play_frames.items())]
            nonzero_frames = [(f, d) for f, d in dl_draws_list if d > 0]
            print(f"  Play frames with dl_draws > 0: {len(nonzero_frames)}/{len(dl_draws_list)}")
            if nonzero_frames:
                first_f, first_d = nonzero_frames[0]
                last_f, last_d = nonzero_frames[-1]
                print(f"  dl_draws active: frames {first_f}-{last_f}")
                print(f"  dl_draws range: [{min(d for _,d in nonzero_frames)}, {max(d for _,d in nonzero_frames)}]")
            # Warn if dl_draws is not sustained across play frames
            if len(nonzero_frames) < 5:
                warnings.append(f"Low dl_draws stability: only {len(nonzero_frames)} frames with draws "
                               f"(target: sustained across 130-400)")
            # Report dominant blocker
            zero_frames = len(dl_draws_list) - len(nonzero_frames)
            if zero_frames > len(dl_draws_list) * 0.5:
                warnings.append(f"Dominant blocker: {zero_frames}/{len(dl_draws_list)} play frames have 0 dl_draws "
                               f"— scene transition unloads BG before stable rendering window")

    print("\n=== J3D Draw Diagnostics ===")
    if j3d_diag_frames:
        # Check key frame windows
        frame_windows = {
            'boot (0-10)': range(0, 11),
            'logo (120-140)': range(120, 141),
            'play (150-200)': range(150, 201),
            'late (300-400)': range(300, 401),
        }
        for label, frange in frame_windows.items():
            frames_in_window = {f: d for f, d in j3d_diag_frames.items() if f in frange}
            if frames_in_window:
                dl_draws_vals = [d.get('dl_draws', 0) for d in frames_in_window.values()]
                dl_verts_vals = [d.get('dl_verts', 0) for d in frames_in_window.values()]
                j3d_vals = [d.get('j3d_entries', 0) for d in frames_in_window.values()]
                print(f"  {label}: dl_draws=[{min(dl_draws_vals)},{max(dl_draws_vals)}] "
                      f"dl_verts=[{min(dl_verts_vals)},{max(dl_verts_vals)}] "
                      f"j3d_entries=[{min(j3d_vals)},{max(j3d_vals)}] "
                      f"({len(frames_in_window)} frames)")

        # Regression checks
        play_frames = {f: d for f, d in j3d_diag_frames.items() if 130 <= f <= 400}
        if play_frames:
            max_j3d = max(d.get('j3d_entries', 0) for d in play_frames.values())
            max_dl_draws = max(d.get('dl_draws', 0) for d in play_frames.values())
            if max_j3d > 0:
                print(f"  Peak j3d_entries in play: {max_j3d}")
            if max_dl_draws > 0:
                print(f"  Peak dl_draws in play: {max_dl_draws}")
    else:
        warnings.append("No j3d_draw_diag found in stderr")

    print(f"\n=== Milestone Count ===")
    unique_milestones = sorted(set(milestones))
    print(f"  Milestones: {len(unique_milestones)}")
    for m in unique_milestones:
        print(f"    - {m}")

    # ================================================================
    # Final verdict
    # ================================================================

    if errors:
        print("\n=== ERRORS (regressions detected) ===")
        for e in errors:
            print(f"  ❌ {e}")
    if warnings:
        print("\n=== WARNINGS ===")
        for w in warnings:
            print(f"  ⚠️  {w}")

    if not errors and not warnings:
        print("\n✅ All telemetry checks passed")
    elif not errors:
        print(f"\n⚠️  {len(warnings)} warning(s), no regressions")

    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
