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
#   tools/crash-report.sh --out /tmp/report.txt # save to file
#   tools/crash-report.sh --binary build-port/tp-pc   # override binary path
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
# Each crash gets a `bt 20` stack trace.  The script deduplicates by
# crash-site function name so the same crash appearing on every retry frame
# doesn't drown the output.
#
# INTERPRETING THE OUTPUT
# ────────────────────────
# Look for the FIRST new crash site — that is usually the root cause.
# Crashes in DynamicModuleControl::do_link (address 0x1000000) are expected;
# they are GCN overlay-link calls that the PAL crash handler suppresses.
# Crashes in daVrbox_Draw / daVrbox2_Draw are the J3DModel→J3DModelData
# cast bugs (fixed in this session).
# Crashes in J3DMatPacket::draw (phase=60) are the next target.
#
# QUICK RECIPE FOR FUTURE SESSIONS
# ──────────────────────────────────
#   # First build
#   cmake -B build-port -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC
#   ninja -C build-port tp-pc
#
#   # Run crash report (captures ALL crashes in one shot)
#   tools/crash-report.sh --frames 400
#
#   # Focus on a specific crash by ignoring leading crashes
#   tools/crash-report.sh --frames 400 --skip-crash-count 5

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${PROJECT_ROOT}/build-port/tp-pc"
FRAMES=400
UNIQUE_ONLY=0
OUT_FILE=""
SKIP_COUNT=0

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --frames)        FRAMES="$2"; shift 2 ;;
        --unique)        UNIQUE_ONLY=1; shift ;;
        --out)           OUT_FILE="$2"; shift 2 ;;
        --binary)        BINARY="$2"; shift 2 ;;
        --skip-crash-count) SKIP_COUNT="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,50p' "$0" | grep '^#' | sed 's/^# //' | sed 's/^#//'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if ! command -v gdb &>/dev/null; then
    echo "ERROR: gdb not found.  Install with: sudo apt-get install gdb" >&2
    exit 1
fi

if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: Binary not found: $BINARY" >&2
    echo "Build with: cmake -B build-port -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTP_VERSION=PC && ninja -C build-port tp-pc" >&2
    exit 1
fi

# Build GDB command file: stop on every SIGSEGV, continue, print bt
GDB_CMD_FILE="$(mktemp /tmp/tp-crash-gdb.XXXXXX)"
trap 'rm -f "$GDB_CMD_FILE"' EXIT

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
echo ""

# Run GDB, capture output
GDB_OUT="$(
    TP_HEADLESS=1 TP_TEST_FRAMES="$FRAMES" \
        gdb -batch -x "$GDB_CMD_FILE" "$BINARY" 2>/dev/null
)"

# Parse: extract crash blocks (from "received signal SIGSEGV" to the bt output)
python3 - <<'PYEOF' "$GDB_OUT" "$UNIQUE_ONLY" "$OUT_FILE"
import sys, re, collections

gdb_out   = sys.argv[1]
unique    = sys.argv[2] == "1"
out_file  = sys.argv[3]

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
        # (i.e. after the bt, there will be a blank line or another event)
        if line.strip() == "" and len(current) > 3:
            crash_blocks.append(current)
            current = []
            in_crash = False
if current and in_crash:
    crash_blocks.append(current)

# Deduplicate by first real function frame
seen_sites = set()
unique_crashes = []
for block in crash_blocks:
    # Find the first non-?? frame
    site = None
    for l in block:
        m = re.match(r'^#\d+\s+.*? in (\S+) \(', l)
        if m and m.group(1) != "??":
            site = m.group(1)
            break
    if site is None:
        site = block[0] if block else "unknown"
    if unique and site in seen_sites:
        continue
    seen_sites.add(site)
    unique_crashes.append((site, block))

# Count occurrences
site_counts = collections.Counter()
for block in crash_blocks:
    for l in block:
        m = re.match(r'^#\d+\s+.*? in (\S+) \(', l)
        if m and m.group(1) != "??":
            site_counts[m.group(1)] += 1
            break

output_lines = []
output_lines.append(f"Total crashes: {len(crash_blocks)}")
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
