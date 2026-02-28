#!/usr/bin/env python3
"""
Downloads and extracts Twilight Princess game data for the PC port.

Usage:
    python3 tools/setup_game_data.py [--version GZ2E01] [--data-dir data]

Requires:
    - ROMS_TOKEN environment variable (GitHub PAT with release read access)
    - gh CLI installed
    - nodtool (auto-downloaded if missing)

The script:
    1. Downloads the disc image (.rvz) from the 'roms' GitHub release
    2. Downloads nodtool if not present
    3. Extracts the disc image to a temp directory
    4. Creates/updates the data/ directory with game files
"""

import argparse
import os
import subprocess
import sys
import shutil
from pathlib import Path


def run(cmd, **kwargs):
    """Run a command, printing it first."""
    print(f"  $ {' '.join(cmd)}", flush=True)
    return subprocess.run(cmd, **kwargs)


def check_gh_cli():
    """Check if gh CLI is available."""
    try:
        subprocess.run(["gh", "--version"], capture_output=True, check=True)
        return True
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False


def download_rom(repo, version, out_dir, token):
    """Download the RVZ disc image from the 'roms' release."""
    rvz_path = Path(out_dir) / f"{version}.rvz"
    if rvz_path.exists():
        print(f"  ROM already exists: {rvz_path}")
        return rvz_path

    os.makedirs(out_dir, exist_ok=True)
    env = os.environ.copy()
    env["GH_TOKEN"] = token

    result = run(
        ["gh", "release", "download", "roms",
         "--repo", repo,
         "--pattern", f"{version}.rvz",
         "--dir", str(out_dir)],
        env=env,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  ERROR: Failed to download ROM: {result.stderr.strip()}")
        return None

    if not rvz_path.exists():
        print(f"  ERROR: Download succeeded but {rvz_path} not found")
        return None

    print(f"  Downloaded: {rvz_path} ({rvz_path.stat().st_size / 1024 / 1024:.0f} MB)")
    return rvz_path


def ensure_nodtool(tools_dir):
    """Download nodtool if not present."""
    nodtool = Path(tools_dir) / "nodtool"
    if nodtool.exists():
        return str(nodtool)

    os.makedirs(tools_dir, exist_ok=True)
    script_dir = Path(__file__).parent
    download_tool = script_dir / "download_tool.py"

    if not download_tool.exists():
        print("  ERROR: tools/download_tool.py not found")
        return None

    result = run(
        [sys.executable, str(download_tool), "nodtool", str(nodtool),
         "--tag", "v2.0.0-alpha.4"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  ERROR: Failed to download nodtool: {result.stderr.strip()}")
        return None

    nodtool.chmod(0o755)
    return str(nodtool)


def extract_disc(nodtool, rvz_path, extract_dir):
    """Extract the disc image using nodtool."""
    files_dir = Path(extract_dir) / "files"
    if files_dir.exists() and any(files_dir.iterdir()):
        print(f"  Disc already extracted: {extract_dir}")
        return True

    os.makedirs(extract_dir, exist_ok=True)
    result = run(
        [nodtool, "extract", str(rvz_path), str(extract_dir)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  ERROR: Failed to extract disc: {result.stderr.strip()}")
        return False

    if not files_dir.exists():
        print(f"  ERROR: Extraction succeeded but {files_dir} not found")
        return False

    print(f"  Extracted disc to: {extract_dir}")
    return True


def setup_data_dir(extract_dir, data_dir):
    """Set up the data directory pointing to extracted game files."""
    files_dir = Path(extract_dir) / "files"
    data_path = Path(data_dir)

    # If data_dir is already a valid directory with game files, skip
    if data_path.is_dir() and not data_path.is_symlink():
        test_file = data_path / "res"
        if test_file.exists():
            print(f"  Data directory already set up: {data_dir}")
            return True

    # Remove existing symlink or empty directory
    if data_path.is_symlink():
        data_path.unlink()
    elif data_path.is_dir():
        # Remove non-empty directory that doesn't have the expected structure
        try:
            shutil.rmtree(str(data_path))
            print(f"  Removed stale data directory: {data_dir}")
        except OSError as e:
            print(f"  ERROR: Cannot remove existing data directory {data_dir}: {e}")
            return False

    # Create symlink to extracted files
    try:
        data_path.symlink_to(files_dir.resolve())
        print(f"  Created symlink: {data_dir} -> {files_dir.resolve()}")
        return True
    except OSError:
        # Symlinks might not work (Windows without admin). Copy instead.
        print(f"  Symlink failed, copying files...")
        shutil.copytree(str(files_dir), str(data_path))
        return True


def main():
    parser = argparse.ArgumentParser(description="Set up Twilight Princess game data for PC port")
    parser.add_argument("--version", default="GZ2E01",
                        help="Game version (default: GZ2E01 = US)")
    parser.add_argument("--data-dir", default="data",
                        help="Output data directory (default: data)")
    parser.add_argument("--repo", default=None,
                        help="GitHub repo (default: auto-detect from git remote)")
    parser.add_argument("--rom-dir", default=None,
                        help="Directory for ROM storage (default: orig/<version>)")
    parser.add_argument("--extract-dir", default=None,
                        help="Directory for extraction (default: build/extract/<version>)")
    parser.add_argument("--tools-dir", default="build/tools",
                        help="Directory for tools like nodtool (default: build/tools)")
    args = parser.parse_args()

    # Check for ROMS_TOKEN
    token = os.environ.get("ROMS_TOKEN", "")
    if not token:
        print("ROMS_TOKEN not set — skipping game data setup.")
        print("Set ROMS_TOKEN to a GitHub PAT with release read access to download game assets.")
        return 0

    # Check for gh CLI
    if not check_gh_cli():
        print("ERROR: gh CLI not found. Install it from https://cli.github.com/")
        return 1

    # Auto-detect repo
    repo = args.repo
    if not repo:
        try:
            result = subprocess.run(
                ["git", "remote", "get-url", "origin"],
                capture_output=True, text=True, check=True
            )
            url = result.stdout.strip()
            # Parse owner/repo from various URL formats
            if "github.com" in url:
                parts = url.rstrip("/").rstrip(".git").split("github.com")[-1]
                parts = parts.lstrip("/:").split("/")
                if len(parts) >= 2:
                    repo = f"{parts[0]}/{parts[1]}"
        except (subprocess.CalledProcessError, IndexError):
            pass
        if not repo:
            print("ERROR: Could not detect GitHub repo. Use --repo owner/name")
            return 1

    rom_dir = args.rom_dir or f"orig/{args.version}"
    extract_dir = args.extract_dir or f"build/extract/{args.version}"

    print(f"Setting up game data for {args.version}...")
    print(f"  Repo: {repo}")
    print(f"  ROM dir: {rom_dir}")
    print(f"  Extract dir: {extract_dir}")
    print(f"  Data dir: {args.data_dir}")

    # Step 1: Download ROM
    print("\n[1/4] Downloading disc image...")
    rvz_path = download_rom(repo, args.version, rom_dir, token)
    if not rvz_path:
        return 1

    # Step 2: Get nodtool
    print("\n[2/4] Ensuring nodtool is available...")
    nodtool = ensure_nodtool(args.tools_dir)
    if not nodtool:
        return 1

    # Step 3: Extract disc
    print("\n[3/4] Extracting disc image...")
    if not extract_disc(nodtool, rvz_path, extract_dir):
        return 1

    # Step 4: Set up data directory
    print("\n[4/4] Setting up data directory...")
    if not setup_data_dir(extract_dir, args.data_dir):
        return 1

    # Verify
    data_path = Path(args.data_dir)
    expected = ["res", "Audiores", "RELS.arc", "rel"]
    found = [d for d in expected if (data_path / d).exists()]
    print(f"\nDone! Game data ready ({len(found)}/{len(expected)} expected directories found)")
    if len(found) < len(expected):
        missing = [d for d in expected if d not in found]
        print(f"  Missing: {missing}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
