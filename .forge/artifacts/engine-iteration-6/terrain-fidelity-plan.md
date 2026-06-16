# Terrain visual-fidelity plan — raise to the BF4/BF1 floor (2026-06-16)

Owner: "our visuals are pretty shit"; floor = Battlefield 4/BF1 (Frostbite) realism
([[visual-fidelity-target]]). The critique now enforces it: `LOW_TEXTURE_DETAIL`
fires on 12/48 cells (all daytime-clear terrain views; ground detail energy 2-6 vs
the 8.0 floor). This is the actionable BLOCK to discharge.

## Diagnosis (from native 3840x1600 captures)
- The triplanar terrain texturing path EXISTS and is wired: `g_buffer.frag` samples
  `u_terrainTextures`/`u_terrainNormals` arrays via the material-LUT `texture_layer`/
  `normal_layer`/`tiling` (Stone/Soil/Grass/Sand/Deepslate, tiling 2.5-4 m). So this
  is NOT missing tech.
- But the RENDERED result is flat/low-fi: the near terrain reads muddy, low-contrast,
  low-detail (256px source textures, weak normal response, heavily minified at the
  down-view distance -> averages to a uniform olive); mid/far terrain is uniform
  olive with no macro material variation; the foliage is sparse billboard tufts (the
  standing GPU-grass debt); and odd blue speckle artifacts appear in the near soil.
- Net: the surface lacks both MICRO detail (near) and MACRO variation (distance), and
  contrast is low.

## Levers, ordered by efficiency x impact
1. **Detail-normal overlay (cheap, near micro-relief).** In `triplanar_normal`,
   sample the existing terrain normal at a 2nd, ~8x-finer "detail" tiling and blend
   (whiteout) over the base normal. Reuses existing assets, no new textures; adds
   high-frequency light/shade -> raises detail energy + realism. Watch cost
   ([[engine-power-scalability-principle]]) — it's a few extra samples.
2. **Macro material variation by slope/height (distance detail).** Blend
   stone/soil/grass/sand by surface slope + altitude (steep->rock, flat-low->soil/
   sand, mid->grass) with noise-jittered boundaries, so the terrain isn't a uniform
   single-material field at distance. Check whether any slope/height blend exists
   today (the flat olive suggests grass dominates everywhere). This is the biggest
   "reads as real terrain" win at the down-view scale.
3. **Texture/contrast quality.** 256px is low for the floor; raise albedo contrast +
   normal strength, and/or move to higher-res detail+macro textures (asset work).
   Investigate + kill the blue-speckle artifact (normal-map decode or material id?).
4. **Lighting/AO richness.** Confirm SSAO + shadow contact darkening actually reads on
   terrain (the flat look may be under-lit contrast); richer ambient/sun balance.
5. **Replace billboard foliage with GPU grass (Wave B).** Continuous scene-lit turf
   is a major fidelity lift + closes the foliage debt.
6. **Far-field shading parity** (separate): the SHIELD-RT far-field writes FLAT
   albedo today; route it through the same triplanar/material path so distance terrain
   stays consistent (folds into the scalable far-field build).

## Gate / loop
Each lever: implement -> rebuild -> re-run `world_visual_sweep` -> `visual_critique.py
analyze --strict` -> read `ground_detail_energy` (must climb toward/over 8.0 and the
LOW_TEXTURE_DETAIL cells clear) + eyeball the native crops at the BF4/BF1 bar. Iterate.
Lever 1+2 are the efficient first pass (no new assets); 3-5 are the deeper lift.

## Pass #1 findings (2026-06-16) — REVERTED, lesson learned
Tried lever 1 (detail-normal overlay at 6x tiling) + a render-only low/mid-freq
albedo break-up. **Ineffective** (detail energy unchanged 1.98 vs 1.99; near crop
visually unchanged) -> reverted (no measured gain + shader cost violates the
measured/scalable principles). WHY:
- The terrain DOES take the textured triplanar path (5 albedo+normal layers load;
  near terrain shows muddy soil) — it is NOT the flat base-color path. The look is
  low-CONTRAST, low-detail texturing, not "no texture".
- A high-freq detail-normal MINIFIES AWAY at the down-view distance; low/mid-freq
  albedo break-up does not raise the HIGH-freq Laplacian metric (and is too
  large-scale to see in the near field). Distant terrain is minification-bound.

### Corrected lever order
1. **Contrast amplification of the existing texture** (in `triplanar_albedo` /
   normal): amplify the AC (high-freq) component — `albedo = mix(meanAlbedo, albedo,
   k)` with k>1, and boost normal strength. This MULTIPLIES existing high-freq -> it
   actually moves the Laplacian metric AND de-muddies the look. The metric-moving
   lever (where there is detail to amplify, i.e. near/mid).
2. **Higher-CONTRAST / higher-res detail textures** — the 256px terrain albedos are
   muddy/low-contrast; this is the base-fidelity lift (asset work). Kill the
   blue-speckle artifact in the soil normal/decode.
3. **Lighting/AO contrast** on terrain (richer sun/ambient + readable AO).
4. **Metric recalibration:** the 8.0 floor may be too aggressive for distance-
   dominated down-views (distant terrain legitimately minifies). Consider a
   near-weighted ROI (e.g. bottom 1/6 = nearest terrain) or a floor calibrated to
   achievable detail once contrast/textures are improved. The floor is provisional.
5. Macro material variation by slope/height is a WORLDGEN change (world_hash bump),
   not render-only — defer to a deliberate hash-bump increment.

### Process note
`world_visual_sweep` writes PPMs; the PPM->PNG conversion is a SEPARATE validator
step. Running the scenario directly requires a manual PPM->PNG before
`visual_critique.py` (which reads sweep/png/), or stale PNGs are critiqued.
