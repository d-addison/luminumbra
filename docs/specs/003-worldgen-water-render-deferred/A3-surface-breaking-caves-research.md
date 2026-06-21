# A3 Research Brief — Surface-Breaking Caves / Sinkholes / Arches

> Produced by a 5-agent ultracode research workflow (2026-06-21), synthesized from IQ
> distance-functions, Minecraft 1.18 noise caves, Paris et al. ACM TOG 2019 (construction-tree
> implicit features), doline power-law (β≈2.5) literature, Zylann voxel-tools, and the NMS GDC talk.
> This grounds the **re-scoped FR-A3** (owner chose surface-breaking features over deferral).

## Load-bearing mechanism (all 4 facets converge on this)
Make the surface cap a **per-column field** instead of a global constant, plus ONE bounded analytic
carve term in `apply_cave_field`:

```
effective_cap(x,z) = mix(kCaveSurfaceCapDepth /*18*/, 0, featureMask(x,z))
```

The EXISTING cave noise (`m_cave_generator->GenSingle3D`) reaches the surface ONLY inside hashed
feature footprints; sealed at 18 m everywhere else. This reuses 100% of the cave machinery, is
**byte-identical wherever `featureMask==0`** (one deliberate re-pin), and is the cheapest route to
"caves visible from the surface."

Three feature types layer on the SAME hashed placement, in payoff order:
1. **Cave mouths** — cap exemption gated by an interior-proximity probe (one extra `GenSingle3D` at
   `y = surface - capDepth`; lift the cap only if that probe is already carved → never punch a blind pit).
2. **Sinkholes/dolines** — subtract an IQ inverted `sdCappedCone` (funnel) or `sdVerticalCapsule`
   (cenote shaft), sized from a truncated power law (β≈2.5, 6–120 m, depth ≈ 0.2–0.5·D),
   `opSmoothSubtraction` with small k; the one feature allowed past the 18 m cap, connecting into the cave field.
3. **Natural arches** — additive capped-torus/elongated span `smin`'d into terrain for the legs, minus a
   horizontal bore capsule, hardness-gated.

## Determinism (reuses shipped primitives)
- Placement = pure unsigned fn of integer cell coords + seed via `SplitMix64` + `CellSeed`
  (`StructurePlacement.cpp:33-62`, FNV1a-seeded splitmix, all-unsigned, no `int*prime` UB) with a NEW
  distinct salt `kSurfaceBreakSalt`. Coarse feature-cell grid (~128 m sinkholes, ~64 m arches), scan a
  FIXED 3×3 neighborhood, decode per-feature params from successive `SplitMix64` lanes. `FloorDiv` for
  negative coords. Each primitive has hard finite support `R < cell_size` (asserts the 3×3 scan suffices).
- **Order-independence:** combine caves+features with ONLY commutative+associative ops — hard `std::max`
  by default (crisp mouths); **exponential** smin `-k*log2(exp2(-a/k)+exp2(-b/k))` for soft lips —
  NEVER the quadratic/cubic polynomial smin (non-associative → SIMD/seam divergence).
- Carve is VISUAL/terrain-field ONLY (same rule as procedural trees): the integer sim reads the resulting
  density classification, never the float carve math.

## SDF changes (file-level)
- `SHIELD_WorldSystem.cpp` anon-namespace helpers (lines ~65-88): `cave_surface_blend(td)` →
  `cave_surface_blend(td, effective_cap)`; `surface_capped_cave_density`/`apply_cave_field` gain
  `effective_cap` + `feature_carve` args. `apply_cave_field` body: `caves = max(td, capped); return
  max(caves, -feature_carve)` (CSG subtraction, order-free). New shared helper
  `sample_surface_breaks(world_pos, surface_h, seed, params) -> {effective_cap, carve}`.
- **Four CPU call sites must route through it identically** (byte-identical float-op sequence, absolute
  world coords): `get_density_at_from_precalculated` (~2531), per-point `SampleWorldGenLayers` (~2504),
  marching-cubes solid-branch probe (~1392-1398), SIMD 3D-buffer carve loop (~2981).
- **GPU parity:** mirror the helper + constants into `res/shaders/sdf_generation.compute` `calculateSDF`
  (~98-115; `CAVE_SURFACE_CAP_DEPTH` ~46); port `CellSeed`/`SplitMix64` to GLSL `uint` (same magics, same
  3×3 order). `test_sdf_gpu_cpu_parity.cpp` enforces CPU==GPU.
- **Heightfield rim** (far-field/coarse visibility): fold a 2D rim depression into
  `ComputeShapedHeightSampleImpl` (~401) + the SIMD batch + `GetTerrainHeightAtCoarse` (~787, after the
  lake carve) so dolines DIP the heightfield even on the SDF-ignoring coarse path.

## Far-field (makes the original FR-A3 meaningful)
Far field is currently pure 2.5D (`FarLodTile` = height_q+material+flags; no volumetric channel) — that's
why FR-A3 had nothing to show. Stage 1 (free): the rim depression flows into far tiles automatically via
`GetTerrainHeightAt` (bump `ComputeTerrainParamsHash` → pristine tiles rebuild). Stage 2 (real void): add
a `kFarLodSampleFlagCaveMouth` (or span byte) to `FarLodTile`, write it in `BuildPristineFarLodTile`, and
have `GenerateFarLodRegionMesh` open/darken those samples + stamp dark/rock material (Wave B1). Shared
border row keeps seams crack-free; near 3D throat collapses to far 2.5D bowl+mask (keep rim radius ≥ a few
far samples so it doesn't pop across tiers).

## Phasing (de-risked)
0. **Plumbing, BYTE-IDENTICAL (no re-pin):** add `TerrainGenParams` {`surface_breaks_enabled=false`,
   `surface_break_density=0`, `feature_cell_size`, `max_feature_radius`, `carve_smoothness`,
   `entrance_min_cap`} (wire `TerrainPresetLoader`, default OFF). Generalize the cap helpers; add an empty
   `sample_surface_breaks` returning `{18,0}`; route all four call sites through it. Verify `world_hash`
   UNCHANGED via `test_worldgen_layer_snapshots`.
1. **Cave mouths** (highest payoff/lowest risk): placement + cap-exemption + interior-proximity probe; hard max.
2. **Sinkholes/dolines:** cone/capsule carve, power-law sizing, optional exp-smin lip; rim into all 3 height paths.
3. **GPU parity:** port to `sdf_generation.compute`; `test_sdf_gpu_cpu_parity` green.
4. **Far-field void** (the deferred FR-A3): `FarLodTile` mask channel + far mesher open/darken + material stamp.
5. **Natural arches** (fast-follow): additive torus/span + bore capsule, hardness-gated.
6. **Single re-pin + gates:** re-pin `test_worldgen_layer_snapshots` ONCE (local-dev policy), re-verify
   GPU/CPU parity + run==replay, re-bless visual gates. All-off baseline stays byte-zero.

## Top risks
Non-associative smin (use hard max / exp-smin only); CPU↔GPU GLSL `uint` hash parity (most fragile);
marching-cubes "flyover" shards near support boundary (small k, conservative fade); blind pits (mandatory
proximity probe); far-field Stage 2 is real schema+mesher work (own phase); the rim must be folded into
ALL THREE height paths or near/far/coarse disagree; enforce `max R < cell_size`.

## Sources
IQ distfunctions / smin / fbmsdf (iquilezles.org); Minecraft 1.18 noise caves (minecraft.wiki/w/Cave);
Paris et al. ACM TOG 2019 (dl.acm.org/doi/10.1145/3342765); doline power-law β≈2.5 (arxiv 1810.07868);
Zylann SdfSmoothSubtract (voxel-tools.readthedocs.io); NMS continuous worldgen (gdcvault 1024265).
