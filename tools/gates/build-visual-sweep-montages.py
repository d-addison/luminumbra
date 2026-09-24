#!/usr/bin/env python3
"""Build fixed-layout world-sweep contact sheets, retaining failed capture slots."""
import argparse
import hashlib
import io
import itertools
import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont, ImageOps

ANGLES = ("yaw000", "yaw120", "yaw240", "down35", "up25", "water")
TIMES = ("dawn", "noon", "dusk", "night")
WEATHERS = ("clear", "storm")
TILE_W, TILE_H, LABEL_H = 320, 224, 44


def load_font(size):
    for name in ("DejaVuSans.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def cell_key(cell):
    return tuple(cell.get(field) for field in ("season", "tod", "weather", "angle"))


def basename(key):
    season, tod, weather, angle = key
    return f"{season}__{tod}__{angle}__{weather}"


def matrix(manifest):
    if not isinstance(manifest, dict) or manifest.get("schema") != "luminumbra.world_visual_sweep.v1":
        raise ValueError("Expected luminumbra.world_visual_sweep.v1.")
    dimensions = manifest.get("matrix", {})
    if not isinstance(dimensions, dict) or any(type(v) is not int for v in dimensions.values()):
        raise ValueError("Expected integer matrix dimensions.")
    seasons = dimensions.get("seasons")
    if seasons not in (1, 2) or dimensions != {"seasons": seasons, "times_of_day": 4, "angles": 6, "weather": 2}:
        raise ValueError("Unsupported world-sweep matrix; expected 1/2 seasons x 4 times x 6 angles x 2 weather states.")
    cells = manifest.get("cells")
    if not isinstance(cells, list) or any(not isinstance(c, dict) for c in cells):
        raise ValueError("Expected a list of cell records.")
    if any(not all(isinstance(v, str) for v in cell_key(c)) for c in cells):
        raise ValueError("Every cell needs string season/time/weather/angle identifiers.")
    return list(itertools.product(("summer", "winter")[:seasons], TIMES, WEATHERS, ANGLES))


def make_tile(key, status, picture):
    tile = Image.new("RGB", (TILE_W, TILE_H), (16, 16, 20))
    if picture is not None:
        scaled = ImageOps.contain(picture, (TILE_W, TILE_H - LABEL_H))
        tile.paste(scaled, ((TILE_W - scaled.width)//2, LABEL_H + (TILE_H-LABEL_H-scaled.height)//2))
    draw = ImageDraw.Draw(tile)
    draw.rectangle((0, 0, TILE_W, LABEL_H - 1), fill=(28, 28, 34) if status == "captured" else (105, 25, 30))
    draw.text((4, 3), " / ".join(key), fill="white", font=load_font(12))
    draw.text((4, 23), status.upper(), fill="white", font=load_font(12))
    if picture is None:
        draw.text((12, LABEL_H + 60), "NO QUALIFIED CAPTURE", fill="white", font=load_font(14))
    return tile


def save_grid(path, tiles, columns, title):
    sheet = Image.new("RGB", (columns*TILE_W, ((len(tiles)+columns-1)//columns)*TILE_H + 32), (8, 8, 10))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % columns)*TILE_W, 32 + (i//columns)*TILE_H))
    ImageDraw.Draw(sheet).text((8, 7), title, fill="white", font=load_font(16))
    sheet.save(path)


def build_review(artifact_dir):
    root = Path(artifact_dir).resolve()
    source = (root / "world-visual-sweep-manifest.json").read_bytes()
    manifest = json.loads(source)
    expected = matrix(manifest)
    indexed = {key: [] for key in expected}
    findings = []
    for cell in manifest["cells"]:
        key = cell_key(cell)
        if key not in indexed:
            findings.append("Unknown cell: " + repr(key))
        else:
            indexed[key].append(cell)
    if manifest.get("expected_cell_count") != len(expected):
        findings.append("Declared expected count does not match the required matrix.")
    for field, flag in (("produced_cell_count", "produced"), ("non_black_cell_count", "non_black")):
        if manifest.get(field) != sum(c.get(flag) is True for c in manifest["cells"]):
            findings.append(f"Declared {field} does not match cell records.")
    producer_failures = manifest.get("failures")
    if not isinstance(producer_failures, list) or not all(isinstance(f, str) for f in producer_failures):
        raise ValueError("Expected producer failure strings.")
    if manifest.get("passed") is not True or producer_failures:
        findings.append("The source producer did not pass; retain its failure evidence.")

    png_dir, out_dir = root / "sweep/png", root / "sweep/montages"
    for directory in (png_dir, out_dir):
        if not directory.resolve().is_relative_to(root):
            raise ValueError("Derived output directory escapes the artifact directory.")
        directory.mkdir(parents=True, exist_ok=True)
    records, tiles = [], {}
    for key in expected:
        entries = indexed[key]
        filename = basename(key)
        target = png_dir / (filename + ".png")
        # These names belong to this tool; a failed rerun must not leave its old PNG.
        target.unlink(missing_ok=True)
        status, picture, record = "captured", None, {"key": list(key)}
        if len(entries) != 1:
            status = "missing record" if not entries else "duplicate record"
        elif entries[0].get("produced") is not True:
            status = "not produced"
        else:
            cell = entries[0]
            try:
                relative = cell.get("file")
                if not isinstance(relative, str) or Path(relative).is_absolute() or ".." in Path(relative).parts:
                    raise ValueError("Invalid capture path.")
                path = (root / relative).resolve()
                if not path.is_relative_to(root) or path.parent != (root / "sweep").resolve():
                    raise ValueError("Capture must be inside the sweep directory.")
                if path.stem != filename or path.suffix != ".ppm":
                    raise ValueError("Capture filename does not match its matrix slot.")
                pixels = path.read_bytes()
                with Image.open(io.BytesIO(pixels)) as decoded:
                    if decoded.format != "PPM":
                        raise ValueError("Expected the engine's PPM capture.")
                    picture = decoded.convert("RGB")
                picture.save(target)
                record.update(source_file=relative, source_sha256=hashlib.sha256(pixels).hexdigest(),
                              png=target.relative_to(root).as_posix(),
                              png_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
                              dimensions=list(picture.size))
                if cell.get("non_black") is not True or any(f.endswith(":" + relative) for f in producer_failures):
                    status = "producer failure"
            except (OSError, ValueError, Image.DecompressionBombError) as error:
                status = "unreadable capture"
                record["error"] = str(error)
        if status != "captured":
            findings.append(f"{filename}: {status}")
        record["status"] = status
        records.append(record)
        tiles[key] = make_tile(key, status, picture)
    overall = "EVIDENCE COMPLETE" if not findings else "FAILED / INCOMPLETE EVIDENCE"
    sheets = []
    for season in dict.fromkeys(k[0] for k in expected):
        for tod in TIMES:
            selected = [tiles[(season, tod, weather, angle)] for weather in WEATHERS for angle in ANGLES]
            name = f"montage_{season}_{tod}.png"
            save_grid(out_dir/name, selected, 6, f"{season} / {tod} - {overall} - visual approval pending")
            sheets.append(name)
    features = {
        "foliage_down_daytime": lambda k: k[3] == "down35" and k[1] != "night" and k[2] == "clear",
        "storm_rain_lightning": lambda k: k[2] == "storm" and k[3] in ("yaw000", "down35"),
        "clouds_up_storm": lambda k: k[3] == "up25" and k[2] == "storm",
        "water_shore": lambda k: k[3] == "water" and k[1] != "night",
    }
    for feature, predicate in features.items():
        selected = [tiles[key] for key in expected if predicate(key)]
        name = f"feature_{feature}.png"
        save_grid(out_dir/name, selected, min(6, len(selected)), f"{feature} - {overall} - visual approval pending")
        sheets.append(name)
    report = {"schema": "luminumbra.world_visual_sweep.review.v1", "manifest_sha256": hashlib.sha256(source).hexdigest(),
              "evidence_complete": not findings, "visual_approval": "pending_user_review",
              "expected_cells": len(expected), "cells": records, "findings": findings,
              "producer_failures": producer_failures,
              "derivation": "PPM decoded to RGB PNG; contact sheets contain aspect-preserving thumbnails and status labels.",
              "sheets": {name: hashlib.sha256((out_dir/name).read_bytes()).hexdigest() for name in sheets}}
    (out_dir / "review.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_dir", nargs="?", default=".")
    args = parser.parse_args(argv)
    try:
        result = build_review(args.artifact_dir)
    except (OSError, ValueError, Image.DecompressionBombError) as error:
        print(f"Cannot build world-sweep review: {error}", file=sys.stderr)
        return 2
    print(f"Contact sheets: {len(result['sheets'])}; expected cells: {result['expected_cells']}; findings: {len(result['findings'])}")
    print(Path(args.artifact_dir).resolve() / "sweep/montages/review.json")
    return 0 if result["evidence_complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
