# Iteration 5 visual debt (pipeline-tracked) — 2026-06-15

Produced by the automated dual-bias visual-critique pipeline (`tools/visual_critique.py`
objective + neutral/adversarial AI passes) over the world-visual-sweep matrix
(48 cells: tod x angle x weather x season). After 3 DR rounds the EGREGIOUS BUGS
are fixed; the items below are tracked QUALITY debt, several requiring the
research-driven reimplementation in the iteration-6 research agenda.

## FIXED (verified by pipeline)
- B1 lightning now strikes cloud->ground with impact (was dangling mid-air).
- B2 floating glow-disc/UFO artifact removed.
- B3 foliage COLOR fixed: green grass, not cyan/teal billboards.
- M6 aurora night-only (no dawn/storm bleed). N2 over-bright bloom tamed.
- night water no longer electric-cyan; storm clouds structured near bolt;
  down-rain reads as streaks not fog; water no longer hard-faceted (ripple normals).

## REMAINING DEBT (tracked; feeds iter-6 research)
| Item | Pipeline finding | Research/approach to fix |
|---|---|---|
| Foliage = sparse billboard tufts, slightly emissive under storm, not continuous scene-lit turf | adversarial MAJOR | GPU grass (Ghost of Tsushima GDC 2021; compute density + indirect; continuous turf; proper scene/cloud-shadow lighting) |
| Aurora reads orb-like in some cells (curtains in up-view, blobs in horizon/water reflection) | adversarial MAJOR | curtain geometry / volumetric aurora; reflection handling |
| Storm atmosphere low-contrast/milky; clouds flat away from bolt | MINOR | volumetric clouds tier-2 (Nubis, SIGGRAPH 2015/17) + froxel fog (Hillaire) |
| Night water still blue-teal cool (not neutral); flat sheet (no ripples beyond normals) | MINOR | Gerstner/FFT ocean (Tessendorf 2001); time-of-day water tint |
| Terrain = smooth cones + blob-rock material decals; distant landmass LOD "slabs"; shoreline seams | MINOR (earlier passes) | hydraulic/thermal erosion (Mei 2007); Transvoxel far-LOD; SHIELD-RT far field |
| 1/48 marginal GREEN_SKY_SPECKLE (dawn-storm down-view, foliage tip-cull at threshold) | objective | tighten foliage horizon cull / subsumed by GPU-grass rework |

The pipeline (`-Mode WorldVisualSweep` + `tools/visual_critique.py`) is the
standing gate: re-run it after any render change to re-assess objectively + with
the dual-bias AI critique.

## OPEN BLOCKs — gate-enforced (iteration-6 critique #4/#5, 2026-06-15)
As of the iteration-6 hygiene pass, `tools/visual_critique.py analyze --strict`
is a REQUIRED step inside the WorldVisualSweep gate (no longer best-effort), and
its per-flag thresholds are pinned by the `VisualCritiqueFlags` ctest
(`tools/test_visual_critique.py`). The gate therefore HARD-FAILS today on the
`GREEN_SKY_SPECKLE` cell above (the foliage tip-cull residual). Per the process
rule, these BLOCKs are discharged **only by a flag-free re-run of the same gate**
— never by reclassifying a flagged cell as "tracked debt":
- **Foliage BLOCK** (billboard tufts / `GREEN_SKY_SPECKLE`) — OPEN until Wave B
  GPU-grass lands and WorldVisualSweep re-runs flag-free.
- **Aurora BLOCK** (orb-like / blob reflections) — OPEN until Wave B aurora
  curtains land and WorldVisualSweep re-runs flag-free.
The MINOR items remain research-tracked for Waves B/C; they are not currently
raising blocking objective flags but are re-assessed on every gate re-run.
