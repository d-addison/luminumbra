#!/usr/bin/env python3
"""Read capture PNGs; emit diagnostic JSON on stdout and a summary on stderr.

Per-image sidecars are <stem>.json or <name>.png.json (at most one), optionally
in --sidecars. Recognized fields: horizon_row_from_top (also under pixels),
camera.pitch/camera.fov (degrees; positive pitch looks up), pitch_degrees,
fov_degrees, and comparison_group/variant for explicitly differing captures.
Known clear/storm sweep and weather-baseline/weather-visual filenames also pair.
Ground comparisons approximate ground with the bottom 30% of the frame.
Inputs are read-only. These heuristics identify candidates, not rendering causes.
"""

import argparse
from collections import Counter
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import stat
import sys

import numpy as np
from PIL import Image

# All detector thresholds and resource bounds are centralized here. Pixel sizes
# below refer to the analysis grid; reports map boxes back to original pixels.
T = {
    "max_entries": 2048, "max_frames": 512, "max_file_bytes": 64 * 1024 * 1024,
    "max_sidecar_bytes": 1024 * 1024, "max_pixels": 16 * 1024 * 1024,
    "analysis_width": 640, "analysis_height": 320, "signature_width": 96,
    "signature_height": 48, "max_variants": 8, "max_label_chars": 256, "max_handle_path": 32768, "sky_palette_size": 16,
    "cluster_area": 0.0003, "cluster_min_pixels": 4,
    "magenta_high": 220, "magenta_low": 40, "black": 0, "white": 255,
    "flat_delta": 2, "rectangle_area": 24, "rectangle_side": 3,
    "rectangle_color_span": 8, "rectangle_fill": 0.94, "rectangle_max_area": 0.20, "rectangle_contrast": 12,
    "horizon_search_start": 0.25, "horizon_search_end": 0.90,
    "horizon_edge": 3, "horizon_margin": 0.05, "default_fov": 70,
    "horizon_limit_heights": 10, "pitch_limit": 89, "fov_min": 1, "fov_max": 179,
    "sky_color_delta": 14, "sky_blue_delta": 8, "sky_dark": 32,
    "sky_fraction": 0.025, "sky_smooth_delta": 5,
    "band_lines": 24, "band_plateau": 3, "band_steps": 6,
    "band_step_min": 2, "band_step_max": 16, "band_step_tolerance": 1,
    "band_span": 0.20, "band_line_fraction": 0.25,
    "similar_mean_delta": 2, "similar_pixel_delta": 8, "similar_fraction": 0.90,
    "comparison_ground_start": 0.70,
    "histogram_bins": 16, "histogram_drift": 0.25,
}


def windows_plain_path(name):
    if name.startswith("\\\\?\\UNC\\"):
        return "\\\\" + name[8:]
    if name.startswith("\\\\?\\"):
        return name[4:]
    return name


def read_regular(path, limit):
    """Open once, bound and read that descriptor; refuse devices and symlinks."""
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0)
    if os.name == "nt":
        # OPEN_REPARSE_POINT opens the link itself, so fstat can refuse it. The
        # final handle path also catches redirected parents and expands 8.3 names.
        import ctypes
        from ctypes import wintypes
        import msvcrt
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
        api.CreateFileW.restype = wintypes.HANDLE
        api.CloseHandle.argtypes = (wintypes.HANDLE,)
        api.CloseHandle.restype = wintypes.BOOL
        original = str(path)
        # Explicit filesystem paths prevent reserved basenames from opening
        # devices and also preserve long drive and UNC paths.
        if original.startswith("\\\\?\\"):
            input_name = original
        elif original.startswith("\\\\"):
            input_name = "\\\\?\\UNC\\" + original[2:]
        else:
            input_name = "\\\\?\\" + original
        handle = api.CreateFileW(input_name, 0x80000000, 7, None, 3, 0x02200000, None)
        if handle == wintypes.HANDLE(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            api.GetFinalPathNameByHandleW.argtypes = (wintypes.HANDLE, wintypes.LPWSTR,
                                                     wintypes.DWORD, wintypes.DWORD)
            api.GetFinalPathNameByHandleW.restype = wintypes.DWORD
            buffer = ctypes.create_unicode_buffer(T["max_handle_path"])
            length = api.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
            if not 0 < length < len(buffer):
                raise OSError("Cannot resolve input handle")
            final = windows_plain_path(buffer.value)
            expected = windows_plain_path(str(path.parent))
            if os.path.normcase(os.path.dirname(final)) != os.path.normcase(expected):
                raise ValueError("Input handle escapes its directory")
            descriptor = msvcrt.open_osfhandle(handle, flags)
        except BaseException:
            api.CloseHandle(handle)
            raise
    else:
        parent = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            descriptor = os.open(path.name, flags | os.O_NOFOLLOW, dir_fd=parent)
        finally:
            os.close(parent)
    with os.fdopen(descriptor, "rb") as source:
        info = os.fstat(source.fileno())
        if (not stat.S_ISREG(info.st_mode) or
                getattr(info, "st_file_attributes", 0) & 0x400):
            raise ValueError("Input is not a regular non-reparse file")
        if info.st_size > limit:
            raise ValueError("Input exceeds byte limit")
        payload = source.read(limit + 1)
        if len(payload) > limit or len(payload) != info.st_size:
            raise ValueError("Input grew or was truncated while reading")
        return payload


def directory(path):
    if not os.fspath(path):
        raise ValueError("Directory path must not be empty")
    root = Path(path).resolve(strict=True)
    names = []
    with os.scandir(root) as entries:
        for entry in entries:
            if len(names) >= T["max_entries"]:
                raise ValueError("Directory exceeds entry limit")
            names.append(entry.name)
    return root, sorted(names)


def load_png(path):
    payload = read_regular(path, T["max_file_bytes"])
    # A complete PNG ends with the zero-length IEND chunk and its CRC. Pixel
    # decoding alone can accept a capture interrupted while writing that chunk.
    if not payload.endswith(b"\x00\x00\x00\x00IEND\xaeB\x60\x82"):
        raise ValueError("PNG end marker is missing or incomplete")
    with Image.open(io.BytesIO(payload)) as image:
        if image.format != "PNG" or image.width * image.height > T["max_pixels"] or image.n_frames != 1:
            raise ValueError("Expected a single-frame PNG within the pixel limit")
        # Fully decode before shrinking: truncated PNGs cannot become clean input.
        image.load()
        if ("A" in image.getbands() or "transparency" in image.info):
            if image.convert("RGBA").getchannel("A").getextrema()[0] != 255:
                raise ValueError("Capture contains transparent pixels")
        source_size = image.size
        image = image.convert("RGB")
        image.thumbnail((T["analysis_width"], T["analysis_height"]), Image.Resampling.NEAREST)
        return np.asarray(image, dtype=np.int16), source_size, hashlib.sha256(payload).hexdigest()


def load_sidecar(root, name):
    found = []
    for candidate in (Path(name).stem + ".json", name + ".json"):
        try:
            payload = read_regular(root / candidate, T["max_sidecar_bytes"])
        except FileNotFoundError:
            continue
        try:
            value = json.loads(payload.decode("utf-8-sig"))
        except RecursionError as error:
            raise ValueError("Sidecar nesting exceeds the JSON parser limit") from error
        if not isinstance(value, dict):
            raise ValueError("Sidecar must be a JSON object")
        found.append(value)
    if len(found) > 1:
        raise ValueError("Ambiguous sidecars")
    return found[0] if found else {}


def components(mask):
    """Four-connected row runs; at most one run per analysis-grid pixel."""
    parents, ranks, boxes, areas = [], [], [], []

    def find(node):
        while parents[node] != node:
            parents[node] = parents[parents[node]]
            node = parents[node]
        return node

    previous = []
    for y, row in enumerate(mask):
        edges = np.flatnonzero(np.diff(np.r_[False, row, False]))
        current, cursor = [], 0
        for start, end in zip(edges[::2], edges[1::2]):
            node = len(parents)
            parents.append(node)
            ranks.append(0)
            boxes.append([int(start), y, int(end), y + 1])
            areas.append(int(end - start))
            while cursor < len(previous) and previous[cursor][1] <= start:
                cursor += 1
            index = cursor
            while index < len(previous) and previous[index][0] < end:
                other = find(previous[index][2])
                current_root = find(node)
                if current_root != other:
                    if ranks[current_root] < ranks[other]:
                        current_root, other = other, current_root
                    parents[other] = current_root
                    if ranks[current_root] == ranks[other]:
                        ranks[current_root] += 1
                index += 1
            current.append((start, end, node))
        previous = current
    combined = {}
    for node, box in enumerate(boxes):
        root = find(node)
        if root not in combined:
            combined[root] = [0, box.copy()]
        item = combined[root]
        item[0] += areas[node]
        item[1] = [min(item[1][0], box[0]), min(item[1][1], box[1]),
                   max(item[1][2], box[2]), max(item[1][3], box[3])]
    return sorted(combined.values(), key=lambda item: (-item[0], item[1]))


def luma(rgb):
    wide = rgb.astype(np.int32)
    return (wide[..., 0] * 54 + wide[..., 1] * 183 + wide[..., 2] * 19) / 256.0


def flat_mask(rgb, delta):
    mask = np.ones(rgb.shape[:2], dtype=bool)
    dx = np.max(np.abs(np.diff(rgb, axis=1)), axis=2) <= delta
    dy = np.max(np.abs(np.diff(rgb, axis=0)), axis=2) <= delta
    mask[:, 1:] &= dx
    mask[:, :-1] &= dx
    mask[1:] &= dy
    mask[:-1] &= dy
    return mask


def cluster_checks(rgb):
    red, green, blue = rgb.transpose(2, 0, 1)
    masks = {"magenta_cluster": (red >= T["magenta_high"]) & (blue >= T["magenta_high"]) &
             (green <= T["magenta_low"]),
             "black_cluster": np.max(rgb, axis=2) <= T["black"],
             "white_cluster": np.min(rgb, axis=2) >= T["white"]}
    minimum = max(T["cluster_min_pixels"], math.ceil(red.size * T["cluster_area"]))
    findings = []
    for check, mask in masks.items():
        selected = [item for item in components(mask) if item[0] >= minimum]
        if selected:
            findings.append(dict(check=check, clusters=len(selected), area=selected[0][0], box=selected[0][1]))
    return findings


def rectangles(rgb):
    height, width = rgb.shape[:2]
    selected = []
    for area, box in components(flat_mask(rgb, T["flat_delta"])):
        x0, y0, x1, y1 = box
        box_area = (x1 - x0) * (y1 - y0)
        if (area < T["rectangle_area"] or min(x1-x0, y1-y0) < T["rectangle_side"] or
                area / box_area < T["rectangle_fill"] or box_area > width*height*T["rectangle_max_area"] or
                x0 < 2 or y0 < 2 or x1 > width-2 or y1 > height-2):
            continue
        interior = rgb[y0:y1, x0:x1]
        if np.max(np.ptp(interior, axis=(0, 1))) > T["rectangle_color_span"]:
            continue
        inside = np.median(interior, axis=(0, 1))
        # Every side must contrast; smooth sky patches and partial edges are excluded.
        sides = (rgb[y0-2, x0:x1], rgb[y1+1, x0:x1], rgb[y0:y1, x0-2], rgb[y0:y1, x1+1])
        if all(np.median(np.max(np.abs(side-inside), axis=1)) >= T["rectangle_contrast"] for side in sides):
            selected.append([area, box])
    if not selected:
        return []
    return [dict(check="opaque_rectangle", rectangles=len(selected), area=selected[0][0], box=selected[0][1])]


def number(value, name, low, high):
    if type(value) not in (int, float) or not low <= value <= high or not math.isfinite(value):
        raise ValueError(f"Invalid {name}")
    return float(value)


def horizon(rgb, metadata, source_height):
    height = len(rgb)
    pixels = metadata.get("pixels", {})
    camera = metadata.get("camera", {})
    if not isinstance(pixels, dict) or not isinstance(camera, dict):
        raise ValueError("Invalid camera/pixels metadata")
    row = metadata.get("horizon_row_from_top", pixels.get("horizon_row_from_top"))
    if row is not None:
        return number(row, "horizon row", -source_height*T["horizon_limit_heights"],
                      source_height*T["horizon_limit_heights"]) * height/source_height, "sidecar"
    pitch = camera.get("pitch", metadata.get("pitch_degrees"))
    if pitch is not None:
        pitch = number(pitch, "pitch", -T["pitch_limit"], T["pitch_limit"])
        fov = number(camera.get("fov", metadata.get("fov_degrees", T["default_fov"])), "fov",
                     T["fov_min"], T["fov_max"])
        source = "pitch" if "fov" in camera or "fov_degrees" in metadata else "pitch_default_fov"
        return height * (1 + math.tan(math.radians(pitch))/math.tan(math.radians(fov/2))) / 2, source
    profile = np.median(rgb, axis=1)
    edges = np.max(np.abs(np.diff(profile, axis=0)), axis=1)
    start, end = int(height*T["horizon_search_start"]), int(height*T["horizon_search_end"])
    if end <= start or np.max(edges[start:end], initial=0) < T["horizon_edge"]:
        return None, "unresolved"
    return float(start + np.argmax(edges[start:end]) + 1), "estimated"


def sky_check(rgb, row):
    height, width = rgb.shape[:2]
    if row is None or row <= 0 or row >= height:
        return []
    margin = max(1, math.ceil(height*T["horizon_margin"]))
    end = min(height, math.ceil(row) + margin)
    reference_end = max(1, int(row) - margin)
    # A fixed palette of row medians bounds color matching to 16 grid passes.
    reference = np.median(rgb[:reference_end], axis=1)
    palette = reference[np.linspace(0, len(reference)-1, T["sky_palette_size"], dtype=int)]
    matches = np.zeros((height-end, width), dtype=bool)
    below = rgb[end:]
    for color in palette:
        matches |= np.max(np.abs(below-color), axis=2) <= T["sky_color_delta"]
    red, green, blue = below.transpose(2, 0, 1)
    sky_color = (blue > red + T["sky_blue_delta"]) & (blue > green)
    dark_void = np.max(below, axis=2) <= T["sky_dark"]
    mask = ((matches & sky_color) | dark_void) & flat_mask(below, T["sky_smooth_delta"])
    fraction = float(np.count_nonzero(mask)) / (height*width)
    if fraction < T["sky_fraction"]:
        return []
    return [dict(check="sky_below_horizon", fraction=fraction)]


def banded(line):
    values = np.rint(line).astype(np.int16)
    edges = np.r_[0, np.flatnonzero(np.diff(values)) + 1, len(values)]
    lengths = np.diff(edges)
    levels = values[edges[:-1]]
    steps = np.diff(levels)
    run, span, previous = 0, 0, None
    for index, step in enumerate(steps):
        valid = (min(lengths[index:index+2]) >= T["band_plateau"] and
                 T["band_step_min"] <= abs(step) <= T["band_step_max"])
        if not valid:
            run, span, previous = 0, 0, None
            continue
        if previous is None or abs(int(step)-previous) > T["band_step_tolerance"]:
            run, span = 0, int(lengths[index])
        run += 1
        span += int(lengths[index+1])
        previous = int(step)
        if run >= T["band_steps"] and span >= len(values)*T["band_span"]:
            return True
    return False


def banding(rgb):
    light = luma(rgb)
    for axis, plane in (("horizontal", light), ("vertical", light.T)):
        lines = plane[np.linspace(0, len(plane)-1, min(len(plane), T["band_lines"]), dtype=int)]
        fraction = sum(banded(line) for line in lines) / len(lines)
        if fraction >= T["band_line_fraction"]:
            return [dict(check="gradient_banding", axis=axis, line_fraction=fraction)]
    return []


def signature(rgb):
    image = Image.fromarray(rgb.astype(np.uint8))
    return np.asarray(image.resize((T["signature_width"], T["signature_height"]),
                                   Image.Resampling.NEAREST), dtype=np.int16)


def histogram(rgb):
    return np.array([np.histogram(rgb[..., channel], bins=T["histogram_bins"], range=(0, 256))[0]
                     for channel in range(3)], dtype=float) / (rgb.shape[0]*rgb.shape[1])


def pair_key(name, metadata):
    if "comparison_group" in metadata or "variant" in metadata:
        group, variant = metadata.get("comparison_group"), metadata.get("variant")
        if (not isinstance(group, str) or not group.strip() or len(group) > T["max_label_chars"] or
                not isinstance(variant, str) or not variant.strip() or len(variant) > T["max_label_chars"]):
            raise ValueError("Comparison requires bounded nonempty comparison_group and variant strings")
        return "metadata:" + group, variant
    stem = re.sub(r"(^|__)sweep__png__", r"\1sweep__", Path(name).stem)
    match = re.fullmatch(r"(.*(?:^|__)sweep__.+__)(clear|storm)", stem)
    if match:
        return match[1], match[2]
    match = re.fullmatch(r"(.*)weather-(baseline|visual)", stem)
    if match:
        return match[1] + "weather", match[2]
    return None


def compare_frames(groups):
    findings, errors = [], []
    for group in sorted(groups):
        variants = groups[group]
        if len(variants) < 2:
            continue
        names = sorted(variants)
        for index, first in enumerate(names):
            for second in names[index+1:]:
                left, right = variants[first], variants[second]
                if left[2] != right[2]:
                    errors.append(dict(files=[left[0], right[0]], error="Compared captures have different dimensions"))
                    continue
                for region, a, b in (("frame", left[1], right[1]),
                                     ("ground", left[1][math.ceil(len(left[1])*T["comparison_ground_start"]):],
                                      right[1][math.ceil(len(right[1])*T["comparison_ground_start"]):])):
                    delta = np.abs(a-b)
                    mean = float(delta.mean())
                    unchanged = float(np.all(delta == 0, axis=2).mean())
                    similar = float(np.all(delta <= T["similar_pixel_delta"], axis=2).mean())
                    if mean <= T["similar_mean_delta"] and similar >= T["similar_fraction"]:
                        findings.append(dict(check="near_identical_sweep", files=[left[0], right[0]],
                                             region=region, mean_delta=mean, unchanged_fraction=unchanged,
                                             within_tolerance_fraction=similar))
                        break
    return findings, errors


def analyze(capture_dir, baseline_dir=None, sidecar_dir=None):
    root, entries = directory(capture_dir)
    names = [name for name in entries if name.lower().endswith(".png")]
    if not names or len(names) > T["max_frames"]:
        raise ValueError("Expected 1 to max_frames PNG captures")
    sidecars = directory(sidecar_dir)[0] if sidecar_dir is not None else root
    baseline = directory(baseline_dir)[0] if baseline_dir is not None else None
    frames, groups, errors = [], {}, []
    for name in names:
        try:
            rgb, size, digest = load_png(root / name)
            metadata = load_sidecar(sidecars, name)
            row, source = horizon(rgb, metadata, size[1])
            findings = cluster_checks(rgb) + rectangles(rgb) + sky_check(rgb, row) + banding(rgb)
            for finding in findings:
                if "box" in finding:
                    finding["box"] = [round(value * size[index % 2]/rgb.shape[1-index % 2])
                                      for index, value in enumerate(finding["box"])]
            if baseline:
                reference, reference_size, _ = load_png(baseline / name)
                if reference_size != size:
                    raise ValueError("Baseline dimensions differ")
                drift = float(np.abs(histogram(rgb)-histogram(reference)).sum(axis=1).mean()/2)
                if drift >= T["histogram_drift"]:
                    findings.append(dict(check="histogram_drift", distance=drift))
            key = pair_key(name, metadata)
            if key:
                group, variant = key
                variants = groups.setdefault(group, {})
                value = (name, signature(rgb), size, digest)
                if variant in variants:
                    previous = variants[variant]
                    if previous[2] != size or previous[3] != digest:
                        raise ValueError("Ambiguous comparison variant")
                elif len(variants) >= T["max_variants"]:
                    raise ValueError("Comparison exceeds variant limit")
                else:
                    variants[variant] = value
            frames.append(dict(file=name, sha256=digest, size=list(size),
                               analysis_size=[rgb.shape[1], rgb.shape[0]], horizon_source=source,
                               horizon_row=None if row is None else row*size[1]/len(rgb), findings=findings))
        except (OSError, ValueError, SyntaxError, Image.DecompressionBombError) as error:
            errors.append(dict(file=name, error=str(error)))
    comparisons, comparison_errors = compare_frames(groups)
    errors.extend(comparison_errors)
    return dict(schema="luminumbra.visual_lint.v1", thresholds=T.copy(), frames=frames,
                comparisons=comparisons, errors=errors,
                finding_count=sum(len(frame["findings"]) for frame in frames)+len(comparisons))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir")
    parser.add_argument("--baseline")
    parser.add_argument("--sidecars")
    args = parser.parse_args(argv)
    try:
        report = analyze(args.capture_dir, args.baseline, args.sidecars)
    except (OSError, ValueError, Image.DecompressionBombError) as error:
        report = dict(schema="luminumbra.visual_lint.v1", frames=[], comparisons=[],
                      errors=[dict(error=str(error))], finding_count=0)
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
    counts = Counter(finding["check"] for frame in report["frames"] for finding in frame["findings"])
    counts.update(finding["check"] for finding in report["comparisons"])
    print(f"Visual lint: {len(report['frames'])} captures; {report['finding_count']} candidates; "
          f"{len(report['errors'])} input errors", file=sys.stderr)
    for check, count in sorted(counts.items()):
        print(f"  {check}: {count}", file=sys.stderr)
    for frame in report["frames"]:
        if frame["findings"]:
            print(f"  {json.dumps(frame['file'])}: " + ", ".join(item["check"] for item in frame["findings"]), file=sys.stderr)
    for item in report["comparisons"]:
        labels = " / ".join(json.dumps(name) for name in item["files"])
        print(f"  {labels}: near_identical_sweep ({item['region']})", file=sys.stderr)
    for item in report["errors"]:
        label = json.dumps(item.get("file", item.get("files", "directory")))
        print(f"  INPUT ERROR {label}: {json.dumps(item['error'])}", file=sys.stderr)
    return 2 if report["errors"] else int(report["finding_count"] > 0)


if __name__ == "__main__":
    sys.exit(main())
