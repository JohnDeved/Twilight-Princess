#!/usr/bin/env bash
# quick-test.sh — Fast single-frame render test for agent iteration
#
# Renders ONE frame from the intro sequence and validates milestones.
# Treats rendering like a video render: each frame takes its time,
# no realtime requirement. Optimized for speed so agents can run
# locally without waiting for CI.
#
# Default: captures the title screen (frame 10, ~10-20s).
# With --3d: captures the 3D gameplay room (frame 129, ~60-90s).
#
# Usage:
#   tools/quick-test.sh                # title screen (~10-20s)
#   tools/quick-test.sh --3d           # 3D gameplay room (~60-90s)
#   tools/quick-test.sh --skip-build   # skip build step
#   tools/quick-test.sh --frame 60     # capture a specific frame
#
# Exit codes:
#   0 = all checks passed
#   1 = test failure
#   2 = build failure

set -euo pipefail

# --- Defaults ---
SKIP_BUILD=0
TARGET_FRAME=10
MODE="title"           # "title" (frame 10) or "3d" (frame 129)
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Output directory — persistent (not auto-cleaned) so agents can inspect BMPs
OUTPUT_DIR="$PROJECT_ROOT/quick-test-output"

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)   SKIP_BUILD=1; shift ;;
        --3d)           MODE="3d"; TARGET_FRAME=129; shift ;;
        --frame)        TARGET_FRAME="$2"; MODE="custom"; shift 2 ;;
        --help|-h)
            echo "Usage: tools/quick-test.sh [--skip-build] [--3d] [--frame N]"
            echo ""
            echo "  --skip-build    Skip the build step (use existing binary)"
            echo "  --3d            Capture 3D gameplay room at frame 129 (~60-90s)"
            echo "  --frame N       Capture a specific frame number"
            echo ""
            echo "Default: captures title screen at frame 10 (~10-20s)"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

cd "$PROJECT_ROOT"

# --- Check prerequisites ---
for tool in Xvfb xdpyinfo python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "❌ Required tool not found: $tool"
        echo "   Install: sudo apt-get install xvfb x11-utils python3"
        exit 2
    fi
done

# Calculate how many frames to run (target + 2 for pipeline flush)
TEST_FRAMES=$((TARGET_FRAME + 2))

# For 3D mode with async rendering, we need a frame delay to let Mesa
# softpipe finish rasterising the heavy 7587-draw batch before capture.
FRAME_DELAY_MS=0
if [ "$MODE" = "3d" ] || [ "$TARGET_FRAME" -ge 128 ]; then
    FRAME_DELAY_MS=90000   # 90s — enough for softpipe locally
fi

# --- Banner ---
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                   TP QUICK RENDER TEST                      ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  Mode:         $MODE"
echo "  Target frame: $TARGET_FRAME"
echo "  Test frames:  $TEST_FRAMES"
echo "  Output dir:   $OUTPUT_DIR"
if [ "$FRAME_DELAY_MS" -gt 0 ]; then
    echo "  Frame delay:  ${FRAME_DELAY_MS}ms (async render wait)"
fi
echo ""

START_TIME=$(date +%s)

# --- Step 0: Game data check ---
if [ ! -d data/res ]; then
    echo "  ⚠  Game data not found in data/res"
    echo "     The test will run but may not produce visible content."
    echo "     Set ROMS_TOKEN + install gh CLI to auto-download game data."
    echo ""
fi

# --- Step 1: Build ---
if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "━━━ Step 1/4: Build ━━━"
    if [ ! -f build/build.ninja ]; then
        echo "  Configuring CMake..."
        cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC 2>&1 | tail -3
    fi
    echo "  Building tp-pc..."
    if ! ninja -C build tp-pc 2>&1 | tail -5; then
        echo ""
        echo "❌ BUILD FAILED"
        exit 2
    fi
    echo "  ✅ Build succeeded"
else
    echo "━━━ Step 1/4: Build (skipped) ━━━"
    if [ ! -f build/tp-pc ]; then
        echo "  ❌ build/tp-pc not found — run without --skip-build first"
        exit 2
    fi
    echo "  ✅ Using existing build/tp-pc"
fi
echo ""

# --- Step 2: Run single-frame render ---
echo "━━━ Step 2/4: Render frame $TARGET_FRAME ━━━"
mkdir -p "$OUTPUT_DIR"

# Start Xvfb if not already running
XVFB_PID=""
XVFB_DISPLAY=":98"
if ! xdpyinfo -display "$XVFB_DISPLAY" >/dev/null 2>&1; then
    Xvfb "$XVFB_DISPLAY" -screen 0 640x480x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do
        xdpyinfo -display "$XVFB_DISPLAY" >/dev/null 2>&1 && break
        sleep 1
    done
fi

# Clean up Xvfb on exit
cleanup() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Set rendering environment
export DISPLAY="$XVFB_DISPLAY"
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-softpipe}"
export LIBGL_ALWAYS_SOFTWARE=1
export TP_VERIFY=1
export TP_VERIFY_DIR="$OUTPUT_DIR"
export TP_VERIFY_CAPTURE_FRAMES="$TARGET_FRAME"
export TP_TEST_FRAMES="$TEST_FRAMES"

# For 3D frames, use async rendering with frame delay
# (TP_SYNC_RENDER crashes with Mesa GL context thread mismatch)
if [ "$FRAME_DELAY_MS" -gt 0 ]; then
    export TP_FRAME_DELAY_MS="$FRAME_DELAY_MS"
    export TP_FRAME_DELAY_START="$TARGET_FRAME"
    unset TP_SYNC_RENDER
else
    unset TP_FRAME_DELAY_MS
    unset TP_SYNC_RENDER
fi

# Don't set TP_HEADLESS — we need real rendering, and the per-frame
# timeout (60s) in m_Do_main.cpp only fires when BOTH TP_HEADLESS=1
# AND DISPLAY is set. Without TP_HEADLESS, no timeout interference.
unset TP_HEADLESS

# Calculate timeout: frame delay + 120s for game logic + render
TIMEOUT_S=$((FRAME_DELAY_MS / 1000 + 120))

echo "  Running game for $TEST_FRAMES frames (timeout ${TIMEOUT_S}s)..."
RENDER_START=$(date +%s)
timeout -k 10 "${TIMEOUT_S}s" build/tp-pc 2>&1 | tee "$OUTPUT_DIR/test.log" || true
RENDER_END=$(date +%s)
RENDER_ELAPSED=$((RENDER_END - RENDER_START))
echo "  Render completed in ${RENDER_ELAPSED}s"

# Check for captured BMP
TARGET_BMP="$OUTPUT_DIR/$(printf 'frame_%04d.bmp' "$TARGET_FRAME")"
if [ -f "$TARGET_BMP" ]; then
    BMP_SIZE=$(stat -c%s "$TARGET_BMP" 2>/dev/null || stat -f%z "$TARGET_BMP" 2>/dev/null || echo 0)
    echo "  ✅ Frame $TARGET_FRAME captured: $TARGET_BMP ($(( BMP_SIZE / 1024 ))KB)"
else
    echo "  ⚠  Frame $TARGET_FRAME BMP not found at $TARGET_BMP"
    echo "     (Check $OUTPUT_DIR/test.log for errors)"
fi
echo ""

# --- Step 3: Parse milestones ---
echo "━━━ Step 3/4: Parse milestones ━━━"
if python3 tools/parse_milestones.py "$OUTPUT_DIR/test.log" \
    --output "$OUTPUT_DIR/milestone-summary.json" \
    --min-milestone 0 2>&1 | tail -5; then
    # Extract milestone count from JSON
    if command -v python3 >/dev/null 2>&1; then
        MILESTONE_COUNT=$(python3 -c "
import json, sys
try:
    d = json.load(open('$OUTPUT_DIR/milestone-summary.json'))
    print(d.get('milestones_reached_count', '?'))
except: print('?')
" 2>/dev/null || echo "?")
        echo "  Milestones reached: $MILESTONE_COUNT"
    fi
else
    echo "  ⚠️  Milestone parsing reported issues"
fi
echo ""

# --- Step 4: Check regression ---
echo "━━━ Step 4/4: Check regression ━━━"
if [ -f milestone-baseline.json ]; then
    python3 tools/check_milestone_regression.py "$OUTPUT_DIR/milestone-summary.json" \
        --baseline milestone-baseline.json \
        --output "$OUTPUT_DIR/regression-report.json" 2>&1 | tail -5 || true
else
    echo "  ⚠  No baseline found (milestone-baseline.json missing)"
fi
echo ""

# --- Summary ---
END_TIME=$(date +%s)
TOTAL_ELAPSED=$((END_TIME - START_TIME))

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Quick test completed in ${TOTAL_ELAPSED}s                              "
echo "╠══════════════════════════════════════════════════════════════╣"
if [ -f "$TARGET_BMP" ]; then
echo "║  ✅ Frame $TARGET_FRAME captured: $TARGET_BMP"
else
echo "║  ⚠  No frame captured (check test.log for errors)"
fi
echo "║  📁 Output: $OUTPUT_DIR/"
echo "║     test.log              — raw test output"
echo "║     milestone-summary.json — parsed milestones"
if [ -f "$TARGET_BMP" ]; then
echo "║     $(basename "$TARGET_BMP")         — rendered frame"
fi
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Check if frame was captured
if [ -f "$TARGET_BMP" ]; then
    # Run pixel coverage check if available
    if [ -f tools/check_bmp_coverage.py ]; then
        echo "Pixel coverage:"
        python3 tools/check_bmp_coverage.py "$TARGET_BMP" 2>/dev/null || true
        echo ""
    fi
    exit 0
else
    exit 1
fi
