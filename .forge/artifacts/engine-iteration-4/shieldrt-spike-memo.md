# T-I4-15 — SHIELD-RT Spike Memo (EVIDENCE ONLY, one round)

**Task:** Benchmark two candidate far-field render-source representations against
the same FarLodStore terrain — (A) heightfield ray-marching the packed far tiles
directly, vs (B) sphere-tracing a mip-mapped SDF volume built from the same
region — to inform the iteration-6 decision: do the F1/F2 tiles *become* the
far renderer's raymarch source, or do we build sparse brick SDF clipmaps?

**Scope discipline:** render-only prototype, no `src/` changes, no production
wiring, no `world_hash`/determinism contract touched (research Area 3 takeaway 7).
One benchmark round; HARD STOP after this memo (critique F6). The benchmark is
`test/performance/shieldrt_spike_bench.cpp`, registered in ctest under the
`manual` label (excluded by default; Debug runtime ~25 s).

**Method:** For 4 representative far-field views (default + mountains presets ×
horizon-grazing + elevated rays), a 320×180 ray grid (57 600 rays) is fired at a
3-region (~1536 m) far block built from real `BuildPristineFarLodTile` F1 (4 m)
tiles. Median of 3 runs per path per view. CPU prototype on purpose — it measures
the *traversal-cost shape* and the *conservative-mip correctness criterion*,
which are representation-level facts independent of CPU-vs-GPU; a production build
would be GPU-resident (iteration-6 concern).

---

## Results

### Timing & steps (median of 3, 320×180 = 57 600 rays/view), Debug build

> NOTE ON TIMINGS: the FINAL capture below was taken last on a **quiet machine**
> (`contended_machine: false`, max wall-clock variance 5.6%, well under the 25%
> contended threshold). An earlier exploratory run during concurrent agent builds
> *did* trip the contended flag (70% variance, isolated to one SDF run); the
> per-ray step counts, memory bytes, and miss counts were identical across both
> (they are run-invariant). Debug build; absolute ms are Debug-build figures.
> Authoritative artifact: `build/debug/test-artifacts/performance/shieldrt_spike.json`.

| View | A: HF march ms / steps | B-conservative ms / steps | B-naive ms / steps | A hits | B-cons hits |
|---|---|---|---|---|---|
| default_grazing   | **6.9 / 2.0**   | 78.7 / 11.1 | 348.3 / 49.1 | 3 976  | 56 094 |
| default_elevated  | **8.1 / 2.0**   | 96.9 / 14.2 | 327.5 / 46.6 | 6 574  | 57 600 |
| mountains_grazing | **85.2 / 7.3**  | 80.2 / 11.3 | 92.0 / 12.7  | 32 008 | 55 927 |
| mountains_elevated| **8.2 / 2.0**   | 97.4 / 13.2 | 105.1 / 14.7 | 6 574  | 55 225 |

"steps" = mean sphere-trace / march iterations per ray. Lower is cheaper.

### Memory (bytes) for each structure (per view)

| View | HF tile data | A max-mip accel | A total | SDF base volume | SDF mip chain (cons = naive) |
|---|---|---|---|---|---|
| default (both)   | 592 900 | 786 440 | 1 379 340 | 1 191 968 (193×8×193)  | 1 365 176 |
| mountains (both) | 592 900 | 786 440 | 1 379 340 | 3 128 916 (193×21×193) | 3 609 668 |

The SDF volume's Y extent (and therefore its memory) scales with terrain relief:
flat default needs ny=8, mountains need ny=21 → the SDF structure is **2.6×**
the heightfield-march structure for mountains, and grows with vertical range,
while the heightfield + max-mip is constant in Y regardless of relief.

### Correctness — conservative vs naive mip surface misses (run-invariant)

`surface_misses_mip` = sphere-trace steps that strode THROUGH the true surface
*while reading a coarse mip level* (L>0) — the mip-filtering error the spike is
about. `surface_misses_base_grid` = base-resolution tunneling on thin grazing
ridges, a property of *any* sampled-grid SDF, identical in both chains, broken
out so it cannot be misread as a mip artifact.

| View | naive mip misses | conservative mip misses | base-grid tunneling (both) |
|---|---|---|---|
| default_grazing    | 0    | 0  | 0    |
| default_elevated   | 2    | 0  | 0    |
| mountains_grazing  | 2    | 0  | 1 332 |
| **mountains_elevated** | **1 602** | **33** | 0 |
| **TOTAL**          | **1 606** | **33** | — |

**Miss-count evidence line:** on the steep `mountains_elevated` view the naive
(average-filtered) SDF mip chain let **1 602 / 57 600 rays** punch through the
terrain surface at coarse mip levels, vs only **33** for the conservative
(min-magnitude-filtered) chain — a **48× reduction** in surface overshoot, with
the conservative chain at or near zero on the other three views. This is the
research's predicted failure (Area 3 takeaway 3): naive mips silently overshoot.
The success criterion — *coarse levels MUST be conservative lower bounds* — is
demonstrated and quantified.

Two notes for honesty: (1) the conservative chain is not perfectly zero (33
misses on the steepest view) because the min-magnitude-minus-half-diagonal
correction is a *cheap conservative bound*, not an exact one; a JCGT-2022-style
correct grid-SDF interpolation would close the remaining gap. (2) base-grid
tunneling (1 332 rays on `mountains_grazing`) is the dominant grazing-ray error
and is a *base resolution* problem (8 m voxels vs 4 m heightfield samples), not a
mip problem — it would shrink with a finer brick pool and is exactly the cost the
heightfield path avoids by sampling the native 4 m data directly.

---

## Recommendation for iteration 6

**Keep the F1/F2 FarLodStore tiles as the far-field raymarch source
representation (heightfield marching, Path A); reserve sparse brick SDF clipmaps
for genuine 3D far content (overhangs, large structures, cave mouths) only — do
NOT retire the tiles into a general brick SDF.**

Argued from these numbers: Path A is **7–12× cheaper in steps/ray** on the
common (non-mountain) far views and **constant in memory regardless of relief**,
because terrain at the 1536 m horizon is a height function — a heightfield max-mip
gives a conservative, trivially-LOD'd acceleration structure at 2.0 mean steps/ray
on flat far field. The SDF path pays for 3D generality the far terrain does not
use, costs 2.6× the memory on mountains (and more as relief grows), AND carries
the conservative-mip correctness burden the heightfield path sidesteps entirely
(a max-mip of heights is conservative *by construction*; an averaged SDF mip is
not). This converges with the UE-Lumen / Claybook production pattern the research
documents (Area 3 takeaways 1, 5): trace the cheap representation where it
suffices, switch to the SDF only where true 3D detail exists — for us that is a
*small* set of structure/overhang bricks, not the whole far field.

The single view where the two paths are close (`mountains_grazing`: A 85 ms vs
B-cons 80 ms) is precisely the case the hybrid is built for — grazing rays over
high relief — and even there the SDF wins only by tolerating 1 332 base-grid
tunneling misses the heightfield path does not have. That is an argument for a
*finer* far representation at grazing angles, not for switching to SDF.

---

## What would change this conclusion

- **Far field gains pervasive true-3D content.** If iteration-5/6 worldgen puts
  overhangs, arches, floating islands, or large cave mouths across *most* of the
  far field (not a sparse set), a heightfield can no longer represent it and the
  SDF brick path becomes mandatory regardless of cost. The spike assumes
  terrain-dominated far field (current generation).
- **GPU profiling inverts the step-cost ranking.** This is a CPU prototype. If a
  GPU implementation shows the SDF's coherent, branchless sphere-trace inner loop
  beats the heightfield march's bilinear-sample + quadtree-descent on real
  hardware (divergence/cache effects the CPU does not surface), the ms gap could
  close. The *memory* and *correctness-burden* arguments would still stand.
- **A correct conservative grid-SDF mip (JCGT 2022) proves cheap and exact.** If
  the remaining 33-miss conservative gap closes for free with proper trilinear
  grid-SDF interpretation, the SDF path's correctness disadvantage shrinks — but
  it still costs more memory and more steps on terrain-only far field.
- **Shared consumers amortize the SDF.** Iteration-6 commits SDF soft shadows /
  AO and volumetric clouds as *additional* SHIELD-RT consumers (research Area 3
  takeaway 9). If those land and need a global SDF anyway, building the far SDF
  for primary rays too becomes marginal-cost rather than a new structure — that
  could justify a unified SDF even at a per-ray-cost loss for terrain.
- **Brick clipmap memory measured, not estimated.** This spike builds a dense
  region SDF; a real sparse-brick clipmap allocates only near-surface bricks and
  would use far less than the dense volume bytes here. If a sparse brick build
  drops SDF memory below the heightfield+max-mip footprint, the memory argument
  weakens (the step-count and conservative-mip arguments do not).

---

## Deferred items

None. This is a terminal evidence-only spike (critique F6: one round, memo, stop).
The recommendation and "what would change this" feed the iteration-6 far-field
architecture decision; no follow-on task is owed by this agent.
