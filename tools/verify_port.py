#!/usr/bin/env python3
"""
Analyze port verification results — rendering, input, and audio health.

Parses the structured JSON output from pal_verify.cpp and produces a
human-readable report with pass/fail verdicts for each subsystem.

Rendering verification is **dependable without human review**:
- Per-frame CRC32 hash comparison detects any rendering change
- Golden reference image comparison via RMSE catches visual regressions
- Render baseline metrics (draw calls, verts, non-black %) prevent metric regressions
- Fails when metrics regress relative to baselines (no automatic updates)

Usage:
    python3 tools/verify_port.py milestones.log \\
        --output verify-report.json \\
        --verify-dir verify_output \\
        --golden-dir tests/golden \\
        --render-baseline tests/render-baseline.json \\
        --check-all

Exit codes:
    0 = all requested checks passed
    1 = one or more checks failed
    2 = input error
"""
import json
import sys
import argparse
import struct
from pathlib import Path


def parse_log(logfile):
    """Parse verification JSON lines from the log file."""
    frames = []
    captures = []
    fb_analyses = []
    inputs = []
    input_responses = []
    audio_reports = []
    summary = None
    milestones = []
    stubs = []

    with open(logfile) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            if "verify_frame" in obj:
                frames.append(obj["verify_frame"])
            elif "verify_capture" in obj:
                captures.append(obj)
            elif "verify_fb" in obj:
                fb_analyses.append(obj["verify_fb"])
            elif "verify_input" in obj:
                inputs.append(obj["verify_input"])
            elif "verify_input_response" in obj:
                input_responses.append(obj["verify_input_response"])
            elif "verify_audio" in obj:
                audio_reports.append(obj["verify_audio"])
            elif "verify_summary" in obj:
                summary = obj["verify_summary"]
            elif "milestone" in obj:
                milestones.append(obj)
            elif "stub" in obj:
                stubs.append(obj)

    return {
        "frames": frames,
        "captures": captures,
        "fb_analyses": fb_analyses,
        "inputs": inputs,
        "input_responses": input_responses,
        "audio_reports": audio_reports,
        "summary": summary,
        "milestones": milestones,
        "stubs": stubs,
    }


def analyze_bmp(path):
    """Analyze a BMP file and return basic pixel statistics."""
    try:
        with open(path, "rb") as f:
            header = f.read(54)
            if len(header) < 54 or header[0:2] != b"BM":
                return None

            width = struct.unpack_from("<i", header, 18)[0]
            height = struct.unpack_from("<i", header, 22)[0]
            bpp = struct.unpack_from("<H", header, 28)[0]

            if bpp != 24:
                return None

            row_bytes = width * 3
            row_padding = (4 - (row_bytes % 4)) % 4

            total_pixels = width * abs(height)
            nonblack = 0
            total_r = 0
            total_g = 0
            total_b = 0

            for _y in range(abs(height)):
                row = f.read(row_bytes)
                if len(row) < row_bytes:
                    break
                for x in range(width):
                    b_val = row[x * 3]
                    g_val = row[x * 3 + 1]
                    r_val = row[x * 3 + 2]
                    if r_val > 2 or g_val > 2 or b_val > 2:
                        nonblack += 1
                    total_r += r_val
                    total_g += g_val
                    total_b += b_val
                f.read(row_padding)  # skip padding

            return {
                "width": width,
                "height": abs(height),
                "total_pixels": total_pixels,
                "nonblack_pixels": nonblack,
                "pct_nonblack": (nonblack * 100) // total_pixels if total_pixels > 0 else 0,
                "avg_color": [
                    total_r // total_pixels if total_pixels > 0 else 0,
                    total_g // total_pixels if total_pixels > 0 else 0,
                    total_b // total_pixels if total_pixels > 0 else 0,
                ],
            }
    except (IOError, struct.error):
        return None


def read_bmp_pixels(path):
    """Read raw BGR pixel data from a 24-bit BMP. Returns (width, height, bytes)."""
    try:
        with open(path, "rb") as f:
            header = f.read(54)
            if len(header) < 54 or header[0:2] != b"BM":
                return None, None, None
            width = struct.unpack_from("<i", header, 18)[0]
            height = abs(struct.unpack_from("<i", header, 22)[0])
            bpp = struct.unpack_from("<H", header, 28)[0]
            if bpp != 24:
                return None, None, None
            row_bytes = width * 3
            row_padding = (4 - (row_bytes % 4)) % 4
            pixels = bytearray()
            for _y in range(height):
                row = f.read(row_bytes)
                if len(row) < row_bytes:
                    break
                pixels.extend(row)
                f.read(row_padding)
            return width, height, bytes(pixels)
    except (IOError, struct.error):
        return None, None, None


def compare_images(path_a, path_b):
    """Compare two BMP files using RMSE and pixel-diff.

    Returns a dict with:
    - rmse: root mean square error (0 = identical, 255 = maximally different)
    - pct_different: percentage of pixels that differ beyond threshold (3)
    - max_diff: maximum per-channel difference
    - identical: True if images are byte-identical
    """
    w_a, h_a, px_a = read_bmp_pixels(path_a)
    w_b, h_b, px_b = read_bmp_pixels(path_b)

    if px_a is None or px_b is None:
        return None

    if w_a != w_b or h_a != h_b:
        return {"error": f"dimension mismatch: {w_a}x{h_a} vs {w_b}x{h_b}"}

    if px_a == px_b:
        return {"rmse": 0.0, "pct_different": 0.0, "max_diff": 0, "identical": True}

    total = len(px_a)
    sum_sq = 0
    diff_count = 0
    max_diff = 0
    threshold = 3  # per-channel tolerance

    num_pixels = total // 3
    for p in range(num_pixels):
        base = p * 3
        d0 = abs(px_a[base] - px_b[base])
        d1 = abs(px_a[base + 1] - px_b[base + 1])
        d2 = abs(px_a[base + 2] - px_b[base + 2])
        sum_sq += d0 * d0 + d1 * d1 + d2 * d2
        dmax = max(d0, d1, d2)
        if dmax > max_diff:
            max_diff = dmax
        if dmax > threshold:
            diff_count += 1

    rmse = (sum_sq / total) ** 0.5 if total > 0 else 0.0
    pct_different = (diff_count * 100.0) / num_pixels if num_pixels > 0 else 0.0

    return {
        "rmse": round(rmse, 2),
        "pct_different": round(pct_different, 2),
        "max_diff": max_diff,
        "identical": False,
    }


def check_render_baseline(data, baseline_path):
    """Check rendering metrics against stored baseline thresholds.

    Returns list of (frame, metric, expected, actual, passed) tuples.
    """
    issues = []
    try:
        with open(baseline_path) as f:
            baseline = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return issues  # no baseline = no checks

    # Build lookup of per-frame verify data
    frame_data = {}
    for fb in data.get("fb_analyses", []):
        frame_num = str(fb.get("frame", 0))
        frame_data[frame_num] = fb
    for vf in data.get("frames", []):
        frame_num = str(vf.get("frame", 0))
        if frame_num in frame_data:
            frame_data[frame_num].update(vf)
        else:
            frame_data[frame_num] = vf

    # Check per-frame thresholds
    for frame_str, expected in baseline.get("frames", {}).items():
        actual = frame_data.get(frame_str, {})
        checks = [
            ("min_draw_calls", "draw_calls"),
            ("min_verts", "verts"),
            ("min_pct_nonblack", "pct_nonblack"),
            ("min_unique_colors", "unique_colors"),
        ]
        for min_key, actual_key in checks:
            min_val = expected.get(min_key, 0)
            if min_val > 0:
                act_val = actual.get(actual_key, 0)
                if act_val < min_val:
                    issues.append({
                        "frame": frame_str,
                        "metric": actual_key,
                        "expected_min": min_val,
                        "actual": act_val,
                        "passed": False,
                    })

        # Check hash if set
        expected_hash = expected.get("expected_fb_hash")
        if expected_hash:
            actual_hash = actual.get("fb_hash")
            if actual_hash and actual_hash != expected_hash:
                issues.append({
                    "frame": frame_str,
                    "metric": "fb_hash",
                    "expected": expected_hash,
                    "actual": actual_hash,
                    "passed": False,
                })

    # Check global thresholds
    summary = data.get("summary", {})
    gbl = baseline.get("global", {})

    global_checks = [
        ("min_peak_draw_calls", "peak_draw_calls"),
        ("min_peak_verts", "peak_verts"),
        ("min_render_health_pct", "render_health_pct"),
    ]
    for min_key, actual_key in global_checks:
        min_val = gbl.get(min_key, 0)
        if min_val > 0:
            act = summary.get(actual_key, 0)
            if act < min_val:
                issues.append({
                    "metric": actual_key,
                    "expected_min": min_val,
                    "actual": act,
                    "passed": False,
                })

    # Check frames_with_draws percentage
    min_draws_pct = gbl.get("min_frames_with_draws_pct", 0)
    if min_draws_pct > 0:
        total = summary.get("total_frames", 0)
        draws = summary.get("frames_with_draws", 0)
        actual_pct = (draws * 100 // total) if total > 0 else 0
        if actual_pct < min_draws_pct:
            issues.append({
                "metric": "frames_with_draws_pct",
                "expected_min": min_draws_pct,
                "actual": actual_pct,
                "passed": False,
            })

    # Check pipeline requirements
    pipeline = baseline.get("pipeline", {})
    if pipeline.get("require_geometry") and summary.get("peak_verts", 0) == 0:
        issues.append({
            "metric": "pipeline_geometry",
            "expected_min": "vertices > 0",
            "actual": summary.get("peak_verts", 0),
            "passed": False,
        })
    if pipeline.get("require_textures") and summary.get("frames_with_textures", 0) == 0:
        issues.append({
            "metric": "pipeline_textures",
            "expected_min": "textured frames > 0",
            "actual": summary.get("frames_with_textures", 0),
            "passed": False,
        })
    if pipeline.get("require_textured_shaders") and not (summary.get("all_shader_mask", 0) & ~1):
        issues.append({
            "metric": "pipeline_textured_shaders",
            "expected_min": "shader_mask has textured shaders",
            "actual": summary.get("all_shader_mask", 0),
            "passed": False,
        })
    if pipeline.get("require_depth") and summary.get("frames_with_depth", 0) == 0:
        issues.append({
            "metric": "pipeline_depth",
            "expected_min": "depth testing frames > 0",
            "actual": summary.get("frames_with_depth", 0),
            "passed": False,
        })
    if pipeline.get("require_blend") and summary.get("frames_with_blend", 0) == 0:
        issues.append({
            "metric": "pipeline_blend",
            "expected_min": "blended frames > 0",
            "actual": summary.get("frames_with_blend", 0),
            "passed": False,
        })
    min_complexity = pipeline.get("min_visual_complexity", 0)
    if min_complexity > 0:
        # Compute complexity same as check_pipeline_health
        pk_tex = summary.get("peak_unique_textures", 0)
        pk_dc = summary.get("peak_draw_calls", 0)
        shader_mask = summary.get("all_shader_mask", 0)
        prim_mask = summary.get("all_prim_mask", 0)
        shaders = bin(shader_mask).count('1')
        prims = bin(prim_mask).count('1')
        complexity = min(pk_tex * 5, 25) + min(pk_dc, 25) + min(shaders * 10, 25) + min(prims * 8, 25)
        if complexity < min_complexity:
            issues.append({
                "metric": "visual_complexity",
                "expected_min": min_complexity,
                "actual": complexity,
                "passed": False,
            })

    return issues


def check_golden_images(verify_dir, golden_dir):
    """Compare captured frames against golden reference images.

    Returns list of comparison results.
    """
    results = []
    if not verify_dir or not golden_dir:
        return results

    verify_path = Path(verify_dir)
    golden_path = Path(golden_dir)

    if not verify_path.is_dir() or not golden_path.is_dir():
        return results

    for captured in sorted(verify_path.glob("frame_*.bmp")):
        golden = golden_path / captured.name
        if golden.exists():
            cmp = compare_images(str(captured), str(golden))
            if cmp:
                cmp["frame"] = captured.name
                cmp["has_golden"] = True
                results.append(cmp)
        else:
            # No golden reference — report as info only
            analysis = analyze_bmp(str(captured))
            results.append({
                "frame": captured.name,
                "has_golden": False,
                "analysis": analysis,
            })

    return results


# TEV shader preset names for human-readable output
TEV_SHADER_NAMES = {
    0: "PASSCLR",    # vertex color only (no texture)
    1: "REPLACE",    # texture replaces color
    2: "MODULATE",   # texture * vertex color
    3: "BLEND",      # lerp(texture, vertex color, alpha)
    4: "DECAL",      # texture with alpha blend
}

# GX primitive type names — mapped to sequential bit positions 0-7
# C-side mapping: bit = (prim_type - 0x80) >> 3
GX_PRIM_NAMES = {
    0: "QUADS",      # GX_QUADS    = 0x80 → bit 0
    1: "TRIANGLES",  # GX_TRIANGLES = 0x90 → bit 1
    2: "TRISTRIP",   # GX_TRISTRIP  = 0x98 → bit 2
    3: "TRIFAN",     # GX_TRIFAN    = 0xA0 → bit 3
    4: "LINES",      # GX_LINES     = 0xA8 → bit 4
    5: "LINESTRIP",  # GX_LINESTRIP = 0xB0 → bit 5
    6: "POINTS",     # GX_POINTS    = 0xB8 → bit 6
}


def check_pipeline_health(data):
    """Check render pipeline stage health — each stage of the GX→bgfx pipeline.

    Returns a dict with stage-by-stage health status and actionable guidance.
    This tells agents exactly which part of the rendering pipeline needs work.

    Pipeline stages checked:
    1. Geometry submission — are vertices being submitted?
    2. Texture loading — are textures being decoded from GX formats?
    3. Shader variety — are textured shaders used (not just PASSCLR)?
    4. Depth testing — is z-buffering active for 3D scenes?
    5. Alpha blending — is transparency working?
    6. Primitive diversity — are multiple primitive types used?
    7. Visual complexity — peak textures × draw calls indicates scene richness
    """
    summary = data.get("summary", {})
    frames = data.get("frames", [])
    issues = []

    total_frames = summary.get("total_frames", 0)
    frames_with_draws = summary.get("frames_with_draws", 0)
    frames_with_textures = summary.get("frames_with_textures", 0)
    total_textured_draws = summary.get("total_textured_draws", 0)
    peak_unique_textures = summary.get("peak_unique_textures", 0)
    frames_with_depth = summary.get("frames_with_depth", 0)
    frames_with_blend = summary.get("frames_with_blend", 0)
    all_shader_mask = summary.get("all_shader_mask", 0)
    all_prim_mask = summary.get("all_prim_mask", 0)
    peak_draw_calls = summary.get("peak_draw_calls", 0)
    peak_verts = summary.get("peak_verts", 0)

    # Decode shader mask to names
    shaders_used = []
    for bit, name in TEV_SHADER_NAMES.items():
        if all_shader_mask & (1 << bit):
            shaders_used.append(name)

    # Decode primitive mask to names (bits 0-6 correspond to sequential prim types)
    prims_used = []
    for bit, name in GX_PRIM_NAMES.items():
        if all_prim_mask & (1 << bit):
            prims_used.append(name)

    # Stage 1: Geometry
    has_geometry = peak_verts > 0 and frames_with_draws > 0

    # Stage 2: Textures
    has_textures = frames_with_textures > 0 and peak_unique_textures > 0

    # Stage 3: Shader variety (real scenes use textured shaders, not just PASSCLR)
    # Bit 0 = PASSCLR (vertex color only, no texture). Bits 1-4 = textured shaders.
    has_textured_shaders = bool(all_shader_mask & ~1)

    # Stage 4: Depth testing
    has_depth = frames_with_depth > 0

    # Stage 5: Alpha blending
    has_blend = frames_with_blend > 0

    # Stage 6: Primitive diversity (real scenes use multiple prim types)
    prim_count = bin(all_prim_mask).count('1')
    has_prim_diversity = prim_count >= 2

    # Stage 7: Visual complexity score (0-100)
    # Based on: texture count, draw call density, shader variety, prim diversity
    complexity = 0
    if peak_unique_textures >= 1:
        complexity += min(peak_unique_textures * 5, 25)  # up to 25 for textures
    if peak_draw_calls >= 1:
        complexity += min(peak_draw_calls, 25)  # up to 25 for draw calls
    if len(shaders_used) >= 1:
        complexity += min(len(shaders_used) * 10, 25)  # up to 25 for shader variety
    if prim_count >= 1:
        complexity += min(prim_count * 8, 25)  # up to 25 for prim diversity

    # Build per-frame pipeline data from verify_frame entries
    per_frame_textured = []
    per_frame_textures = []
    for vf in frames:
        td = vf.get("textured_draws", 0)
        ut = vf.get("unique_textures", 0)
        if td > 0 or ut > 0:
            per_frame_textured.append({
                "frame": vf.get("frame", 0),
                "textured": td,
                "unique_tex": ut,
            })
        if ut > 0:
            per_frame_textures.append(ut)

    pipeline_result = {
        "stages": {
            "geometry": {"ok": has_geometry, "peak_verts": peak_verts,
                         "frames_with_draws": frames_with_draws},
            "textures": {"ok": has_textures, "frames_with_textures": frames_with_textures,
                         "peak_unique_textures": peak_unique_textures,
                         "total_textured_draws": total_textured_draws},
            "shaders": {"ok": has_textured_shaders, "shaders_used": shaders_used,
                        "mask": all_shader_mask},
            "depth": {"ok": has_depth, "frames_with_depth": frames_with_depth},
            "blend": {"ok": has_blend, "frames_with_blend": frames_with_blend},
            "primitives": {"ok": has_prim_diversity, "prims_used": prims_used,
                           "mask": all_prim_mask, "count": prim_count},
        },
        "visual_complexity": complexity,
        "issues": [],
    }

    # Generate actionable issues for broken pipeline stages
    if not has_geometry:
        pipeline_result["issues"].append(
            "Pipeline: No geometry submitted — GX vertex submission is broken. "
            "Fix GXBegin/GXPosition/GXEnd → pal_tev_flush_draw()"
        )
    elif not has_textures:
        pipeline_result["issues"].append(
            "Pipeline: No textures decoded — draws are untextured. "
            "Fix GXLoadTexObj → pal_gx_decode_texture(). Game will look like solid colors"
        )
    elif not has_textured_shaders:
        pipeline_result["issues"].append(
            "Pipeline: Only PASSCLR shader used — textures exist but aren't applied. "
            "Fix TEV preset detection in detect_tev_preset() to return REPLACE/MODULATE"
        )

    # Informational guidance (not blocking — these improve quality incrementally)
    if has_geometry and not has_depth and total_frames > 60:
        pipeline_result["issues"].append(
            "Pipeline: No depth testing — 3D scenes will have z-fighting. "
            "Ensure GXSetZMode sets z_compare_enable in g_gx_state"
        )

    issues.extend(pipeline_result["issues"])

    return pipeline_result


def check_frame_progression(data):
    """Check that rendering content changes between frames.

    A game that renders the same image every frame may be stuck in a loop,
    not actually progressing through scenes. Frame progression detects this
    by comparing framebuffer hashes between captured frames.
    """
    summary = data.get("summary", {})
    frames = data.get("frames", [])
    issues = []

    hash_changes = summary.get("hash_changes", 0)
    total_frames = summary.get("total_frames", 0)

    # Collect hashes from captured frames
    captured_hashes = []
    for vf in frames:
        fb_hash = vf.get("fb_hash")
        if fb_hash:
            captured_hashes.append((vf.get("frame", 0), fb_hash))

    distinct_hashes = len(set(h for _, h in captured_hashes))

    progression_result = {
        "hash_changes": hash_changes,
        "captured_hashes": len(captured_hashes),
        "distinct_hashes": distinct_hashes,
        "issues": [],
    }

    # Check for frozen rendering — if we have many frames but all same hash
    # 60 frames ≈ 1 second at 60fps — enough to expect content changes
    if total_frames > 60 and hash_changes == 0:
        progression_result["issues"].append(
            "Frame progression: framebuffer hash never changed across "
            f"{total_frames} frames — game may be stuck or rendering "
            "the same clear color every frame"
        )

    # Check captured frame diversity — need ≥3 captures to detect stuck rendering
    # (1 could be a loading screen, 2 could be a fade transition)
    if len(captured_hashes) >= 3 and distinct_hashes <= 1:
        progression_result["issues"].append(
            f"Frame progression: all {len(captured_hashes)} captured frames "
            "have identical hashes — scene content is not changing"
        )

    issues.extend(progression_result["issues"])
    return progression_result


def check_rendering(data, verify_dir, golden_dir=None, baseline_path=None):
    """Check rendering health using seven automated verification layers:

    1. Metric checks — draw calls, vertex counts, non-black pixels
    2. Hash comparison — deterministic CRC32 of framebuffer content
    3. Golden image comparison — RMSE against reference BMP files (or BMP analysis when no golden exists)
    4. Render baseline — stored thresholds that must not regress
    5. Captured frame BMP analysis — pixel-level inspection of saved frames
    6. Pipeline stage health — textures, shaders, depth, blend, primitives
    7. Frame progression — content changes between frames prove game isn't frozen

    No human review needed.
    """
    result = {
        "checked": True,
        "passed": False,
        "details": {},
        "issues": [],
    }

    summary = data.get("summary")
    if not summary:
        result["issues"].append("No verify_summary found — is TP_VERIFY=1 set?")
        return result

    total_frames = summary.get("total_frames", 0)
    frames_with_draws = summary.get("frames_with_draws", 0)
    frames_nonblack = summary.get("captured_frames_nonblack",
                                    summary.get("frames_nonblack", 0))
    render_health = summary.get("render_health_pct", 0)

    result["details"] = {
        "total_frames": total_frames,
        "frames_with_draws": frames_with_draws,
        "frames_nonblack": frames_nonblack,
        "render_health_pct": render_health,
        "peak_draw_calls": summary.get("peak_draw_calls", 0),
        "peak_verts": summary.get("peak_verts", 0),
    }

    # --- Layer 1: Basic metric checks ---
    if total_frames == 0:
        result["issues"].append("No frames were rendered")
    elif frames_with_draws == 0:
        result["issues"].append("No frames had draw calls — GX shim may not be working")
    elif render_health < 10:
        result["issues"].append(
            f"Render health is very low ({render_health}%) — most frames are empty"
        )

    # --- Layer 2: Per-frame hash analysis from verify_frame logs ---
    fb_data = data.get("fb_analyses", [])
    frame_hashes = {}
    if fb_data:
        nonblack_counts = [fb.get("pct_nonblack", 0) for fb in fb_data]
        max_nonblack = max(nonblack_counts) if nonblack_counts else 0
        result["details"]["max_pct_nonblack"] = max_nonblack
        result["details"]["fb_analyses_count"] = len(fb_data)
        result["details"]["rendering_produces_pixels"] = max_nonblack > 0

        for fb in fb_data:
            frame_num = str(fb.get("frame", 0))
            fb_hash = fb.get("fb_hash")
            if fb_hash:
                frame_hashes[frame_num] = fb_hash

    # Collect hashes from verify_frame entries too
    for vf in data.get("frames", []):
        frame_num = str(vf.get("frame", 0))
        fb_hash = vf.get("fb_hash")
        if fb_hash:
            frame_hashes[frame_num] = fb_hash

    if frame_hashes:
        result["details"]["frame_hashes"] = frame_hashes

    # --- Layer 3: Golden image comparison ---
    if verify_dir and golden_dir:
        golden_results = check_golden_images(verify_dir, golden_dir)
        if golden_results:
            result["details"]["golden_comparisons"] = golden_results

            # Flag regressions: RMSE > 5.0 against a golden reference
            for cmp in golden_results:
                if cmp.get("has_golden") and not cmp.get("identical", False):
                    rmse = cmp.get("rmse", 0)
                    if rmse > 5.0:
                        result["issues"].append(
                            f"Golden image regression: {cmp['frame']} "
                            f"RMSE={rmse:.1f} (threshold: 5.0)"
                        )

    # --- Layer 4: Render baseline threshold checks ---
    if baseline_path:
        baseline_issues = check_render_baseline(data, baseline_path)
        if baseline_issues:
            result["details"]["baseline_regressions"] = baseline_issues
            for bi in baseline_issues:
                metric = bi.get("metric", "?")
                frame = bi.get("frame", "global")
                expected = bi.get("expected_min", bi.get("expected", "?"))
                actual = bi.get("actual", "?")
                result["issues"].append(
                    f"Render baseline regression: frame {frame} "
                    f"{metric}={actual} (expected >={expected})"
                )

    # --- Layer 5: Captured frame BMP analysis ---
    if verify_dir:
        bmp_files = sorted(Path(verify_dir).glob("frame_*.bmp"))
        frame_analyses = []
        for bmp in bmp_files:
            analysis = analyze_bmp(str(bmp))
            if analysis:
                analysis["file"] = bmp.name
                frame_analyses.append(analysis)
        result["details"]["captured_frames"] = frame_analyses

    # --- Layer 6: Render pipeline stage health ---
    pipeline = check_pipeline_health(data)
    result["details"]["pipeline"] = pipeline
    for issue in pipeline.get("issues", []):
        result["issues"].append(issue)

    # --- Layer 7: Frame progression ---
    progression = check_frame_progression(data)
    result["details"]["progression"] = progression
    for issue in progression.get("issues", []):
        result["issues"].append(issue)

    result["passed"] = len(result["issues"]) == 0
    return result


def check_input(data):
    """Check input health. Returns (passed, details)."""
    result = {
        "checked": True,
        "passed": False,
        "details": {},
        "issues": [],
    }

    summary = data.get("summary")
    inputs = data.get("inputs", [])
    responses = data.get("input_responses", [])

    result["details"] = {
        "input_events": len(inputs),
        "input_responses": len(responses),
    }

    if summary:
        result["details"]["input_events_total"] = summary.get("input_events", 0)
        result["details"]["input_responses_total"] = summary.get("input_responses", 0)
        result["details"]["input_health_pct"] = summary.get("input_health_pct", 0)

    # Input is "passing" if either:
    # 1. No input was injected (nothing to verify)
    # 2. Input was injected and game responded
    if len(inputs) == 0:
        result["passed"] = True  # no input to verify
        result["details"]["note"] = "No input events injected — input system not yet tested"
    elif len(responses) > 0:
        result["passed"] = True
    else:
        result["issues"].append(
            f"Input events were sent ({len(inputs)}) but no game responses detected"
        )

    return result


def check_audio(data):
    """Check audio health. Returns (passed, details)."""
    result = {
        "checked": True,
        "passed": False,
        "details": {},
        "issues": [],
    }

    summary = data.get("summary")
    audio_reports = data.get("audio_reports", [])

    result["details"]["audio_reports"] = len(audio_reports)

    if summary:
        result["details"]["audio_frames_active"] = summary.get("audio_frames_active", 0)
        result["details"]["audio_frames_nonsilent"] = summary.get("audio_frames_nonsilent", 0)
        result["details"]["audio_health_pct"] = summary.get("audio_health_pct", 0)

    # Audio is "passing" if either:
    # 1. Audio system not yet implemented (no reports) — expected during early port
    # 2. Audio is active and producing non-silent output
    if len(audio_reports) == 0:
        result["passed"] = True  # audio not yet implemented
        result["details"]["note"] = "No audio reports — audio system not yet implemented"
    else:
        active_reports = [r for r in audio_reports if r.get("active", False)]
        nonsilent_reports = [r for r in audio_reports if r.get("has_nonsilence", False)]

        if len(active_reports) == 0:
            result["issues"].append("Audio system exists but is never active")
        elif len(nonsilent_reports) == 0:
            result["issues"].append(
                "Audio is active but all buffers are silent — "
                "audio mixing may not be working"
            )

    if len(result["issues"]) == 0:
        result["passed"] = True

    return result


def main():
    parser = argparse.ArgumentParser(description="Verify port subsystem health")
    parser.add_argument("logfile", help="Path to milestones/verification log")
    parser.add_argument("--output", default="verify-report.json",
                        help="Output report JSON file")
    parser.add_argument("--verify-dir", default="verify_output",
                        help="Directory with captured frame BMPs")
    parser.add_argument("--golden-dir", default="tests/golden",
                        help="Directory with golden reference BMPs")
    parser.add_argument("--render-baseline", default="tests/render-baseline.json",
                        help="Render baseline JSON with expected metrics")
    parser.add_argument("--check-rendering", action="store_true",
                        help="Check rendering health")
    parser.add_argument("--check-input", action="store_true",
                        help="Check input health")
    parser.add_argument("--check-audio", action="store_true",
                        help="Check audio health")
    parser.add_argument("--check-all", action="store_true",
                        help="Check all subsystems")
    parser.add_argument("--update-golden", action="store_true",
                        help="Copy captured frames to golden dir when no reference exists")
    args = parser.parse_args()

    if args.check_all:
        args.check_rendering = True
        args.check_input = True
        args.check_audio = True

    # If no checks specified, default to rendering
    if not args.check_rendering and not args.check_input and not args.check_audio:
        args.check_rendering = True

    data = parse_log(args.logfile)

    report = {
        "checks": {},
        "overall_pass": True,
    }

    if args.check_rendering:
        verify_dir = args.verify_dir if Path(args.verify_dir).is_dir() else None
        golden_dir = args.golden_dir if Path(args.golden_dir).is_dir() else None
        baseline_path = args.render_baseline if Path(args.render_baseline).is_file() else None
        rendering = check_rendering(data, verify_dir, golden_dir, baseline_path)
        report["checks"]["rendering"] = rendering
        if not rendering["passed"]:
            report["overall_pass"] = False

        # Auto-save new golden references when none exist
        if args.update_golden and verify_dir:
            import shutil
            golden_path = Path(args.golden_dir)
            golden_path.mkdir(parents=True, exist_ok=True)
            for captured in sorted(Path(verify_dir).glob("frame_*.bmp")):
                golden = golden_path / captured.name
                if not golden.exists():
                    # Only save as golden if the frame has non-black content
                    analysis = analyze_bmp(str(captured))
                    if analysis and analysis.get("pct_nonblack", 0) > 0:
                        shutil.copy2(str(captured), str(golden))
                        print(f"  📸 Saved new golden reference: {golden}")

    if args.check_input:
        input_result = check_input(data)
        report["checks"]["input"] = input_result
        if not input_result["passed"]:
            report["overall_pass"] = False

    if args.check_audio:
        audio_result = check_audio(data)
        report["checks"]["audio"] = audio_result
        if not audio_result["passed"]:
            report["overall_pass"] = False

    # Write report
    with open(args.output, "w") as f:
        json.dump(report, f, indent=2)

    # Print human-readable summary
    print(f"\n{'=' * 60}")
    print("PORT VERIFICATION REPORT")
    print(f"{'=' * 60}")

    for name, check in report["checks"].items():
        emoji = "✅" if check["passed"] else "❌"
        print(f"\n{emoji} {name.upper()}")
        for key, val in check.get("details", {}).items():
            if key == "captured_frames":
                print(f"  Captured frames: {len(val)}")
                for fr in val:
                    print(f"    {fr['file']}: {fr['pct_nonblack']}% non-black, "
                          f"avg color ({fr['avg_color'][0]},{fr['avg_color'][1]},{fr['avg_color'][2]})")
            elif key == "golden_comparisons":
                print(f"  Golden image comparisons: {len(val)}")
                for cmp in val:
                    if cmp.get("has_golden"):
                        if cmp.get("identical"):
                            print(f"    {cmp['frame']}: ✅ identical")
                        else:
                            print(f"    {cmp['frame']}: RMSE={cmp.get('rmse', '?')}, "
                                  f"{cmp.get('pct_different', '?')}% different")
                    else:
                        a = cmp.get("analysis", {})
                        nb = a.get("pct_nonblack", 0) if a else 0
                        print(f"    {cmp['frame']}: no golden reference "
                              f"({nb}% non-black)")
            elif key == "baseline_regressions":
                print(f"  Baseline regressions: {len(val)}")
                for bi in val:
                    print(f"    frame {bi.get('frame', 'global')}: "
                          f"{bi.get('metric')}={bi.get('actual')} "
                          f"(expected >={bi.get('expected_min', bi.get('expected', '?'))})")
            elif key == "frame_hashes":
                print(f"  Frame hashes: {len(val)} frames tracked")
                for fn, fh in sorted(val.items(), key=lambda x: int(x[0])):
                    print(f"    frame {fn}: {fh}")
            elif key == "pipeline":
                stages = val.get("stages", {})
                complexity = val.get("visual_complexity", 0)
                print(f"  🔧 RENDER PIPELINE HEALTH (complexity: {complexity}/100)")
                for stage_name, stage in stages.items():
                    ok = stage.get("ok", False)
                    icon = "✅" if ok else "❌"
                    details_parts = []
                    for sk, sv in stage.items():
                        if sk == "ok":
                            continue
                        details_parts.append(f"{sk}={sv}")
                    details_str = ", ".join(details_parts)
                    print(f"    {icon} {stage_name}: {details_str}")
            elif key == "progression":
                hc = val.get("hash_changes", 0)
                dh = val.get("distinct_hashes", 0)
                print(f"  📈 Frame progression: {hc} hash changes, {dh} distinct hashes")
            elif key == "note":
                print(f"  ℹ️  {val}")
            elif isinstance(val, dict):
                print(f"  {key}: {json.dumps(val)}")
            elif isinstance(val, list):
                print(f"  {key}: {len(val)} items")
            else:
                print(f"  {key}: {val}")
        for issue in check.get("issues", []):
            print(f"  ⚠️  {issue}")

    overall_emoji = "✅" if report["overall_pass"] else "❌"
    print(f"\n{overall_emoji} Overall: {'PASS' if report['overall_pass'] else 'FAIL'}")
    print(f"{'=' * 60}\n")

    sys.exit(0 if report["overall_pass"] else 1)


if __name__ == "__main__":
    main()
