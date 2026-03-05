#!/usr/bin/env bash
# quick-test.sh — Fast render test for agent iteration
#
# Renders frame(s) from the intro sequence and validates milestones.
# Treats rendering like a video render: each frame takes its time,
# no realtime requirement. Used both locally and by CI.
#
# Default: captures the title screen (frame 10, ~10-20s).
# With --3d: adds the 3D gameplay room (frame 129, ~60-90s).
#
# Usage:
#   tools/quick-test.sh                  # title screen (~10-20s)
#   tools/quick-test.sh --3d             # 3D gameplay room (~60-90s)
#   tools/quick-test.sh --frame 10 --3d  # both title + 3D in one pass
#   tools/quick-test.sh --skip-build     # skip build step
#   tools/quick-test.sh --frame 60       # capture a specific frame
#
# CI usage (render-only mode, CI handles post-processing):
#   tools/quick-test.sh --skip-build --render-only \
#     --output-dir verify_output --frame 10 --3d --frame-delay 120000
#
# Exit codes:
#   0 = all checks passed (or --render-only mode completed)
#   1 = test failure (no frames captured)
#   2 = build failure

set -euo pipefail

# --- Defaults ---
SKIP_BUILD=0
RENDER_ONLY=0
FRAMES=()
FRAME_DELAY_OVERRIDE=""
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR=""

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)   SKIP_BUILD=1; shift ;;
        --3d)           FRAMES+=(129); shift ;;
        --frame)        FRAMES+=("$2"); shift 2 ;;
        --output-dir)   OUTPUT_DIR="$2"; shift 2 ;;
        --render-only)  RENDER_ONLY=1; shift ;;
        --frame-delay)  FRAME_DELAY_OVERRIDE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: tools/quick-test.sh [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --skip-build       Skip the build step (use existing binary)"
            echo "  --3d               Capture 3D gameplay room at frame 129 (~60-90s)"
            echo "  --frame N          Capture frame N (can specify multiple times)"
            echo "  --output-dir DIR   Output directory (default: quick-test-output/)"
            echo "  --render-only      Only render + capture, skip milestone parse/regression"
            echo "  --frame-delay MS   Override frame delay for 3D frames (default: 90000ms)"
            echo ""
            echo "Default: captures title screen at frame 10 (~10-20s)"
            echo ""
            echo "CI usage:"
            echo "  tools/quick-test.sh --skip-build --render-only \\"
            echo "    --output-dir verify_output --frame 10 --3d --frame-delay 120000"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

cd "$PROJECT_ROOT"

# Default to frame 10 if no frames specified
if [ ${#FRAMES[@]} -eq 0 ]; then
    FRAMES=(10)
fi

# Remove duplicates and sort numerically
FRAMES=($(printf '%s\n' "${FRAMES[@]}" | sort -un))

# Output directory — persistent (not auto-cleaned) so agents can inspect BMPs
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/quick-test-output}"

# Calculate max frame for TEST_FRAMES
MAX_FRAME=0
for f in "${FRAMES[@]}"; do
    [ "$f" -gt "$MAX_FRAME" ] && MAX_FRAME="$f"
done
TEST_FRAMES=$((MAX_FRAME + 2))

# Build capture frames list (comma-separated)
CAPTURE_LIST=$(IFS=,; echo "${FRAMES[*]}")

# Determine frame delay (needed if any frame >= 128)
FRAME_DELAY_MS=0
DELAY_START=0
for f in "${FRAMES[@]}"; do
    if [ "$f" -ge 128 ]; then
        FRAME_DELAY_MS="${FRAME_DELAY_OVERRIDE:-90000}"
        DELAY_START="$f"
        break
    fi
done

# Mode description
if [ ${#FRAMES[@]} -eq 1 ]; then
    case "${FRAMES[0]}" in
        10)  MODE="title" ;;
        129) MODE="3d" ;;
        *)   MODE="custom (frame ${FRAMES[0]})" ;;
    esac
else
    MODE="multi (frames: $CAPTURE_LIST)"
fi

# --- Check prerequisites ---
for tool in Xvfb xdpyinfo python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "❌ Required tool not found: $tool"
        echo "   Install: sudo apt-get install xvfb x11-utils python3"
        exit 2
    fi
done

# Steps count depends on mode
if [ "$RENDER_ONLY" -eq 1 ]; then
    TOTAL_STEPS=2
else
    TOTAL_STEPS=4
fi

# --- Banner ---
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                   TP QUICK RENDER TEST                      ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  Mode:           $MODE"
echo "  Capture frames: $CAPTURE_LIST"
echo "  Test frames:    $TEST_FRAMES"
echo "  Output dir:     $OUTPUT_DIR"
if [ "$FRAME_DELAY_MS" -gt 0 ]; then
    echo "  Frame delay:    ${FRAME_DELAY_MS}ms at frame $DELAY_START"
fi
if [ "$RENDER_ONLY" -eq 1 ]; then
    echo "  Render-only:    yes (skip milestone parse/regression)"
fi
echo ""

START_TIME=$(date +%s)

# --- Step 0: Game data check ---
if [ ! -d data/res ]; then
    echo "  ⚠  Game data not found in data/res"
    echo "     The test will run but may not produce visible content."
    echo ""
fi

# --- Step 1: Build ---
if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "━━━ Step 1/$TOTAL_STEPS: Build ━━━"
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
    echo "━━━ Step 1/$TOTAL_STEPS: Build (skipped) ━━━"
    if [ ! -f build/tp-pc ]; then
        echo "  ❌ build/tp-pc not found — run without --skip-build first"
        exit 2
    fi
    echo "  ✅ Using existing build/tp-pc"
fi
echo ""

# --- Step 2: Render ---
echo "━━━ Step 2/$TOTAL_STEPS: Render frames ($CAPTURE_LIST) ━━━"
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
export TP_VERIFY_CAPTURE_FRAMES="$CAPTURE_LIST"
export TP_TEST_FRAMES="$TEST_FRAMES"

# For 3D frames, use async rendering with frame delay
# (TP_SYNC_RENDER crashes with Mesa GL context thread mismatch)
if [ "$FRAME_DELAY_MS" -gt 0 ]; then
    export TP_FRAME_DELAY_MS="$FRAME_DELAY_MS"
    export TP_FRAME_DELAY_START="$DELAY_START"
fi
unset TP_SYNC_RENDER 2>/dev/null || true

# Don't set TP_HEADLESS — we need real rendering, and the per-frame
# timeout (60s) in m_Do_main.cpp only fires when BOTH TP_HEADLESS=1
# AND DISPLAY is set. Without TP_HEADLESS, no timeout interference.
unset TP_HEADLESS 2>/dev/null || true

# Calculate timeout: frame delay + 120s for game logic + render
TIMEOUT_S=$((FRAME_DELAY_MS / 1000 + 120))

echo "  Running game for $TEST_FRAMES frames (timeout ${TIMEOUT_S}s)..."
RENDER_START=$(date +%s)
timeout -k 10 "${TIMEOUT_S}s" build/tp-pc 2>&1 | tee "$OUTPUT_DIR/test.log" || true
RENDER_END=$(date +%s)
RENDER_ELAPSED=$((RENDER_END - RENDER_START))
echo "  Render completed in ${RENDER_ELAPSED}s"

# Check for captured BMPs
CAPTURED=0
for f in "${FRAMES[@]}"; do
    BMP="$OUTPUT_DIR/$(printf 'frame_%04d.bmp' "$f")"
    if [ -f "$BMP" ]; then
        BMP_SIZE=$(stat -c%s "$BMP" 2>/dev/null || stat -f%z "$BMP" 2>/dev/null || echo 0)
        echo "  ✅ Frame $f captured: $BMP ($(( BMP_SIZE / 1024 ))KB)"
        CAPTURED=$((CAPTURED + 1))
    else
        echo "  ⚠  Frame $f BMP not found at $BMP"
    fi
done
echo "  Captured: $CAPTURED/${#FRAMES[@]} frames"
echo ""

# --- Render-only mode: stop here ---
if [ "$RENDER_ONLY" -eq 1 ]; then
    END_TIME=$(date +%s)
    TOTAL_ELAPSED=$((END_TIME - START_TIME))
    echo "━━━ Render completed in ${TOTAL_ELAPSED}s (render-only mode) ━━━"
    echo "  Output: $OUTPUT_DIR/"
    # Run pixel coverage check if available
    if [ -f tools/check_bmp_coverage.py ]; then
        for f in "${FRAMES[@]}"; do
            BMP="$OUTPUT_DIR/$(printf 'frame_%04d.bmp' "$f")"
            if [ -f "$BMP" ]; then
                echo "  Pixel coverage for frame $f:"
                python3 tools/check_bmp_coverage.py "$BMP" 2>/dev/null || true
            fi
        done
    fi
    exit 0
fi

# --- Step 3: Parse milestones ---
echo "━━━ Step 3/$TOTAL_STEPS: Parse milestones ━━━"
if python3 tools/parse_milestones.py "$OUTPUT_DIR/test.log" \
    --output "$OUTPUT_DIR/milestone-summary.json" \
    --min-milestone 0 2>&1 | tail -5; then
    # Extract milestone count from JSON
    MILESTONE_COUNT=$(python3 -c "
import json, sys
try:
    d = json.load(open('$OUTPUT_DIR/milestone-summary.json'))
    print(d.get('milestones_reached_count', '?'))
except: print('?')
" 2>/dev/null || echo "?")
    echo "  Milestones reached: $MILESTONE_COUNT"
else
    echo "  ⚠️  Milestone parsing reported issues"
fi
echo ""

# --- Step 4: Check regression ---
echo "━━━ Step 4/$TOTAL_STEPS: Check regression ━━━"
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

echo ""
echo "━━━ Quick test completed in ${TOTAL_ELAPSED}s ━━━"
echo ""
for f in "${FRAMES[@]}"; do
    BMP="$OUTPUT_DIR/$(printf 'frame_%04d.bmp' "$f")"
    if [ -f "$BMP" ]; then
        echo "  ✅ Frame $f captured: $BMP"
    else
        echo "  ⚠  Frame $f not captured"
    fi
done
echo ""
echo "  Output: $OUTPUT_DIR/"
echo "    test.log               — raw test output"
echo "    milestone-summary.json — parsed milestones"
for f in "${FRAMES[@]}"; do
    BMP_NAME="$(printf 'frame_%04d.bmp' "$f")"
    if [ -f "$OUTPUT_DIR/$BMP_NAME" ]; then
        echo "    $BMP_NAME          — rendered frame $f"
    fi
done
echo ""

# Run pixel coverage check if available
if [ -f tools/check_bmp_coverage.py ]; then
    for f in "${FRAMES[@]}"; do
        BMP="$OUTPUT_DIR/$(printf 'frame_%04d.bmp' "$f")"
        if [ -f "$BMP" ]; then
            echo "Pixel coverage for frame $f:"
            python3 tools/check_bmp_coverage.py "$BMP" 2>/dev/null || true
            echo ""
        fi
    done
fi

# Exit based on capture success
if [ "$CAPTURED" -gt 0 ]; then
    exit 0
else
    exit 1
fi
