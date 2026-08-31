#!/usr/bin/env python3
"""framediff.py — compare two rendered frames by horizontal band means.

 render-scale diagnostic. Splits each image into three horizontal bands
(SKY = top 30%, HORIZON = 30-55%, GROUND = 55-100%) and reports the mean RGB of
each band plus the per-band ratio b/a. A correct render-scale seam makes the 0.67
frame's band means ~= the 1.0 reference (ratios ~1.0); the scale<1.0 viewport-seam
bug crushed them (sky ~0.29x, horizon ~0.11x, ground ~0.07x).

Usage:  python tools/framediff.py <img_a_1.0_reference> <img_b_0.67>
The second image is resized to the first's dimensions before sampling so a
different-resolution capture still compares band-for-band. Requires Pillow.
"""
import sys
from PIL import Image

BANDS = [("SKY     (top 30%)", 0.00, 0.30),
         ("HORIZON (30-55%) ", 0.30, 0.55),
         ("GROUND  (55-100%)", 0.55, 1.00)]


def band_mean(img, y0, y1):
    w, h = img.size
    px = img.load()
    r = g = b = n = 0
    # Coarse grid sample (fast, deterministic) — every 29th col, 17th row.
    for y in range(int(y0 * h), int(y1 * h), 17):
        for x in range(0, w, 29):
            pr, pg, pb = px[x, y][:3]
            r += pr; g += pg; b += pb; n += 1
    if n == 0:
        return (0.0, 0.0, 0.0)
    return (r / n, g / n, b / n)


def main():
    if len(sys.argv) != 3:
        print("usage: python tools/framediff.py <img_a_1.0_ref> <img_b_0.67>")
        return 2
    a = Image.open(sys.argv[1]).convert("RGB")
    b = Image.open(sys.argv[2]).convert("RGB")
    if b.size != a.size:
        b = b.resize(a.size)
    print(f"A (reference) = {sys.argv[1]}  {a.size}")
    print(f"B (candidate) = {sys.argv[2]}  {b.size}")
    print(f"{'band':<18} {'A mean RGB':>22} {'B mean RGB':>22}   ratio B/A (lum)")
    for name, y0, y1 in BANDS:
        ma = band_mean(a, y0, y1)
        mb = band_mean(b, y0, y1)
        la = 0.2126 * ma[0] + 0.7152 * ma[1] + 0.0722 * ma[2]
        lb = 0.2126 * mb[0] + 0.7152 * mb[1] + 0.0722 * mb[2]
        ratio = (lb / la) if la > 0.01 else float("nan")
        print(f"{name:<18} ({ma[0]:6.1f},{ma[1]:6.1f},{ma[2]:6.1f}) "
              f"({mb[0]:6.1f},{mb[1]:6.1f},{mb[2]:6.1f})   {ratio:6.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
