#!/usr/bin/env python3
"""
Convert SPI panel boot logo PNG -> RGB565 for U-Boot jd9853_logo.

Default source: <sdk>/logo/boot_logo.png
Default output: <sdk>/build/generated/spi_bootlogo/logo_172x320.rgb565

Used automatically when CONFIG_CMD_JD9853_LOGO=y (SPI JD9853 panel).
"""

import argparse
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    Image = None

DEFAULT_WIDTH = 172
DEFAULT_HEIGHT = 320


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def default_logo(width, height):
    out = bytearray()
    for y in range(height):
        for x in range(width):
            t = y / max(height - 1, 1)
            r = int(8 + 20 * t)
            g = int(24 + 60 * t)
            b = int(72 + 120 * t)
            if 60 <= x < 112 and 120 <= y < 200:
                r, g, b = 220, 230, 245
            elif 40 <= x < 132 and 210 <= y < 240:
                r, g, b = 40, 120, 200
            out += struct.pack("<H", rgb565(r, g, b))
    return bytes(out)


def png_to_rgb565(path, width, height):
    if Image is None:
        sys.exit("Pillow required for PNG input: pip install pillow")
    img = Image.open(path).convert("RGB").resize((width, height), Image.LANCZOS)
    out = bytearray()
    for y in range(height):
        for x in range(width):
            r, g, b = img.getpixel((x, y))
            out += struct.pack("<H", rgb565(r, g, b))
    return bytes(out)


def sdk_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    root = sdk_root()
    default_png = os.path.join(root, "logo", "boot_logo.png")
    default_out = os.path.join(root, "build", "generated", "spi_bootlogo",
                               "logo_172x320.rgb565")

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-i", "--input", default=default_png,
                    help=f"source PNG (default: {default_png})")
    ap.add_argument("-o", "--output", default=default_out,
                    help=f"RGB565 output (default: {default_out})")
    ap.add_argument("-W", "--width", type=int, default=DEFAULT_WIDTH)
    ap.add_argument("-H", "--height", type=int, default=DEFAULT_HEIGHT)
    args = ap.parse_args()

    expected = args.width * args.height * 2
    if os.path.isfile(args.input):
        data = png_to_rgb565(args.input, args.width, args.height)
        src = args.input
    else:
        print(f"gen_spi_bootlogo: {args.input} not found, using built-in default",
              file=sys.stderr)
        data = default_logo(args.width, args.height)
        src = "<builtin>"

    if len(data) != expected:
        sys.exit(f"bad size {len(data)}, expected {expected}")

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(data)
    print(f"gen_spi_bootlogo: {src} -> {args.output} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
