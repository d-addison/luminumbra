#!/usr/bin/env python3
"""Timelapse assembler — turn a directory of rendered frames into a video.

A reusable visual-progress tool: the client dumps a frame sequence while time
advances (day/night, plant growth, erosion, ...); this assembles those frames into
an animated GIF (always, via Pillow) and an MP4 (when ffmpeg is on PATH) so changes
over time can be reviewed at a glance.

Frames may be .ppm (the engine's native glReadPixels dump) or .png/.jpg. They are
sorted naturally (frame_0002 before frame_0010) by the trailing integer in the name.

Usage:
    python tools/timelapse.py <frames_dir> -o out.gif [--fps 12] [--scale 0.5]
                                            [--pattern "*.ppm"] [--mp4 out.mp4]
    python tools/timelapse.py --selftest          # headless self-check (no engine)

Exit code 0 on success, non-zero on failure.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile


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
        img = Image.open(f).convert("RGB")
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


def build(frames_dir, out_gif, fps, scale, pattern, mp4):
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
        frames = list(ImageSequence.Iterator(Image.open(out_gif)))
        assert len(frames) == n, f"gif has {len(frames)} frames, expected {n}"
        print(f"timelapse selftest OK: {n} synthetic frames -> {out_gif}")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Assemble a frame sequence into a timelapse video.")
    ap.add_argument("frames_dir", nargs="?", help="directory of frame images")
    ap.add_argument("-o", "--out", default="timelapse.gif", help="output GIF path")
    ap.add_argument("--fps", type=float, default=12.0, help="playback frames per second")
    ap.add_argument("--scale", type=float, default=1.0, help="downscale factor (e.g. 0.5)")
    ap.add_argument("--pattern", default="*.ppm", help="frame glob (default *.ppm)")
    ap.add_argument("--mp4", default=None, help="also write an MP4 here (needs ffmpeg)")
    ap.add_argument("--selftest", action="store_true", help="run the headless self-check")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.frames_dir:
        ap.error("frames_dir is required (or use --selftest)")
    build(args.frames_dir, args.out, args.fps, args.scale, args.pattern, args.mp4)
    return 0


if __name__ == "__main__":
    sys.exit(main())
