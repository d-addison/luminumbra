# Spec: Worldgen / Water / Render — Deferred Roadmap

> Research-hardened (3 Explore agents + 2 Plan agents, 2026-06-21) against the 2026-06-21
> handover. **Owner decisions baked in:** structures → TRUE VOXEL stamping; far-field SDF →
> include now (hybrid cave-span design); climbing → traversal-lite; stale visual gates →
> re-bless to current noon look. Companion to `docs/specs/002-create-world-deferred/`
> (shared files `main_client.cpp`, `TerrainPresetLoader.*` — additive seams only).

## Context
A worldgen/water/render session (HEAD `72a9f7d7`, branch `feat/polyglot-audit-roadmap`)
landed view-distance, elevation/flat lakes, the amplified preset, the water shader +
particles + foliage, and hydro-via-prefetch, then handed off 10 deferred items. This spec
captures the remaining work as a single, determinism-safe push: render-only improvements
land first (no re-pin), world_hash-affecting terrain changes are batched into **one**
re-pin, and perf/traversal/gate work lands last.

Substrate (verified): `Systems::SHIELD_WorldSystem` (`WaterLevelAt`, `LakeSurfaceLevel`,
`LakeCarveAmount`, `ContinentalBaseHeight`, `SampleHydroOffsetMeters`, `PrefetchHydroRegions`,
scalar/batched/coarse height paths, `GenerateChunkData`); `World::SitesInArea`/`AssembleStructure`
(`StructurePlacement.{h,cpp}` — built, unused); `MarchingCubes` (`GenerateCoarseHeightfieldTerrain`,
`GenerateFarLodRegionMesh`, `GenerateWaterMesh`, vertex material PASS 1/1b); `FarLodStore`/`FarLodSystem`;
`WaterfallDetect.{h,cpp}` (already wires `waterfall.frag` to river-outlet drops);
`BuildProcgenRockPalette` + `FoliagePass`; `PhysicsSystem` (Jolt `CharacterVirtual`, 50° max slope);
`WorldPersistenceRoundtrip` (chunk JSON + sub-hashes); `.forge/scripts/validate-engine-frontier.ps1`
(gates + pinned hashes).

## Goals
- Authored structures (cairn/ruin) actually appear in the world as **mineable voxels** with
  authored materials (new per-voxel material channel on the chunk SDF).
- Caves / overhangs / runtime edits remain visible **at distance** (true far-field SDF).
- Inland lakes read perfectly flat even across 768 m cell boundaries.
- Far-field and amplified-preset hydro have no near/far drainage seam.
- Water/foam/waterfalls verified clean in-engine; a bush/shrub foliage layer adds depth.
- Player can step up / mantle short ledges (traversal-lite); frame budget reaches ~300 fps.
- Stale noon visual gates re-blessed to a confirmed look; determinism goldens re-pinned once.

## Non-Goals
- A full cliff-climbing state machine (hand-holds / stamina) — traversal-lite only this push.
- Far-LOD per-voxel structure materials (structures classify analytically at distance — documented follow-up).
- Free-topology worldgen evaluator (out of scope; see spec 002 non-goals).
- Any change to the create-world UI surface (owned by spec 002).
- Automating the backup push (owner-run only).

## Functional Requirements

### FR-A · Render-only (no world_hash re-pin)
- **FR-A1 (water/foam/waterfalls):** Verify in-engine (`waterchk`/`lakes` scenes) that smooth
  value-noise foam removed the grid glitches and that `WaterfallDetect`-found outlets render
  `waterfall.frag`. If grid artifacts persist, fix water-mesh resolution / surface z-fight
  (`kFarWaterDepthBiasMeters`), not the foam. SSR/caustics tuning in `res/shaders/water.frag`
  only as needed.
- **FR-A2 (bush/shrub layer):** Add a bush palette (reuse `BuildProcgenRockPalette` icosphere,
  ~0.3–0.6× scale, denser clustering) scattered by `FoliagePass` and driven by `BiomeTable`
  vegetation density. Render-only (vegetation is out of `content_hash`).
- **FR-A3 (surface-breaking caves + far-field SDF) — RE-SCOPED (owner, 2026-06-21).** Red-team
  found the original "hybrid far-field cave" plan UNSOUND: `apply_cave_field = max(terrain_density,
  surface_capped_cave_density)` only carves DOWNWARD and the 18 m surface cap means caves NEVER break
  the surface or overhang → far-field cave geometry would be fully occluded by the heightfield skin
  (renders nothing). Owner chose to **add genuine surface-breaking features** so there is something to
  show: deterministic **sinkholes / arches / cave-mouths** authored into the SDF (lower/condition the
  18 m cap where a feature is placed), which is a **near-field worldgen change → world_hash-affecting
  → re-pin** (joins the Wave-B cluster). THEN the far-field reduction (`SampleCaveColumnCoarse` single
  source; sparse `cave_column_offsets`/`cave_spans` on a **versioned** `FarLodTile` with byte-zero
  empty-case hashing so disabled worlds stay byte-identical; cave geometry only on flagged columns;
  F1 full / F2 mouths / fade) renders them at distance, with the seam-fallback (`ReadTransitionFace…`)
  and boundary skirts taught to consult the same cave samples so the live/far boundary doesn't crack.
  Research DONE → see `A3-surface-breaking-caves-research.md`. Load-bearing mechanism: make the 18 m
  cap a **per-column field** `effective_cap(x,z)=mix(18,0,featureMask(x,z))` so cave noise reaches the
  surface ONLY inside hashed feature footprints (byte-identical when disabled). Reuses `SplitMix64`/
  `CellSeed`; hard-max/exp-smin only (order-free); 7-phase rollout (Phase 0 byte-identical, no re-pin).
  All hashing unsigned; guard normalize/division.

### FR-B · World_hash-affecting (batched → ONE re-pin)
- **FR-B1 (material channel + voxel structures) — red-team-hardened.** Add lazily-allocated
  `material_data` **+ `pending_material_data`** (`std::vector<u8>`) to `Chunk`. The stamp helper runs
  **inside `GenerateChunkData`** (NOT only at the tail) so it executes for the off-thread LOD0
  scratch generation too; called from BOTH the CPU branch (before `clear_voxel_data_dirty`) AND the
  GPU branch (before its early return, after `sdf_data` is full); explicitly **skipped on the step>1
  coarse path**. The meshing job must move `scratch.material_data → chunk->pending_material_data` and
  `process_completed_meshing_jobs` must publish it alongside `pending_sdf_data` (else promoted chunks
  lose structure materials → run≠replay). Enumerate `SitesInArea` over the chunk AABB **padded by a
  per-pool footprint radius** (cairn boxes have negative mins → boundary structures); drop each site
  to a **single integer floor of `GetTerrainHeightAt(site.x,z)` computed once per site** (identical
  across all chunks/paths); skip sub-`SEA_LEVEL` sites; stamp only in-bounds voxels (`sdf_data=-1` +
  material). Marching cubes (first-writer-wins edge cache, fixed scan order): PASS 1b classifies
  analytically, then re-applies stored material only where it is `!=0` — empty `material_data` ⇒
  byte-identical. Persistence: add to `ChunkToJson`/`ApplyChunkJson` (**tolerant read; do NOT add to
  `RequiredChunkFormatFields`** — breaks old-save back-compat + the single-missing-field negative
  fixture), `PersistedFields`, and the **terrain** sub-hash (empty vector serializes `[]` ⇒ byte-zero
  for structures-off worlds). Mining clears `sdf_data`+`material_data` (find the LIVE edit path, not
  just the harness). Lazy alloc = exact padded volume, guard `.empty()` everywhere. Far-LOD carries
  no structure material (documented gap). **Re-pin ONLY the gates whose preset actually enables
  structures** — `structures_enabled` defaults false, so the empty-roster gates likely DON'T move
  (re-pinning them would turn green→red); verify each before touching, include line ~5485 if it moves.
  All footprint/coord math unsigned/guarded.
- **FR-B2 (lake spill-level):** Replace the 768 m snap in `LakeSurfaceLevel` with a per-basin
  flood-fill / spill-level so boundary-straddling lakes don't step; keep `WaterLevelAt`, the carve,
  and the `WaterSystem` rest-clamp consistent with the single source of truth.
- **FR-B3 (far/amplified hydro):** Prefetch far hydro regions and sample `SampleHydroOffsetMeters`
  in the far-LOD bake to remove the near/far drainage seam; set `amplified.json` `hydro.enabled=true`.

### FR-C · Traversal / perf / gates
- **FR-C1 (traversal-lite climbing):** In `PhysicsSystem` add step-up / mantle assists for steep
  faces and short ledges around the two `mMaxSlopeAngle=50°` sites and the `CharacterVirtual` update;
  no climb state machine. Deterministic (30 Hz sim).
- **FR-C2 (300 fps):** Add impostor/LOD for the instanced tree+rock set (none today) and trim the
  skybox pass. Hold the `RenderBudget` gate (skybox ≤1.5 ms, ssao ≤0.7 ms, total ≤3.33 ms).
- **FR-C3 (re-bless gates):** Render current noon `SkyboxVisual`/`PlayerView`, confirm the look with
  the owner (PNG), then re-bless the baselines.

## Non-Functional Requirements
- **NFR-1 (determinism):** `run==replay` byte-exact must always hold. All procgen hashing uses
  unsigned math; guard every `normalize(cross())`/division (release-only UB). After FR-B lands,
  re-pin the golden literals in `validate-engine-frontier.ps1` once (lines ~4688, ~5213, ~5331, and
  ~5435/~5485 if their gates go red); regenerate the persistence test-artifact fixtures.
- **NFR-2 (memory):** `material_data` lazily allocated (~4.8 KB only on structure-bearing chunks);
  far-tile `cave_spans` sparse (≈0 on cave-free regions).
- **NFR-3 (realism):** The 5 natural presets keep TerrainRealism β ∈ [1.8, 2.2] (lower `height_offset`,
  don't add amplitude); `amplified` stays `realism_exempt`.
- **NFR-4 (concurrency):** `main_client.cpp`, `TerrainPresetLoader.*`, `Chunk.h`,
  `SHIELD_WorldSystem.h`, `MarchingCubes.cpp` are concurrently edited — additive seams only;
  force-recompile stale objects after header edits.
- **NFR-5 (build hygiene):** Prepend `C:/msys64/ucrt64/bin` to all build/ctest/gate calls; build the
  tree you test (`--preset release` vs `--preset debug`); kill the client before relinking.

## Acceptance Criteria
- [ ] AC-A1: in-engine captures show no foam grid glitch and a rendered waterfall at a detected outlet.
- [ ] AC-A2: bushes scatter by biome density; render-only (no world_hash change); foliage capture shows them.
- [ ] AC-A3: a cave mouth / overhang visible in BOTH a near and a ~1000 m far capture of the same site;
      coarse↔far↔full-res cave-parity gtests green; far-tile build holds its frame-budget share; no re-pin.
- [ ] AC-B1: a known cairn's Stone voxels appear in its chunk (`material_data` + mesh `material_id==1`);
      mining removes them; persistence roundtrip preserves `material_data`; determinism test pinned to a
      FIXED `TerrainGenParams` literal passes; chunk-format-validation updated.
- [ ] AC-B2: a lake straddling a 768 m boundary has a single flat surface (no step) in-engine.
- [ ] AC-B3: no near/far drainage seam in a far capture; `amplified` renders with hydro on, smooth.
- [ ] AC-Bpin: all determinism gates (PopulatedWorldReplay, ReplayRoundtrip, LockstepLoopback) + TerrainRealism + BiomeCoverage green after a single re-pin.
- [ ] AC-C1: player steps up a short ledge / mantles a low wall that previously blocked them; run==replay holds.
- [ ] AC-C2: `RenderBudget` gate green at total ≤3.33 ms on a fixed camera pose (toward ~300 fps).
- [ ] AC-C3: noon `SkyboxVisual`/`PlayerView` gates green against owner-confirmed re-blessed baselines.

## Sequencing
Wave A (A1 → A2 → A3, render-only, no re-pin) → Wave B (B1 → B2 → B3, then ONE re-pin) →
Wave C (C1 → C2 → C3). Item 1 (backup push) is owner-run and never automated; a `git bundle`
is prepared and the exact `git push backup …` command handed over.

## Open Questions
- None blocking. FR-C1 climbing depth and FR-C3 noon look are owner-confirmed (traversal-lite;
  re-bless current). Far-LOD structure materials are an accepted documented follow-up.
