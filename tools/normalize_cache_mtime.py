#!/usr/bin/env python3

import os
import argparse
from pathlib import Path


ROOTS = [
    Path("build/tools"),
    Path("build/compilers"),
    Path("build/binutils"),
]


def reference_timestamp() -> int:
    # Tool downloads are driven by tools/download_tool.py in generated rules.
    # Ensure cached tool outputs are never older than that input file.
    ref = Path("tools/download_tool.py")
    if ref.is_file():
        return int(ref.stat().st_mtime)
    return int(Path.cwd().stat().st_mtime)


def touch_path(path: Path, ts: int) -> None:
    try:
        os.utime(path, (ts, ts), follow_symlinks=False)
    except FileNotFoundError:
        return
    except OSError:
        return


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", type=str, default="")
    args = parser.parse_args()

    roots = list(ROOTS)
    if args.version:
        roots.extend(
            [
                Path("orig") / args.version / "sys",
                Path("orig") / args.version / "files" / "RELS" / "rels",
            ]
        )

    updated = 0
    ts = reference_timestamp()

    for root in roots:
        if not root.exists():
            continue

        touch_path(root, ts)
        updated += 1

        if root.is_dir():
            for dirpath, dirnames, filenames in os.walk(root):
                current = Path(dirpath)
                touch_path(current, ts)
                updated += 1

                for name in dirnames:
                    touch_path(current / name, ts)
                    updated += 1

                for name in filenames:
                    touch_path(current / name, ts)
                    updated += 1

    print(f"Normalized mtimes for {updated} cache entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
