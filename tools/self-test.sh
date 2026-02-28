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
#
# Exit codes:
#   0 = all checks passed
#   1 = test failure (regression, integrity, or crash)
#   2 = build failure

set -euo pipefail

# --- Defaults ---
SKIP_BUILD=0
FRAMES=2000
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tp-self-test.XXXXXX")"

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
        --skip-build) SKIP_BUILD=1; shift ;;
        --frames)     FRAMES="$2"; shift 2 ;;
        --quick)      SKIP_BUILD=1; FRAMES=100; shift ;;
        --help|-h)
            echo "Usage: tools/self-test.sh [--skip-build] [--frames N] [--quick]"
            echo ""
            echo "  --skip-build  Skip the build step (use existing binary)"
            echo "  --frames N    Number of frames to run (default: 2000)"
            echo "  --quick       Alias for --skip-build --frames 100"
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

timeout 120s build/tp-pc 2>&1 | tee "$TMP_DIR/milestones.log" || true
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
    --output "$TMP_DIR/regression-report.json"; then
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
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

if [ "$PASS" -eq 1 ]; then
    exit 0
else
    exit 1
fi
