#!/usr/bin/env python3
"""Automated, objective (non-AI) visual critique of the world-visual-sweep matrix.

Reads the sweep manifest + cell PNGs and extracts per-cell objective image
metrics, then emits hard defect flags + a structured report (JSON + markdown).
This is the NON-AI half of the dual-track critique: deterministic pixel math,
no model judgement. The AI half (neutral describe + adversarial flaw-hunt) is
produced separately by vision passes and merged by `combine` below.

Usage:
  python tools/visual_critique.py analyze <sweep_dir>            # objective only
  python tools/visual_critique.py combine <sweep_dir> <ai.json>  # merge AI critique

A "cell" is one (season,tod,angle,weather) frame. The manifest carries the
ground-truth labels + engine-side counters (foliage_draws, particle_draws,
lightning_active, ...). The objective metrics below are computed from pixels so
they are an INDEPENDENT check on the engine counters and on the rendered look.
"""
import sys, os, json, glob
import numpy as np
from PIL import Image

# --- objective thresholds (tuned to the defects this matrix must catch) -------
T = {
    "black_frac_dead": 0.92,        # >92% near-black pixels => dead/black frame
    "near_black": 14,               # luma <= this is "near black"
    "blown_frac_washed": 0.35,      # >35% blown highlights => washed out
    "blown": 250,
    "low_contrast_std": 6.0,        # whole-frame luma std below this => flat/featureless
    "sky_green_speck_frac": 0.0015, # fraction of sky pixels that are isolated green => firefly speckle
    "cloud_struct_std_min": 8.0,    # up-storm sky luma std below this => flat overcast (no cloud structure)
    "aurora_dusk_chroma_frac": 0.010, # green/magenta chroma fraction in a dusk sky => aurora leak
    "foliage_ground_green_min": 0.010, # daytime down-view: ground green-cover fraction floor
    "rain_anis_min": 1.15,          # storm: vertical/horizontal gradient ratio floor (streaks)
}

def luma(a):  # a: HxWx3 uint8
    return (0.2126*a[...,0] + 0.7152*a[...,1] + 0.0722*a[...,2])

def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)

def region(a, part):
    h = a.shape[0]
    if part == "sky":    return a[: h//3]
    if part == "ground": return a[2*h//3 :]
    return a

def isolated_green_mask(a):
    """green-dominant pixels that are LOCAL outliers (firefly specks), not a green field."""
    r, g, b = a[...,0], a[...,1], a[...,2]
    green = (g > r + 28) & (g > b + 28) & (g > 70)
    # local-outlier: brighter than a 5px-shifted neighborhood average on all sides
    L = luma(a)
    nb = np.zeros_like(L)
    for dy, dx in ((5,0),(-5,0),(0,5),(0,-5)):
        nb += np.roll(L, (dy,dx), (0,1))
    nb /= 4.0
    outlier = L > nb + 30
    return green & outlier

def analyze_cell(path, m):
    a = load(path)
    L = luma(a)
    h, w = L.shape
    n = h*w
    sat = (a.max(-1) - a.min(-1))  # crude saturation (0..255)
    flags = []
    metrics = {
        "mean_luma": float(L.mean()),
        "std_luma": float(L.std()),
        "p1_luma": float(np.percentile(L,1)),
        "p99_luma": float(np.percentile(L,99)),
        "black_frac": float((L <= T["near_black"]).mean()),
        "blown_frac": float((L >= T["blown"]).mean()),
        "mean_sat": float(sat.mean()),
        "sky_luma": float(luma(region(a,"sky")).mean()),
        "ground_luma": float(luma(region(a,"ground")).mean()),
    }
    # --- universal flags ---
    if metrics["black_frac"] > T["black_frac_dead"]:
        flags.append("DEAD_BLACK_FRAME")
    if metrics["std_luma"] < T["low_contrast_std"] and metrics["black_frac"] > 0.5:
        flags.append("FLAT_DARK_NO_DETAIL")
    if metrics["blown_frac"] > T["blown_frac_washed"]:
        flags.append("WASHED_OUT")

    # --- green-firefly speckle (anywhere, weighted to the sky) ---
    sky = region(a, "sky")
    sky_specks = isolated_green_mask(sky)
    speck_frac = float(sky_specks.mean())
    metrics["sky_green_speck_frac"] = speck_frac
    if speck_frac > T["sky_green_speck_frac"]:
        flags.append("GREEN_SKY_SPECKLE")

    storm = bool(m.get("storm"))
    daytime = bool(m.get("daytime"))
    tod = m.get("tod"); angle = m.get("angle")

    # --- night-storm legibility: dark but must have SOME structure/contrast ---
    if storm and tod == "night":
        if metrics["black_frac"] > 0.85 and metrics["std_luma"] < 7.0:
            flags.append("NIGHT_STORM_TOO_BLACK")

    # --- cloud structure in up-view storm ---
    if storm and m.get("pitched_up"):
        if metrics["std_luma"] < T["cloud_struct_std_min"]:
            flags.append("CLOUDS_FLAT_NO_STRUCTURE")

    # --- aurora-at-dusk: green/magenta chroma in a dusk sky ---
    if tod == "dusk":
        r,g,bl = sky[...,0], sky[...,1], sky[...,2]
        aurora = ((g > r+20)&(g > bl+10)) | ((r>g+25)&(bl>g+25))  # green curtain or magenta
        af = float(aurora.mean())
        metrics["dusk_sky_chroma_frac"] = af
        if af > T["aurora_dusk_chroma_frac"]:
            flags.append("AURORA_AT_DUSK")

    # --- foliage ground cover in daytime down-view ---
    if daytime and m.get("pitched_down") and not storm:
        gr = region(a,"ground"); r,g,bl = gr[...,0], gr[...,1], gr[...,2]
        cover = float(((g > r+8)&(g > bl+8)&(g>50)).mean())
        metrics["ground_green_cover_frac"] = cover
        if cover < T["foliage_ground_green_min"]:
            flags.append("FOLIAGE_SPARSE")

    # --- rain streak anisotropy in storm horizon cells ---
    if storm and not m.get("pitched_up"):
        gy = np.abs(np.diff(L, axis=0)).mean()
        gx = np.abs(np.diff(L, axis=1)).mean()
        anis = float(gy / (gx + 1e-6))
        metrics["rain_vh_anisotropy"] = anis

    return {"file": os.path.basename(path), "label": {k: m.get(k) for k in
            ("season","tod","angle","weather","feature")}, "metrics": metrics,
            "engine": {k: m.get(k) for k in ("foliage_draws","particle_draws",
            "lightning_active","cloud_coverage","water_like_ratio","mean_luminance")},
            "flags": flags}

def analyze(sweep_dir):
    man = json.load(open(os.path.join(sweep_dir, "world-visual-sweep-manifest.json")))
    cells = man.get("cells") if isinstance(man, dict) else man
    png_dir = os.path.join(sweep_dir, "sweep", "png")
    out = []
    for m in cells:
        f = os.path.basename(m["file"]).replace(".ppm",".png")
        p = os.path.join(png_dir, f)
        if os.path.exists(p):
            out.append(analyze_cell(p, m))
    rep = {"sweep_dir": sweep_dir, "n_cells": len(out), "cells": out}
    rep["defects"] = [{"file": c["file"], "label": c["label"], "flags": c["flags"]}
                      for c in out if c["flags"]]
    rep["flag_counts"] = {}
    for c in out:
        for fl in c["flags"]:
            rep["flag_counts"][fl] = rep["flag_counts"].get(fl,0)+1
    return rep

def md(rep):
    lines = [f"# Objective visual critique — {rep['n_cells']} cells",
             "", f"**Defect cells: {len(rep['defects'])} / {rep['n_cells']}**", ""]
    if rep["flag_counts"]:
        lines.append("## Flag tally")
        for k,v in sorted(rep["flag_counts"].items(), key=lambda x:-x[1]):
            lines.append(f"- `{k}` × {v}")
    else:
        lines.append("No objective defect flags raised.")
    lines.append("\n## Defect cells")
    for d in rep["defects"]:
        lab = d["label"]; lines.append(f"- **{lab['season']}/{lab['tod']}/{lab['angle']}/{lab['weather']}** "
                                       f"({d['file']}): {', '.join(d['flags'])}")
    return "\n".join(lines)

def combine(sweep_dir, ai_json):
    obj = analyze(sweep_dir)
    ai = json.load(open(ai_json)) if os.path.exists(ai_json) else {}
    # high-confidence = objective flag AND any AI (neutral/critical) note on same cell
    return {"objective": obj, "ai": ai}

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv)>1 else "analyze"
    sd = sys.argv[2]
    rep = analyze(sd)
    json.dump(rep, open(os.path.join(sd, "objective-critique.json"), "w"), indent=1)
    open(os.path.join(sd, "objective-critique.md"), "w").write(md(rep))
    print(md(rep))
