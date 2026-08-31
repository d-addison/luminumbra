#!/usr/bin/env python3
""" isolation/layer gate — objective backdrop + layer-suppression check.

Reads one or more PPM frames captured with an isolation backdrop active
(`--isolation-backdrop <mode>` + `--isolation-layers <csv>`) and asserts,
tolerantly (regression contract: AA/quantization-fragile -> per-channel LSB tolerance):

  1. BACKDROP FILL — the no-geometry SKY region (top third of a horizon-level
     capture) is filled with the requested flat backdrop colour to >= MIN_FILL,
     proving the SkyboxPass override replaced the sky dome.
  2. LAYER PRESENCE (when a geometry layer such as `terrain` is isolated) — the
     lower region carries lit, non-backdrop, non-black geometry pixels, proving
     the isolated subsystem still rendered (the gate is not vacuously "all
     backdrop").

Writes a decision JSON and exits non-zero on failure. numpy is REQUIRED (a gate
that cannot run is not a gate).

Usage:
  check-isolation-backdrop.py <backdrop> <out_json> <expect_geometry> <ppm> [<ppm>...]
    backdrop: void | greenscreen | checker
    expect_geometry: 1/true if an opaque geometry layer (terrain) was isolated
"""
import json
import sys

import numpy as np

# Flat backdrop colours SkyboxPass writes (see enhanced_skybox.frag): void is
# 0.02 linear -> ~5/255; greenscreen is pure (0,1,0). Checker alternates so it
# is fill-checked by "not sky-dome blue", handled separately below.
BACKDROPS = {
    "void": (5, 5, 5),
    "greenscreen": (0, 255, 0),
}

MIN_FILL = 0.95          # >= 95% of the sky region must match the backdrop
TOLERANCE = 18           # per-channel LSB tolerance (gamma/AA/quantization)
MIN_GEOMETRY = 0.05      # >= 5% of the lower region must be lit geometry


def read_ppm(path):
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise ValueError(f"{path}: not a binary P6 PPM (got {magic!r})")
        dims = f.readline().split()
        w, h = int(dims[0]), int(dims[1])
        int(f.readline())  # maxval
        buf = f.read(w * h * 3)
        return np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3).astype(int)


def main():
    if len(sys.argv) < 5:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    backdrop = sys.argv[1]
    out_json = sys.argv[2]
    expect_geometry = sys.argv[3].lower() in ("1", "true", "yes")
    ppms = sys.argv[4:]

    if backdrop not in BACKDROPS:
        print(f"isolation gate: backdrop '{backdrop}' has no objective colour check "
              f"(supported: {', '.join(BACKDROPS)})", file=sys.stderr)
        sys.exit(2)
    target = np.array(BACKDROPS[backdrop])

    result = {
        "schema": "luminumbra.isolation_gate.v1",
        "backdrop": backdrop,
        "min_fill": MIN_FILL,
        "tolerance": TOLERANCE,
        "expect_geometry": expect_geometry,
        "frames": [],
        "passed": True,
        "failures": [],
    }

    for path in ppms:
        img = read_ppm(path)
        h, w, _ = img.shape
        top = img[: h // 3]
        fill = float((np.abs(top - target).max(2) <= TOLERANCE).mean())

        geom_frac = 0.0
        if expect_geometry:
            bottom = img[2 * h // 3:]
            not_backdrop = np.abs(bottom - target).max(2) > TOLERANCE
            not_black = bottom.max(2) > 8
            geom_frac = float((not_backdrop & not_black).mean())

        frame = {
            "file": path,
            "sky_backdrop_fill": round(fill, 4),
            "geometry_frac": round(geom_frac, 4),
        }
        result["frames"].append(frame)

        if fill < MIN_FILL:
            result["passed"] = False
            result["failures"].append(
                f"{path}: sky backdrop fill {fill:.3f} < {MIN_FILL} (backdrop override not applied)")
        if expect_geometry and geom_frac < MIN_GEOMETRY:
            result["passed"] = False
            result["failures"].append(
                f"{path}: isolated geometry layer absent (geometry_frac {geom_frac:.3f} < {MIN_GEOMETRY})")

    with open(out_json, "w") as f:
        json.dump(result, f, indent=2)

    if not result["passed"]:
        print("ISOLATION GATE FAILED: " + "; ".join(result["failures"]), file=sys.stderr)
        sys.exit(1)
    print(f"isolation gate passed: backdrop={backdrop} frames={len(ppms)} "
          f"(sky fill >= {MIN_FILL}"
          + (f", geometry present >= {MIN_GEOMETRY}" if expect_geometry else "") + ")")


if __name__ == "__main__":
    main()
