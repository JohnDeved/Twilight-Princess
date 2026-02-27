#!/usr/bin/env python3

import os
import subprocess
import sys
from pathlib import Path


def run_git(*args: str) -> str:
    proc = subprocess.run(
        ["git", *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return proc.stdout


def tracked_files() -> set[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout
    return {p.decode("utf-8", errors="surrogateescape") for p in out.split(b"\0") if p}


def main() -> int:
    if not (Path.cwd() / ".git").exists():
        print("restore_mtime.py must run in a git repository", file=sys.stderr)
        return 1

    files = tracked_files()
    if not files:
        print("No tracked files found")
        return 0

    # Walk history oldest -> newest so each file ends up with its latest commit time.
    log = subprocess.run(
        [
            "git",
            "log",
            "--reverse",
            "--name-only",
            "--pretty=format:%ct",
            "--no-renames",
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout.splitlines()

    current_ts: int | None = None
    mtimes: dict[str, int] = {}
    for line in log:
        if not line:
            continue
        if line.isdigit():
            current_ts = int(line)
            continue
        if current_ts is None:
            continue
        if line in files:
            mtimes[line] = current_ts

    updated = 0
    for rel_path, ts in mtimes.items():
        path = Path(rel_path)
        if path.is_file():
            os.utime(path, (ts, ts))
            updated += 1

    print(f"Restored mtimes for {updated} tracked files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
