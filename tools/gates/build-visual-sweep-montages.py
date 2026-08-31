#!/usr/bin/env python3
""": assemble labelled contact-sheet montages from the
world_visual_sweep capture matrix so the orchestrator can review visual quality.

Reads <artifact_dir>/world-visual-sweep-manifest.json + the per-cell PPMs under
<artifact_dir>/sweep/, converts every PPM to PNG, and builds:
  * one montage per (season, time-of-day): rows = weather (clear / storm),
    cols = camera angle. Each cell label (angle + weather + key feature signal)
    is burned into the tile.
  * one per-feature montage that lines up the cells whose feature the matrix is
    meant to SHOW (down-daytime foliage, storm rain/lightning, up clouds, water).

Output PNGs land under <artifact_dir>/sweep/montages/. Absolute paths are printed.
"""
import json
import os
import sys
from PIL import Image, ImageDraw, ImageFont


def load_font(size):
    for name in ("DejaVuSans.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def label_for(cell):
    bits = [cell["angle"], cell["weather"]]
    if cell.get("foliage_draws", 0) > 0:
        bits.append(f"fol={cell['foliage_instances']}")
    if cell.get("particle_draws", 0) > 0:
        bits.append(f"rain={cell['particles_drawn']}")
    if cell.get("lightning_active"):
        bits.append("bolt")
    if cell.get("cloud_coverage", 0) > 0 and cell["angle"] == "up25":
        bits.append(f"cloud={cell['cloud_coverage']:.2f}")
    if cell.get("water_aimed") and cell.get("water_like_ratio", 0) > 0:
        bits.append(f"water={cell['water_like_ratio']:.2f}")
    return "  ".join(bits)


def make_tile(art_dir, cell, tile_w, tile_h, font):
    ppm = os.path.join(art_dir, cell["file"])
    img = Image.open(ppm).convert("RGB").resize((tile_w, tile_h - 24))
    tile = Image.new("RGB", (tile_w, tile_h), (16, 16, 20))
    tile.paste(img, (0, 24))
    draw = ImageDraw.Draw(tile)
    draw.rectangle([0, 0, tile_w, 24], fill=(28, 28, 34))
    draw.text((4, 5), label_for(cell), fill=(235, 235, 245), font=font)
    return tile


def grid(tiles, cols, tile_w, tile_h):
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tile_w, rows * tile_h), (8, 8, 10))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * tile_w, (i // cols) * tile_h))
    return sheet


def main():
    art_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    manifest = json.load(open(os.path.join(art_dir, "world-visual-sweep-manifest.json")))
    cells = manifest["cells"]
    out_dir = os.path.join(art_dir, "sweep", "montages")
    os.makedirs(out_dir, exist_ok=True)
    font = load_font(13)

    tile_w, tile_h = 320, 204  # 16:9-ish thumbnail + label strip
    angle_order = ["yaw000", "yaw120", "yaw240", "down35", "up25", "water"]
    weather_order = ["clear", "storm"]
    produced = []

    # Also emit per-cell PNGs alongside the PPMs (handy for direct review).
    png_dir = os.path.join(art_dir, "sweep", "png")
    os.makedirs(png_dir, exist_ok=True)
    for c in cells:
        if not c.get("produced"):
            continue
        src = os.path.join(art_dir, c["file"])
        dst = os.path.join(png_dir, os.path.basename(c["file"]).replace(".ppm", ".png"))
        Image.open(src).convert("RGB").save(dst)

    # Per (season, tod) montage: rows=weather, cols=angle.
    seasons = sorted({c["season"] for c in cells})
    tods = ["dawn", "noon", "dusk", "night"]
    for season in seasons:
        for tod in tods:
            tiles = []
            for weather in weather_order:
                for angle in angle_order:
                    match = [c for c in cells if c["season"] == season and c["tod"] == tod
                             and c["weather"] == weather and c["angle"] == angle and c.get("produced")]
                    if match:
                        tiles.append(make_tile(art_dir, match[0], tile_w, tile_h, font))
            if not tiles:
                continue
            sheet = grid(tiles, len(angle_order), tile_w, tile_h)
            # Title banner.
            banner_h = 30
            titled = Image.new("RGB", (sheet.width, sheet.height + banner_h), (8, 8, 10))
            titled.paste(sheet, (0, banner_h))
            d = ImageDraw.Draw(titled)
            d.text((8, 7), f"world_visual_sweep  {season}  {tod}  (rows: clear / storm, cols: angle)",
                   fill=(255, 255, 255), font=load_font(16))
            path = os.path.join(out_dir, f"montage_{season}_{tod}.png")
            titled.save(path)
            produced.append(os.path.abspath(path))

    # Per-feature montage: line up the cells each feature is meant to SHOW.
    feature_specs = [
        ("foliage_down_daytime", lambda c: c["pitched_down"] and c["daytime"] and not c["storm"]),
        ("storm_rain_lightning", lambda c: c["storm"] and c["angle"] in ("yaw000", "down35")),
        ("clouds_up_storm", lambda c: c["pitched_up"] and c["storm"]),
        ("water_shore", lambda c: c["water_aimed"] and c["daytime"]),
    ]
    for fname, pred in feature_specs:
        sel = [c for c in cells if pred(c) and c.get("produced")]
        if not sel:
            continue
        tiles = [make_tile(art_dir, c, tile_w, tile_h, font) for c in sel]
        cols = min(6, len(tiles))
        sheet = grid(tiles, cols, tile_w, tile_h)
        banner_h = 30
        titled = Image.new("RGB", (sheet.width, sheet.height + banner_h), (8, 8, 10))
        titled.paste(sheet, (0, banner_h))
        d = ImageDraw.Draw(titled)
        d.text((8, 7), f"feature: {fname}", fill=(255, 255, 255), font=load_font(16))
        path = os.path.join(out_dir, f"feature_{fname}.png")
        titled.save(path)
        produced.append(os.path.abspath(path))

    print(f"montages: {len(produced)}")
    for p in produced:
        print(p)


if __name__ == "__main__":
    main()
