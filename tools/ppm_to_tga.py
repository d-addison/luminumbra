#!/usr/bin/env python3
"""Convert a binary P6 PPM to an uncompressed 24-bit TGA (top-down, BGR).

RmlUi's reference GL3 backend (RmlUi_Renderer_GL3.cpp::LoadTexture) only decodes
uncompressed truecolor TGA (dataType 2), NOT PNG/stb. UI <img src> thumbnails must
therefore be TGA. This reads the engine's PPM captures and writes a TGA the backend
loads: top-left origin (imageDescriptor bit5 set) so no Y-flip, BGR byte order.

Usage: ppm_to_tga.py <in.ppm> [out.tga] [width]
"""
import sys

# Reuse the PPM reader + box downscaler from the sibling converter.
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from ppm_to_png import read_ppm, downscale  # noqa: E402


def write_tga(path, width, height, rgb):
    # 18-byte header: idlen=0, cmaptype=0, datatype=2 (uncompressed truecolor),
    # cmap spec=0 (5 bytes), x/y origin=0 (4 bytes), width, height (LE16),
    # bpp=24, imageDescriptor=0x20 (top-left origin → backend reads rows as-is).
    header = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    width & 0xFF, (width >> 8) & 0xFF,
                    height & 0xFF, (height >> 8) & 0xFF,
                    24, 0x20])
    out = bytearray(len(header) + width * height * 3)
    out[:len(header)] = header
    o = len(header)
    # RGB (top-down) -> BGR (top-down).
    for i in range(0, len(rgb), 3):
        out[o] = rgb[i + 2]
        out[o + 1] = rgb[i + 1]
        out[o + 2] = rgb[i]
        o += 3
    with open(path, "wb") as f:
        f.write(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    in_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else in_path.rsplit(".", 1)[0] + ".tga"
    target_w = int(argv[3]) if len(argv) > 3 else 0
    w, h, rgb = read_ppm(in_path)
    if target_w:
        w, h, rgb = downscale(w, h, rgb, target_w)
    write_tga(out_path, w, h, rgb)
    print(f"{in_path} -> {out_path} ({w}x{h})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
