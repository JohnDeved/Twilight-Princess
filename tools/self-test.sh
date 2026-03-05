#!/usr/bin/env bash
# self-test.sh — One-command local test pipeline for AI agents
#
# Runs the same checks as CI without needing to push:
#   1. Build (incremental)
#   2. Logic test (Noop renderer, 400 frames)
#   3. Render test (via quick-test.sh)
#   4. Parse milestones
#   5. Check milestone regression
#   6. Verify rendering/input/audio
#   7. Validate telemetry
#
# Usage:
#   tools/self-test.sh              # full pipeline (build + test)
#   tools/self-test.sh --skip-build # test only (assumes already built)
#   tools/self-test.sh --frames 300 # fewer logic frames (default: 400)
#   tools/self-test.sh --quick      # alias for --skip-build --frames 100
#   tools/self-test.sh --crash-report          # run under GDB, print crash stack traces
#   tools/self-test.sh --crash-report --unique # deduplicated crash sites only
#
# Exit codes:
#   0 = all checks passed
#   1 = test failure (regression, integrity, or crash)
#   2 = build failure

set -euo pipefail

# --- Defaults ---
SKIP_BUILD=0
FRAMES=400
CRASH_REPORT=0
CRASH_UNIQUE=0
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tp-self-test.XXXXXX")" || {
    echo "ERROR: Failed to create temporary directory" >&2
    exit 2
}

# Clean up temp dir on exit
cleanup_tmp() {
    if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then
        rm -rf -- "$TMP_DIR"
    fi
}
trap cleanup_tmp EXIT

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)   SKIP_BUILD=1; shift ;;
        --frames)       FRAMES="$2"; shift 2 ;;
        --quick)        SKIP_BUILD=1; FRAMES=100; shift ;;
        --crash-report) CRASH_REPORT=1; shift ;;
        --unique)       CRASH_UNIQUE=1; shift ;;
        --help|-h)
            echo "Usage: tools/self-test.sh [--skip-build] [--frames N] [--quick]"
            echo "                          [--crash-report [--unique]]"
            echo ""
            echo "  --skip-build    Skip the build step (use existing binary)"
            echo "  --frames N      Number of logic test frames (default: 400)"
            echo "  --quick         Alias for --skip-build --frames 100"
            echo "  --crash-report  Run under GDB and print all crash stack traces"
            echo "  --unique        With --crash-report: only show first crash at each site"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

cd "$PROJECT_ROOT"

# --- Setup ---
mkdir -p "$TMP_DIR"
PASS=1

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                    TP PORT SELF-TEST                        ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  Logic frames: $FRAMES"
echo "  Skip build:   $SKIP_BUILD"
echo "  Output dir:   $TMP_DIR"
echo ""

# --- Step 0: Game data ---
if [ ! -d data/res ]; then
    if [ -n "${ROMS_TOKEN:-}" ] && command -v gh >/dev/null 2>&1; then
        echo "━━━ Step 0: Downloading game data ━━━"
        python3 tools/setup_game_data.py \
            --data-dir data \
            --rom-dir orig/GZ2E01 \
            --extract-dir build/extract/GZ2E01 \
            --tools-dir build/tools
        echo "  ✅ Game data ready"
    else
        echo "  ⚠  Game data not found (set ROMS_TOKEN + install gh CLI to auto-download)"
    fi
else
    echo "  ✅ Game data found"
fi
echo ""

# --- Step 1: Build ---
if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "━━━ Step 1/7: Build ━━━"
    if [ ! -f build/build.ninja ]; then
        echo "  Configuring CMake..."
        cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC
    fi
    echo "  Building tp-pc..."
    if ! ninja -C build tp-pc; then
        echo ""
        echo "❌ BUILD FAILED"
        exit 2
    fi
    echo "  ✅ Build succeeded"
else
    echo "━━━ Step 1/7: Build (skipped) ━━━"
    if [ ! -f build/tp-pc ]; then
        echo "  ❌ build/tp-pc not found — run without --skip-build first"
        exit 2
    fi
    echo "  ✅ Using existing build/tp-pc"
fi
echo ""

# --- Step 2: Logic test (Noop renderer, matches CI Phase 1) ---
echo "━━━ Step 2/7: Logic test (Noop, $FRAMES frames) ━━━"
unset DISPLAY || true
export TP_HEADLESS=1
export TP_TEST_FRAMES="$FRAMES"
timeout 120s build/tp-pc 2>&1 | tee "$TMP_DIR/milestones_logic.log" || true
echo "  ✅ Logic test completed"
echo ""

# --- Step 3: Render test (via quick-test.sh, matches CI Phase 2) ---
echo "━━━ Step 3/7: Render test (softpipe, frame 10 + 129) ━━━"
mkdir -p "$TMP_DIR/verify"
tools/quick-test.sh --skip-build --render-only \
    --output-dir "$TMP_DIR/verify" \
    --frame 10 --3d || true
cp "$TMP_DIR/verify/test.log" "$TMP_DIR/milestones_render.log" 2>/dev/null || touch "$TMP_DIR/milestones_render.log"
echo "  ✅ Render test completed"
echo ""

# Merge logs (matches CI)
cat "$TMP_DIR/milestones_logic.log" "$TMP_DIR/milestones_render.log" > "$TMP_DIR/milestones.log"

# --- Step 4: Parse milestones ---
# Uses logic log for boot milestones (16/16 count), with --goal-log
# to supplement goal milestones (GOAL_INTRO_VISIBLE etc.) from the render phase.
echo "━━━ Step 4/7: Parse milestones ━━━"
if ! python3 tools/parse_milestones.py "$TMP_DIR/milestones_logic.log" \
    --output "$TMP_DIR/milestone-summary.json" \
    --min-milestone 0 \
    --goal-log "$TMP_DIR/milestones_render.log"; then
    echo "  ⚠️  Milestone parsing reported issues"
    PASS=0
fi
echo ""

# --- Step 5: Check regression ---
echo "━━━ Step 5/7: Check milestone regression ━━━"
if ! python3 tools/check_milestone_regression.py "$TMP_DIR/milestone-summary.json" \
    --baseline milestone-baseline.json \
    --output "$TMP_DIR/regression-report.json" \
    --auto-update; then
    echo "  ⚠️  Regression detected or integrity failure"
    PASS=0
fi
echo ""

# --- Step 6: Verify rendering ---
echo "━━━ Step 6/7: Verify rendering ━━━"
if ! python3 tools/verify_port.py "$TMP_DIR/milestones.log" \
    --output "$TMP_DIR/verify-report.json" \
    --verify-dir "$TMP_DIR/verify" \
    --golden-dir tests/golden \
    --render-baseline tests/render-baseline.json \
    --check-all; then
    echo "  ⚠️  Verification issues found"
    PASS=0
fi
echo ""

# --- Step 7: Validate telemetry (matches CI) ---
echo "━━━ Step 7/7: Validate telemetry ━━━"
TP_TELEMETRY_ENFORCE_PEAK_DL=1 TP_TELEMETRY_PEAK_DL_MIN=7000 \
TP_TELEMETRY_CRASH_FRAME=129 \
python3 tools/validate_telemetry.py "$TMP_DIR/milestones.log" "$TMP_DIR/milestones_logic.log" \
    > "$TMP_DIR/telemetry-validation.txt" 2>&1 || true
TELEMETRY_EXIT=$?
if [ "$TELEMETRY_EXIT" -ne 0 ]; then
    echo "  ⚠️  Telemetry validation reported issues"
    tail -20 "$TMP_DIR/telemetry-validation.txt"
    PASS=0
else
    echo "  ✅ Telemetry validation passed"
fi
echo ""

# --- Step 8 (optional): GDB crash report ---
if [ "$CRASH_REPORT" -eq 1 ]; then
    if ! command -v gdb &>/dev/null; then
        echo "━━━ Bonus: GDB crash report (SKIPPED — gdb not found) ━━━"
        echo "  Install: sudo apt-get install gdb"
        echo ""
    else
        CRASH_REPORT_FRAMES="${FRAMES}"
        CRASH_UNIQUE_ARG=""
        [ "$CRASH_UNIQUE" -eq 1 ] && CRASH_UNIQUE_ARG="--unique"
        echo "━━━ Bonus: GDB crash report ($CRASH_REPORT_FRAMES frames) ━━━"
        echo "  Running binary under GDB to collect all crash stack traces..."
        echo "  (This may take a few minutes)"
        echo ""
        CRASH_OUT="$TMP_DIR/crash-report.txt"
        if tools/crash-report.sh \
            --binary build/tp-pc \
            --frames "$CRASH_REPORT_FRAMES" \
            ${CRASH_UNIQUE_ARG} \
            --out "$CRASH_OUT"; then
            echo ""
            echo "  ✅ Crash report saved to: $CRASH_OUT"
            # Extract crash summary line for the final report
            CRASH_SUMMARY=$(grep -m1 "^Total crashes:" "$CRASH_OUT" 2>/dev/null || echo "")
            UNIQUE_SUMMARY=$(grep -m1 "^Unique crash sites:" "$CRASH_OUT" 2>/dev/null || echo "")
            echo "  $CRASH_SUMMARY"
            echo "  $UNIQUE_SUMMARY"
            # Show crash site summary table
            echo ""
            echo "  Crash site breakdown:"
            grep -A50 "^Crash site summary" "$CRASH_OUT" 2>/dev/null | head -20 | sed 's/^/    /'
        else
            echo "  ⚠️  Crash report failed"
            PASS=0
        fi
        echo ""
    fi
fi

# --- Summary ---
echo "╔══════════════════════════════════════════════════════════════╗"
if [ "$PASS" -eq 1 ]; then
    echo "║  ✅ ALL CHECKS PASSED                                      ║"
else
    echo "║  ❌ SOME CHECKS FAILED — see details above                 ║"
fi
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║  Output files in $TMP_DIR:"
echo "║    milestones_logic.log    — Phase 1 (Noop) output"
echo "║    milestones_render.log   — Phase 2 (softpipe) output"
echo "║    milestones.log          — merged test output"
echo "║    milestone-summary.json  — parsed milestones"
echo "║    regression-report.json  — regression check"
echo "║    verify-report.json      — rendering verification"
echo "║    telemetry-validation.txt — telemetry checks"
echo "║    verify/                 — captured frame BMPs"
if [ "$CRASH_REPORT" -eq 1 ]; then
echo "║    crash-report.txt        — GDB crash stack traces"
fi
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

if [ "$PASS" -eq 1 ]; then
    exit 0
else
    exit 1
fi
