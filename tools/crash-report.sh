#!/usr/bin/env bash
# crash-report.sh — Run tp-pc under GDB and capture all crash stack traces
#
# This is the primary tool for localizing crashes in the PC port.  It:
#   1. Runs the headless binary under GDB with SIGSEGV not passed to the game
#   2. Continues past every crash automatically, collecting bt for each one
#   3. Writes a deduplicated, human-readable crash report to stdout / file
#
# Usage (from repo root):
#   tools/crash-report.sh                       # default 400 frames
#   tools/crash-report.sh --frames 200          # fewer frames (faster)
#   tools/crash-report.sh --unique              # only first occurrence of each crash site
#   tools/crash-report.sh --filter-known        # hide expected/suppressed crashes
#   tools/crash-report.sh --actionable          # --unique + --filter-known (best for debugging)
#   tools/crash-report.sh --out /tmp/report.txt # save to file
#   tools/crash-report.sh --binary build/tp-pc  # override binary path
#
# The binary must already be built (RelWithDebInfo for line info).
#
# HOW IT WORKS
# ─────────────
# GDB intercepts SIGSEGV before the game's own signal handler runs.  Because
# the PAL crash handler uses sigsetjmp/siglongjmp the game would normally
# recover — but under GDB we want to see ALL crashes, not just unrecovered
# ones.  The script passes SIGSEGV to the GDB handler (stop+print+nopass),
# so it halts at every fault.  A `continue` command resumes after each one,
# so the binary runs to TEST_COMPLETE even with many crashes.
#
# Each crash gets a `bt 25` stack trace.  The script deduplicates by
# crash-site function name so the same crash appearing on every retry frame
# doesn't drown the output.
#
# INTERPRETING THE OUTPUT
# ────────────────────────
# Look for the FIRST new crash site — that is usually the root cause.
# Use --actionable to filter out known-benign crashes and show only unique sites.
#
# Known-benign crash sites (filtered by --filter-known / --actionable):
#   DynamicModuleControl::do_link — expected; GCN overlay-link calls suppressed by PAL
#
# CURRENT CRASH TARGETS (as of 2026-03-04)
# ──────────────────────────────────────────
#   J3DMatPacket::draw (phase=60) on frame 130+ — likely j3dSys stale state
#     after the first successful flush cycle (frames 128-129 produce 7615 draws)
#   dKankyo_*_Packet::draw — cloud/vrkumo/housi kankyo model actors using GCN API
#
# QUICK RECIPE FOR FUTURE SESSIONS
# ──────────────────────────────────
#   # First build
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC
#   ninja -C build tp-pc
#
#   # Run actionable crash report (unique + filtered)
#   tools/crash-report.sh --actionable --frames 400
#
#   # Deep dive: all crashes, more context, save to file
#   tools/crash-report.sh --frames 400 --out /tmp/crash.txt
#
#   # Focus on crashes at specific frame range
#   tools/crash-report.sh --frames 200 --skip-crash-count 3 --unique

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY=""
FRAMES=400
UNIQUE_ONLY=0
FILTER_KNOWN=0
OUT_FILE=""
SKIP_COUNT=0

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --frames)        FRAMES="$2"; shift 2 ;;
        --unique)        UNIQUE_ONLY=1; shift ;;
        --filter-known)  FILTER_KNOWN=1; shift ;;
        --actionable)    UNIQUE_ONLY=1; FILTER_KNOWN=1; shift ;;
        --out)           OUT_FILE="$2"; shift 2 ;;
        --binary)        BINARY="$2"; shift 2 ;;
        --skip-crash-count) SKIP_COUNT="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,65p' "$0" | grep '^#' | sed 's/^# //' | sed 's/^#//'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# Auto-discover binary: prefer build/tp-pc, then build-port/tp-pc
if [[ -z "$BINARY" ]]; then
    if [[ -f "${PROJECT_ROOT}/build/tp-pc" ]]; then
        BINARY="${PROJECT_ROOT}/build/tp-pc"
    elif [[ -f "${PROJECT_ROOT}/build-port/tp-pc" ]]; then
        BINARY="${PROJECT_ROOT}/build-port/tp-pc"
    fi
fi

if ! command -v gdb &>/dev/null; then
    echo "ERROR: gdb not found.  Install with: sudo apt-get install gdb" >&2
    exit 1
fi

if [[ -z "$BINARY" || ! -f "$BINARY" ]]; then
    echo "ERROR: Binary not found: ${BINARY:-<none>}" >&2
    echo "Build with: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC && ninja -C build tp-pc" >&2
    exit 1
fi

# Build GDB command file: stop on every SIGSEGV, continue, print bt
GDB_CMD_FILE="$(mktemp /tmp/tp-crash-gdb.XXXXXX)"
GDB_OUT_FILE="$(mktemp /tmp/tp-crash-gdb-out.XXXXXX)"
trap 'rm -f "$GDB_CMD_FILE" "$GDB_OUT_FILE"' EXIT

{
    echo "set pagination off"
    echo "set print thread-events off"
    echo "handle SIGSEGV stop print nopass"
    # Skip leading crashes if requested (e.g. to reach the Nth crash type)
    if [[ "$SKIP_COUNT" -gt 0 ]]; then
        echo "ignore 1 ${SKIP_COUNT}"
    fi
    echo "run"
    # Issue up to 200 continue+bt pairs — enough for 400 frames with many crashes
    for i in $(seq 1 200); do
        echo "continue"
        echo "bt 25"
    done
} > "$GDB_CMD_FILE"

echo "━━━ PC Port crash report ━━━"
echo "Binary : $BINARY"
echo "Frames : $FRAMES"
if [ "$UNIQUE_ONLY" -eq 1 ] && [ "$FILTER_KNOWN" -eq 1 ]; then
    echo "Mode   : actionable (unique + known-benign filtered)"
elif [ "$UNIQUE_ONLY" -eq 1 ]; then
    echo "Mode   : unique crash sites only"
elif [ "$FILTER_KNOWN" -eq 1 ]; then
    echo "Mode   : all crashes (known-benign filtered)"
fi
echo ""

# Run GDB, capture output to a temp file (avoids "Argument list too long")
TP_HEADLESS=1 TP_TEST_FRAMES="$FRAMES" \
    gdb -batch -x "$GDB_CMD_FILE" "$BINARY" > "$GDB_OUT_FILE" 2>/dev/null || true

# Parse: extract crash blocks (from "received signal SIGSEGV" to the bt output)
python3 - "$GDB_OUT_FILE" "$UNIQUE_ONLY" "$OUT_FILE" "$FILTER_KNOWN" <<'PYEOF'
import sys, re, collections

with open(sys.argv[1]) as f:
    gdb_out = f.read()
unique       = sys.argv[2] == "1"
out_file     = sys.argv[3]
filter_known = sys.argv[4] == "1" if len(sys.argv) > 4 else False

# Known-benign crash sites (GCN overlay linker calls suppressed by PAL crash handler)
KNOWN_BENIGN_PATTERNS = [
    r"DynamicModuleControl::do_link",
    r"do_link\b",
]

def get_crash_site(block):
    """Return the first non-?? function name in a crash block."""
    for l in block:
        m = re.match(r'^#\d+\s+.*? in (\S+) \(', l)
        if m and m.group(1) not in ("??", ""):
            return m.group(1)
    return None

def is_known_benign(site):
    if site is None:
        return False
    for pat in KNOWN_BENIGN_PATTERNS:
        if re.search(pat, site):
            return True
    return False

lines = gdb_out.splitlines()

# Split into crash blocks
crash_blocks = []
current = []
in_crash = False
for line in lines:
    if "received signal SIGSEGV" in line:
        if current and in_crash:
            crash_blocks.append(current)
        current = [line]
        in_crash = True
    elif in_crash:
        current.append(line)
        # Stop collecting bt after we see a line that is NOT a frame line
        if line.strip() == "" and len(current) > 3:
            crash_blocks.append(current)
            current = []
            in_crash = False
if current and in_crash:
    crash_blocks.append(current)

# Filter known-benign crashes if requested
if filter_known:
    actionable_blocks = [b for b in crash_blocks if not is_known_benign(get_crash_site(b))]
    filtered_count = len(crash_blocks) - len(actionable_blocks)
    crash_blocks_display = actionable_blocks
else:
    filtered_count = 0
    crash_blocks_display = crash_blocks

# Deduplicate by first real function frame
seen_sites = set()
unique_crashes = []
for block in crash_blocks_display:
    site = get_crash_site(block) or (block[0] if block else "unknown")
    if unique and site in seen_sites:
        continue
    seen_sites.add(site)
    unique_crashes.append((site, block))

# Count occurrences (over the display set)
site_counts = collections.Counter()
for block in crash_blocks_display:
    site = get_crash_site(block)
    if site:
        site_counts[site] += 1

output_lines = []
output_lines.append(f"Total crashes: {len(crash_blocks)}")
if filter_known and filtered_count:
    output_lines.append(f"  ({filtered_count} known-benign filtered)")
output_lines.append(f"Actionable crashes: {len(crash_blocks_display)}")
output_lines.append(f"Unique crash sites: {len(seen_sites)}")
output_lines.append("")

# Summary table
output_lines.append("Crash site summary (by frequency):")
for site, count in site_counts.most_common():
    output_lines.append(f"  {count:4d}x  {site}")
output_lines.append("")

output_lines.append("=" * 70)
output_lines.append("CRASH DETAILS" + (" (first occurrence only)" if unique else ""))
output_lines.append("=" * 70)
for idx, (site, block) in enumerate(unique_crashes, 1):
    output_lines.append(f"\n--- Crash #{idx}: {site} ---")
    output_lines.extend(block)

result = "\n".join(output_lines)
print(result)
if out_file:
    with open(out_file, "w") as f:
        f.write(result + "\n")
    print(f"\nReport saved to: {out_file}", file=sys.stderr)
PYEOF
