#!/usr/bin/env python3
"""Convert individual tools/faces PNGs into flash-resident RGB565/RLE assets.

Requires Pillow. Run from any directory; outputs are relative to the repository.
Example: python3 tools/import_faces.py --size 120 --face-size 00=96
"""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

ROOT = Path(__file__).resolve().parents[1]
FACE_IDS = range(16)


def encode_runs(indices, width):
    """Pack 1-16 repeated palette indices per byte, never across row edges."""
    runs = bytearray()
    for start in range(0, len(indices), width):
        end = start + width
        pos = start
        while pos < end:
            color = indices[pos]
            count = 1
            while count < 16 and pos + count < end and indices[pos + count] == color:
                count += 1
            runs.append(((count - 1) << 4) | color)
            pos += count
    return runs


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rgb888(color):
    r, g, b = (color >> 11) & 31, (color >> 5) & 63, color & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def convert(image):
    # No dithering: flat palette regions compress better and avoid stippled faces.
    indexed = image.quantize(colors=16, method=Image.Quantize.FASTOCTREE,
                             dither=Image.Dither.NONE)
    rgb = indexed.getpalette()
    palette = [rgb565(*rgb[i:i + 3]) for i in range(0, 48, 3)]
    # Merge entries which become identical in RGB565 before run-length encoding.
    canonical = [palette.index(color) for color in palette]
    indices = bytes(canonical[index] for index in indexed.tobytes())
    runs = encode_runs(indices, image.width)
    preview = Image.new("RGB", image.size)
    preview.putdata([rgb888(palette[index]) for index in indices])
    return palette, runs, preview


def size_arg(value):
    size = int(value)
    if not 16 <= size <= 240:
        raise argparse.ArgumentTypeError("size must be 16-240 pixels")
    return size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, default=ROOT / "tools/faces",
                        help="directory containing PNGs prefixed with IDs 00 through 15")
    parser.add_argument("--size", type=size_arg, default=240,
                        help="maximum native image dimension (default: 240)")
    parser.add_argument("--face-size", action="append", default=[], metavar="ID=SIZE",
                        help="override one face's resolution; may be repeated")
    parser.add_argument("--output", type=Path, default=ROOT / "ESP32_S3/face_assets.h")
    parser.add_argument("--previews", type=Path,
                        help="optional output directory for rendered previews")
    args = parser.parse_args()
    sizes = dict.fromkeys(FACE_IDS, args.size)
    for override in args.face_size:
        try:
            name, value = override.split("=", 1)
            if len(name) != 2 or any(c not in "0123456789" for c in name) or int(name) not in sizes:
                raise ValueError("face ID must be 00-15")
            sizes[int(name)] = size_arg(value)
        except (ValueError, argparse.ArgumentTypeError) as error:
            parser.error(f"invalid --face-size {override!r}: {error}")

    # Read every source before writing outputs, so missing/broken replacements
    # leave the existing firmware assets and previews intact.
    paths = {}
    if not args.input_dir.is_dir():
        parser.error(f"face directory not found: {args.input_dir}")
    for path in sorted(args.input_dir.iterdir()):
        if not path.is_file() or path.suffix.lower() != ".png":
            continue
        prefix = path.stem[:2]
        if len(prefix) != 2 or any(c not in "0123456789" for c in prefix):
            parser.error(f"PNG must start with a two-digit face ID: {path.name}")
        face_id = int(prefix)
        if face_id not in FACE_IDS:
            parser.error(f"face ID must be 00-15: {path.name}")
        if face_id in paths:
            parser.error(f"duplicate face ID {face_id:02d}: {paths[face_id].name}, {path.name}")
        paths[face_id] = path
    missing = sorted(set(FACE_IDS) - paths.keys())
    if missing:
        parser.error("missing face IDs: " + ", ".join(f"{i:02d}" for i in missing))
    images = {}
    for face_id in FACE_IDS:
        path = paths[face_id]
        try:
            with Image.open(path) as source:
                rgba = ImageOps.exif_transpose(source).convert("RGBA")
                background = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
                images[face_id] = Image.alpha_composite(background, rgba).convert("RGB")
        except (OSError, ValueError) as error:
            parser.error(f"cannot read face image {path}: {error}")
    if args.previews:
        if args.previews.resolve() == args.input_dir.resolve():
            parser.error("preview directory must differ from the source directory")
        args.previews.mkdir(parents=True, exist_ok=True)
    output = ["// Generated by tools/import_faces.py; do not edit by hand.",
              "#ifndef FACE_ASSETS_H", "#define FACE_ASSETS_H",
              '#include <pgmspace.h>', '#include "face_bitmap.h"', ""]
    descriptors = {}
    total_bytes = 0
    # Preview each face at native size on a 240x240 display, plus a caption below.
    contact = Image.new("RGB", (4 * 240, 4 * 264), "#202020")
    labels = ImageDraw.Draw(contact)
    for face_id in FACE_IDS:
        name = f"{face_id:02d}"
        col, row = face_id % 4, face_id // 4
        crop = images[face_id]
        crop.thumbnail((sizes[face_id], sizes[face_id]), Image.Resampling.LANCZOS)
        palette, runs, preview = convert(crop)
        if args.previews:
            preview.save(args.previews / f"{name}.png")
        output.append(f"static const uint16_t FACE_{name}_PALETTE[16] PROGMEM = {{")
        output.append("  " + ", ".join(f"0x{color:04x}" for color in palette) + "\n};")
        output.append(f"static const uint8_t FACE_{name}_RUNS[] PROGMEM = {{")
        for offset in range(0, len(runs), 24):
            output.append("  " + ", ".join(f"0x{value:02x}" for value in runs[offset:offset + 24]) + ",")
        output.append("};\n")
        descriptors[face_id] = (f"  {{{crop.width}, {crop.height}, FACE_{name}_PALETTE, "
                             f"FACE_{name}_RUNS, sizeof(FACE_{name}_RUNS)}}, // {name}")
        total_bytes += 32 + len(runs)
        contact.paste(preview, (col * 240 + (240 - crop.width) // 2,
                               row * 264 + (240 - crop.height) // 2))
        labels.text((col * 240 + 8, row * 264 + 244), f"Face {name}", fill="white")
        print(f"{name}: {crop.width}x{crop.height}: {32 + len(runs):,} bytes")
    output += ["static const FaceBitmap FACE_BITMAPS[] = {",
               *(descriptors[face_id] for face_id in FACE_IDS), "};", "#endif", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(output))
    if args.previews:
        contact.save(args.previews / "contact_sheet.png")
    print(f"All 16 faces: {total_bytes:,} bytes of pixel data and palettes (plus descriptors)")


if __name__ == "__main__":
    main()
