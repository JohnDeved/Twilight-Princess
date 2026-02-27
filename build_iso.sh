#!/usr/bin/env bash
#
# build_iso.sh - Build a playable GameCube ISO/RVZ from modified Twilight Princess source
#
# Usage:
#   ./build_iso.sh [--version GZ2E01|GZ2P01] [--no-build] [--setup]
#
# Ninja integration:
#   ninja iso   # builds framework + RELs as needed, then packages ISO
#   ninja rvz   # converts TheLegendOfZeldaTwilightPrincess_<VERSION>.iso -> .rvz
#
# Prerequisites (one-time setup):
#   1. Place disc image (.rvz/.iso) in orig/<VERSION>/
#   2. Run: python configure.py --no-check [--version <VERSION>]
#   3. Run: ninja  (initial build to verify setup)
#   4. Run: pip install pyisotools
#   5. Run this script's --setup mode first:
#      ./build_iso.sh --version GZ2E01 --setup
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DTK="${SCRIPT_DIR}/build/tools/dtk"
NODTOOL="${SCRIPT_DIR}/build/tools/nodtool"
RARC_PACK="${SCRIPT_DIR}/tools/rarc_pack.py"

# Defaults
VERSION="GZ2E01"
DO_BUILD=true
DO_SETUP=false

copy_if_different() {
    local src="$1"
    local dst="$2"
    if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
        return 1
    fi
    cp "$src" "$dst"
    return 0
}

ensure_tool() {
    local tool_name="$1"
    local tool_path="$2"
    local tool_tag="$3"

    if [ -f "$tool_path" ]; then
        return
    fi

    mkdir -p "$(dirname "$tool_path")"
    echo "Downloading ${tool_name}..."
    python3 "${SCRIPT_DIR}/tools/download_tool.py" "$tool_name" "$tool_path" --tag "$tool_tag"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            VERSION="$2"
            shift 2
            ;;
        --no-build)
            DO_BUILD=false
            shift
            ;;
        --setup)
            DO_SETUP=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--version GZ2E01|GZ2P01] [--no-build] [--setup]"
            echo ""
            echo "Options:"
            echo "  --version VERSION   Game version (default: GZ2E01)"
            echo "  --no-build          Skip ninja build (use existing build output)"
            echo "  --setup             One-time setup: extract disc image for ISO rebuilding"
            echo "  -h, --help          Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

ORIG_DIR="${SCRIPT_DIR}/orig/${VERSION}"
BUILD_DIR="${SCRIPT_DIR}/build/${VERSION}"
DISC_ROOT="${ORIG_DIR}/pyiso_root/root"
RELS_EXTRACTED="${ORIG_DIR}/RELS_extracted"
OUTPUT_ISO="${SCRIPT_DIR}/build/${VERSION}/TheLegendOfZeldaTwilightPrincess_${VERSION}.iso"

echo "=== Twilight Princess ISO Builder ==="
echo "Version:    ${VERSION}"
echo "Build dir:  ${BUILD_DIR}"
echo "Disc root:  ${DISC_ROOT}"
echo "Output ISO: ${OUTPUT_ISO}"
echo ""

# ─── Setup mode ───────────────────────────────────────────────────────────────
if [ "$DO_SETUP" = true ]; then
    echo "=== One-time setup ==="
    ensure_tool "dtk" "${DTK}" "v1.8.0"

    # Find disc image
    DISC_IMAGE=""
    for ext in iso gcm; do
        for f in "${ORIG_DIR}"/*.${ext}; do
            if [ -f "$f" ]; then
                DISC_IMAGE="$f"
                break 2
            fi
        done
    done

    # Try .rvz if no .iso found
    if [ -z "$DISC_IMAGE" ]; then
        for f in "${ORIG_DIR}"/*.rvz; do
            if [ -f "$f" ]; then
                echo "Found RVZ: $f"
                echo "Converting to ISO..."
                ensure_tool "nodtool" "${NODTOOL}" "v2.0.0-alpha.4"
                ISO_PATH="${ORIG_DIR}/game.iso"
                "${NODTOOL}" convert "$f" "$ISO_PATH"
                DISC_IMAGE="$ISO_PATH"
                break
            fi
        done
    fi

    if [ -z "$DISC_IMAGE" ]; then
        echo "ERROR: No disc image found in ${ORIG_DIR}/"
        echo "Place a .rvz, .iso, or .gcm file there first."
        exit 1
    fi

    echo "Disc image: ${DISC_IMAGE}"

    # Extract with pyisotools (creates the config needed for rebuild)
    if [ ! -d "${DISC_ROOT}" ]; then
        echo "Extracting disc with pyisotools..."
        python3 -m pyisotools "$DISC_IMAGE" E --dest "${ORIG_DIR}/pyiso_root"
        echo "Extraction complete."
    else
        echo "Disc already extracted at ${DISC_ROOT}"
    fi

    # Extract RELS.arc contents directly via VFS (no full disc extract needed)
    if [ ! -d "${RELS_EXTRACTED}" ]; then
        echo "Extracting RELS.arc contents..."
        mkdir -p "${RELS_EXTRACTED}"
        "${DTK}" vfs cp "${DISC_IMAGE}:/files/RELS.arc:" "${RELS_EXTRACTED}/"
        echo "RELS.arc extraction complete."
    else
        echo "RELS.arc already extracted at ${RELS_EXTRACTED}"
    fi

    echo ""
    echo "Setup complete! You can now:"
    echo "  1. Run: python configure.py --no-check --version ${VERSION}"
    echo "  2. Make code changes in src/"
    echo "  3. Run: ./build_iso.sh --version ${VERSION}"
    exit 0
fi

# ─── Verify setup ─────────────────────────────────────────────────────────────
ensure_tool "dtk" "${DTK}" "v1.8.0"

if [ ! -d "${DISC_ROOT}" ]; then
    echo "ERROR: Disc root not found at ${DISC_ROOT}"
    echo "Run setup first: ./build_iso.sh --version ${VERSION} --setup"
    exit 1
fi

if [ ! -d "${RELS_EXTRACTED}" ]; then
    echo "ERROR: RELS extracted dir not found at ${RELS_EXTRACTED}"
    echo "Run setup first: ./build_iso.sh --version ${VERSION} --setup"
    exit 1
fi

# ─── Step 1: Build with ninja ────────────────────────────────────────────────
if [ "$DO_BUILD" = true ]; then
    echo "=== Step 1: Building with ninja ==="
    ninja
    echo ""
fi

# Verify build outputs exist
if [ ! -f "${BUILD_DIR}/framework.dol" ]; then
    echo "ERROR: ${BUILD_DIR}/framework.dol not found. Run ninja first."
    exit 1
fi

# ─── Step 2: Copy rebuilt DOL ────────────────────────────────────────────────
echo "=== Step 2: Replacing main.dol ==="
if copy_if_different "${BUILD_DIR}/framework.dol" "${DISC_ROOT}/sys/main.dol"; then
    echo "  Updated sys/main.dol"
else
    echo "  main.dol unchanged"
fi

# ─── Step 3: Copy standalone REL files ───────────────────────────────────────
echo "=== Step 3: Replacing standalone REL files ==="
REL_DEST="${DISC_ROOT}/files/rel/Final/Release"
STANDALONE_COUNT=0
STANDALONE_UPDATED=0

# Find all built REL files and check if they belong to standalone (not in RELS.arc)
for rel_dir in "${BUILD_DIR}"/*/; do
    rel_name="$(basename "$rel_dir")"
    rel_file="${rel_dir}${rel_name}.rel"

    if [ ! -f "$rel_file" ]; then
        continue
    fi

    # Check if this REL is standalone (exists in files/rel/Final/Release/)
    if [ -f "${REL_DEST}/${rel_name}.rel" ]; then
        if copy_if_different "$rel_file" "${REL_DEST}/${rel_name}.rel"; then
            STANDALONE_UPDATED=$((STANDALONE_UPDATED + 1))
        fi
        STANDALONE_COUNT=$((STANDALONE_COUNT + 1))
    fi
done
echo "  Checked ${STANDALONE_COUNT} standalone REL files (${STANDALONE_UPDATED} updated)"

# ─── Step 4: Replace RELS.arc REL files and repack ──────────────────────────
echo "=== Step 4: Rebuilding RELS.arc ==="

RELS_INPUTS_SHA_FILE="${BUILD_DIR}/.rels_arc_inputs.sha1"
CURRENT_RELS_INPUTS_SHA="$(python3 - "${BUILD_DIR}" "${RELS_EXTRACTED}" <<'PY'
import hashlib
import sys
from pathlib import Path

build_dir = Path(sys.argv[1])
rels_extracted = Path(sys.argv[2])

h = hashlib.sha1()
for subdir in ("amem", "mmem"):
    sub_path = rels_extracted / subdir
    if not sub_path.is_dir():
        continue
    for rel_path in sorted(sub_path.glob("*.rel")):
        rel_name = rel_path.stem
        built_rel = build_dir / rel_name / f"{rel_name}.rel"
        src = built_rel if built_rel.is_file() else rel_path
        h.update(f"{subdir}/{rel_name}.rel\0".encode())
        h.update(src.read_bytes())

print(h.hexdigest())
PY
)"

PREV_RELS_INPUTS_SHA=""
if [ -f "${RELS_INPUTS_SHA_FILE}" ]; then
    PREV_RELS_INPUTS_SHA="$(<"${RELS_INPUTS_SHA_FILE}")"
fi

if [ "${CURRENT_RELS_INPUTS_SHA}" = "${PREV_RELS_INPUTS_SHA}" ] && [ -f "${DISC_ROOT}/files/RELS.arc" ]; then
    echo "  RELS.arc inputs unchanged, skipping repack"
else
    # Create a working copy of the extracted RELS
    RELS_WORK="${BUILD_DIR}/RELS_work"
    rm -rf "${RELS_WORK}"
    mkdir -p "${RELS_WORK}"
    cp -R "${RELS_EXTRACTED}/." "${RELS_WORK}"

    # Replace REL files in the working copy
    ARCHIVE_COUNT=0
    for subdir in amem mmem; do
        if [ ! -d "${RELS_WORK}/${subdir}" ]; then
            continue
        fi
        for rel_file in "${RELS_WORK}/${subdir}"/*.rel; do
            rel_name="$(basename "$rel_file" .rel)"
            built_rel="${BUILD_DIR}/${rel_name}/${rel_name}.rel"
            if [ -f "$built_rel" ]; then
                cp "$built_rel" "$rel_file"
                ARCHIVE_COUNT=$((ARCHIVE_COUNT + 1))
            fi
        done
    done
    echo "  Replaced ${ARCHIVE_COUNT} REL files in RELS working copy"

    # Repack RELS.arc
    echo "  Packing RELS.arc..."
    python3 "${RARC_PACK}" "${RELS_WORK}" "${DISC_ROOT}/files/RELS.arc"
    printf '%s\n' "${CURRENT_RELS_INPUTS_SHA}" > "${RELS_INPUTS_SHA_FILE}"
fi

# ─── Step 5: Build ISO ──────────────────────────────────────────────────────
echo "=== Step 5: Building ISO ==="
ISO_INPUTS_SHA_FILE="${BUILD_DIR}/.iso_inputs.sha1"
CURRENT_ISO_INPUTS_SHA="$(python3 - "${BUILD_DIR}" "${REL_DEST}" "${DISC_ROOT}/sys/main.dol" "${DISC_ROOT}/files/RELS.arc" <<'PY'
import hashlib
import sys
from pathlib import Path

build_dir = Path(sys.argv[1])
rel_dest = Path(sys.argv[2])
main_dol = Path(sys.argv[3])
rels_arc = Path(sys.argv[4])

h = hashlib.sha1()
h.update(main_dol.read_bytes())
h.update(rels_arc.read_bytes())

for rel_dir in sorted([p for p in build_dir.iterdir() if p.is_dir()]):
    rel_name = rel_dir.name
    built_rel = rel_dir / f"{rel_name}.rel"
    disc_rel = rel_dest / f"{rel_name}.rel"
    if built_rel.is_file() and disc_rel.is_file():
        h.update(f"standalone/{rel_name}.rel\0".encode())
        h.update(disc_rel.read_bytes())

print(h.hexdigest())
PY
)"

PREV_ISO_INPUTS_SHA=""
if [ -f "${ISO_INPUTS_SHA_FILE}" ]; then
    PREV_ISO_INPUTS_SHA="$(<"${ISO_INPUTS_SHA_FILE}")"
fi

if [ "${CURRENT_ISO_INPUTS_SHA}" = "${PREV_ISO_INPUTS_SHA}" ] && [ -f "${OUTPUT_ISO}" ]; then
    echo "  ISO inputs unchanged, skipping rebuild"
    touch "${OUTPUT_ISO}"
else
    # IMPORTANT: --dest must be an absolute path; pyisotools joins it with the
    # source root, so a relative path would embed the ISO inside the source tree.
    mkdir -p "${BUILD_DIR}"
    OUTPUT_ISO_ABS="$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "${OUTPUT_ISO}")"
    python3 -m pyisotools "${ORIG_DIR}/pyiso_root/root" B --dest "${OUTPUT_ISO_ABS}"
    printf '%s\n' "${CURRENT_ISO_INPUTS_SHA}" > "${ISO_INPUTS_SHA_FILE}"
fi
echo ""

# ─── Done ────────────────────────────────────────────────────────────────────
if [ -f "${OUTPUT_ISO}" ]; then
    ISO_SIZE=$(du -h "${OUTPUT_ISO}" | cut -f1)
    echo "=== Build complete! ==="
    echo "  ISO: ${OUTPUT_ISO} (${ISO_SIZE})"
    echo ""
    echo "For RVZ: run ninja rvz"
    echo "To play: Open in Dolphin emulator"
else
    echo "ERROR: ISO build failed - output file not created"
    exit 1
fi
