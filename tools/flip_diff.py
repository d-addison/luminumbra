#!/usr/bin/env python3
"""FLIP-style perceptual golden-image visual-regression diff for luminumbra.

Compares a CANDIDATE image against a GOLDEN reference and emits:
  (a) a scalar perceptual error score in [0,1] (0 == identical),
  (b) a per-pixel error HEATMAP PNG (localizes WHICH region regressed),
  (c) a pass/fail verdict against a threshold (non-zero exit on FAIL) for CI.

This is the parity harness the GL->Vulkan migration uses: render the SAME
deterministic frame (--timelapse / --frame-scan / --ui-screenshot) on the GL
backend, bless it as the golden, then render it on Vulkan and flip_diff the
candidate against the golden. The heatmap localizes the regressed render pass
(e.g. a wrong shadow term lights up the shadowed geometry, a tone-map drift
lights up the whole frame uniformly) so you fix the pass, not guess.

It is ALSO a general visual-regression gate for any render-only change.

Backend tiering (auto-detect + graceful degrade, never hard-fail on a missing
optional dependency):
  1. NVIDIA FLIP        -- pip `flip-evaluator` (module `flip_evaluator`) or a
                           reference `flip` impl. The real perceptual metric.
  2. SSIM               -- scikit-image structural similarity (error = 1-SSIM),
                           a solid perceptual approximation.
  3. luma+gradient      -- numpy-only fallback: blended absolute luminance error
                           and Sobel-gradient (edge/structure) error. No SciPy.
  4. pure-stdlib        -- last resort if numpy is also absent: mean abs RGB
                           error. Always available; keeps the gate runnable
                           anywhere Python runs.

`--backend` forces a specific tier (flip|ssim|luma|stdlib|auto); default auto
picks the best available. The chosen backend is printed + recorded in the JSON
so a regression is never silently judged by a weaker metric than you think.

Image I/O: PNG and binary P6 PPM are read with a pure-stdlib decoder (the same
formats the engine + ppm_to_png.py emit), so capturing -> diffing needs no PIL.
PIL is used if present (handles more formats) but is never required.

Usage:
  python tools/flip_diff.py <candidate> <golden> [--out heatmap.png]
                            [--threshold 0.05] [--backend auto] [--json r.json]
                            [--update]          # bless candidate as the golden
  python tools/flip_diff.py --selftest          # self-diff == 0 across backends

Exit codes: 0 = PASS (or selftest ok / update ok), 1 = FAIL (over threshold),
2 = usage / I/O error.
"""
import sys
import os
import struct
import zlib
import json
import argparse

# numpy is the workhorse for tiers 1-3; tier 4 (stdlib) runs without it.
try:
    import numpy as _np
except Exception:  # pragma: no cover - exercised only on numpy-less hosts
    _np = None

# PIL is optional: a convenience for exotic formats, never load-bearing.
try:
    from PIL import Image as _PILImage
except Exception:
    _PILImage = None


# ---------------------------------------------------------------------------
# Image I/O  (pure-stdlib PNG + P6 PPM decode; PIL used opportunistically)
# ---------------------------------------------------------------------------
def _read_ppm(path):
    """Decode a binary P6 PPM -> (w, h, bytes-RGB). Mirrors ppm_to_png.py."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ValueError(f"{path}: not a binary P6 PPM")
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx:idx + 1] not in (b"\n", b"\r"):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    width, height, maxval = fields
    idx += 1
    if maxval != 255:
        raise ValueError(f"{path}: only maxval 255 supported (got {maxval})")
    expected = width * height * 3
    raster = data[idx:idx + expected]
    if len(raster) != expected:
        raise ValueError(f"{path}: short raster {len(raster)} != {expected}")
    return width, height, raster


def _read_png_stdlib(path):
    """Minimal PNG decoder (8-bit, color types 0/2/4/6, no interlace) -> (w,h,RGB bytes).

    Pure stdlib (zlib only). Enough for engine/UI captures; for anything fancier
    PIL is used when available. Raises on unsupported variants so the caller can
    surface a clear error rather than mis-decode.
    """
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # 4 len + 4 tag + payload + 4 crc
        if tag == b"IHDR":
            (width, height, bit_depth, color_type,
             comp, filt, interlace) = struct.unpack(">IIBBBBB", payload)
            if comp != 0 or filt != 0 or interlace != 0:
                raise ValueError(f"{path}: unsupported PNG (interlace/compress)")
            if bit_depth != 8:
                raise ValueError(f"{path}: only 8-bit PNG supported (got {bit_depth})")
            if color_type not in (0, 2, 4, 6):
                raise ValueError(f"{path}: unsupported PNG color type {color_type}")
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    if width is None:
        raise ValueError(f"{path}: missing IHDR")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 3)
    prev = bytearray(stride)
    rpos = 0
    for y in range(height):
        ftype = raw[rpos]; rpos += 1
        line = bytearray(raw[rpos:rpos + stride]); rpos += stride
        # Undo PNG row filter.
        if ftype == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ftype != 0:
            raise ValueError(f"{path}: bad PNG filter {ftype}")
        # Expand to RGB.
        op = y * width * 3
        if channels == 1:       # gray
            for x in range(width):
                v = line[x]; o = op + x * 3
                out[o] = out[o + 1] = out[o + 2] = v
        elif channels == 2:     # gray+alpha
            for x in range(width):
                v = line[x * 2]; o = op + x * 3
                out[o] = out[o + 1] = out[o + 2] = v
        elif channels == 3:     # rgb
            out[op:op + stride] = line
        else:                   # rgba -> drop alpha
            for x in range(width):
                s = x * 4; o = op + x * 3
                out[o] = line[s]; out[o + 1] = line[s + 1]; out[o + 2] = line[s + 2]
        prev = line
    return width, height, bytes(out)


def load_rgb(path):
    """Load an image -> (w, h, flat RGB bytes). PPM/PNG via stdlib; PIL otherwise."""
    if not os.path.isfile(path):
        raise FileNotFoundError(f"image not found: {path}")
    ext = os.path.splitext(path)[1].lower()
    if ext in (".ppm",):
        return _read_ppm(path)
    if ext in (".png",):
        try:
            return _read_png_stdlib(path)
        except Exception:
            if _PILImage is None:
                raise
    # Fallback / non-PNG-PPM formats: require PIL.
    if _PILImage is None:
        raise ValueError(f"{path}: cannot decode {ext} without PIL installed")
    im = _PILImage.open(path).convert("RGB")
    w, h = im.size
    return w, h, im.tobytes()


def _to_array(path):
    """Load -> float32 HxWx3 array in [0,1]. Requires numpy."""
    w, h, rgb = load_rgb(path)
    arr = _np.frombuffer(rgb, dtype=_np.uint8).astype(_np.float32) / 255.0
    return arr.reshape(h, w, 3)


def _check_same_size(a_shape, b_shape, ca, cb):
    if a_shape[:2] != b_shape[:2]:
        raise ValueError(
            f"image size mismatch: candidate {ca} is {a_shape[1]}x{a_shape[0]}, "
            f"golden {cb} is {b_shape[1]}x{b_shape[0]} -- "
            "capture both at the same resolution (see docs/visual-regression.md)")


# ---------------------------------------------------------------------------
# Heatmap writer  (pure-stdlib PNG; turbo-ish colormap)
# ---------------------------------------------------------------------------
def _colormap(t):
    """Map error t in [0,1] -> (r,g,b) 0..255 on a blue->cyan->green->yellow->red ramp."""
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    # Piecewise linear over 4 segments.
    stops = [(0.0, (0, 0, 128)),      # deep blue  = no error
             (0.25, (0, 192, 255)),   # cyan
             (0.5, (0, 255, 0)),      # green
             (0.75, (255, 255, 0)),   # yellow
             (1.0, (255, 0, 0))]      # red        = max error
    for i in range(len(stops) - 1):
        t0, c0 = stops[i]
        t1, c1 = stops[i + 1]
        if t <= t1:
            f = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
            return tuple(int(c0[k] + f * (c1[k] - c0[k]) + 0.5) for k in range(3))
    return stops[-1][1]


def _write_png_rgb(path, width, height, rgb_bytes):
    """Write 8-bit RGB PNG (stdlib). Shares the encoder shape with ppm_to_png.py."""
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgb_bytes[y * stride:(y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def write_heatmap(path, err_map, normalize=True):
    """err_map: HxW float in [0,1] -> colormapped PNG.

    normalize: rescale the map to its OWN max before colormapping, so a small
    but localized regression is still visible (the regressed region reads hot
    even when the absolute error is low). The scalar score is always the
    absolute (un-normalized) mean — only the heatmap visualization is rescaled.
    Returns the scale factor used (1.0 if not normalized)."""
    h, w = err_map.shape
    vis = err_map
    scale = 1.0
    if normalize:
        m = float(err_map.max())
        if m > 1e-9:
            scale = 1.0 / m
            vis = err_map * scale
    flat = vis.reshape(-1)
    out = bytearray(w * h * 3)
    # Vectorize the colormap via a 256-entry LUT for speed.
    lut = _np.array([_colormap(i / 255.0) for i in range(256)], dtype=_np.uint8)
    idx = _np.clip((flat * 255.0 + 0.5).astype(_np.int32), 0, 255)
    rgb = lut[idx].reshape(-1)
    out[:] = bytes(rgb.tobytes())
    _write_png_rgb(path, w, h, bytes(out))
    return scale


def _write_heatmap_stdlib(path, err_rows, w, h):
    """Stdlib-only heatmap: err_rows is a flat list of floats in [0,1]."""
    out = bytearray(w * h * 3)
    for i, e in enumerate(err_rows):
        r, g, b = _colormap(e)
        o = i * 3
        out[o] = r; out[o + 1] = g; out[o + 2] = b
    _write_png_rgb(path, w, h, bytes(out))


# ---------------------------------------------------------------------------
# Backends  -> each returns (scalar_error in [0,1], err_map HxW in [0,1])
# ---------------------------------------------------------------------------
def _luma(arr):
    """Rec.709 luminance of an HxWx3 [0,1] array."""
    return arr[..., 0] * 0.2126 + arr[..., 1] * 0.7152 + arr[..., 2] * 0.0722


def backend_flip(cand, gold):
    """NVIDIA FLIP via `flip_evaluator` (pip flip-evaluator) or reference `flip`.

    Returns (mean_flip, flip_error_map). FLIP's map is already perceptual + in
    [0,1], so we use it directly for both the scalar and the heatmap.
    """
    mod = None
    try:
        import flip_evaluator as mod  # pip install flip-evaluator
    except Exception:
        try:
            import flip as mod         # reference impl exposing evaluate/compute_ldr
        except Exception:
            raise RuntimeError("FLIP backend unavailable")
    a = _to_array(cand)
    b = _to_array(gold)
    _check_same_size(a.shape, b.shape, cand, gold)
    err_map = None
    mean = None
    evaluate = getattr(mod, "evaluate", None)
    if evaluate is not None:
        # flip-evaluator API: evaluate(reference, test, dynamicRangeString,
        #   inputsRGB=True, applyMagma=True, computeMeanError=True, ...)
        #   -> (error_map, mean_error, parameters). applyMagma=True returns a
        # 3-channel magma VISUALIZATION; we want the raw scalar map, so disable
        # it. We render our OWN colormap (consistent with the stdlib path).
        try:
            res = evaluate(b, a, "LDR", applyMagma=False)
        except TypeError:
            res = evaluate(b, a, "LDR")
        emap = _np.asarray(res[0], dtype=_np.float32)
        emap = _np.squeeze(emap)    # HxWx1 -> HxW
        if emap.ndim == 3:          # magma RGB visualization -> reduce to luma
            emap = _luma(emap)
        err_map = emap
        if len(res) > 1 and _np.isscalar(res[1]):
            mean = float(res[1])
    else:
        # reference `flip` impl variants.
        for fn_name in ("compute_ldr", "ldrflip"):
            fn = getattr(mod, fn_name, None)
            if fn is None:
                continue
            res = fn(b, a)
            arr = _np.asarray(res)
            if arr.ndim >= 2:
                err_map = arr if arr.ndim == 2 else _luma(arr)
                break
    if err_map is None:
        raise RuntimeError("FLIP returned no usable error map")
    err_map = _np.clip(err_map.astype(_np.float32), 0.0, 1.0)
    if mean is None:
        mean = float(err_map.mean())
    return mean, err_map


def backend_ssim(cand, gold):
    """scikit-image SSIM. error = 1 - SSIM (mean); heatmap = 1 - local SSIM map."""
    from skimage.metrics import structural_similarity as ssim
    a = _to_array(cand)
    b = _to_array(gold)
    _check_same_size(a.shape, b.shape, cand, gold)
    score, smap = ssim(b, a, channel_axis=2, full=True, data_range=1.0)
    err_map = _np.clip(1.0 - smap.mean(axis=2), 0.0, 1.0)
    return float(1.0 - score), err_map


def backend_luma(cand, gold):
    """numpy-only perceptual approx: blend of absolute luma error and Sobel-gradient
    (structure/edge) error. No SciPy. Catches both tone/brightness drift (luma term)
    and geometry/edge shifts (gradient term)."""
    a = _to_array(cand)
    b = _to_array(gold)
    _check_same_size(a.shape, b.shape, cand, gold)
    la, lb = _luma(a), _luma(b)
    luma_err = _np.abs(la - lb)  # [0,1]

    def sobel(img):
        # manual 3x3 Sobel via shifted slices (avoids scipy).
        gx = _np.zeros_like(img)
        gy = _np.zeros_like(img)
        p = _np.pad(img, 1, mode="edge")
        gx = (p[1:-1, 2:] - p[1:-1, :-2])
        gy = (p[2:, 1:-1] - p[:-2, 1:-1])
        return _np.sqrt(gx * gx + gy * gy)

    grad_err = _np.abs(sobel(la) - sobel(lb))
    grad_err = _np.clip(grad_err, 0.0, 1.0)
    # Also fold per-channel color error so a hue-only shift (same luma) is caught.
    color_err = _np.abs(a - b).mean(axis=2)
    err_map = _np.clip(0.5 * luma_err + 0.3 * grad_err + 0.2 * color_err, 0.0, 1.0)
    return float(err_map.mean()), err_map


def backend_stdlib(cand, gold, out_heatmap=None):
    """Last-resort pure-stdlib metric: mean absolute RGB error in [0,1].

    Used only when numpy is unavailable. Writes its own heatmap if requested
    (it cannot share the numpy heatmap path)."""
    wa, ha, ra = load_rgb(cand)
    wb, hb, rb = load_rgb(gold)
    if (wa, ha) != (wb, hb):
        raise ValueError(
            f"image size mismatch: candidate {wa}x{ha} vs golden {wb}x{hb}")
    n = len(ra)
    total = 0
    err_rows = [0.0] * (wa * ha)
    for px in range(wa * ha):
        o = px * 3
        d = (abs(ra[o] - rb[o]) + abs(ra[o + 1] - rb[o + 1])
             + abs(ra[o + 2] - rb[o + 2]))
        e = d / (3 * 255.0)
        err_rows[px] = e
        total += d
    mean = total / (n * 255.0)
    if out_heatmap:
        _write_heatmap_stdlib(out_heatmap, err_rows, wa, ha)
    return float(mean), err_rows, (wa, ha)


# ---------------------------------------------------------------------------
# Backend selection
# ---------------------------------------------------------------------------
def _available_backends():
    avail = []
    try:
        import flip_evaluator  # noqa: F401
        avail.append("flip")
    except Exception:
        try:
            import flip  # noqa: F401
            avail.append("flip")
        except Exception:
            pass
    try:
        import skimage.metrics  # noqa: F401
        if _np is not None:
            avail.append("ssim")
    except Exception:
        pass
    if _np is not None:
        avail.append("luma")
    avail.append("stdlib")
    return avail


def pick_backend(requested):
    avail = _available_backends()
    if requested and requested != "auto":
        if requested not in avail:
            raise RuntimeError(
                f"backend '{requested}' unavailable; have: {', '.join(avail)}")
        return requested
    # auto: best first
    for tier in ("flip", "ssim", "luma", "stdlib"):
        if tier in avail:
            return tier
    return "stdlib"


# ---------------------------------------------------------------------------
# Top-level compare
# ---------------------------------------------------------------------------
def compare(candidate, golden, out_heatmap=None, backend="auto",
            heatmap_normalize=True):
    """Return a result dict. Writes the heatmap PNG if out_heatmap is given."""
    chosen = pick_backend(backend)
    if chosen == "stdlib":
        mean, err_rows, (w, h) = backend_stdlib(candidate, golden, out_heatmap)
        return {"backend": chosen, "score": mean, "width": w, "height": h,
                "heatmap": out_heatmap}
    fn = {"flip": backend_flip, "ssim": backend_ssim, "luma": backend_luma}[chosen]
    score, err_map = fn(candidate, golden)
    h, w = err_map.shape
    scale = 1.0
    if out_heatmap:
        scale = write_heatmap(out_heatmap, err_map, normalize=heatmap_normalize)
    return {"backend": chosen, "score": float(score), "width": int(w),
            "height": int(h), "heatmap": out_heatmap,
            "heatmap_normalized": bool(out_heatmap and heatmap_normalize
                                       and scale != 1.0),
            "max_error": float(err_map.max()), "p99_error": float(
                _np.percentile(err_map, 99))}


# ---------------------------------------------------------------------------
# Golden update (bless)
# ---------------------------------------------------------------------------
def bless_golden(candidate, golden):
    """Copy candidate -> golden (creating dirs). Returns the golden path."""
    import shutil
    d = os.path.dirname(os.path.abspath(golden))
    if d and not os.path.isdir(d):
        os.makedirs(d, exist_ok=True)
    shutil.copyfile(candidate, golden)
    return golden


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
def selftest():
    """Diff a synthesized image against ITSELF on every available backend ==> 0.

    Also checks the heatmap encodes/decodes and that a deliberately perturbed
    copy scores > 0. No external fixtures; writes to a temp dir."""
    import tempfile
    ok = True
    avail = _available_backends()
    print(f"[selftest] available backends: {', '.join(avail)}")
    tmp = tempfile.mkdtemp(prefix="flip_selftest_")
    w, h = 48, 32

    if _np is not None:
        # Synthesize a gradient + checker so SSIM/gradient terms are non-degenerate.
        yy, xx = _np.mgrid[0:h, 0:w]
        base = ((xx * 5 + yy * 7) % 256).astype(_np.uint8)
        chk = (((xx // 4 + yy // 4) % 2) * 80).astype(_np.uint8)
        img = _np.stack([(base + chk) & 0xFF,
                         (base * 2) & 0xFF,
                         (chk + 40) & 0xFF], axis=2).astype(_np.uint8)
        ref = os.path.join(tmp, "ref.png")
        _write_png_rgb(ref, w, h, img.reshape(-1).tobytes())
        # Perturbed copy: shift one channel a little in a patch.
        pert = img.copy()
        pert[8:20, 8:24, 0] = _np.clip(
            pert[8:20, 8:24, 0].astype(_np.int32) + 60, 0, 255).astype(_np.uint8)
        cand = os.path.join(tmp, "cand.png")
        _write_png_rgb(cand, w, h, pert.reshape(-1).tobytes())
    else:
        # stdlib-only: build a tiny PPM by hand.
        ref = os.path.join(tmp, "ref.ppm")
        cand = os.path.join(tmp, "cand.ppm")
        body = bytearray()
        for i in range(w * h):
            body += bytes(((i * 5) % 256, (i * 3) % 256, (i * 7) % 256))
        hdr = f"P6\n{w} {h}\n255\n".encode()
        with open(ref, "wb") as f:
            f.write(hdr + body)
        pert = bytearray(body)
        for px in range(100, 200):
            pert[px * 3] = (pert[px * 3] + 60) % 256
        with open(cand, "wb") as f:
            f.write(hdr + bytes(pert))

    for b in avail:
        # self vs self must be 0 (or within fp epsilon).
        r0 = compare(ref, ref, out_heatmap=os.path.join(tmp, f"hm_{b}.png"),
                     backend=b)
        self_score = r0["score"]
        # self vs perturbed must be > 0.
        r1 = compare(cand, ref, backend=b)
        diff_score = r1["score"]
        zero_ok = self_score <= 1e-6
        pos_ok = diff_score > self_score
        status = "PASS" if (zero_ok and pos_ok) else "FAIL"
        if not (zero_ok and pos_ok):
            ok = False
        print(f"[selftest] backend={b:7s} self={self_score:.8f} "
              f"perturbed={diff_score:.6f}  {status}")
    # Heatmap round-trips (decode what we wrote, for numpy backends).
    if _np is not None:
        try:
            hm = os.path.join(tmp, f"hm_{avail[0]}.png")
            if os.path.isfile(hm):
                _read_png_stdlib(hm)
                print(f"[selftest] heatmap decode OK: {hm}")
        except Exception as e:
            print(f"[selftest] heatmap decode FAILED: {e}")
            ok = False
    print(f"[selftest] {'ALL PASS' if ok else 'FAILURES PRESENT'}")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv):
    p = argparse.ArgumentParser(
        prog="flip_diff.py",
        description="FLIP-style perceptual golden-image visual-regression diff.")
    p.add_argument("candidate", nargs="?", help="candidate image (PNG/PPM)")
    p.add_argument("golden", nargs="?", help="golden reference image (PNG/PPM)")
    p.add_argument("--out", help="write per-pixel error heatmap PNG here")
    p.add_argument("--raw-heatmap", action="store_true",
                   help="do NOT normalize the heatmap to its own max "
                        "(use the absolute [0,1] error scale)")
    p.add_argument("--threshold", type=float, default=0.05,
                   help="max allowed mean perceptual error (default 0.05)")
    p.add_argument("--backend", default="auto",
                   choices=["auto", "flip", "ssim", "luma", "stdlib"],
                   help="force a metric backend (default auto = best available)")
    p.add_argument("--json", help="write the full result as JSON here")
    p.add_argument("--update", action="store_true",
                   help="bless the candidate as the new golden, then exit 0")
    p.add_argument("--selftest", action="store_true",
                   help="run the built-in self-test (self-diff == 0) and exit")
    args = p.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    if not args.candidate or not args.golden:
        p.print_usage()
        print("error: candidate and golden are required (or use --selftest)",
              file=sys.stderr)
        return 2

    if args.update:
        try:
            dst = bless_golden(args.candidate, args.golden)
        except Exception as e:
            print(f"error: update failed: {e}", file=sys.stderr)
            return 2
        print(f"BLESSED {args.candidate} -> golden {dst}")
        return 0

    try:
        result = compare(args.candidate, args.golden, out_heatmap=args.out,
                         backend=args.backend,
                         heatmap_normalize=not args.raw_heatmap)
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    score = result["score"]
    passed = score <= args.threshold
    result["threshold"] = args.threshold
    result["pass"] = passed

    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=2)

    verdict = "PASS" if passed else "FAIL"
    extra = ""
    if "max_error" in result:
        extra = f"  max={result['max_error']:.4f} p99={result['p99_error']:.4f}"
    hm = f"  heatmap={result['heatmap']}" if result.get("heatmap") else ""
    print(f"[flip_diff] backend={result['backend']} "
          f"score={score:.6f} threshold={args.threshold:.6f} "
          f"-> {verdict}{extra}{hm}")
    if not passed:
        print(f"[flip_diff] REGRESSION: perceptual error {score:.6f} exceeds "
              f"threshold {args.threshold:.6f}. "
              f"Inspect the heatmap to localize the regressed region/pass.",
              file=sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
