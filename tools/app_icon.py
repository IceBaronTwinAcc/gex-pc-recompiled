#!/usr/bin/env python3
"""Generate platform icons (Pillow) or embed the SDL bitmap (standard library)."""

import argparse
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / "assets/icons"


def generate():
    from PIL import Image

    original = Image.open(ICONS / "source.png").convert("RGBA")
    edge = max(original.size)
    square = Image.new("RGBA", (edge, edge))
    square.paste(original, ((edge - original.width) // 2, (edge - original.height) // 2))
    sizes = (16, 24, 32, 48, 64, 128, 256, 512)
    for size in sizes:
        square.resize((size, size), Image.Resampling.LANCZOS).save(ICONS / f"gex-{size}.png")
    square.resize((256, 256), Image.Resampling.LANCZOS).save(
        ICONS / "gex.ico", sizes=[(size, size) for size in sizes if size <= 256])
    square.resize((1024, 1024), Image.Resampling.LANCZOS).save(ICONS / "gex.icns")

    # BITMAPV4HEADER retains alpha without needing a runtime PNG decoder.
    bitmap = square.resize((128, 128), Image.Resampling.LANCZOS)
    pixels = bitmap.tobytes("raw", "BGRA")
    header = bytearray(108)
    struct.pack_into("<IiiHHII", header, 0, 108, 128, -128, 1, 32, 3, len(pixels))
    struct.pack_into("<5I", header, 40, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, 0x73524742)
    (ICONS / "window.bmp").write_bytes(
        struct.pack("<2sIHHI", b"BM", 122 + len(pixels), 0, 0, 122) + header + pixels)

    resources = ROOT / "android/res"
    for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                          ("xxhdpi", 144), ("xxxhdpi", 192)):
        directory = resources / f"mipmap-{density}"
        directory.mkdir(parents=True, exist_ok=True)
        square.resize((size, size), Image.Resampling.LANCZOS).save(directory / "ic_launcher.png")
    foreground = Image.new("RGBA", (432, 432))
    foreground.paste(square.resize((264, 264), Image.Resampling.LANCZOS), (84, 84))
    directory = resources / "drawable-nodpi"
    directory.mkdir(parents=True, exist_ok=True)
    foreground.save(directory / "ic_launcher_foreground.png")


def embed(output):
    data = (ICONS / "window.bmp").read_bytes()
    rows = (", ".join(f"0x{value:02x}" for value in data[index:index + 16])
            for index in range(0, len(data), 16))
    output.write_text("static const unsigned char gex_app_icon_bmp[] = {\n    " +
                      ",\n    ".join(rows) + "\n};\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("generate")
    embedding = commands.add_parser("embed")
    embedding.add_argument("output", type=Path)
    arguments = parser.parse_args()
    if arguments.command == "generate":
        generate()
    else:
        embed(arguments.output)


if __name__ == "__main__":
    main()
