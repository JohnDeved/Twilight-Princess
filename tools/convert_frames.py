#!/usr/bin/env python3
"""
Convert captured BMP frames to PNG and generate an MP4 video.

This script is used by CI to produce embeddable images for PR comments
and a render progress video showing all captured frames in sequence.

Uses only Python standard library for PNG conversion (no Pillow needed).
Uses ffmpeg for video generation (available on GitHub Actions runners).

Usage:
    python3 tools/convert_frames.py verify_output \\
        --png-dir verify_png \\
        --video verify_output/render_progress.mp4 \\
        --fps 4
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path


def bmp_to_png(bmp_path, png_path):
    """Convert a 24-bit BMP to PNG using only the standard library."""
    with open(bmp_path, "rb") as f:
        header = f.read(54)
        if len(header) < 54 or header[0:2] != b"BM":
            return False

        width = struct.unpack_from("<i", header, 18)[0]
        height_raw = struct.unpack_from("<i", header, 22)[0]
        height = abs(height_raw)
        bottom_up = height_raw > 0
        bpp = struct.unpack_from("<H", header, 28)[0]
        if bpp != 24:
            return False

        row_bytes = width * 3
        row_padding = (4 - (row_bytes % 4)) % 4

        # Read all pixel rows
        rows = []
        for _ in range(height):
            row = f.read(row_bytes)
            if len(row) < row_bytes:
                return False
            f.read(row_padding)
            rows.append(row)

        # BMP is bottom-up by default
        if bottom_up:
            rows.reverse()

    # Build PNG
    def write_png(path, w, h, rows_bgr):
        """Write a minimal PNG file from BGR row data."""

        def chunk(chunk_type, data):
            c = chunk_type + data
            crc = struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
            return struct.pack(">I", len(data)) + c + crc

        # IHDR
        ihdr_data = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
        ihdr = chunk(b"IHDR", ihdr_data)

        # IDAT - convert BGR to RGB and add filter byte
        raw = bytearray()
        for row_bgr in rows_bgr:
            raw.append(0)  # filter: None
            for x in range(w):
                b_val = row_bgr[x * 3]
                g_val = row_bgr[x * 3 + 1]
                r_val = row_bgr[x * 3 + 2]
                raw.append(r_val)
                raw.append(g_val)
                raw.append(b_val)

        compressed = zlib.compress(bytes(raw), 9)
        idat = chunk(b"IDAT", compressed)

        # IEND
        iend = chunk(b"IEND", b"")

        # Write
        with open(path, "wb") as out:
            out.write(b"\x89PNG\r\n\x1a\n")
            out.write(ihdr)
            out.write(idat)
            out.write(iend)

    write_png(png_path, width, height, rows)
    return True


def generate_video(png_dir, output_path, fps=4):
    """Generate an MP4 video from PNG frames using ffmpeg."""
    pngs = sorted(Path(png_dir).glob("frame_*.png"))
    if not pngs:
        print("No PNG frames found for video generation")
        return False

    # Create a temporary file list for ffmpeg
    list_path = os.path.join(png_dir, "frames.txt")
    duration = 1.0 / fps
    with open(list_path, "w") as f:
        for png in pngs:
            f.write(f"file '{png.resolve()}'\n")
            f.write(f"duration {duration}\n")
        # Repeat last frame to avoid ffmpeg duration issue
        f.write(f"file '{pngs[-1].resolve()}'\n")

    try:
        result = subprocess.run(
            [
                "ffmpeg", "-y",
                "-f", "concat",
                "-safe", "0",
                "-i", list_path,
                "-vf", "scale=640:480",
                "-c:v", "libx264",
                "-pix_fmt", "yuv420p",
                "-preset", "fast",
                "-crf", "23",
                output_path,
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode == 0:
            size = os.path.getsize(output_path)
            print(f"Video generated: {output_path} ({size} bytes, {len(pngs)} frames @ {fps}fps)")
            return True
        else:
            print(f"ffmpeg failed: {result.stderr[:500]}")
            return False
    except FileNotFoundError:
        print("ffmpeg not found — skipping video generation")
        return False
    except subprocess.TimeoutExpired:
        print("ffmpeg timed out — skipping video generation")
        return False
    finally:
        if os.path.exists(list_path):
            os.remove(list_path)


def main():
    parser = argparse.ArgumentParser(description="Convert BMP frames to PNG and generate video")
    parser.add_argument("bmp_dir", help="Directory containing frame_*.bmp files")
    parser.add_argument("--png-dir", default="verify_png",
                        help="Output directory for PNG files")
    parser.add_argument("--video", default=None,
                        help="Output path for MP4 video")
    parser.add_argument("--fps", type=int, default=4,
                        help="Frames per second for video (default: 4)")
    args = parser.parse_args()

    bmp_dir = Path(args.bmp_dir)
    png_dir = Path(args.png_dir)

    if not bmp_dir.is_dir():
        print(f"BMP directory not found: {bmp_dir}")
        sys.exit(0)  # Not an error — may not have captured frames

    bmps = sorted(bmp_dir.glob("frame_*.bmp"))
    if not bmps:
        print("No frame_*.bmp files found")
        sys.exit(0)

    png_dir.mkdir(parents=True, exist_ok=True)

    converted = 0
    for bmp in bmps:
        png_path = png_dir / bmp.with_suffix(".png").name
        if bmp_to_png(str(bmp), str(png_path)):
            converted += 1
            print(f"  Converted: {bmp.name} → {png_path.name}")
        else:
            print(f"  Failed: {bmp.name}")

    print(f"\nConverted {converted}/{len(bmps)} frames to PNG")

    if args.video and converted > 0:
        generate_video(str(png_dir), args.video, args.fps)


if __name__ == "__main__":
    main()
