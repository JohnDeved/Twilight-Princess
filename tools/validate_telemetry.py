#!/usr/bin/env python3
"""Parse Phase 1 JSON telemetry and flag regressions in rendering metrics.

Usage:
    TP_HEADLESS=1 TP_TEST_FRAMES=400 build/tp-pc 2>stderr.log 1>stdout.log
    python3 tools/validate_telemetry.py stdout.log stderr.log

Parses stdout for:
    - tev_config_summary: unique TEV combiner configurations
    - draw_path_summary: categorized draw skip/success counts
    - collision_stub_calls: call frequency for collision stubs

Parses stderr for:
    - j3d_draw_diag: per-frame dl_draws, dl_verts, j3d_entries
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
    milestones = []

    for obj in stdout_objs:
        if 'tev_config_summary' in obj:
            tev_config = obj['tev_config_summary']
        if 'draw_path_summary' in obj:
            draw_path = obj['draw_path_summary']
        if 'collision_stub_calls' in obj:
            collision = obj['collision_stub_calls']
        if 'milestone' in obj:
            milestones.append(obj['milestone'])

    # j3d_draw_diag from stderr
    j3d_diag_frames = {}
    frame_stats = {}
    for obj in stderr_objs:
        if 'j3d_draw_diag' in obj:
            d = obj['j3d_draw_diag']
            frame = d.get('frame', 0)
            j3d_diag_frames[frame] = d
        if 'render' in obj and obj.get('render') == 'frame_stats':
            frame = obj.get('frame', 0)
            frame_stats[frame] = obj
        if 'milestone' in obj:
            milestones.append(obj['milestone'])

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
            # Note: j3d_entries/dl_draws may be 0 in noop mode — these
            # metrics are primarily meaningful in softpipe (Phase 2).
            # The TEV flush path (ok_submitted above) tracks actual draws.
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
