# Spec: Create-World — Preview, Semantic Knobs, E2E Coverage, Layer Graph

> Critique-hardened (4-way devil's-advocate, 2026-06-20). **Owner decision: land all 4 items in one
> push.** To keep that safe, the critics' risk mitigations are baked in: Item 4 ships as a
> **constrained** graph that compiles to the *existing* `TerrainGenParams` (NOT a free-topology
> evaluator — that would require rewriting the determinism-pinned worldgen core and is an explicit
> non-goal), built on the existing ImGui `debug/WorldGenViewer`, behind a flag. Item 2 uses the
> **separate-persisted-layer** model (owner decision). Two blockers (gate honesty, preview spike)
> still land first. Report: `.forge/critique-create-world-deferred-20260620-234835.md`.

## Context
Create-world exposes ~30 raw worldgen params (seeded from presets, embedded per-world,
save-as-named-preset). This spec adds the three durable, day-to-few-days follow-ups that cohere
(all UI over the *existing, stable* `TerrainGenParams` model, all testable in the existing headless
harness). The node-graph editor is a separate multi-week epic and is explicitly out of scope here.

Substrate: `Systems::SHIELD_WorldSystem(JobSystem*, WaterSystem*, params, seed)` + `GetTerrainHeightAt`
/`GetTerrainHeightAtCoarse`; `LoadTerrainPreset`; `Client::BuildCustomPreset`; RmlUi GL3 (TGA-only
images, `GenerateTexture` for raw bytes); `Client::WorldParamGetter`/`WorldPresetSaver`/`List`;
`test/ui/ui_smoke_test.cpp` (drives the real manager headless); **existing** ImGui
`debug/WorldGenViewer` that already does `RecreateWorldSystem` + `RegenerateTexture` + apply-params.

## Non-Goals
- A **free-topology** node evaluator that re-derives terrain from an arbitrary DAG (would require
  rewriting the determinism-pinned scalar/SIMD/far-LOD generation core; it's a future engine effort,
  not this push). Item 4 here is a **constrained** graph over the fixed pipeline only.
- Any refactor of `TerrainGenParams` / `LoadTerrainPreset` (the worldgen core is determinism-pinned
  and concurrently edited — **additive seams only**).
- A pixel-accurate WYSIWYG preview of the final rendered world (v1 preview is explicitly labeled).
- A shipping-grade cinematic RmlUi graph canvas (Item 4 v1 is a flagged ImGui authoring tool).

---

## BLOCKER 1 — Item 3a: gate honesty (must land before any new UI test is trusted)
**Problem:** `ctest` counts `GTEST_SKIP` as pass; the behavioral `ui_smoke_test` cases skip without a
GL context; `ctest_manifest.json` hardcodes `"passed": true`; the validator only checks the manifest
*lists* test names. So the UI suite is green-on-zero-coverage if the gate ever lacks GL, and
`RedesignedControlsAreFunctional`/`SaveAndListUserPresetsAreFunctional` aren't even pinned.
**FR-3a:**
- Promote skip→failure for `required_ui_tests`: `set_tests_properties(... SKIP_RETURN_CODE 99)` +
  run with `ctest --no-tests=error`, and a post-ctest assertion (parse `Testing/Temporary/`) that
  **0 of the pinned UI tests skipped**.
- If the gate may run headless, provision a software-GL fallback (Mesa llvmpipe `opengl32.dll` shim)
  so a 4.5 core context exists; else document the gate as GL-required.
- Pin `RedesignedControlsAreFunctional`, `SaveAndListUserPresetsAreFunctional` (and all 3b tests) in
  the validator's `required_ui_tests`. Separate a GL-free contract from GL-required assertions
  (mirror the WaterfallVisual `gl_skip_reason` pattern).

## BLOCKER 2 — Preview-cost spike (must precede Item 1 commit)
Measure: per-regen `SHIELD_WorldSystem` construction cost + memory (it loads the biome table + walks
the structures dir in `reinitialize_noise`, and the first `GetTerrainHeightAt` triggers a hydraulic
bake whose cache lives on the throwaway world). Confirm the 96² sample budget. Decide the params
boundary (in-memory vs temp file) **without refactoring the loader**. Outcome reshapes Item 1 + AC-1.

---

## Item 1 — Live preview (HIGH ROI)
**Goal:** a small thumbnail of the *current* params that updates as you tune, so it's a feedback loop.

**Design (post-critique):**
- **Truthful image, not a bare heightmap.** `GetTerrainHeightAt` is elevation-only (no biome color,
  no water — rivers/lakes are *carved* so they'd read as dark pits). v1 preview MUST be one of:
  (a) **render the real low-res pipeline to a small FBO** (preferred; reuse `WorldGenViewer`'s
  `RegenerateTexture` path), or (b) a thumbnail colorized by **biome id** + the **sea plane** drawn
  at sea level, **labeled "elevation preview — not final look."** Never ship an unlabeled hypsometric
  ramp that misrepresents the world.
- **One cached preview world**, reused via `set_params`/`set_seed` (NOT reconstructed per regen);
  keep its hydro cache warm. For a height thumbnail, sample with hydro/biome/structures bypassed.
- **Synchronous** sample+encode on the main thread (the UI has no JobSystem; texture upload is
  GL-thread-only). Upload via **`GenerateTexture`** (raw bytes) — no temp file, no `LoadTexture`
  cache-staleness. Debounce ~250 ms; latest-wins via a generation counter.
- **Cache invalidation:** assign the new texture handle directly (GenerateTexture) or
  `ReleaseTextures` the prior preview; do NOT rely on "toggle src".
- **Sampling:** window ≥1 continentalness wavelength (~1–2 km) so macro knobs read; supersample
  (2×2) or use `GetTerrainHeightAtCoarse` so river carve/cliffs don't alias.
- **Failure fallback:** if params don't load, show a placeholder thumbnail + a non-blocking note
  (the loader can be transiently RED during the concurrent-edit window).

**Tests:** `WorldgenPreview` unit — same params→identical output; amplitude↑→measurably higher relief
stat; encoder under a pinned perf budget (own `TerrainGenParams` literal, **not** `default.json`).
Logic test: 5 rapid regen requests → exactly one encode (newest). A render-doesn't-crash headless test.

## Item 2 — Semantic knobs (default surface) — gated behind Item 1
**Goal:** ~6 outcome knobs as the default surface; raw 28 demoted to an "advanced" fold.

**Design — separate persisted layer (OWNER DECISION; the lossy one-way lerp is rejected):** persist a
knob vector + a sparse raw-override diff; apply = `applyKnobLayer(knobs)` then overlay `overrides`.
Both surfaces stay honest (knob shows knob intent; advanced shows the override delta); reopen is
exact. Curated presets carry no knob layer → knobs render **neutral ("preset / custom")**, never
inverse-lerped; engaging a knob snapshots the preset as the override baseline so authored splines
survive. The knob vector + override diff persist in the world save AND user presets alongside the
resolved params.

**Requirements:**
- Knob→param relationships are **tuned response curves** (per-param splines) with cross-knob coupling
  and ramped enable-flags (no discontinuous feature pop), **validated by a monotonicity test** that
  sweeps each knob 0→1 and asserts Item 1's relief/roughness metric moves monotonically.
- Bind through the `WorldgenOverride`/`WorldParamGetter` **bridge**, not raw RML elements, so a later
  graph layer can't strand the knobs.
- **One canonical param-descriptor table** (range/step/label/default) feeds the RML min/max AND the
  knob map; startup assertion that knob endpoints lie within declared ranges.

**Tests:** knob extremes hit expected params; monotonicity sweep; reopen-stability; e2e that moving a
knob moves the mapped controls.

## Item 3b — e2e coverage for the rest of the UI
Pause `resume_btn`/`quit_menu_btn` (assert the callback arg string), `settings_btn`/`gallery_btn`
nav; gallery back-arrow nav + card/pager presence; world-select `load_selected_btn` empty-selection
guard (no callback); `ReloadActiveDocument()` reload; world-select/gallery `<img src>` resolves to an
existing TGA. All pinned and asserted to **run, not skip** (depends on 3a).
**Also:** user-preset management — delete/rename + overwrite-collision confirm (current
`WorldPresetSaver` silently overwrites on slug collision) + a slug-collision test.

## Item 4 — Constrained layer graph (flagged authoring tool)
**Goal:** author worldgen as a small graph of the *existing* fixed stages, so the layered structure
is visible/editable — WITHOUT a free-topology evaluator (determinism non-goal).

**Design (de-risked per critique):**
- **Scope = constrained, not free.** The graph is a fixed-topology DAG mirroring the real pipeline
  (base FBM → domain warp → splines → biome relief → rivers → lakes → cliffs → hydro). Nodes expose
  the same scalars as the flat params; edges are the fixed stage order (mostly read-only in v1). The
  graph **compiles bit-exactly to `TerrainGenParams`** — so `world_hash`/run==replay/far-LOD are
  unchanged. A free evaluator (custom nodes, arbitrary topology) is a separate future engine effort.
- **Canvas = evolve `debug/WorldGenViewer`.** Build the node UI on the existing ImGui inspector
  (which already does `RecreateWorldSystem` + `RegenerateTexture` + apply-params), behind a
  `--worldgen-graph` flag. It is an **authoring/dev tool**, not the shipping cinematic create surface
  (that stays RmlUi knobs + params). Item 1's preview is the graph's viewport.
- **Persistence:** the graph serializes to `generation_params.graph` in the preset/world save; load
  prefers the embedded params (graph is provenance/editing metadata). Save-as-preset persists it.
- **No core refactor:** additive only; the compile target is the existing struct. Do this LAST, and
  pin its determinism test against a fixed literal, not `default.json`.

**Tests:** graph round-trip (serialize==deserialize); `compile(graph)` is **bit-exact** to the source
`generation_params` (field-for-field JSON); a golden graph reproduces a curated preset's params exactly.

---

## Acceptance criteria (testable)
- [ ] AC-3a: with the pinned UI tests forced to skip, the gate **fails** (skip≠pass); with GL present
      all pinned UI tests run green.
- [ ] AC-spike: documented per-regen cost + chosen preview path; AC-1 budget set from it.
- [ ] AC-1: `WorldgenPreview` encode completes under the pinned budget on its own params literal; 5
      rapid requests → exactly one (newest) encode (debounce/latest-wins). (No wall-clock e2e.)
- [ ] AC-1b: the preview image is labeled/colorized so it is not mistaken for the final render (water
      reads as water, not pits).
- [ ] AC-2: 6 knobs drive the params via response curves; each knob's relief metric is monotone over
      0→1; reopening a saved world re-derives knob positions within a stated tolerance (model A: exact).
- [ ] AC-2b: curated presets are never flattened by a knob touch (authored splines preserved).
- [ ] AC-3b: pause/gallery/world-select-guard/hot-reload/thumbnail + user-preset delete/rename/
      collision interactions are e2e-tested, **run (not skipped)**, and pinned.
- [ ] AC-4: a layer graph round-trips; `compile(graph)` is **bit-exact** to the source
      `generation_params`; a golden graph reproduces a curated preset's params field-for-field.

## Sequencing (all 4 in one push)
3a (gate honesty) → preview spike → Item 1 (preview) → 3b (interaction tests) → Item 2 (knobs) →
Item 4 (constrained graph, LAST — additive compile to existing params; pin to a fixed literal).
Item 4 is gated behind a flag and built on `WorldGenViewer`; if the concurrent worldgen agent is mid
header-edit, force-recompile stale objects and proceed additively.

## Resolved decisions
- Item 2 model: **(A) separate persisted layer** (owner).
- Item 4: **constrained graph compiling to existing params**, flagged, on `WorldGenViewer`; free
  evaluator is a non-goal (owner: all 4 in one push, de-risked this way).
- Preview path: decided by the spike (real-pipeline FBO preferred; labeled biome+sea thumbnail floor).
- Gate GL: confirm capability or ship the software-GL fallback (in 3a).
