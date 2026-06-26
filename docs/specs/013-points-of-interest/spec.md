# Spec 013: Points of Interest — Photogenic Caves, Landmarks & Biome Variety

> Status: IN PROGRESS (created 2026-06-26). Follows the photography-depth spine (spec 012)
> and living-world (spec 011). Grounded in a deep-research brief (103-agent, adversarially
> verified; run wf_ae777895-822) on procedural architecture, dramatic caves, and photogenic
> landmark design. Companion to the standing audio rule.
>
> **Build approach DECIDED — HYBRID (research finding [0], high confidence):** keep the
> existing socket/jigsaw structure substrate for variety + seamless joins, but the "designed"
> read comes from the AUTHORED KIT (varied tiles + transition/corner/seam/focal pieces), NOT
> the solver — "procedural beauty is bounded by the kit, not the solver" (HAW buildings thesis;
> Stälberg/Townscaper postmortem). Reserve shape-grammars (CGA/CityEngine) for a few authored
> hero buildings, not a pipeline rebuild. WFC "determinism" = never-backtracked, NOT bit-exact;
> our solve must stay seeded/integer so world_hash holds (the placement grid already is).
>
> **Sequencing surprise (research finding [4]):** the single highest photo-payoff-per-effort win
> is NOT new geometry — it is player-controllable TIME-OF-DAY (golden hour) + weather/atmosphere
> presets in photo mode (confirmed by Ghost of Yotei's photo mode + golden-hour photography
> literature). We already have the time-of-day + weather systems AND wired luminance->sun in
> spec 012. So that is Phase 0 — near-free, and it makes EVERY future POI photogenic at golden hour.
>
> **Substrate already built (exploration-confirmed):** the screen-space god-ray / crepuscular pass
> ALREADY EXISTS (RenderPipeline.cpp:2005-2047, projects the sun to screen-space, additive blend);
> the doline / surface-break SDF carver is fully built + deterministic, just default-OFF
> (`surface_breaks_enabled`). So Phase 1 is "activate + tune + verify", not "build a pass".

## Context

"Project Capture" is a zen photography game: the player walks a procedural world and takes
compelling photos. The world has terrain, biomes, rivers, water, foliage, and a living-world
creature layer — but few **destinations**. For a photography game, points of interest are the
highest-leverage content: they give the player a reason to walk somewhere and a composition worth
framing (a silhouette catching golden hour, an arch shot through, a light shaft in a cave).

The engine already has strong, mostly-dormant substrate: a deterministic seeded **placement grid**
(`StructurePlacement`, live — stamping cairn/ruin structures), a **socket/jigsaw** structure
system (WFC-adjacent), **SDF caves** (3D noise, surface-capped) + a **dormant doline carver**, the
existing **god-ray pass**, a `.glb -> .lmesh` instanced-LOD mesh pipeline, and a queryable
**BiomeTable**. This spec turns that substrate into photogenic POIs across three pillars, sequenced
by photographic-payoff-per-effort from the research.

## Goals

- **Phase 0 — Photo environment control:** the player scrubs time-of-day (to reach golden hour) and
  cycles weather/atmosphere presets while in photo mode. Render-only, determinism-neutral.
- **Phase 1 — Dramatic cave light:** activate the dormant doline/surface-break carver so caves get
  mouths/skylights, and verify the existing god-ray pass throws dramatic light shafts through them.
  A cave becomes a place you go to *shoot the light*.
- **Phase 2 — Biome POI variety + hero landmarks:** make the live placement grid biome-aware (each
  biome a legible "district" with its own signature landmark vocabulary + density — Lynch), and add
  2-3 authored hero `.lmesh` landmarks placed with silhouette/occlusion doctrine.
- **Phase 3 — Hybrid procedural architecture:** expand the socket kit (varied tiles + corner/seam/
  focal hero pieces) so procedural ruins/temples read as *designed*.
- **Cross-cutting:** bioluminescent cave flora/crystals (light + subjects), water grottos with
  reflections, and POIs tied to photo objectives (reuse spec 012 `ObjectiveKind`).

## Non-Goals

- No rebuild of the structure pipeline around CGA shape grammars (reserved for hero pieces only).
- No new god-ray renderer (it exists); Phase 1 reuses it.
- No change to the deterministic sim contract — placement is deterministic/seeded; geometry is
  visual-only; photo-mode environment control is render-only and never advances the sim clock.
- Not a perf spec; new passes stay within budget (god-rays already shipped).

## Functional Requirements

### Phase 0 — Photo environment control (RENDER/UI, client-only)

- **FR-0.1 (time-of-day scrub).** In photo mode, player input scrubs the render time-of-day across
  a full day (reach golden hour / blue hour / midnight). While active, the photo-mode TOD OVERRIDES
  the normal per-frame `update_time_of_day` drift; exiting photo mode restores the live clock.
- **FR-0.2 (weather/atmosphere presets).** In photo mode, player input cycles weather presets
  (Clear / Fog / Rain / Snow / Storm via `RenderPipeline::set_weather`), overriding the sim-driven
  `set_weather_state` push while active; restored on exit.
- **FR-0.3 (HUD readout).** The photo-mode viewfinder shows the current time-of-day (e.g. "07:20
  golden") and weather preset, alongside the existing aperture/shutter/iso/EV readouts.
- **FR-0.4 (determinism-neutral).** TOD/weather overrides touch ONLY the render pipeline's visual
  state, never the sim day clock or `WeatherSystem` — world_hash unaffected, photo mode stays the
  client-only observer the sim-isolation guard pins.

### Phase 1 — Dramatic cave light (WORLDGEN data + RENDER verification)

- **FR-1.1 (activate dolines).** Ship a cave-rich terrain preset (e.g. `caverns`) with
  `surface_breaks_enabled=true` + tuned `surface_break_density` / `max_feature_radius` /
  `carve_smoothness` so caves get mouths/skylights. Existing presets stay default-OFF
  (byte-identical, no re-pin); the new preset owns its own hash.
- **FR-1.2 (light shafts verified).** Confirm the existing god-ray pass (RenderPipeline.cpp:2005)
  throws visible crepuscular shafts through a doline/cave-mouth when the sun is above the horizon and
  visible through the opening — the cave "money shot", best at low sun (pairs with Phase 0).
- **FR-1.3 (cave subjects — cross-cutting, optional this phase).** Seed bioluminescent flora/crystals
  (emissive materials, reuse the foliage scatter) inside chambers as light sources + photo subjects;
  water grottos reflect via the existing water system.

### Phase 2 — Biome variety + hero landmarks (WORLDGEN, deferred to next slice)

- **FR-2.1 (biome-aware placement).** Filter POI type + density by `GetBiomeAt()` so each biome reads
  as a distinct district (desert shrines, tundra cairns, forest ruins). Reuse the `StructurePlacement`
  grid; add a biome filter.
- **FR-2.2 (hero landmarks).** 2-3 authored `.lmesh` hero structures (arch, watchtower, standing
  stones) placed SPARSELY as destinations, instanced + LOD'd (the tree path).
- **FR-2.3 (silhouette/occlusion doctrine).** Place landmarks behind/near terrain so they are glimpsed
  not fully shown (sustains "what's over there" — BotW/CEDEC, Robert Yang); tall POIs get upward cues
  (framing/birds/centered silhouette — players don't look up without a trigger); contrast against the
  biome palette (Itti & Koch salience).
- **FR-2.4 (photo objectives).** Tie POIs to spec-012 objectives ("photograph the sunken arch at
  golden hour") via a new objective kind or a location+behavior match.

### Phase 3 — Hybrid procedural architecture (deferred)

- **FR-3.1 (kit expansion).** Grow the socket/jigsaw piece kit: varied tiles + dedicated TRANSITION
  pieces (corners, material seams) + focal hero pieces — the research's "beauty is in the seams".
- **FR-3.2 (seeded WFC solve).** Keep the assembly bit-exact/seeded (SplitMix64 streams) so structure
  voxel hashes stay run==replay; geometry visual-only.
- **FR-3.3 (weathering/asymmetry).** Procedural weathering + asymmetry so ruins read as designed.

## Non-Functional Requirements

- **NFR-1 (determinism).** Phase 0 render-only (no world_hash). Phase 1+ worldgen changes ship as
  NEW presets (existing baselines byte-identical, no re-pin); new-preset geometry is deterministic/
  seeded (doline salt 0xA3CA7E5 already exists). Per-preset hash pinned once when a preset stabilizes.
- **NFR-2 (reuse over greenfield).** Reuse the placement grid, doline carver, god-ray pass, `.lmesh`
  pipeline, BiomeTable, emissive materials, foliage scatter, water system, and the spec-012 objective
  system. New systems only where the substrate genuinely lacks (the kit expansion).
- **NFR-3 (perf).** No new fullscreen pass in Phase 0/1 (god-rays already shipped + budgeted). Phase 2
  landmarks use instanced LOD/impostors. Validate with `--render-benchmark` if a phase adds draws.
- **NFR-4 (photogenic-first).** Every phase is justified by photo payoff; cheap-high-impact wins first
  (Phase 0 TOD/weather, Phase 1 cave shafts) per the research sequencing.

## Acceptance Criteria

- [ ] **AC-0 (photo env control).** In photo mode, the player scrubs time-of-day (a sunrise->noon->
      dusk->night sweep is reachable) and cycles weather presets; the viewfinder shows TOD + weather;
      `--smoke` world_hash is byte-identical with photo mode entered + scrubbed (sim untouched).
- [ ] **AC-1 (cave light).** A `caverns` preset world shows cave mouths/skylights with the god-ray
      pass throwing visible light shafts through them at low sun; captured headlessly via
      `--timelapse-living` (or a scene capture) + sent as a showcase. Existing presets byte-identical.
- [ ] **AC-2 (biome variety).** [Phase 2] each biome shows a distinct POI vocabulary + density; a
      hero landmark reads as a destination (glimpsed, then approached).
- [ ] **AC-3 (no regressions).** `common_tests` green; client+server build clean; existing visual
      baselines unaffected (Phase 0/1 add no draws to existing presets).

## Suggested phasing (payoff-per-effort, research-sequenced)

1. **Phase 0 — Photo environment control** (cheapest, highest payoff; reuses set_time_of_day/weather).
2. **Phase 1 — Dramatic cave light** (activate dolines + verify god-rays; mostly data + verification).
3. **Phase 2 — Biome variety + hero landmarks** (biome filter on the placement grid + authored meshes).
4. **Phase 3 — Hybrid procedural architecture** (kit expansion; deepest, last).

## Open Questions

- **OQ-1.** Photo-mode TOD scrub granularity: continuous drag vs stepped presets (dawn/noon/dusk/night)?
  Lean continuous + snap labels.
- **OQ-2.** Should the `caverns` doline preset be a standalone preset or a knob on existing presets
  (FeatureDensity)? Standalone first (no re-pin); knob later.
- **OQ-3.** Hero landmarks: source authored `.glb` meshes (needs art) vs grey-box placeholders first?
  Placeholder geometry first to prove placement/objective loop.
- **OQ-4.** Bioluminescent cave subjects: reuse plant species (emissive flag) vs a new crystal scatter?

## References (grounding — current code + research)

- Placement: `world/StructurePlacement.h/.cpp` (`SiteInCell`/`SitesInArea`, seeded grid).
- Caves + dolines: `systems/SHIELD_WorldSystem.h` (cave params L36-40, surface-break params L61-66),
  `.cpp` `sample_surface_breaks` (~L969), `sdCappedCone` (~L179), salt `kSurfaceBreakSalt` (~L76).
- God-rays (EXISTING): `rendering/RenderPipeline.cpp:2005-2047` (sun screen projection + additive blend).
- TOD/weather: `RenderPipeline.{h,cpp}` `set_time_of_day` (L607), `update_time_of_day` (L4342),
  `set_weather`/`WeatherType` (L641/129), client push `main_client.cpp` (~L4407, ~L1648 render).
- Photo mode: `main_client.cpp` photo block (~L6838), `game/PhotoMode.h`, `data/ui/photo_mode.rml`.
- Mesh pipeline: `tools/asset_processor.cpp` (.glb->.lmesh), `rendering/TreeLod.h` (LOD/impostor).
- Objectives (spec 012): `game/Objectives.h` (`ObjectiveKind`, `BehavioralMatch`).
- Research brief: deep-research run wf_ae777895-822 (8 verified findings; HAW thesis, Townscaper/
  Stälberg, CityEngine/CGA, GPU Gems 3 god-rays, BotW/CEDEC, Journey/Lynch, Itti & Koch, Ghost of Yotei).
