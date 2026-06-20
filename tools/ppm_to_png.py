#!/usr/bin/env python3
"""Convert a binary P6 PPM to PNG using only the Python standard library.

Used by the UI fidelity gate: the client writes captures as PPM (via the proven
WritePixelBufferPpm path); this converts them to PNG so the visual-critique agents
can read them with the image tooling. No PIL/numpy dependency.

Usage: ppm_to_png.py <in.ppm> [out.png]
"""
import sys
import struct
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ValueError(f"{path}: not a binary P6 PPM")
    # Parse the header: magic, width, height, maxval — each separated by whitespace,
    # with possible comment lines (#...). Find the pixel data start after maxval.
    idx = 2
    fields = []
    while len(fields) < 3:
        # skip whitespace
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":  # comment to end of line
            while idx < len(data) and data[idx:idx + 1] not in (b"\n", b"\r"):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    width, height, maxval = fields
    idx += 1  # single whitespace after maxval precedes the raster
    if maxval != 255:
        raise ValueError(f"{path}: only maxval 255 supported (got {maxval})")
    expected = width * height * 3
    raster = data[idx:idx + expected]
    if len(raster) != expected:
        raise ValueError(f"{path}: short raster {len(raster)} != {expected}")
    return width, height, raster


def write_png(path, width, height, rgb):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    # Prepend the filter byte (0 = none) to each scanline.
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * stride:(y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    in_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else in_path.rsplit(".", 1)[0] + ".png"
    w, h, rgb = read_ppm(in_path)
    write_png(out_path, w, h, rgb)
    print(f"{in_path} -> {out_path} ({w}x{h})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
