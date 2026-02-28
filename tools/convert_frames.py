#!/usr/bin/env python3
"""
Convert captured BMP frames to PNG and generate an MP4 video.

This script is used by CI to produce embeddable images for PR comments
and a render progress video showing all captured frames in sequence.

Reads frame_metadata.txt (written by C capture code) to burn debug text
overlay into frames. Text is rendered using a minimal bitmap font in Python.

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


# Minimal 5x7 bitmap font for debug overlay (ASCII 0x20-0x7A)
# Each entry: list of 7 bytes, bits 4..0 = columns left-to-right
FONT = {
    ' ': [0x00,0x00,0x00,0x00,0x00,0x00,0x00],
    '!': [0x04,0x04,0x04,0x04,0x04,0x00,0x04],
    '0': [0x0E,0x11,0x13,0x15,0x19,0x11,0x0E],
    '1': [0x04,0x0C,0x04,0x04,0x04,0x04,0x0E],
    '2': [0x0E,0x11,0x01,0x06,0x08,0x10,0x1F],
    '3': [0x0E,0x11,0x01,0x06,0x01,0x11,0x0E],
    '4': [0x02,0x06,0x0A,0x12,0x1F,0x02,0x02],
    '5': [0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E],
    '6': [0x06,0x08,0x10,0x1E,0x11,0x11,0x0E],
    '7': [0x1F,0x01,0x02,0x04,0x08,0x08,0x08],
    '8': [0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E],
    '9': [0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C],
    ':': [0x00,0x00,0x04,0x00,0x04,0x00,0x00],
    'A': [0x0E,0x11,0x11,0x1F,0x11,0x11,0x11],
    'B': [0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E],
    'C': [0x0E,0x11,0x10,0x10,0x10,0x11,0x0E],
    'D': [0x1E,0x11,0x11,0x11,0x11,0x11,0x1E],
    'E': [0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F],
    'F': [0x1F,0x10,0x10,0x1E,0x10,0x10,0x10],
    'G': [0x0E,0x11,0x10,0x17,0x11,0x11,0x0E],
    'H': [0x11,0x11,0x11,0x1F,0x11,0x11,0x11],
    'I': [0x0E,0x04,0x04,0x04,0x04,0x04,0x0E],
    'K': [0x11,0x12,0x14,0x18,0x14,0x12,0x11],
    'L': [0x10,0x10,0x10,0x10,0x10,0x10,0x1F],
    'M': [0x11,0x1B,0x15,0x15,0x11,0x11,0x11],
    'N': [0x11,0x19,0x15,0x13,0x11,0x11,0x11],
    'O': [0x0E,0x11,0x11,0x11,0x11,0x11,0x0E],
    'P': [0x1E,0x11,0x11,0x1E,0x10,0x10,0x10],
    'R': [0x1E,0x11,0x11,0x1E,0x14,0x12,0x11],
    'S': [0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E],
    'T': [0x1F,0x04,0x04,0x04,0x04,0x04,0x04],
    'U': [0x11,0x11,0x11,0x11,0x11,0x11,0x0E],
    'V': [0x11,0x11,0x11,0x11,0x0A,0x0A,0x04],
    'W': [0x11,0x11,0x11,0x15,0x15,0x1B,0x11],
    'X': [0x11,0x11,0x0A,0x04,0x0A,0x11,0x11],
    'Y': [0x11,0x11,0x0A,0x04,0x04,0x04,0x04],
    'Z': [0x1F,0x01,0x02,0x04,0x08,0x10,0x1F],
    '-': [0x00,0x00,0x00,0x1F,0x00,0x00,0x00],
    '.': [0x00,0x00,0x00,0x00,0x00,0x00,0x04],
    'a': [0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F],
    'b': [0x10,0x10,0x1E,0x11,0x11,0x11,0x1E],
    'c': [0x00,0x00,0x0E,0x11,0x10,0x11,0x0E],
    'd': [0x01,0x01,0x0F,0x11,0x11,0x11,0x0F],
    'e': [0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E],
    'f': [0x06,0x08,0x1E,0x08,0x08,0x08,0x08],
    'i': [0x04,0x00,0x0C,0x04,0x04,0x04,0x0E],
    'l': [0x0C,0x04,0x04,0x04,0x04,0x04,0x0E],
    'n': [0x00,0x00,0x1E,0x11,0x11,0x11,0x11],
    'o': [0x00,0x00,0x0E,0x11,0x11,0x11,0x0E],
    'p': [0x00,0x00,0x1E,0x11,0x1E,0x10,0x10],
    'r': [0x00,0x00,0x16,0x19,0x10,0x10,0x10],
    's': [0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E],
    't': [0x08,0x08,0x1E,0x08,0x08,0x09,0x06],
    'u': [0x00,0x00,0x11,0x11,0x11,0x11,0x0F],
}


def load_metadata(metadata_path):
    """Load frame metadata from frame_metadata.txt.

    Returns dict mapping frame_number -> (line0, line1).
    """
    meta = {}
    if not os.path.exists(metadata_path):
        return meta
    with open(metadata_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('|', 2)
            if len(parts) >= 2:
                try:
                    frame = int(parts[0])
                    line0 = parts[1] if len(parts) > 1 else ''
                    line1 = parts[2] if len(parts) > 2 else ''
                    meta[frame] = (line0, line1)
                except ValueError:
                    continue
    return meta


def burn_text_into_pixels(pixels, width, height, text, x, y, color=(0, 255, 0)):
    """Burn text into RGB pixel bytearray using bitmap font at 2x scale."""
    for ci, ch in enumerate(text):
        glyph = FONT.get(ch, FONT.get(ch.upper(), FONT.get('?', None)))
        if not glyph:
            continue
        px = x + ci * 12
        for row_idx in range(7):
            row_bits = glyph[row_idx]
            for col in range(5):
                if row_bits & (1 << (4 - col)):
                    for sy in range(2):
                        for sx in range(2):
                            dx = px + col * 2 + sx
                            dy = y + row_idx * 2 + sy
                            if 0 <= dx < width and 0 <= dy < height:
                                idx = (dy * width + dx) * 3
                                pixels[idx] = color[0]
                                pixels[idx + 1] = color[1]
                                pixels[idx + 2] = color[2]


def bmp_to_png(bmp_path, png_path, debug_text=None):
    """Convert a 24-bit BMP to PNG using only the standard library.

    If debug_text is provided as (line0, line1), burn it into the image.
    """
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

    # Burn debug text overlay if provided
    if debug_text:
        line0, line1 = debug_text
        # Build flat RGB pixel buffer from BGR rows
        pixels = bytearray(width * height * 3)
        for y, row_bgr in enumerate(rows):
            for x in range(width):
                idx = (y * width + x) * 3
                pixels[idx] = row_bgr[x * 3 + 2]      # R
                pixels[idx + 1] = row_bgr[x * 3 + 1]   # G
                pixels[idx + 2] = row_bgr[x * 3]        # B
        # Burn text lines
        if line0:
            burn_text_into_pixels(pixels, width, height, line0, 4, 4)
        if line1:
            burn_text_into_pixels(pixels, width, height, line1, 4, 24)
        # Convert back to BGR rows
        new_rows = []
        for y in range(height):
            row = bytearray(width * 3)
            for x in range(width):
                idx = (y * width + x) * 3
                row[x * 3] = pixels[idx + 2]       # B
                row[x * 3 + 1] = pixels[idx + 1]   # G
                row[x * 3 + 2] = pixels[idx]        # R
            new_rows.append(bytes(row))
        rows = new_rows

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
        print("ffmpeg not found — install ffmpeg to enable video generation "
              "(e.g., apt-get install ffmpeg)")
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

    # Load debug text metadata if available
    metadata_path = bmp_dir / "frame_metadata.txt"
    metadata = load_metadata(str(metadata_path))
    if metadata:
        print(f"Loaded debug metadata for {len(metadata)} frames")

    png_dir.mkdir(parents=True, exist_ok=True)

    converted = 0
    for bmp in bmps:
        png_path = png_dir / bmp.with_suffix(".png").name
        # Extract frame number from filename (frame_NNNN.bmp)
        frame_num = None
        try:
            frame_num = int(bmp.stem.split('_')[1])
        except (IndexError, ValueError):
            pass
        debug_text = metadata.get(frame_num) if frame_num is not None else None
        if bmp_to_png(str(bmp), str(png_path), debug_text=debug_text):
            converted += 1
            print(f"  Converted: {bmp.name} → {png_path.name}"
                  f"{' (+debug text)' if debug_text else ''}")
        else:
            print(f"  Failed: {bmp.name}")

    print(f"\nConverted {converted}/{len(bmps)} frames to PNG")

    if args.video and converted > 0:
        generate_video(str(png_dir), args.video, args.fps)


if __name__ == "__main__":
    main()
