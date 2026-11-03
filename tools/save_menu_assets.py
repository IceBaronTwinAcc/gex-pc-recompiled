#!/usr/bin/env python3
"""Extract Video Deck artwork from supplied screenshots, or embed the extracted pack."""

import argparse
from pathlib import Path
import struct

NAMES = ("162827", "162829", "162831", "162833", "162835",
         "162836", "162839", "162840", "162842", "162843")


def extract(folder, output):
    from PIL import Image
    frames = []
    for name in NAMES:
        image = Image.open(folder / f"Screenshot_20261102_{name}.png").convert("RGB")
        width, height = image.size
        if width * 9 != height * 16:
            raise ValueError(f"{name}: expected a 16:9 screenshot with a 4:3 pillarboxed image")
        frames.append(image.crop((width // 8, 0, width * 7 // 8, height))
                      .resize((320, 240), Image.Resampling.BOX))
    sheet = Image.new("RGB", (320, 240 * len(frames)))
    for i, frame in enumerate(frames):
        sheet.paste(frame, (0, i * 240))
    # Reserve saturated ramps: median-cut alone loses the small bright PLAY
    # triangles and red indicator lamps amongst the much larger brown cabinet.
    palette_image = sheet.quantize(colors=192)
    reserved = palette_image.getpalette()[:192 * 3]
    reserved += [value for i in range(32) for value in (0, i * 255 // 31, 0)]
    reserved += [value for i in range(32) for value in (i * 255 // 31, 0, 0)]
    palette_image.putpalette(reserved)
    indexed = sheet.quantize(palette=palette_image, dither=Image.Dither.NONE)
    palette = indexed.getpalette()
    pixels = indexed.tobytes()
    states = [pixels[i * 76800:(i + 1) * 76800] for i in range(10)]
    base = bytearray(states[9])
    # EXIT is selected in the otherwise unselected reference frame.
    for y in range(160, 191):
        base[y * 320 + 225:y * 320 + 277] = states[0][y * 320 + 225:y * 320 + 277]
    packed = bytearray(b"GEXART1\0")
    for i in range(256):
        r, g, b = palette[i * 3:i * 3 + 3]
        packed.extend(struct.pack("<H", (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10))
    packed.extend(base)
    offsets_at = len(packed)
    packed.extend(bytes(40))
    for state, frame in enumerate(states):
        struct.pack_into("<I", packed, offsets_at + state * 4, len(packed))
        i = 0
        while i < len(base):
            if frame[i] == base[i]:
                i += 1
                continue
            start = i
            while i < len(base) and frame[i] != base[i] and i - start < 65535:
                i += 1
            packed.extend(struct.pack("<IH", start, i - start))
            packed.extend(frame[start:i])
        packed.extend(struct.pack("<I", 0xFFFFFFFF))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(packed)
    print(f"Extracted background and ten selections: {len(packed)} bytes")


def embed(source, output):
    data = source.read_bytes()
    if data[:8] != b"GEXART1\0" or len(data) < 77360:
        raise ValueError("invalid Video Deck artwork")
    lines = ["/* Generated from the extracted artwork pack. */",
             "static const unsigned char gex_save_art[] = {"]
    lines.extend("    " + ",".join(f"0x{v:02x}" for v in data[i:i + 24]) + ","
                 for i in range(0, len(data), 24))
    lines.append("};\n")
    output.write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("extract", "embed"))
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    (extract if args.operation == "extract" else embed)(args.source, args.output)


if __name__ == "__main__":
    main()
