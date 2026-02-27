#!/usr/bin/env python3
"""
RARC archive packer for GameCube/Wii games.

Packs a directory tree into a RARC (.arc) archive, with Yaz0 compression
of individual files using dtk (decomp-toolkit). Designed for repacking
RELS.arc in Twilight Princess.

Usage:
    python rarc_pack.py <input_dir> <output_arc> [--dtk PATH] [--no-compress]

The input directory becomes the root of the archive. For RELS.arc, the structure
should be:
    input_dir/
        amem/
            d_a_alldie.rel
            ...
        mmem/
            d_a_andsw.rel
            ...

Key format details for RELS.arc:
  - File type flag 0xA500 = Yaz0-compressed, ARAM preload
  - Entries are ordered: real entries first, then "." and ".." at the end
  - File IDs must equal their node index (synced IDs)
  - mram_size = 0, aram_size = data_len (all data goes to ARAM)
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Optional
from pathlib import Path


def rarc_hash(name: str) -> int:
    """Compute the RARC name hash."""
    h = 0
    for c in name:
        h = (h * 3 + ord(c)) & 0xFFFF
    return h


def dir_identifier(name: str) -> bytes:
    """Generate the 4-byte directory type identifier from a name."""
    ident = name.upper()[:4]
    ident = ident.ljust(4, " ")
    return ident.encode("ascii")


def align(offset: int, alignment: int) -> int:
    """Align offset up to the given alignment."""
    return (offset + alignment - 1) & ~(alignment - 1)


def yaz0_compress_dtk(dtk_path: str, file_path: Path, output_path: Path) -> bytes:
    """Compress a file using dtk yaz0 compress and return the compressed bytes."""
    result = subprocess.run(
        [dtk_path, "yaz0", "compress", str(file_path), "-o", str(output_path)],
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"dtk yaz0 compress failed for {file_path}: {result.stderr.decode()}"
        )
    return output_path.read_bytes()


def pack_rarc(
    input_dir: Path,
    output_path: Path,
    compress: bool = True,
    dtk_path: Optional[str] = None,
) -> None:
    """Pack a directory into a RARC archive."""
    input_dir = input_dir.resolve()

    # Derive root name from output archive filename (e.g., "RELS.arc" -> "rels")
    root_name = output_path.stem.lower()

    # Find dtk binary
    if dtk_path is None:
        # Look relative to this script's location (tools/ -> build/tools/dtk)
        script_dir = Path(__file__).resolve().parent
        dtk_path = str(script_dir.parent / "build" / "tools" / "dtk")
    if compress and not Path(dtk_path).exists():
        print(f"Warning: dtk not found at {dtk_path}, falling back to no compression")
        compress = False

    # Collect directory structure
    directories = []  # [(name, parent_idx, [(type, name, data_or_dir_idx)])]

    def scan_dir(dir_path: Path, parent_idx: int) -> int:
        dir_idx = len(directories)
        dir_name = dir_path.name if dir_idx > 0 else root_name
        entries = []
        directories.append((dir_name, parent_idx, entries))

        # Sort treating '_' as greater than all letters (matches original RARC ordering).
        # In ASCII '_' = 0x5F which is before lowercase letters, but the original
        # archive sorts as if '_' comes after 'z', so e.g. "econt" < "e_dn".
        def rarc_sort_key(p: Path) -> str:
            return p.name.replace("_", "{")  # '{' = 0x7B, just after 'z' = 0x7A

        subdirs = sorted(
            [p for p in dir_path.iterdir() if p.is_dir()], key=rarc_sort_key
        )
        files = sorted(
            [p for p in dir_path.iterdir() if p.is_file()], key=rarc_sort_key
        )

        # Subdirs first, then files (matches original RELS.arc ordering)
        for subdir in subdirs:
            child_idx = scan_dir(subdir, dir_idx)
            entries.append(("dir", subdir.name, child_idx))

        for f in files:
            entries.append(("file", f.name, f))  # Store Path, read later

        return dir_idx

    scan_dir(input_dir, -1)

    # Build string table
    string_table = bytearray()
    string_offsets = {}

    def add_string(s: str) -> int:
        if s in string_offsets:
            return string_offsets[s]
        offset = len(string_table)
        string_offsets[s] = offset
        string_table.extend(s.encode("ascii") + b"\x00")
        return offset

    # Add "." and ".." first (they appear early in the original string table)
    add_string(".")
    add_string("..")

    # Pre-add all directory and file names
    for dir_name, _, entries in directories:
        add_string(dir_name)
        for entry_type, entry_name, _ in entries:
            add_string(entry_name)

    # Count nodes per directory (entries + "." + "..")
    node_counts = []
    total_nodes = 0
    for _, _, entries in directories:
        count = len(entries) + 2  # +2 for "." and ".."
        node_counts.append(count)
        total_nodes += count

    # Assign first_index for each directory
    first_indices = []
    idx = 0
    for count in node_counts:
        first_indices.append(idx)
        idx += count

    # Build file data section with 32-byte alignment
    # Use a temp directory for dtk yaz0 compression
    file_data = bytearray()
    file_data_offsets = {}
    file_data_sizes = {}

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)

        for dir_idx, (dir_name, parent_idx, entries) in enumerate(directories):
            for entry_idx, (entry_type, entry_name, entry_data) in enumerate(entries):
                if entry_type != "file":
                    continue

                file_path = entry_data  # Path object
                raw_data = file_path.read_bytes()

                # Align to 32 bytes
                padding = align(len(file_data), 32) - len(file_data)
                file_data.extend(b"\x00" * padding)

                if compress:
                    # Use dtk for fast native Yaz0 compression
                    tmp_out = tmpdir_path / f"{entry_name}.yaz0"
                    compressed = yaz0_compress_dtk(dtk_path, file_path, tmp_out)
                    file_data_offsets[(dir_idx, entry_idx)] = len(file_data)
                    file_data_sizes[(dir_idx, entry_idx)] = len(compressed)
                    file_data.extend(compressed)
                else:
                    file_data_offsets[(dir_idx, entry_idx)] = len(file_data)
                    file_data_sizes[(dir_idx, entry_idx)] = len(raw_data)
                    file_data.extend(raw_data)

    # Match original RELS.arc behavior: pad file data section to 32-byte boundary.
    # Without this tail padding, data_size/total_size become smaller than the
    # original archive (e.g. by 19 bytes for GZ2E01), which can break runtime
    # loading expectations.
    file_data.extend(b"\x00" * (align(len(file_data), 32) - len(file_data)))

    # Build directory table
    dir_table = bytearray()
    for dir_idx, (dir_name, parent_idx, entries) in enumerate(directories):
        ident = b"ROOT" if dir_idx == 0 else dir_identifier(dir_name)
        name_h = rarc_hash(dir_name)
        count = node_counts[dir_idx]
        first = first_indices[dir_idx]
        dir_table.extend(
            struct.pack(
                ">4sIHHI", ident, string_offsets[dir_name], name_h, count, first
            )
        )

    # Build node table
    # IMPORTANT: Real entries (subdirs/files) come FIRST,
    # then "." and ".." entries come LAST in each directory.
    # File IDs must equal their node index for the game's REL loader.
    node_table = bytearray()
    file_id = 0
    node_id = 0

    for dir_idx, (dir_name, parent_idx, entries) in enumerate(directories):
        # Real entries first (subdirs and files)
        for entry_idx, (entry_type, entry_name, entry_data) in enumerate(entries):
            entry_hash = rarc_hash(entry_name)
            entry_name_off = string_offsets[entry_name]

            if entry_type == "dir":
                child_dir_idx = entry_data
                node_table.extend(
                    struct.pack(
                        ">HHHHI I I",
                        0xFFFF,
                        entry_hash,
                        0x0200,
                        entry_name_off,
                        child_dir_idx,
                        0x10,
                        0x00000000,
                    )
                )
            else:
                # File entry
                # Type 0xA500 = Yaz0 compressed + ARAM preload (required for RELS.arc)
                # Type 0x1100 = uncompressed + MRAM
                file_type = 0xA500 if compress else 0x1100
                data_off = file_data_offsets[(dir_idx, entry_idx)]
                data_len = file_data_sizes[(dir_idx, entry_idx)]
                node_table.extend(
                    struct.pack(
                        ">HHHHI I I",
                        node_id,
                        entry_hash,
                        file_type,
                        entry_name_off,
                        data_off,
                        data_len,
                        0x00000000,
                    )
                )
                file_id += 1
            node_id += 1

        # "." entry - self reference (AFTER real entries)
        dot_hash = rarc_hash(".")
        dot_name_off = string_offsets["."]
        node_table.extend(
            struct.pack(
                ">HHHHI I I",
                0xFFFF,
                dot_hash,
                0x0200,
                dot_name_off,
                dir_idx,
                0x10,
                0x00000000,
            )
        )
        node_id += 1

        # ".." entry - parent reference (AFTER real entries)
        dotdot_hash = rarc_hash("..")
        dotdot_name_off = string_offsets[".."]
        parent_ref = 0xFFFFFFFF if parent_idx < 0 else parent_idx
        node_table.extend(
            struct.pack(
                ">HHHHI I I",
                0xFFFF,
                dotdot_hash,
                0x0200,
                dotdot_name_off,
                parent_ref,
                0x10,
                0x00000000,
            )
        )
        node_id += 1

    # Pad string table to 32-byte alignment
    str_table_padded = string_table + b"\x00" * (
        align(len(string_table), 32) - len(string_table)
    )

    # Pad file data to 32-byte alignment.
    # RELS.arc in TP expects data_size/total_size to include this trailing padding.
    file_data.extend(b"\x00" * (align(len(file_data), 32) - len(file_data)))

    # Calculate offsets for the info block
    info_block_size = 0x20
    dir_table_offset = info_block_size
    dir_table_size = len(dir_table)

    node_table_offset = align(dir_table_offset + dir_table_size, 32)
    node_table_size = len(node_table)

    str_table_offset = align(node_table_offset + node_table_size, 32)
    str_table_size = len(str_table_padded)

    data_offset = align(str_table_offset + str_table_size, 32)
    data_size = len(file_data)

    # Total file size
    header_len = 0x20
    total_size = header_len + data_offset + data_size

    # Build the final binary
    output = bytearray()

    # RARC Header (0x20 bytes)
    output.extend(
        struct.pack(
            ">4s I I I",
            b"RARC",
            total_size,
            header_len,
            data_offset,  # relative to end of header
        )
    )
    output.extend(
        struct.pack(
            ">I I I I",
            data_size,
            0x00000000,  # mram_size (0 for RELS.arc - all data goes to ARAM)
            data_size,  # aram_size
            0x00000000,  # dvd_size
        )
    )

    # Info Block (0x20 bytes)
    output.extend(
        struct.pack(
            ">I I I I",
            len(directories),
            dir_table_offset,
            total_nodes,
            node_table_offset,
        )
    )
    output.extend(
        struct.pack(
            ">I I H H I",
            len(str_table_padded),
            str_table_offset,
            total_nodes,  # file_count (synced with node_count)
            0x0100,  # sync_flag
            0x00000000,
        )
    )

    # Write directory table (with any needed padding)
    output.extend(b"\x00" * (dir_table_offset - info_block_size))
    output.extend(dir_table)

    # Write node table
    padding = node_table_offset - (dir_table_offset + dir_table_size)
    output.extend(b"\x00" * padding)
    output.extend(node_table)

    # Write string table
    padding = str_table_offset - (node_table_offset + node_table_size)
    output.extend(b"\x00" * padding)
    output.extend(str_table_padded)

    # Write file data
    padding = data_offset - (str_table_offset + len(str_table_padded))
    output.extend(b"\x00" * padding)
    output.extend(file_data)

    # Write output
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(output))
    print(
        f"Packed RARC archive: {output_path} ({len(output)} bytes, {file_id} files, {len(directories)} dirs)"
    )


def main():
    parser = argparse.ArgumentParser(description="Pack a directory into a RARC archive")
    parser.add_argument("input_dir", type=Path, help="Input directory to pack")
    parser.add_argument("output", type=Path, help="Output .arc file")
    parser.add_argument(
        "--no-compress",
        action="store_true",
        help="Do not Yaz0-compress individual files",
    )
    parser.add_argument(
        "--dtk",
        type=str,
        default=None,
        help="Path to dtk binary (default: build/tools/dtk)",
    )
    args = parser.parse_args()

    if not args.input_dir.is_dir():
        print(f"Error: {args.input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    pack_rarc(
        args.input_dir,
        args.output,
        compress=not args.no_compress,
        dtk_path=args.dtk,
    )


if __name__ == "__main__":
    main()
