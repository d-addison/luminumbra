#!/usr/bin/env python3
"""Per-flag fixture tests for tools/visual_critique.py ( ).

A gate CI never runs isn't a gate: these fixtures pin every objective flag the
WorldVisualSweep critique can raise, so a threshold edit that silently stops
catching a defect fails here. Pure numpy (no Pillow / no disk) -- each fixture
builds a synthetic RGB array that should trip exactly the targeted defect and
asserts analyze_array() raises it. Run directly; exits non-zero on any failure.

  python tools/test_visual_critique.py
"""
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import visual_critique as vc  # noqa: E402

H, W = 90, 120  # small frame; region split is top-third sky / bottom-third ground

def frame(value):
    a = np.empty((H, W, 3), dtype=np.float32)
    a[...] = value
    return a

_failures = []

def check(name, arr, meta, expect_present=(), expect_absent=()):
    res = vc.analyze_array(arr, meta)
    flags = set(res["flags"])
    for f in expect_present:
        if f not in flags:
            _failures.append(f"[{name}] expected flag {f!r} but got {sorted(flags)}")
    for f in expect_absent:
        if f in flags:
            _failures.append(f"[{name}] flag {f!r} should NOT fire but got {sorted(flags)}")
    return res

# --- DEAD_BLACK_FRAME: >92% near-black pixels ---
check("dead_black", frame(0), {}, expect_present=["DEAD_BLACK_FRAME"])

# --- FLAT_DARK_NO_DETAIL: 70% black, low std, but not dead (<92% black) ---
a = frame(20.0)                       # luma 20 -> NOT near-black
a[: int(0.7 * H)] = 10.0              # 70% near-black (luma 10), low variance
check("flat_dark", a, {}, expect_present=["FLAT_DARK_NO_DETAIL"],
      expect_absent=["DEAD_BLACK_FRAME"])

# --- WASHED_OUT: >35% blown highlights ---
a = frame(120.0)
a[: int(0.45 * H)] = 255.0            # 45% blown
check("washed_out", a, {}, expect_present=["WASHED_OUT"])

# --- GREEN_SKY_SPECKLE: sparse isolated green specks in the sky band ---
a = frame(20.0)                       # dark neutral background
for i, (y, x) in enumerate([(2, 5), (4, 30), (6, 60), (8, 90), (10, 110),
                            (12, 15), (14, 45), (16, 75), (18, 100), (20, 20)]):
    a[y, x] = (10.0, 220.0, 10.0)     # bright green, local outlier vs neighbors
check("green_speckle", a, {}, expect_present=["GREEN_SKY_SPECKLE"])

# --- CLOUDS_FLAT_NO_STRUCTURE: up-pitched storm, near-zero structure ---
check("clouds_flat", frame(100.0), {"storm": True, "pitched_up": True},
      expect_present=["CLOUDS_FLAT_NO_STRUCTURE"])

# --- AURORA_AT_DUSK: green-dominant chroma in a dusk sky ---
a = frame(80.0)
a[: H // 3] = (50.0, 130.0, 60.0)     # green curtain in the sky band
check("aurora_dusk", a, {"tod": "dusk"}, expect_present=["AURORA_AT_DUSK"])

# --- FOLIAGE_SPARSE: NEUTRAL noon down-view with no green ground cover ---
a = frame(110.0)
a[2 * H // 3:] = (130.0, 110.0, 100.0)  # brown/grey ground, not green
check("foliage_sparse_noon", a,
      {"daytime": True, "pitched_down": True, "storm": False, "tod": "noon"},
      expect_present=["FOLIAGE_SPARSE"])

# --- FOLIAGE_SPARSE must NOT false-fire under warm RAKING dusk light ---
# Present foliage reads orange (r>g) and dim under low warm sun, so the green-
# cover heuristic measures ~0 cover even though grass IS there. The detector must
# only assert on the neutral noon cell, never warm dusk/dawn. (Regression guard
# for the  update the baseline: the dusk cell flagged a present-foliage false positive.)
a = frame(70.0)
a[2 * H // 3:] = (95.0, 55.0, 35.0)     # warm, dim, orange-lit grass (g << r)
check("foliage_sparse_dusk_no_false_positive", a,
      {"daytime": True, "pitched_down": True, "storm": False, "tod": "dusk"},
      expect_absent=["FOLIAGE_SPARSE"])

# --- NIGHT_STORM_TOO_BLACK: storm night, >85% black, low contrast ---
a = frame(18.0)
a[: int(0.9 * H)] = 0.0               # 90% black
check("night_storm_black", a, {"storm": True, "tod": "night"},
      expect_present=["NIGHT_STORM_TOO_BLACK"], expect_absent=["DEAD_BLACK_FRAME"])

# --- rain anisotropy metric must be computed for storm horizon cells ---
a = frame(60.0)
a[::2] = 120.0                        # horizontal banding -> vertical gradient
res = vc.analyze_array(a, {"storm": True})
if "rain_vh_anisotropy" not in res["metrics"]:
    _failures.append("[rain_anis] storm horizon cell missing rain_vh_anisotropy metric")

# --- LOW_TEXTURE_DETAIL: daytime clear terrain view with flat/untextured ground ---
# (visual-fidelity  visual-fidelity floor). Solid green ground -> ~zero high-freq detail.
a = frame(110.0)
a[2 * H // 3:] = (95.0, 135.0, 80.0)  # SOLID green ground (low-fi, no surface detail)
check("low_texture_detail", a,
      {"daytime": True, "pitched_down": True, "storm": False, "angle": "down35"},
      expect_present=["LOW_TEXTURE_DETAIL"])

# --- a TEXTURED ground clears the fidelity floor (no flag) ---
a = frame(110.0)
g = np.full((H - 2 * H // 3, W, 3), (95.0, 135.0, 80.0), dtype=np.float32)
g[::2] += 45.0                        # high-frequency surface detail -> above the floor
a[2 * H // 3:] = g
check("textured_ground_ok", a,
      {"daytime": True, "pitched_down": True, "storm": False, "angle": "down35"},
      expect_absent=["LOW_TEXTURE_DETAIL"])

# --- fidelity floor does NOT judge pitched-up sky views or water cells ---
a = frame(110.0)
a[2 * H // 3:] = (95.0, 135.0, 80.0)  # flat, but it's a sky/up view -> not a terrain test
check("up_view_no_fidelity", a,
      {"daytime": True, "pitched_up": True, "storm": False, "angle": "up25"},
      expect_absent=["LOW_TEXTURE_DETAIL"])

# --- brightness INDEPENDENCE (the 2026-06-16 fix): dim-but-textured terrain PASSES the
#     normalized floor even though its RAW |Laplacian| is below the old absolute floor.
#     The old light-coupled metric would have spuriously flagged this dusk-dim terrain. ---
a = frame(45.0)
g = np.full((H - 2 * H // 3, W, 3), (38.0, 52.0, 33.0), dtype=np.float32)  # dim (~dusk) base
g[::2] += 3.0                         # faint but real surface texture
a[2 * H // 3:] = g
res = check("dim_textured_ok", a,
            {"daytime": True, "pitched_down": True, "storm": False, "angle": "down35"},
            expect_absent=["LOW_TEXTURE_DETAIL"])
if res["metrics"]["ground_detail_energy"] >= vc.T["fidelity_detail_min"]:
    _failures.append("[dim_textured] fixture must have RAW detail below the old absolute "
                     "floor (else it doesn't prove the brightness-coupling fix)")

# --- the fidelity floor judges ONLY down-pitched cells: an eye-level horizon vista with
#     flat ground is NOT flagged (distant terrain legitimately minifies — low detail is
#     correct there, not a defect). ---
a = frame(110.0)
a[2 * H // 3:] = (95.0, 135.0, 80.0)  # flat ground, eye-level vista (no pitch flags)
check("eye_level_vista_no_fidelity", a,
      {"daytime": True, "storm": False, "angle": "yaw000"},
      expect_absent=["LOW_TEXTURE_DETAIL"])

# --- sun-elevation (raking) awareness: a modestly-textured ground that PASSES at noon
#     (overhead light -> lower achievable micro-shadow -> scaled floor) would FAIL at
#     dawn (raking light -> full floor). Same texture, different bar by sun elevation. ---
def _modest_ground():
    a = frame(110.0)
    g = np.full((H - 2 * H // 3, W, 3), (95.0, 135.0, 80.0), dtype=np.float32)
    g[::2] += 4.0   # faint texture -> ground_relative_detail ~0.064 (between 0.056 and 0.08)
    a[2 * H // 3:] = g
    return a
res = check("noon_raking_ok", _modest_ground(),
            {"daytime": True, "pitched_down": True, "storm": False, "angle": "down35", "tod": "noon"},
            expect_absent=["LOW_TEXTURE_DETAIL"])
rd = res["metrics"]["ground_relative_detail"]
if not (0.056 < rd < vc.T["fidelity_relative_detail_min"]):
    _failures.append(f"[noon_raking] fixture rel_detail {rd:.4f} must sit between noon floor "
                     f"(0.056) and the full floor (0.08) to prove the raking scale")
check("dawn_raking_flag", _modest_ground(),
      {"daytime": True, "pitched_down": True, "storm": False, "angle": "down35", "tod": "dawn"},
      expect_present=["LOW_TEXTURE_DETAIL"])

# --- CLEAN frame: a normal lit daytime scene raises NO blocking flags ---
a = frame(90.0)
a[: H // 3] = (120.0, 150.0, 210.0)   # blue sky
g = np.full((H - 2 * H // 3, W, 3), (80.0, 140.0, 70.0), dtype=np.float32)
g[::2] += 40.0                        # textured green ground -> clears the fidelity floor
a[2 * H // 3:] = g
res = check("clean", a, {"daytime": True, "pitched_down": True, "angle": "down35"},
            expect_absent=list(vc.HARD_FLAGS) + list(vc.FIDELITY_FLAGS))

# --- the blocking-flag registry must cover every flag these fixtures raise ---
raised = {"DEAD_BLACK_FRAME", "FLAT_DARK_NO_DETAIL", "WASHED_OUT", "GREEN_SKY_SPECKLE",
          "CLOUDS_FLAT_NO_STRUCTURE", "AURORA_AT_DUSK", "FOLIAGE_SPARSE",
          "NIGHT_STORM_TOO_BLACK"}
missing = raised - vc.HARD_FLAGS
if missing:
    _failures.append(f"HARD_FLAGS is missing blocking flags: {sorted(missing)}")

# --- the fidelity-floor flag must be registered in FIDELITY_FLAGS (blocking) ---
if "LOW_TEXTURE_DETAIL" not in vc.FIDELITY_FLAGS:
    _failures.append("FIDELITY_FLAGS is missing LOW_TEXTURE_DETAIL")

if _failures:
    print("VISUAL CRITIQUE FIXTURE FAILURES:")
    for f in _failures:
        print("  - " + f)
    sys.exit(1)
print("visual_critique fixtures passed: all objective flags pinned, clean frame clean.")
