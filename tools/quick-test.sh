#!/usr/bin/env bash
# tools/quick-test.sh — Shared render phase runner for Phase 3 and Phase 4.
#
# Encapsulates Phase 3 (3D room, frame 129) and Phase 4 (gameplay intro,
# frame 200) capture logic so CI (port-test.yml) and self-test (self-test.sh)
# both invoke the same code path without drift.
#
# Usage:
#   tools/quick-test.sh --phase 3 [options]    # 3D room capture, frame 129
#   tools/quick-test.sh --phase 4 [options]    # Gameplay intro capture, frame 200
#
# Options:
#   --phase N         Which phase to run: 3 (3D room) or 4 (gameplay) (required)
#   --binary PATH     tp-pc binary path (default: build/tp-pc)
#   --output-dir DIR  BMP captures + log output directory
#                     (default: quick-test-output-phase3 / quick-test-output-phase4)
#   --timeout SECS    Override default timeout (phase 3 default: 590, phase 4: 120)
#   --best-effort     Exit 0 even if the coverage gate fails (informational only)
#
# Exit codes:
#   0 = pass (gate satisfied, or --best-effort)
#   1 = gate failure (only when --best-effort is not set)
#   2 = usage / binary-not-found error

set -euo pipefail

PHASE=""
BINARY="${BINARY:-build/tp-pc}"
OUTPUT_DIR=""
TIMEOUT_SECS=""
BEST_EFFORT=0
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --phase)       PHASE="$2"; shift 2 ;;
        --binary)      BINARY="$2"; shift 2 ;;
        --output-dir)  OUTPUT_DIR="$2"; shift 2 ;;
        --timeout)     TIMEOUT_SECS="$2"; shift 2 ;;
        --best-effort) BEST_EFFORT=1; shift ;;
        --help|-h)
            sed -n '2,20p' "$0"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$PHASE" ]]; then
    echo "ERROR: --phase N is required (supported: 3, 4)" >&2
    exit 2
fi

cd "$PROJECT_ROOT"

if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: binary not found: $BINARY" >&2
    exit 2
fi

# --- Phase configuration ---
case "$PHASE" in
    3)
        OUTPUT_DIR="${OUTPUT_DIR:-quick-test-output-phase3}"
        TIMEOUT_SECS="${TIMEOUT_SECS:-590}"
        PHASE_DESC="3D room frame capture (frame 129)"
        DISPLAY_NUM=":102"
        CAPTURE_FRAME="0129"
        GATE="frame_0129:1"
        GATE_MSG="3D room gate FAILED: frame_0129 pct_nonblack < 1% — check centroid camera (centroid_view_switch) and view 1 depth-clear in gx_render.cpp"
        ;;
    4)
        OUTPUT_DIR="${OUTPUT_DIR:-quick-test-output-phase4}"
        TIMEOUT_SECS="${TIMEOUT_SECS:-120}"
        PHASE_DESC="gameplay intro frame capture (frame 200)"
        DISPLAY_NUM=":103"
        CAPTURE_FRAME="0200"
        GATE="frame_0200:1"
        GATE_MSG="Gameplay gate FAILED: frame_0200 pct_nonblack < 1% — check PROC_TITLE draw pipeline (TP_ENABLE_PROC_TITLE=1) and daTitle_c::Draw() in d_a_title.cpp"
        ;;
    *)
        echo "ERROR: Unknown phase: $PHASE (supported: 3, 4)" >&2
        exit 2 ;;
esac

LOG_FILE="$OUTPUT_DIR/milestones_phase${PHASE}.log"
mkdir -p "$OUTPUT_DIR"

echo "=== Phase $PHASE: $PHASE_DESC ==="
echo "  Binary:     $BINARY"
echo "  Output dir: $OUTPUT_DIR"
echo "  Timeout:    ${TIMEOUT_SECS}s"

# --- Start Xvfb (only if caller has not already set DISPLAY) ---
XVFB_PID=""
if [[ -z "${DISPLAY:-}" ]]; then
    Xvfb "$DISPLAY_NUM" -screen 0 640x480x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    export DISPLAY="$DISPLAY_NUM"
    for i in 1 2 3 4 5; do
        xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1 && break
        sleep 1
    done
fi

_cleanup_xvfb() {
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
        XVFB_PID=""
    fi
}
trap _cleanup_xvfb EXIT INT TERM

# --- Environment variables ---
export GALLIUM_DRIVER=softpipe
export LIBGL_ALWAYS_SOFTWARE=1
export TP_VERIFY=1
export TP_VERIFY_DIR="$OUTPUT_DIR"
unset TP_SKIP_DL_DRAWS  2>/dev/null || true
unset TP_SYNC_RENDER    2>/dev/null || true

case "$PHASE" in
    3)
        export TP_TEST_FRAMES=130
        export TP_VERIFY_CAPTURE_FRAMES="129"
        export TP_FRAME_DELAY_MS=350000
        export TP_FRAME_DELAY_START=129
        export TP_SKIP_FADE=1
        unset TP_ENABLE_PROC_TITLE 2>/dev/null || true
        ;;
    4)
        export TP_TEST_FRAMES=201
        export TP_VERIFY_CAPTURE_FRAMES="200"
        export TP_FRAME_DELAY_MS=60000
        export TP_FRAME_DELAY_START=200
        export TP_SKIP_FADE=1
        export TP_ENABLE_PROC_TITLE=1
        ;;
esac

# --- Run the game ---
timeout -k 10 "${TIMEOUT_SECS}s" "$BINARY" 2>&1 | tee "$LOG_FILE" || true
RUN_EXIT=${PIPESTATUS[0]}
echo "Phase $PHASE exit: $RUN_EXIT"

_cleanup_xvfb
trap - EXIT INT TERM

# --- Report BMP coverage ---
BMP_COUNT=$(ls "$OUTPUT_DIR"/frame_*.bmp 2>/dev/null | wc -l)
echo "Phase $PHASE BMP frames: $BMP_COUNT"
python3 tools/check_bmp_coverage.py "$OUTPUT_DIR"/frame_*.bmp 2>/dev/null || true

# --- Phase-specific diagnostic greps ---
if [[ "$PHASE" == "3" ]]; then
    echo "Phase 3 RASC inject values:"
    grep '"rasc_inject"' "$LOG_FILE" 2>/dev/null || echo "(none found)"
    echo "Phase 3 geometry-centroid camera fallback:"
    grep '"geom_centroid_cam"' "$LOG_FILE" 2>/dev/null || echo "(centroid not triggered)"
    echo "Phase 3 centroid view switch (depth-clear view):"
    grep '"centroid_view_switch"' "$LOG_FILE" 2>/dev/null || echo "(centroid view switch not triggered)"
    echo "Phase 3 fade overlay (darwFilter) — shows alpha at frame 129 transition:"
    grep '"darwFilter"' "$LOG_FILE" 2>/dev/null | head -10 || echo "(no fade overlay calls)"
    echo "Phase 3 3D geometry diagnostics (MVP + frustum check):"
    grep '"rasc_geom_dump"' "$LOG_FILE" 2>/dev/null | head -5 || echo "(none found)"
    echo "Phase 3 per-frame draw counts (frames 125-210):"
    grep '"frame_dc"' "$LOG_FILE" 2>/dev/null | head -90 || echo "(none found)"
elif [[ "$PHASE" == "4" ]]; then
    echo "Phase 4 per-frame draw counts (frames 125-210):"
    grep '"frame_dc"' "$LOG_FILE" 2>/dev/null | head -90 || echo "(none found)"
    echo "Phase 4 fade overlay (darwFilter) — shows alpha at frame 200:"
    grep '"darwFilter"' "$LOG_FILE" 2>/dev/null | head -10 || echo "(no fade overlay calls)"
    echo "Phase 4 PROC_TITLE enable status + j3d_draw_diag at frame 200:"
    grep 'TP_ENABLE_PROC_TITLE\|j3d_draw_diag.*"frame":200' "$LOG_FILE" 2>/dev/null | head -5 || \
        echo "(check $LOG_FILE for draw counts)"
    echo "Phase 4 opening scene crash catch:"
    grep 'demo/event crash at' "$LOG_FILE" 2>/dev/null || echo "(no crash catch logged)"
fi

# --- Coverage gate ---
GATE_PASS=0
BMP_PATH="$OUTPUT_DIR/frame_${CAPTURE_FRAME}.bmp"
if [[ -f "$BMP_PATH" ]]; then
    python3 tools/check_bmp_coverage.py "$BMP_PATH" \
        --require "$GATE" && GATE_PASS=1 || true
fi

if [[ "$GATE_PASS" -eq 0 ]]; then
    echo "⚠️  $GATE_MSG"
    [[ "$BEST_EFFORT" -eq 1 ]] || exit 1
else
    echo "✅ Phase $PHASE gate passed ($GATE)"
fi

# --- Convert raw frames to MP4 (optional, best-effort) ---
if [[ -f "$OUTPUT_DIR/raw_frames.bin" ]]; then
    FRAME_SIZE=$(wc -c < "$OUTPUT_DIR/raw_frames.bin" 2>/dev/null || echo 0)
    PIXEL_BYTES=$((640 * 480 * 4))
    if [[ "$FRAME_SIZE" -gt 0 && "$PIXEL_BYTES" -gt 0 ]]; then
        NUM_FRAMES=$(( FRAME_SIZE / PIXEL_BYTES ))
        echo "Phase $PHASE raw video: $NUM_FRAMES frames"
        ffmpeg -y -f rawvideo -pixel_format rgba -video_size 640x480 \
            -framerate 5 -i "$OUTPUT_DIR/raw_frames.bin" \
            -c:v libx264 -pix_fmt yuv420p -crf 18 \
            "$OUTPUT_DIR/phase${PHASE}_render.mp4" 2>/dev/null && \
            echo "Phase $PHASE MP4: $(ls -la "$OUTPUT_DIR/phase${PHASE}_render.mp4")" || \
            echo "Phase $PHASE ffmpeg conversion failed"
        rm -f "$OUTPUT_DIR/raw_frames.bin"
    fi
fi

exit 0
