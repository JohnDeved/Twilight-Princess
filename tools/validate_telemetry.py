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
    chain_invalid = []
    cNdIt_cycles = []
    cNd_node_cycles = []   # cNd_last_cycle / cNd_length_cycle / cNd_first_cycle
    kankyo_crash_count = 0
    zblend_prop_by_frame = {}
    play_state_samples = []   # play_state JSON: depth_bits/blend_bits per draw
    frame_draw_diag_samples = []  # frame_draw_diag: per-frame first-3-draw TEV diagnostics
    rasc_inject_samples = []  # rasc_inject: mat/amb color used for RASC draws
    # Sequential pass: tag chain_invalid and cycle events with the current frame
    # number (the last j3d_draw_diag frame seen before each event in log order).
    # This lets us assert that cycles only appear after the known crash frame.
    _cur_frame = 0
    for obj in stderr_objs:
        if 'j3d_draw_diag' in obj:
            d = obj['j3d_draw_diag']
            _cur_frame = d.get('frame', _cur_frame)
            j3d_diag_frames[_cur_frame] = d
        if 'render' in obj and obj.get('render') == 'frame_stats':
            frame_stats[obj.get('frame', 0)] = obj
        if 'bg_draw_entry' in obj:
            bg_draw_entries.append(obj['bg_draw_entry'])
        if 'bg_kankyo_stats' in obj:
            bg_kankyo_stats.append(obj['bg_kankyo_stats'])
        if 'j3d_chain_invalid' in obj:
            chain_invalid.append(dict(obj['j3d_chain_invalid'], _frame=_cur_frame))
        if 'cNdIt_cycle' in obj or 'cNdIt_judge_cycle' in obj:
            cNdIt_cycles.append(dict(obj, _frame=_cur_frame))
        if 'cNd_last_cycle' in obj or 'cNd_length_cycle' in obj or 'cNd_first_cycle' in obj or 'cNd_setobj_cycle' in obj:
            cNd_node_cycles.append(dict(obj, _frame=_cur_frame))
        if 'zblend_prop' in obj:
            p = obj['zblend_prop']
            zblend_prop_by_frame[p.get('frame', 0)] = p
        if 'play_state' in obj:
            play_state_samples.append(obj['play_state'])
        if 'frame_draw_diag' in obj:
            frame_draw_diag_samples.append(obj['frame_draw_diag'])
        if 'rasc_inject' in obj:
            rasc_inject_samples.append(obj['rasc_inject'])
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
        passclr_fill = draw_path.get('passclr_fill_count', draw_path.get('skip_passclr_fill', 0))
        print(f"  Total draw attempts: {total}")
        print(f"  Successfully submitted: {ok} ({100*ok/max(total,1):.1f}%)")
        if passclr_fill > 0:
            print(f"  passclr_fill_count: {passclr_fill} (TEVREG0 fills rendered in order)")
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

    if rasc_inject_samples:
        print("\n=== RASC Color Injection (3D room material/ambient) ===")
        for s in rasc_inject_samples:
            print(f"  draw={s.get('draw','?')} mat={s.get('mat','?')} "
                  f"amb={s.get('amb','?')} mat_src={s.get('mat_src','?')}")

    print("\n=== 3D Rendering Stability ===")
    # Check BG draw lifecycle
    bg_draw_count = len(bg_draw_entries)
    print(f"  BG draw frames: {bg_draw_count}")
    if bg_draw_count > 0:
        bg_rooms = set(e.get('roomNo', -1) for e in bg_draw_entries)
        print(f"  BG rooms drawn: {sorted(bg_rooms)}")

    # Check kankyo crash dependency
    total_kankyo_faults = sum(s.get('faults', 0) for s in bg_kankyo_stats)
    total_kankyo_bypasses = sum(s.get('bypasses', s.get('successes', 0)) for s in bg_kankyo_stats)
    print(f"  Kankyo faults: {total_kankyo_faults}")
    print(f"  Kankyo bypasses: {total_kankyo_bypasses}")
    print(f"  Kankyo crash lines: {kankyo_crash_count}")
    if kankyo_crash_count > 0:
        errors.append(f"REGRESSION: {kankyo_crash_count} kankyo crash-recovery events "
                     f"(should be 0 — clean bypass replaces signal-based recovery)")
    # Known crash frame: PROC_CAMERA Execute crash at frame 130 corrupts the
    # actor delete-queue linked list.  All cycle events (chain_cycle, cNdIt_cycle,
    # cNd_node_cycles) should only appear AFTER this frame.  Any cycle event on a
    # clean frame (< CRASH_FRAME) indicates a new, unexpected corruption source.
    CRASH_FRAME = int(os.getenv("TP_TELEMETRY_CRASH_FRAME", "130"))

    print(f"  Invalid J3D chains detected: {len(chain_invalid)}")
    cycle_only = []
    if chain_invalid:
        first_bad = chain_invalid[0]
        print(f"  First invalid chain: slot={first_bad.get('slot')} len={first_bad.get('len')} "
              f"invalid_ptr={first_bad.get('invalid_ptr')} self_loop={first_bad.get('self_loop')} "
              f"vptr_low={first_bad.get('vptr_low')} chain_cycle={first_bad.get('chain_cycle', 0)} "
              f"frame={first_bad.get('_frame', '?')}")
        # chain_cycle=1 means a multi-node cycle in the packet list was detected and safely
        # skipped by the chain-cycle guard added in J3DDrawBuffer::drawHead(). This is expected
        # when actor-Execute crashes at frame 130+ corrupt J3D list state (game proceeds safely).
        # Only hard-fail on true pointer corruption (invalid_ptr, self_loop, vptr_low).
        hard_invalid = [c for c in chain_invalid if
                        c.get('invalid_ptr') or c.get('self_loop') or c.get('vptr_low')]
        cycle_only = [c for c in chain_invalid if c.get('chain_cycle') and not
                      (c.get('invalid_ptr') or c.get('self_loop') or c.get('vptr_low'))]
        if cycle_only:
            warnings.append(f"chain_cycle: {len(cycle_only)} packet chain cycles detected "
                            f"(safely broken by draw-loop cap; expected after actor Execute crashes)")
            # Assert: chain_cycle events should only appear after the known crash frame.
            # Any cycle on a clean frame indicates a new corruption source.
            early_cycles = [c for c in cycle_only if c.get('_frame', CRASH_FRAME) < CRASH_FRAME]
            if early_cycles:
                warnings.append(
                    f"NEW CORRUPTION SOURCE: {len(early_cycles)} chain_cycle event(s) on frames "
                    f"< {CRASH_FRAME} (first at frame {early_cycles[0].get('_frame','?')}, "
                    f"slot={early_cycles[0].get('slot','?')}) — cycles before the known crash "
                    f"frame indicate a new list-corruption path that was not present before")
        if hard_invalid:
            errors.append(f"REGRESSION: detected {len(hard_invalid)} corrupt packet chains in drawHead "
                          f"(first: slot={hard_invalid[0].get('slot')} "
                          f"invalid_ptr={hard_invalid[0].get('invalid_ptr')} "
                          f"self_loop={hard_invalid[0].get('self_loop')} "
                          f"vptr_low={hard_invalid[0].get('vptr_low')})")

    # Actor linked-list cycle events (cNdIt_cycle / cNdIt_judge_cycle)
    # These fire when the actor-framework iteration hits the 10000-node safety cap,
    # which indicates a circular linked-list caused by a mid-modification crash.
    # Warn (not error) — the cap safely breaks the loop and game progresses.
    cNdIt_count = len(cNdIt_cycles)
    if cNdIt_cycles:
        warnings.append(f"cNdIt_cycle: {cNdIt_count} actor-list cycle events "
                        f"(safely broken by iteration cap; expected after actor Execute crashes)")
        # Assert: cycle events should only appear after the known crash frame.
        early_cNdIt = [c for c in cNdIt_cycles if c.get('_frame', CRASH_FRAME) < CRASH_FRAME]
        if early_cNdIt:
            warnings.append(
                f"NEW CORRUPTION SOURCE: {len(early_cNdIt)} cNdIt_cycle event(s) on frames "
                f"< {CRASH_FRAME} (first at frame {early_cNdIt[0].get('_frame','?')}) — "
                f"actor-list cycles before the known crash frame indicate a new corruption path")
    # Count regression gate: warn if cycle count exceeds a configured max.
    # Set TP_TELEMETRY_MAX_CHAIN_CYCLES=N to flag when chain or actor-list cycles increase.
    max_chain_cycles = int(os.getenv("TP_TELEMETRY_MAX_CHAIN_CYCLES", "0"))
    total_cycle_count = len(cycle_only)
    if max_chain_cycles > 0 and total_cycle_count > max_chain_cycles:
        warnings.append(
            f"cycle count regression: {total_cycle_count} chain_cycle events > baseline "
            f"{max_chain_cycles} (TP_TELEMETRY_MAX_CHAIN_CYCLES); new corruption source likely")
    max_ndIt_cycles = int(os.getenv("TP_TELEMETRY_MAX_NDIT_CYCLES", "0"))
    if max_ndIt_cycles > 0 and cNdIt_count > max_ndIt_cycles:
        warnings.append(
            f"cycle count regression: {cNdIt_count} cNdIt_cycle events > baseline "
            f"{max_ndIt_cycles} (TP_TELEMETRY_MAX_NDIT_CYCLES); new corruption source likely")
    print(f"  Actor-list cycles detected: {cNdIt_count}")

    # Node-level cycle events (cNd_last_cycle / cNd_length_cycle / cNd_first_cycle)
    # These fire when cNd_Last/cNd_LengthOf/cNd_First hit the 10000-node safety cap,
    # indicating a corrupted doubly-linked list (circular). Warn only.
    cNd_count = len(cNd_node_cycles)
    if cNd_node_cycles:
        warnings.append(f"cNd_node_cycle: {cNd_count} node-level list cycle events "
                        f"(safely broken by iteration cap; expected after actor Execute crashes)")
        # Assert: node-level cycles should only appear after the known crash frame.
        early_cNd = [c for c in cNd_node_cycles if c.get('_frame', CRASH_FRAME) < CRASH_FRAME]
        if early_cNd:
            warnings.append(
                f"NEW CORRUPTION SOURCE: {len(early_cNd)} cNd_node_cycle event(s) on frames "
                f"< {CRASH_FRAME} (first at frame {early_cNd[0].get('_frame','?')}) — "
                f"node-level cycles before the known crash frame indicate a new corruption path")
    max_nd_cycles = int(os.getenv("TP_TELEMETRY_MAX_ND_CYCLES", "0"))
    if max_nd_cycles > 0 and cNd_count > max_nd_cycles:
        warnings.append(
            f"cycle count regression: {cNd_count} cNd_node_cycle events > baseline "
            f"{max_nd_cycles} (TP_TELEMETRY_MAX_ND_CYCLES); new corruption source likely")
    print(f"  Node-level list cycles: {cNd_count}")

    # Check dl_draws across sustained frame range
    if j3d_diag_frames:
        play_frames = {f: d for f, d in j3d_diag_frames.items() if 127 <= f <= 400}
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
                               f"— room geometry not present in draw queue for most frames")
            # Packet-flow assertion: if j3d_entries stay nonzero while dl_draws stay zero
            # for too long, drawHead packet dispatch is likely broken.
            consecutive_stalled_frames = 0
            max_consecutive_stalled_frames = 0
            for f, d in sorted(play_frames.items()):
                if d.get('j3d_entries', 0) > 0 and d.get('dl_draws', 0) == 0:
                    consecutive_stalled_frames += 1
                    max_consecutive_stalled_frames = max(max_consecutive_stalled_frames, consecutive_stalled_frames)
                else:
                    consecutive_stalled_frames = 0
            consecutive_nonzero_draws = 0
            max_consecutive_nonzero_draws = 0
            for f, d in sorted(play_frames.items()):
                if d.get('j3d_entries', 0) > 0 and d.get('dl_draws', 0) > 0:
                    consecutive_nonzero_draws += 1
                    max_consecutive_nonzero_draws = max(max_consecutive_nonzero_draws, consecutive_nonzero_draws)
                else:
                    consecutive_nonzero_draws = 0
            print(f"  Max consecutive (j3d_entries>0 && dl_draws==0): {max_consecutive_stalled_frames}")
            print(f"  Max consecutive (j3d_entries>0 && dl_draws>0): {max_consecutive_nonzero_draws}")
            strict_flow = os.getenv("TP_TELEMETRY_ENFORCE_J3D_FLOW", "0") == "1"
            strict_sustain = os.getenv("TP_TELEMETRY_ENFORCE_DL_SUSTAIN", "0") == "1"
            sustain_min = int(os.getenv("TP_TELEMETRY_DL_SUSTAIN_MIN", "3"))
            # Set TP_TELEMETRY_ENFORCE_J3D_FLOW=1 to promote packet-flow
            # stalls to hard failures (enable when packet-flow is expected stable).
            if strict_flow and max_consecutive_stalled_frames >= 20:
                errors.append(f"REGRESSION: {max_consecutive_stalled_frames} consecutive frames with "
                             f"j3d_entries>0 but dl_draws==0")
            elif max_consecutive_stalled_frames >= 20:
                warnings.append(f"Packet-flow blocker: {max_consecutive_stalled_frames} consecutive frames with "
                               f"j3d_entries>0 but dl_draws==0 (set TP_TELEMETRY_ENFORCE_J3D_FLOW=1 to fail)")
            # Set TP_TELEMETRY_ENFORCE_DL_SUSTAIN=1 to require a sustained
            # run of nonzero play-window draws once packet-flow is expected stable.
            if strict_sustain and max_consecutive_nonzero_draws < sustain_min:
                errors.append(f"REGRESSION: sustained play-window draws too short "
                             f"(max consecutive j3d_entries>0 && dl_draws>0 = "
                             f"{max_consecutive_nonzero_draws}, required >= {sustain_min})")
            elif max_consecutive_nonzero_draws < sustain_min:
                warnings.append(f"Unsustained draw window: max consecutive "
                                f"j3d_entries>0 && dl_draws>0 = {max_consecutive_nonzero_draws} "
                                f"(set TP_TELEMETRY_ENFORCE_DL_SUSTAIN=1 to fail)")

    print("\n=== Z/Blend Propagation ===")
    if zblend_prop_by_frame:
        latest_f = max(zblend_prop_by_frame.keys())
        latest = zblend_prop_by_frame[latest_f]
        print(f"  Latest frame: {latest_f}")
        print(f"  GXSetZMode calls: {latest.get('gx_set_z', 0)}")
        print(f"  GXSetBlendMode calls: {latest.get('gx_set_blend', 0)}")
        print(f"  bgfx submits with depth bits: {latest.get('submit_depth', 0)}")
        print(f"  bgfx submits with blend bits: {latest.get('submit_blend', 0)}")
    else:
        warnings.append("No zblend_prop telemetry found in stderr")

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

        # Regression checks — play window (frames 130-400)
        play_frames = {f: d for f, d in j3d_diag_frames.items() if 127 <= f <= 400}
        if play_frames:
            max_j3d = max(d.get('j3d_entries', 0) for d in play_frames.values())
            max_dl_draws = max(d.get('dl_draws', 0) for d in play_frames.values())
            if max_j3d > 0:
                print(f"  Peak j3d_entries in play: {max_j3d}")
            if max_dl_draws > 0:
                print(f"  Peak dl_draws in play: {max_dl_draws}")

        # ── Peak dl_draws regression gate (all frames) ──────────────────────
        # We track the maximum dl_draws ever seen across all frames to protect
        # against regressions like the vrbox J3DModel→J3DModelData cast that
        # produced 7615 draws on frames 128-129.  A drop to 0 means the render
        # pipeline is producing a black screen.
        #
        # Threshold:  TP_TELEMETRY_PEAK_DL_MIN  (default 500)
        # Enforce:    TP_TELEMETRY_ENFORCE_PEAK_DL=1  (default: warning only)
        peak_dl_draws = max(
            (d.get('dl_draws', 0) for d in j3d_diag_frames.values()), default=0)
        peak_dl_frame = (
            max(j3d_diag_frames.keys(),
                key=lambda f: j3d_diag_frames[f].get('dl_draws', 0))
            if j3d_diag_frames else None)
        peak_dl_min = int(os.getenv("TP_TELEMETRY_PEAK_DL_MIN", "500"))
        enforce_peak_dl = os.getenv("TP_TELEMETRY_ENFORCE_PEAK_DL", "0") == "1"
        print(f"  Peak dl_draws (all frames): {peak_dl_draws}"
              + (f" at frame {peak_dl_frame}" if peak_dl_frame is not None else ""))
        if peak_dl_draws < peak_dl_min:
            msg = (f"Peak dl_draws regression: {peak_dl_draws} < {peak_dl_min} "
                   f"(set TP_TELEMETRY_PEAK_DL_MIN to adjust, "
                   f"TP_TELEMETRY_ENFORCE_PEAK_DL=1 to hard-fail)")
            if enforce_peak_dl:
                errors.append(msg)
            else:
                warnings.append(msg)
    else:
        warnings.append("No j3d_draw_diag found in stderr")

    # ── Play-window Z/Blend state diagnostic ─────────────────────────
    # play_state lines are emitted by pal_tev_flush_draw for the first 5 draws
    # of each play-window frame once cumulative draw count exceeds 5000.
    # They log the actual uint64_t bgfx state flags assembled before bgfx::submit,
    # breaking out depth_bits (BGFX_STATE_DEPTH_TEST_MASK), blend_bits
    # (BGFX_STATE_BLEND_MASK), write_rgb and write_a.
    # Expected for opaque BG geometry: depth_bits=1, blend_bits=0, write_rgb=1
    print(f"\n=== Play-window Z/Blend State (first samples) ===")
    if play_state_samples:
        samples_with_depth = sum(1 for s in play_state_samples if s.get('depth_bits', 0))
        samples_with_blend = sum(1 for s in play_state_samples if s.get('blend_bits', 0))
        samples_with_rgb   = sum(1 for s in play_state_samples if s.get('write_rgb', 0))
        total_samples = len(play_state_samples)
        print(f"  play_state samples: {total_samples}")
        print(f"  depth_bits active: {samples_with_depth}/{total_samples} "
              f"({100.0*samples_with_depth/max(total_samples,1):.1f}%)")
        print(f"  blend_bits active: {samples_with_blend}/{total_samples} "
              f"({100.0*samples_with_blend/max(total_samples,1):.1f}%)")
        print(f"  write_rgb active:  {samples_with_rgb}/{total_samples} "
              f"({100.0*samples_with_rgb/max(total_samples,1):.1f}%)")
        # Show first 3 unique state values for diagnosis
        seen_states = {}
        for s in play_state_samples:
            state_hex = s.get('state', '0x0')
            if state_hex not in seen_states:
                seen_states[state_hex] = s
                if len(seen_states) >= 3:
                    break
        for state_hex, s in seen_states.items():
            print(f"  sample: state={state_hex} preset={s.get('preset','?')} "
                  f"depth={s.get('depth_bits',0)} blend={s.get('blend_bits',0)} "
                  f"rgb={s.get('write_rgb',0)} z_en={s.get('z_en',0)}")
        # Warn if no depth bits in any sample (likely Z/Blend propagation gap)
        if samples_with_depth == 0 and total_samples > 0:
            warnings.append(
                f"Z/Blend gap: {total_samples} play_state samples all have depth_bits=0 "
                f"(GXSetZMode calls may not be reaching bgfx state)")
        if samples_with_rgb == 0 and total_samples > 0:
            warnings.append(
                f"Z/Blend gap: {total_samples} play_state samples all have write_rgb=0 "
                f"(color writes disabled, geometry will be invisible)")
    else:
        print("  No play_state samples found (fires when total draw_count > 5000)")

    # Per-frame draw diagnostic: TEV registers and vertex colors for Phase 2
    # Emitted by frame_draw_diag events from pal_tev_flush_draw (first 3 draws/frame,
    # frames 6-200, works in Phase 2 where total draw count never exceeds 5000).
    if frame_draw_diag_samples:
        frames_seen = sorted(set(s.get('frame', 0) for s in frame_draw_diag_samples))
        print(f"\n=== Per-Frame Draw Diagnostic ({len(frame_draw_diag_samples)} samples, frames {frames_seen[0]}-{frames_seen[-1]}) ===")
        # Group by frame and show first few
        from collections import defaultdict
        by_frame = defaultdict(list)
        for s in frame_draw_diag_samples:
            by_frame[s.get('frame', 0)].append(s)
        # Show a representative sample: frames 10,30,60,90,120 if present
        sample_frames = [f for f in [10, 30, 60, 90, 120] if f in by_frame]
        if not sample_frames:
            sample_frames = frames_seen[:5]
        for frm in sample_frames:
            draws = by_frame[frm]
            print(f"  Frame {frm}: {len(draws)} draws")
            for d in draws[:3]:
                preset   = d.get('preset', '?')
                tevR0    = d.get('tevR0', [0,0,0,0])
                tevR1    = d.get('tevR1', [0,0,0,0])
                vtx_clr  = d.get('vtx_clr', [0,0,0,0])
                blend    = d.get('blend_mode', 0)
                inject   = d.get('inject', 0)
                has_vtx  = d.get('has_vtx_clr', 0)
                nverts   = d.get('nverts', 0)
                tcd      = d.get('tev0_cd', [0,0,0,0])
                color_src = f"inject={vtx_clr}" if inject else (f"vtx_clr(raw)" if has_vtx else "none")
                print(f"    draw_id={d.get('draw_id','?')} preset={preset} blend={blend} nverts={nverts}")
                print(f"      TEV0 formula [a,b,c,d]={tcd}  tevR0={tevR0}  tevR1={tevR1}  color={color_src}")
        # Check for non-red (RGB) content: if any non-PASSCLR draws have colored tevR0/R1
        blend_draws = [s for s in frame_draw_diag_samples if s.get('preset', '') in ('fs_gx_blend', 'fs_gx_modulate', 'fs_gx_replace')]
        if blend_draws:
            has_full_rgb = any(
                (s.get('tevR1', [0,0,0,0])[1] > 10 or s.get('tevR1', [0,0,0,0])[2] > 10 or
                 s.get('tevR0', [0,0,0,0])[1] > 10 or s.get('tevR0', [0,0,0,0])[2] > 10)
                for s in blend_draws
            )
            if has_full_rgb:
                print(f"  ✅ Full-RGB content detected in BLEND/MODULATE draws (G or B > 10)")
            else:
                warnings.append(
                    f"frame_draw_diag: {len(blend_draws)} BLEND/MODULATE draws all have G=B=0 in "
                    f"TEV registers — red-channel-only output expected (shader uniform gap)")
        else:
            print(f"  ℹ️  No BLEND/MODULATE/REPLACE draws in sample frames (only PASSCLR)")
    else:
        print("  No frame_draw_diag samples found (frames 6-200, first 3 draws/frame)")

    print(f"\n=== Milestone Count ===")
    unique_milestones = sorted(set(milestones))
    print(f"  Milestones: {len(unique_milestones)}")
    for m in unique_milestones:
        print(f"    - {m}")

    goal_milestones = {
        "GOAL_INTRO_GEOMETRY",
        "GOAL_INTRO_VISIBLE",
        "GOAL_DEPTH_BLEND_ACTIVE",
    }
    reached_goals = sorted([m for m in unique_milestones if m in goal_milestones])
    print(f"  Goal milestones reached: {len(reached_goals)}/{len(goal_milestones)}")
    for m in reached_goals:
        print(f"    - ✅ {m}")
    missing_goals = sorted(goal_milestones - set(reached_goals))
    for m in missing_goals:
        print(f"    - ⏳ {m} (informational)")

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
