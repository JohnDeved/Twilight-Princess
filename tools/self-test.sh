#!/usr/bin/env bash
# self-test.sh — One-command local test pipeline for AI agents
#
# Runs the same checks as CI without needing to push:
#   1. Build (incremental)
#   2. Run headless test
#   3. Parse milestones
#   4. Check milestone regression
#   5. Verify rendering/input/audio
#
# Usage:
#   tools/self-test.sh              # full pipeline (build + test)
#   tools/self-test.sh --skip-build # test only (assumes already built)
#   tools/self-test.sh --frames 300 # fewer frames for quick smoke test
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
FRAMES=2000
CRASH_REPORT=0
CRASH_UNIQUE=0
RUN_PHASES=()   # extra render phases to run via quick-test.sh (e.g. "3 4")
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
        --phase)        RUN_PHASES+=("$2"); shift 2 ;;
        --crash-report) CRASH_REPORT=1; shift ;;
        --unique)       CRASH_UNIQUE=1; shift ;;
        --help|-h)
            echo "Usage: tools/self-test.sh [--skip-build] [--frames N] [--quick]"
            echo "                          [--phase 3] [--phase 4]"
            echo "                          [--crash-report [--unique]]"
            echo ""
            echo "  --skip-build    Skip the build step (use existing binary)"
            echo "  --frames N      Number of frames to run (default: 2000)"
            echo "  --quick         Alias for --skip-build --frames 100"
            echo "  --phase N       Run render phase N (3=3D room, 4=gameplay) via quick-test.sh"
            echo "                  May be repeated: --phase 3 --phase 4"
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
echo "  Frames:     $FRAMES"
echo "  Skip build: $SKIP_BUILD"
echo "  Output dir: $TMP_DIR"
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
    echo "━━━ Step 1/5: Build ━━━"
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
    echo "━━━ Step 1/5: Build (skipped) ━━━"
    if [ ! -f build/tp-pc ]; then
        echo "  ❌ build/tp-pc not found — run without --skip-build first"
        exit 2
    fi
    echo "  ✅ Using existing build/tp-pc"
fi
echo ""

# --- Step 2: Run headless test ---
echo "━━━ Step 2/5: Run headless test ($FRAMES frames) ━━━"
mkdir -p "$TMP_DIR/verify"
export TP_HEADLESS=1
export TP_TEST_FRAMES="$FRAMES"
export TP_VERIFY=1
export TP_VERIFY_DIR="$TMP_DIR/verify"
export TP_VERIFY_CAPTURE_FRAMES="1,10,30,60,120,300,600,1200,1800"

# Start Xvfb for software OpenGL rendering (no GPU needed)
XVFB_PID=""
if ! xdpyinfo -display :99 >/dev/null 2>&1; then
    Xvfb :99 -screen 0 640x480x24 &
    XVFB_PID=$!
    # Wait for Xvfb to be ready
    for i in 1 2 3 4 5 6 7 8 9 10; do
        xdpyinfo -display :99 >/dev/null 2>&1 && break
        sleep 1
    done
    export DISPLAY=:99
else
    export DISPLAY=:99
fi

# Use softpipe (not llvmpipe) to avoid LLVM JIT crashes in CI
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-softpipe}"
export LIBGL_ALWAYS_SOFTWARE=1

timeout 120s build/tp-pc 2>&1 | tee "$TMP_DIR/milestones.log" || true
if [ -n "$XVFB_PID" ]; then
    kill "$XVFB_PID" 2>/dev/null || true
fi
echo "  ✅ Test run completed"
echo ""

# --- Step 3: Parse milestones ---
echo "━━━ Step 3/5: Parse milestones ━━━"
if ! python3 tools/parse_milestones.py "$TMP_DIR/milestones.log" \
    --output "$TMP_DIR/milestone-summary.json" \
    --min-milestone 0; then
    echo "  ⚠️  Milestone parsing reported issues"
    PASS=0
fi
echo ""

# --- Step 4: Check regression ---
echo "━━━ Step 4/5: Check milestone regression ━━━"
if ! python3 tools/check_milestone_regression.py "$TMP_DIR/milestone-summary.json" \
    --baseline milestone-baseline.json \
    --output "$TMP_DIR/regression-report.json" \
    --auto-update; then
    echo "  ⚠️  Regression detected or integrity failure"
    PASS=0
fi
echo ""

# --- Step 5: Verify rendering ---
echo "━━━ Step 5/5: Verify rendering ━━━"
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

# --- Step 6 (optional): Render phase capture (Phase 3 / Phase 4) ---
if [ "${#RUN_PHASES[@]}" -gt 0 ]; then
    for phase in "${RUN_PHASES[@]}"; do
        PHASE_OUT="$TMP_DIR/phase${phase}"
        echo "━━━ Step 6: Render phase $phase capture (via quick-test.sh) ━━━"
        if ! tools/quick-test.sh \
                --phase "$phase" \
                --binary build/tp-pc \
                --output-dir "$PHASE_OUT"; then
            echo "  ⚠️  Phase $phase gate failed"
            PASS=0
        fi
        echo ""
    done
fi

# --- Step 7 (optional): GDB crash report ---
if [ "$CRASH_REPORT" -eq 1 ]; then
    if ! command -v gdb &>/dev/null; then
        echo "━━━ Step 7: GDB crash report (SKIPPED — gdb not found) ━━━"
        echo "  Install: sudo apt-get install gdb"
        echo ""
    else
        CRASH_REPORT_FRAMES="${FRAMES}"
        CRASH_UNIQUE_ARG=""
        [ "$CRASH_UNIQUE" -eq 1 ] && CRASH_UNIQUE_ARG="--unique"
        echo "━━━ Step 7: GDB crash report ($CRASH_REPORT_FRAMES frames) ━━━"
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
echo "║    milestones.log          — raw test output"
echo "║    milestone-summary.json  — parsed milestones"
echo "║    regression-report.json  — regression check"
echo "║    verify-report.json      — rendering verification"
echo "║    verify/                 — captured frame BMPs"
if [ "${#RUN_PHASES[@]}" -gt 0 ]; then
    for phase in "${RUN_PHASES[@]}"; do
echo "║    phase${phase}/               — Phase $phase BMP captures + log"
    done
fi
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
