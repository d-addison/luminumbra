#!/usr/bin/env python3
"""Timelapse assembler — turn a directory of rendered frames into a video.

A reusable visual-progress tool: the client dumps a frame sequence while time
advances (day/night, plant growth, erosion, ...); this assembles those frames into
an animated GIF (always, via Pillow) and an MP4 (when ffmpeg is on PATH) so changes
over time can be reviewed at a glance.

Frames may be .ppm (the engine's native glReadPixels dump) or .png/.jpg. They are
sorted naturally (frame_0002 before frame_0010) by the trailing integer in the name.
With --manifest PATH, planned order and verified payload bytes replace filename
ordering; incomplete or invalid sequences refuse publication. No manifest is
discovered implicitly.

Usage:
    python tools/timelapse.py <frames_dir> -o out.gif [--fps 12] [--scale 0.5]
                                            [--pattern "*.ppm"] [--mp4 out.mp4] [--manifest PATH]
    python tools/timelapse.py --selftest          # headless self-check (no engine)

Exit code 0 on success, non-zero on failure.
"""

import argparse
from contextlib import ExitStack
import fnmatch
import glob
import hashlib
import json
import math
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile


# Assembly uses lower byte ceilings than capture's transport reservations.
MAX_FRAMES = 4096
MAX_PAYLOADS = 12
MAX_PAYLOAD_BYTES = 128 * 1024 * 1024
MAX_INPUT_BYTES = 512 * 1024 * 1024
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_ENTRIES = 100000
MAX_PIXELS = 16777216
SEQUENCE_SCHEMA = "luminumbra.temporal_audit_sequence"


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def json_pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def json_integer(text):
    require(len(text) <= 20, "JSON integer bound")
    return int(text)


def reject_number(_):
    raise ValueError("non-integer JSON number")


def read_sequence(root, manifest, open_regular):
    """Read only the explicit manifest; bound it before allocating or scanning.

    The manifest may be anywhere. The shared reader verifies its selected
    regular-file descriptor on both POSIX and Windows before reading bytes.
    """
    path = Path(manifest).resolve(strict=True)
    with open_regular(path.parent, path) as stream:
        before = os.fstat(stream.fileno())
        require(before.st_size <= MAX_JSON_BYTES, "manifest byte budget exceeded")
        data = stream.read(before.st_size + 1)
        after = os.fstat(stream.fileno())
        require(len(data) == before.st_size
                and (before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                == (after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                "manifest changed during read")
        document = json.loads(data, object_pairs_hook=json_pairs, parse_int=json_integer,
                              parse_float=reject_number, parse_constant=reject_number)
        require(isinstance(document, dict), "sequence manifest must be a JSON object")
        names = []
        # One flat pass of at most 100,000 entries; unrelated files are not read.
        with os.scandir(root) as entries:
            for index, entry in enumerate(entries):
                require(index < MAX_ENTRIES, "directory entry budget exceeded")
                if (entry.name.startswith("frame-") or Path(entry.name).suffix.lower()
                        in (".ppm", ".png", ".jpg", ".jpeg")):
                    # DirEntry.stat() has zero file identities on Windows. Use
                    # os.stat and compare with the still-open manifest handle;
                    # aliases (including DOS short names) need no name match.
                    status = os.stat(root / entry.name)
                    if (status.st_dev, status.st_ino) == (before.st_dev, before.st_ino):
                        continue
                names.append(entry.name)
    return document, names, path


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def sequence_frames(document, names, pattern):
    """Validate the complete join before reading payloads or making any output."""
    require(set(document) == {"schema", "version", "status", "plan", "plan_sha256", "frames"}
            and document.get("schema") == SEQUENCE_SCHEMA, "unknown sequence metadata")
    require(integer(document.get("version"), 1, 1), "unknown sequence version")
    require(document.get("status") == "complete", "sequence is not complete")
    plan, frames = document.get("plan"), document.get("frames")
    require(isinstance(plan, dict), "missing sequence plan")
    canonical = json.dumps(plan, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False, allow_nan=False).encode("utf-8")
    require(hashlib.sha256(canonical).hexdigest() == document.get("plan_sha256"),
            "plan hash mismatch")
    planned = plan.get("frames")
    require(isinstance(planned, list) and 0 < len(planned) <= MAX_FRAMES,
            "planned frame count budget exceeded or empty")
    require(isinstance(frames, list) and len(frames) == len(planned),
            "missing or extra frame records")
    require(integer(plan.get("start_tick"), 0, (1 << 62) - 1)
            and integer(plan.get("end_tick"), plan["start_tick"], (1 << 62) - 1),
            "invalid plan tick bounds")
    by_index = {}
    for frame in frames:
        require(isinstance(frame, dict) and integer(frame.get("index"), 0, len(planned) - 1),
                "invalid frame index")
        require(frame["index"] not in by_index, "duplicate frame index")
        by_index[frame["index"]] = frame
    ordered, seen, total, previous = [], set(), 0, None
    for index, scheduled in enumerate(planned):
        require(isinstance(scheduled, dict) and type(scheduled.get("index")) is int
                and scheduled["index"] == index, "invalid planned frame index")
        require(set(scheduled) == {"index", "tick", "sub_tick_60", "kind", "control",
                                   "dense", "hold_frame"}, "unknown planned frame metadata")
        require(integer(scheduled["tick"], plan["start_tick"], plan["end_tick"])
                and integer(scheduled["sub_tick_60"], 0, 59)
                and integer(scheduled["kind"], 0, 2)
                and integer(scheduled["control"], 0, 2)
                and type(scheduled["dense"]) is bool
                and integer(scheduled["hold_frame"], 0, 119), "invalid planned frame metadata")
        phase = scheduled["tick"], scheduled["sub_tick_60"]
        require(previous is None or previous <= phase, "out-of-order planned ticks")
        previous = phase
        frame = by_index[index]
        recorded = {k: v for k, v in frame.items() if k != "payloads"}
        require(recorded == scheduled
                and all(type(recorded[k]) is type(v) for k, v in scheduled.items()),
                "frame differs from planned tick order")
        payloads = frame.get("payloads")
        require(isinstance(payloads, list) and 0 < len(payloads) <= MAX_PAYLOADS,
                "missing payloads or payload count budget exceeded")
        images = []
        for payload in payloads:
            require(isinstance(payload, dict) and set(payload) == {"path", "bytes", "sha256"},
                    "invalid payload metadata")
            name = payload["path"]
            require(isinstance(name, str) and len(name) <= 128
                    and re.fullmatch(r"frame-" + str(index) + r"\.[a-z0-9_.-]+", name)
                    and not name.endswith("."), "invalid payload path")
            require(name not in seen, "duplicate payload path")
            seen.add(name)
            require(integer(payload["bytes"], 1, MAX_PAYLOAD_BYTES)
                    and payload["bytes"] <= MAX_INPUT_BYTES - total,
                    "input byte budget exceeded")
            total += payload["bytes"]
            require(isinstance(payload["sha256"], str)
                    and re.fullmatch(r"[0-9a-f]{64}", payload["sha256"]), "invalid payload hash")
            if fnmatch.fnmatchcase(name, pattern):
                images.append(name)
        require(len(images) == 1, "pattern must select exactly one image per planned frame")
        ordered.append((payloads, images[0]))
    # Include sidecars and all image extensions, even extras hidden by --pattern.
    actual = {name for name in names if name.startswith("frame-")
              or Path(name).suffix.lower() in (".ppm", ".png", ".jpg", ".jpeg")}
    require(seen <= set(names), "missing sequence payload")
    require(actual <= seen, "extra sequence frame or payload")
    return ordered


def stage_sequence(root, ordered, staging, open_regular):
    """Hash and copy the same descriptor in 64 KiB blocks; never reopen input."""
    selected, identities = [], set()
    for payloads, image in ordered:
        for payload in payloads:
            name = payload["path"]
            with open_regular(root, root / name) as source:
                before = os.fstat(source.fileno())
                identity = before.st_dev, before.st_ino
                require(identity not in identities, "duplicate payload file identity")
                identities.add(identity)
                require(before.st_size == payload["bytes"], "payload byte size mismatch")
                digest, remaining = hashlib.sha256(), payload["bytes"]
                with ExitStack() as stack:
                    target = (stack.enter_context(open(staging / name, "xb"))
                              if name == image else None)
                    while remaining:
                        block = source.read(min(65536, remaining))
                        require(bool(block), "partial payload read")
                        remaining -= len(block)
                        digest.update(block)
                        if target is not None:
                            require(target.write(block) == len(block), "partial staging write")
                    require(not source.read(1), "payload grew during read")
                after = os.fstat(source.fileno())
                require((before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                        == (after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                        "payload changed during read")
                require(digest.hexdigest() == payload["sha256"], "payload hash mismatch")
            if name == image:
                selected.append(str(staging / name))
    return selected


def load_sequence_images(files, scale):
    from PIL import Image

    images, total = [], 0
    try:
        for filename in files:
            with Image.open(filename) as source:
                width, height = source.size
                require(0 < width <= 16384 and 0 < height <= 16384
                        and width * height <= MAX_PIXELS, "image dimension budget exceeded")
                scaled = max(1, int(width * scale)), max(1, int(height * scale))
                require(max(scaled) <= 16384 and scaled[0] * scaled[1] <= MAX_PIXELS,
                        "scaled image dimension budget exceeded")
                # Count both the conversion and resized RGB buffers before either
                # allocation. Retained images together use at most 512 MiB.
                cost = 3 * (width * height + scaled[0] * scaled[1])
                require(cost <= MAX_INPUT_BYTES - total, "decoded image byte budget exceeded")
                total += cost
                img = source.convert("RGB")
            if scale != 1.0:
                try:
                    resized = img.resize(scaled, Image.BILINEAR)
                finally:
                    img.close()
                img = resized
            images.append(img)
        return images
    except BaseException:
        for img in images:
            img.close()
        raise


def build_sequence(root, sequence, out_gif, fps, scale, pattern, mp4, open_regular):
    require(math.isfinite(fps) and fps >= 1000 / 655350
            and math.isfinite(scale) and 0 < scale <= 16384,
            "fps and scale must be finite and positive within assembly bounds")
    ordered = sequence_frames(*sequence[:2], pattern)
    ffmpeg = shutil.which("ffmpeg") if mp4 else None
    require(not mp4 or ffmpeg is not None, "ffmpeg not found on PATH; sequence output refused")
    with ExitStack() as stack:
        staging = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="timelapse_")))
        files = stage_sequence(root, ordered, staging, open_regular)
        images = load_sequence_images(files, scale)
        for img in images:
            stack.callback(img.close)
        # Close encoded files before publication (also required on Windows).
        destinations = [Path(out_gif)] + ([Path(mp4)] if mp4 else [])
        # Only capture inputs are protected; prior GIF/MP4 results may be replaced.
        # Leaf Path objects retain Windows case-insensitive comparisons without
        # copying the root prefix for up to 4096 * 12 payload paths.
        protected = {Path(payload["path"]) for payloads, _ in ordered for payload in payloads}
        require(not any(path == sequence[2]
                        or (path.parent == root and Path(path.name) in protected)
                        for path in (destination.resolve() for destination in destinations)),
                "output destination aliases sequence input")
        require(len({p.resolve() for p in destinations}) == len(destinations),
                "output destinations alias each other")
        outputs = []
        for destination in destinations:
            directory = Path(stack.enter_context(tempfile.TemporaryDirectory(
                prefix=".timelapse_", dir=destination.absolute().parent)))
            outputs.append(directory / destination.name)
        write_gif(images, str(outputs[0]), fps)
        if mp4:
            for index, img in enumerate(images):
                img.save(staging / f"f_{index:05d}.png")
            command = [ffmpeg, "-y", "-framerate", str(fps),
                       "-i", str(staging / "f_%05d.png"), "-c:v", "libx264",
                       "-pix_fmt", "yuv420p", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2",
                       str(outputs[1])]
            try:
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            except (OSError, subprocess.CalledProcessError) as error:
                raise ValueError(f"ffmpeg failed; sequence output refused: {error}") from error
            require(outputs[1].is_file() and outputs[1].stat().st_size > 0,
                    "ffmpeg produced no video; sequence output refused")
        for output, destination in zip(outputs, destinations):
            os.replace(output, destination)
        print(f"timelapse: wrote {out_gif} ({len(images)} frames @ {fps} fps)")
        if mp4:
            print(f"timelapse: wrote {mp4}")
        return len(images)


def natural_key(path):
    """Sort key: the LAST integer run in the filename (frame_0010 > frame_2)."""
    nums = re.findall(r"\d+", os.path.basename(path))
    return (int(nums[-1]) if nums else -1, path)


def collect_frames(frames_dir, pattern):
    files = sorted(glob.glob(os.path.join(frames_dir, pattern)), key=natural_key)
    if not files:
        raise SystemExit(f"timelapse: no frames matching {pattern!r} in {frames_dir}")
    return files


def load_frames(files, scale):
    from PIL import Image  # Pillow — same dep the PPM->PNG gate flow uses

    images = []
    for f in files:
        with Image.open(f) as source:
            img = source.convert("RGB")
        if scale != 1.0:
            w, h = img.size
            img = img.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.BILINEAR)
        images.append(img)
    return images


def write_gif(images, out_path, fps):
    duration_ms = max(1, int(round(1000.0 / fps)))
    images[0].save(
        out_path,
        save_all=True,
        append_images=images[1:],
        duration=duration_ms,
        loop=0,
        optimize=True,
        disposal=2,
    )
    return out_path


def write_mp4(images, out_path, fps):
    """Encode an MP4 via ffmpeg if available; returns the path or None if skipped."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("timelapse: ffmpeg not found on PATH — skipping MP4 (GIF still written)")
        return None
    tmp = tempfile.mkdtemp(prefix="timelapse_")
    try:
        for i, img in enumerate(images):
            img.save(os.path.join(tmp, f"f_{i:05d}.png"))
        cmd = [
            ffmpeg, "-y", "-framerate", str(fps),
            "-i", os.path.join(tmp, "f_%05d.png"),
            "-c:v", "libx264", "-pix_fmt", "yuv420p",
            "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2",  # x264 needs even dimensions
            out_path,
        ]
        subprocess.run(cmd, check=True, capture_output=True)
        return out_path
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def build(frames_dir, out_gif, fps, scale, pattern, mp4, *, manifest=None):
    # Keep the original no-manifest pipeline, including encoder fallback, intact.
    if manifest is not None:
        try:
            root = Path(frames_dir).resolve()
            open_regular = runpy.run_path(str(Path(__file__).resolve().parent / "perf" /
                                              "fixture_paths.py"))["open_regular"]
            sequence = read_sequence(root, manifest, open_regular)
            return build_sequence(root, sequence, out_gif, fps, scale, pattern, mp4, open_regular)
        except (OSError, ValueError, RuntimeError) as error:
            raise SystemExit(f"timelapse: manifest {manifest!s}: {error}") from error
    files = collect_frames(frames_dir, pattern)
    images = load_frames(files, scale)
    write_gif(images, out_gif, fps)
    print(f"timelapse: wrote {out_gif} ({len(images)} frames @ {fps} fps)")
    if mp4:
        if write_mp4(images, mp4, fps):
            print(f"timelapse: wrote {mp4}")
    return len(images)


def selftest():
    """Generate synthetic frames and assemble them — proves the tool works headlessly."""
    from PIL import Image

    tmp = tempfile.mkdtemp(prefix="timelapse_selftest_")
    try:
        n = 8
        for i in range(n):
            img = Image.new("RGB", (64, 48), (10 * i, 20, 200 - 10 * i))
            # .ppm to exercise the engine's native format + the natural sort
            img.save(os.path.join(tmp, f"frame_{i:04d}.ppm"))
        out_gif = os.path.join(tmp, "out.gif")
        count = build(tmp, out_gif, fps=12, scale=1.0, pattern="*.ppm", mp4=None)
        assert count == n, f"expected {n} frames, assembled {count}"
        assert os.path.getsize(out_gif) > 0, "gif is empty"
        from PIL import ImageSequence
        with Image.open(out_gif) as source:
            frame_count = sum(1 for _ in ImageSequence.Iterator(source))
        assert frame_count == n, f"gif has {frame_count} frames, expected {n}"
        print(f"timelapse selftest OK: {n} synthetic frames -> {out_gif}")
        return 0
    finally:
        shutil.rmtree(tmp)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Assemble a frame sequence into a timelapse video.")
    ap.add_argument("frames_dir", nargs="?", help="directory of frame images")
    ap.add_argument("-o", "--out", default="timelapse.gif", help="output GIF path")
    ap.add_argument("--fps", type=float, default=12.0, help="playback frames per second")
    ap.add_argument("--scale", type=float, default=1.0, help="downscale factor (e.g. 0.5)")
    ap.add_argument("--pattern", default="*.ppm", help="frame glob (default *.ppm)")
    ap.add_argument("--mp4", default=None, help="also write an MP4 here (needs ffmpeg)")
    ap.add_argument("--manifest", default=None,
                    help="explicit sequence manifest path (strict verified assembly)")
    ap.add_argument("--selftest", action="store_true", help="run the headless self-check")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.frames_dir:
        ap.error("frames_dir is required (or use --selftest)")
    build(args.frames_dir, args.out, args.fps, args.scale, args.pattern, args.mp4,
          manifest=args.manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
