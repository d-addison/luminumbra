#!/usr/bin/env python3
"""Automated, objective (non-AI) visual critique of the world-visual-sweep matrix.

Reads the sweep manifest + cell PNGs and extracts per-cell objective image
metrics, then emits hard defect flags + a structured report (JSON + markdown).
This is the NON-AI half of the dual-track critique: deterministic pixel math,
no model judgement. The AI half (neutral describe + adversarial flaw-hunt) is
produced separately by vision passes and merged by `combine` below.

Usage:
  python tools/visual_critique.py analyze <sweep_dir> [--strict]   # objective only
  python tools/visual_critique.py combine <sweep_dir> <ai.json>    # merge AI critique

  --strict : exit non-zero if ANY objective defect flag is raised. This is the
             mode the WorldVisualSweep validator gate runs. Per the
             process rule, a visual-critique BLOCK is discharged ONLY by a
             passing (flag-free) re-run of this gate -- never by reclassifying a
             flagged cell as "tracked debt". So there is intentionally NO
             allowlist: every flag below blocks until the render is fixed.

A "cell" is one (season,tod,angle,weather) frame. The manifest carries the
ground-truth labels + engine-side counters (foliage_draws, particle_draws,
lightning_active, ...). The objective metrics below are computed from pixels so
they are an INDEPENDENT check on the engine counters and on the rendered look.
"""
import sys, os, json
import numpy as np
from PIL import Image

# --- objective thresholds -----------------------------------------------------
# Every threshold is pinned by a fixture in tools/test_visual_critique.py. Do
# not retune one without updating its evidence-backed fixture.
T = {
    "black_frac_dead": 0.92,        # >92% near-black => dead/black frame (night dome was once fully unlit)
    "near_black": 14,               # luma <= this counts as "near black"
    "blown_frac_washed": 0.35,      # >35% blown => washed out (noon sand/far-water sheet clipped to white)
    "blown": 250,                   # luma >= this counts as "blown" highlight
    "low_contrast_std": 6.0,        # whole-frame luma std below this => flat/featureless (grey-fog storm)
    "sky_green_speck_frac": 0.0015, # isolated-green sky fraction => firefly speckle (cyan-billboard foliage era)
    "cloud_struct_std_min": 8.0,    # up-storm sky luma std below this => flat overcast (no cloud structure)
    "aurora_dusk_chroma_frac": 0.010, # green/magenta chroma in a dusk sky => aurora leaking out of night
    "foliage_ground_green_min": 0.010, # daytime down-view: ground green-cover floor (foliage must be present)
    "rain_anis_min": 1.15,          # storm: vertical/horizontal gradient ratio floor (rain reads as streaks)
    # --- visual-fidelity floor ------------------------------------------------
    # Terrain micro-contrast is brightness-normalized ground detail: mean |Laplacian| of
    # the ground luma divided by mean ground luma. The raw |Laplacian| metric retained
    # below for telemetry scales with absolute luma, making it unsuitable as a gate across
    # lighting conditions. The normalized metric is light-independent: flat/untextured
    # terrain -> ~0 at any time of day; a detailed surface carries visible relative
    # contrast. The floor is judged only on down-pitched daytime-clear cells because
    # eye-level horizon views legitimately minify distant terrain. Its fixture is pinned
    # by tools/test_visual_critique.py.
    "fidelity_relative_detail_min": 0.08,  # |Laplacian|/luma floor (light-independent)
    # Per-time-of-day raking scale on the fidelity floor: overhead noon light casts less
    # micro-shadow than raking dawn/dusk light on the same texture, so noon's bar is lower
    # (sun-elevation awareness; see the LOW_TEXTURE_DETAIL gate). Missing tod -> 1.0.
    "raking_floor_scale": {"dawn": 1.0, "noon": 0.7, "dusk": 1.0},
    "fidelity_luma_floor": 8.0,            # eps for the division; below this a cell is too dark to judge
    "fidelity_detail_min": 8.0,            # RAW |Laplacian| floor — telemetry only now (light-coupled; superseded)
}

# Flags that BLOCK the gate in --strict mode. All objective defect flags are
# blocking by design (no reclassification escape hatch). Listed explicitly so a
# new flag must be consciously added here to gain blocking power, and so the
# fixture test can assert the set is complete.
HARD_FLAGS = {
    "DEAD_BLACK_FRAME",
    "FLAT_DARK_NO_DETAIL",
    "WASHED_OUT",
    "GREEN_SKY_SPECKLE",
    "NIGHT_STORM_TOO_BLACK",
    "CLOUDS_FLAT_NO_STRUCTURE",
    "AURORA_AT_DUSK",
    "FOLIAGE_SPARSE",
}

# Visual-FIDELITY-FLOOR flags (owner principle: minimum realistic fidelity at the
# Battlefield 4/ / Frostbite level — don't ship flat/low-fi visuals). Distinct
# from the DEFECT flags above: those catch BROKEN renders, these catch renders that
# work but fall below the beauty floor. They BLOCK in --strict exactly like the
# defect flags, so the critique enforces the floor; a fidelity BLOCK is discharged
# only by raising the visuals (e.g. texturing the far/near terrain), never by
# reclassification. (These are expected to fire on the current sub-floor visuals —
# that is the point: the gate now objectively reflects "the visuals are not there
# yet" and drives the work.)
FIDELITY_FLAGS = {
    "LOW_TEXTURE_DETAIL",
}

def luma(a):  # a: HxWx3 uint8
    return (0.2126*a[...,0] + 0.7152*a[...,1] + 0.0722*a[...,2])

def detail_energy(L):
    """High-frequency surface detail of a luma plane: mean abs discrete Laplacian
    (4-neighbour). Flat/untextured surfaces -> near 0; textured surfaces (real
    terrain micro-detail, normal-mapped relief) -> markedly higher. This is the
    objective proxy for the visual-fidelity  texture-fidelity floor."""
    if L.shape[0] < 3 or L.shape[1] < 3:
        return 0.0
    lap = np.abs(4.0*L[1:-1, 1:-1] - L[:-2, 1:-1] - L[2:, 1:-1] - L[1:-1, :-2] - L[1:-1, 2:])
    return float(lap.mean())

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
    """Thin wrapper: load the PNG and run the pure-numpy core. Kept so callers
    that have a file path do not need to touch Pillow themselves; the fixture
    tests call analyze_array directly with synthetic arrays (numpy only)."""
    res = analyze_array(load(path), m)
    res["file"] = os.path.basename(path)
    return res

def analyze_array(a, m):
    """Pure-numpy objective critique of one cell. `a` is an HxWx3 float32 RGB
    array (0..255); `m` is the manifest cell dict (labels + engine counters).
    Returns {file,label,metrics,engine,flags}. No file I/O -> directly testable."""
    a = np.asarray(a, dtype=np.float32)
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
        "ground_detail_energy": detail_energy(luma(region(a,"ground"))),
    }
    # Brightness-normalized micro-contrast: |Laplacian| / mean-luma is light-independent,
    # so the SAME texture scores identically at noon and dusk (the raw |Laplacian| above
    # scales with luma and spuriously fails dim-but-textured terrain). This is the metric
    # the visual-fidelity  fidelity floor judges on.
    metrics["ground_relative_detail"] = (
        metrics["ground_detail_energy"] / max(metrics["ground_luma"], T["fidelity_luma_floor"]))
    # --- universal flags ---
    if metrics["black_frac"] > T["black_frac_dead"]:
        flags.append("DEAD_BLACK_FRAME")
    if metrics["std_luma"] < T["low_contrast_std"] and metrics["black_frac"] > 0.5:
        flags.append("FLAT_DARK_NO_DETAIL")
    if metrics["blown_frac"] > T["blown_frac_washed"]:
        flags.append("WASHED_OUT")

    # --- green-firefly speckle (the old cyan-billboard foliage bug) ---
    # The "sky" region is the TOP THIRD of the frame, which is only actually sky for
    # level / upward views. In a DOWN-pitched view (e.g. down35) the top third is
    # terrain + foliage, so green tree canopies there are legitimate ground content,
    # NOT sky fireflies. Only run the sky-firefly check where the top third is sky.
    sky = region(a, "sky")
    sky_specks = isolated_green_mask(sky)
    speck_frac = float(sky_specks.mean())
    metrics["sky_green_speck_frac"] = speck_frac
    _is_down_view = str(m.get("angle", "")).startswith("down")
    # Fireflies (the cyan-billboard bug) are SPARSE green dots on a non-green sky. A
    # legitimate AURORA is a connected green FIELD that fills much of the night sky.
    # Distinguish them: if a large fraction of the sky is green-ish, it's an aurora
    # (the night-aurora-presence gate WANTS that) -> not firefly speckle.
    _sr, _sg, _sb = sky[..., 0], sky[..., 1], sky[..., 2]
    green_field_frac = float((((_sg > _sr + 15) & (_sg > _sb + 15)).mean()))
    metrics["sky_green_field_frac"] = green_field_frac
    if (speck_frac > T["sky_green_speck_frac"] and not _is_down_view
            and green_field_frac < 0.06):
        flags.append("GREEN_SKY_SPECKLE")

    storm = bool(m.get("storm"))
    daytime = bool(m.get("daytime"))
    tod = m.get("tod"); angle = m.get("angle")

    # --- VISUAL-FIDELITY FLOOR: terrain micro-contrast (visual-fidelity  realism) ---
    # Judge ONLY on DOWN-PITCHED daytime-clear cells: those frame NEAR terrain (where
    # micro-detail is the honest signal). Eye-level horizon vistas legitimately minify
    # distant terrain (low detail is correct there, not a defect); pitched-up sky views
    # and water cells put no near terrain in the ground third. The metric is the
    # BRIGHTNESS-NORMALIZED micro-contrast so dim dusk terrain is not penalized for being
    # dark (the raw |Laplacian| floor conflated texture with light level).
    # Sun-elevation (RAKING) awareness: flat OVERHEAD light (noon) casts far less micro-
    # shadow than RAKING low-sun light (dawn/dusk) on the SAME textured surface, so the
    # achievable micro-contrast is intrinsically lower at high sun. Measured on the 1024
    # AmbientCG terrain: identical ground scores noon ~0.067 vs dawn/dusk ~0.10 — a ~1.5x
    # lighting-geometry gap, NOT a texture deficit. Scale the floor by the expected raking
    # so a well-textured noon cell is judged against a flat-light-appropriate bar (the same
    # light-awareness principle as the brightness-normalized metric above). dawn/dusk =
    # full raking (1.0); noon = overhead (0.7). Pinned by tools/test_visual_critique.py.
    if daytime and not storm and bool(m.get("pitched_down")) and angle != "water":
        floor = T["fidelity_relative_detail_min"] * T["raking_floor_scale"].get(tod, 1.0)
        if metrics["ground_relative_detail"] < floor:
            flags.append("LOW_TEXTURE_DETAIL")

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
    # The green-cover heuristic ((g>r+8)&(g>bl+8)&(g>50)) only detects foliage
    # reliably under NEUTRAL, BRIGHT daylight. Under warm RAKING light (dawn/dusk)
    # healthy grass reads orange (r>g) and dim (g<50), so the test yields FALSE
    # NEGATIVES — the foliage is present, just warm-lit (measured: noon cover ~0.58
    # vs dusk ~0.008 for the SAME scatter). Foliage placement is time-of-day-
    # invariant, so assert presence on the canonical neutral NOON cell only — the
    # same noon-as-reference principle the LOW_TEXTURE_DETAIL raking scale uses.
    # cover is still recorded for every cell as telemetry. Pinned by
    # tools/test_visual_critique.py (noon asserts; warm dusk does not false-fire).
    if daytime and m.get("pitched_down") and not storm:
        gr = region(a,"ground"); r,g,bl = gr[...,0], gr[...,1], gr[...,2]
        cover = float(((g > r+8)&(g > bl+8)&(g>50)).mean())
        metrics["ground_green_cover_frac"] = cover
        if tod == "noon" and cover < T["foliage_ground_green_min"]:
            flags.append("FOLIAGE_SPARSE")

    # --- rain streak anisotropy in storm horizon cells ---
    if storm and not m.get("pitched_up"):
        gy = np.abs(np.diff(L, axis=0)).mean()
        gx = np.abs(np.diff(L, axis=1)).mean()
        anis = float(gy / (gx + 1e-6))
        metrics["rain_vh_anisotropy"] = anis

    return {"file": (os.path.basename(m["file"]) if m.get("file") else None),
            "label": {k: m.get(k) for k in
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
    """Merge the objective track with an AI-critique JSON keyed by cell file.
    The AI json is {"<file.png>": {"notes": [...], "critical": bool}, ...} as
    emitted by the vision describe + adversarial flaw-hunt passes. A cell is
    HIGH CONFIDENCE when an objective flag AND an AI note land on the same cell."""
    obj = analyze(sweep_dir)
    ai = json.load(open(ai_json)) if (ai_json and os.path.exists(ai_json)) else {}
    high = []
    for c in obj["cells"]:
        ac = ai.get(c["file"]) or {}
        ai_notes = ac.get("notes") or []
        if c["flags"] and ai_notes:
            high.append({"file": c["file"], "label": c["label"],
                         "objective_flags": c["flags"], "ai_notes": ai_notes})
    return {"objective": obj, "ai": ai, "high_confidence": high}

def _strict_exit(rep):
    """Exit 1 if any blocking objective flag was raised (gate mode). Blocking =
    DEFECT flags (broken renders) + FIDELITY flags (sub-threshold visuals)."""
    blocking = HARD_FLAGS | FIDELITY_FLAGS
    hard = {fl: n for fl, n in rep["flag_counts"].items() if fl in blocking}
    if hard:
        tally = ", ".join(f"{k}x{v}" for k, v in sorted(hard.items(), key=lambda x: -x[1]))
        fid = ", ".join(f"{k}x{v}" for k, v in sorted(hard.items(), key=lambda x: -x[1])
                        if k in FIDELITY_FLAGS)
        print(f"\nGATE FAIL (--strict): {sum(hard.values())} blocking flag(s) across "
              f"{len(rep['defects'])} cell(s): {tally}", file=sys.stderr)
        if fid:
            print(f"  (visual-fidelity-floor flags: {fid} — raise the visuals to the "
                  f"visual-fidelity  floor to clear)", file=sys.stderr)
        sys.exit(1)
    print("\nGATE PASS (--strict): no blocking objective/fidelity flags.")
    sys.exit(0)

if __name__ == "__main__":
    args = sys.argv[1:]
    strict = "--strict" in args
    args = [a for a in args if a != "--strict"]
    cmd = args[0] if args else "analyze"
    if cmd == "combine":
        sd = args[1]
        ai_json = args[2] if len(args) > 2 else None
        rep = combine(sd, ai_json)
        json.dump(rep, open(os.path.join(sd, "combined-critique.json"), "w"), indent=1)
        print(f"combined critique: {len(rep['high_confidence'])} high-confidence cell(s) "
              f"(objective flag AND AI note); wrote {os.path.join(sd, 'combined-critique.json')}")
    else:  # analyze
        sd = args[1] if len(args) > 1 else "."
        rep = analyze(sd)
        json.dump(rep, open(os.path.join(sd, "objective-critique.json"), "w"), indent=1)
        open(os.path.join(sd, "objective-critique.md"), "w").write(md(rep))
        print(md(rep))
        if strict:
            _strict_exit(rep)
