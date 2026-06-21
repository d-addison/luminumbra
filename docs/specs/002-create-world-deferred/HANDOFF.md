# HANDOFF — Create-World Deferred Items (spec 002)

**For:** the next dev/session picking up the create-world feature set.
**Owner intent (latest):** the create-world screen should center on a **Photoshop-style live preview
pane** — a rendered 3D **slice/diorama** of the candidate world you **orbit by dragging the mouse**
(turntable, not a flythrough), that updates **in real time** as you slide worldgen knobs or change
**weather / time-of-day**. Water reads as water, **waterfalls show**, the sky is the real atmosphere.
**Read `spec.md` (this folder) first** — it's the critique-hardened source of truth. Then this doc for
state, gotchas, and the file map.

---

## 1. Where things stand (all committed on `feat/polyglot-audit-roadmap`)

**Landed & verified (UI overhaul + worldgen authoring):**
- RmlUi **GL3 backend** swap (frosted glass/box-shadow/blur now render). `--ui-screenshot <a,b,c>` UI
  fidelity gate → PPM, `tools/ppm_to_png.py`. **TGA-only** image loader — generated UI images must be
  TGA (`tools/ppm_to_tga.py`) or uploaded via the backend's `GenerateTexture` (raw bytes).
- All **8 screens** restyled to `references/ui/*.png` (frosted panels, wide-tracked serif titles,
  pill buttons). Live **golden-hour world backdrop** behind the menus (menu branch renders a scenic
  world; `dusk.json` vantage).
- Real **TGA thumbnails** (world-select + gallery). **UI hot reload** (`--ui-hot-reload`).
- **Functional controls**: vsync toggle, window-mode stepper, preset chips, keybind rebind — all
  e2e-tested.
- **Create-world full parameter control**: collapsible "customize" panel, ~30 worldgen params +
  a toggle for **every** feature (shaping/erosion/rivers/lakes/biomes/relief/caves/cliffs/structures/
  island-mask), seeded from the selected preset.
- **Custom worlds are self-contained**: resolved params embed in the world's own save
  (`worlds/saves/<id>/preset.json`); `GameSession::CreateWorld(name,seed,type,const std::string* customJson)`
  (3-arg overload kept); `LoadWorld` prefers the embedded preset. `Client::BuildCustomPreset` (in
  `world/WorldgenOverride.{h,cpp}`) **diffs vs base** (only real deltas) + `ClampTerrainParams` validates.
- **Save-as-named-preset**: writes `user_<slug>.json`, lists saved presets as gold chips.
- **e2e harness** `test/ui/ui_smoke_test.cpp` drives the *real* `Rml_UIManager` headless via
  `Element::Click()`. Unit/integration tests for the merge + a terrain-differs check.
- **Blocker 3a (gate honesty) DONE**: a `GTEST_SKIP` UI test now **fails** ctest instead of passing
  (was green-on-zero-coverage without GL). `FAIL_REGULAR_EXPRESSION "SKIPPED"` on the GL UI targets.

**Planning artifacts:** `spec.md` (this folder) + `.forge/critique-create-world-deferred-20260620-234835.md`
(the 4-way devil's-advocate report that reshaped this plan).

**Owner decisions (locked):** all 4 items in one push; **knobs = separate persisted layer**; **Item 4 =
constrained graph** compiling bit-exact to the existing params (free-topology evaluator is a non-goal).

---

## 2. The work remaining (sequence — each step builds on the prior, in the main tree)

> The chain is largely **linear** (knobs need the preview; graph last) and builds are serial on one
> tree + one GPU, so don't parallelize across worktrees — do it sequentially.

1. **Blocker 2 — preview-render spike.** Stand up a bounded preview `SHIELD_WorldSystem` +
   `EnsureSurfaceReadyNear` (small radius), render once via `renderPipeline.render_frame` into a small
   FBO (512–768 px), measure per-frame + rebuild cost + memory; pick radius/FBO size that holds the
   frame budget. Reuse the menu-backdrop render path + `debug/WorldGenViewer`.
2. **Item 1 — the live diorama** (the centerpiece; see §3).
3. **Item 3b — interaction e2e** (pause resume/quit args, gallery nav, world-select empty-guard,
   hot-reload, thumbnail-resolves) **+ user-preset delete/rename/overwrite-collision** (current
   `WorldPresetSaver` silently overwrites). Pin all in `required_ui_tests`, run-not-skip.
4. **Item 2 — semantic knobs** (separate persisted layer; see §4).
5. **Item 4 — constrained layer graph** (see §5).

---

## 3. Item 1 — the live diorama (DETAILED — this is the headline)

**Vision:** like rotating a 3D object in Photoshop/a model viewer. A bounded slice of the candidate
world sits in a panel; drag to spin (turntable), scroll to zoom; sliders (knobs) + a weather/tod row
update it live.

**Build it from existing infra — do NOT reinvent rendering:**
- `main_client.cpp` already renders a **live world behind the menu** (the F4 menu backdrop:
  `g_menu_backdrop_active`, a `SHIELD_WorldSystem` + `renderPipeline.render_frame` + a slow camera).
  The diorama is that pattern, but (a) rendered into an **offscreen FBO** instead of the backbuffer,
  (b) scoped to a **bounded region** (small streaming radius), (c) driven by an **orbit/arcball**
  camera bound to mouse-drag over the preview rect, (d) using the **candidate** params, rebuilt on
  knob change.
- `debug/WorldGenViewer.{h,cpp}` already does `RecreateWorldSystem(params,seed)` + `RegenerateTexture`
  (renders a preview to a texture shown in ImGui). Reuse/extend its world-build + render-to-texture.
- Weather/atmosphere is live via `renderPipeline.set_weather(type,intensity)`,
  `set_time_of_day(t)` (0=noon, ~0.24=golden dusk), `set_cloud_state(CloudRenderState)`. Waterfalls
  come from the water system + `rendering/WaterfallDetect` — they appear automatically in a real render
  once chunks stream.

**Mechanics:**
- A `WorldgenPreview` controller (client) owns: a cached preview `SHIELD_WorldSystem` (rebuilt via
  `set_params`/`set_seed` on debounced param change — confirm those setters exist on the header;
  else reconstruct), an orbit `Camera` (yaw/pitch/dist around a fixed look-at), the FBO, and the
  current weather/tod.
- **Params boundary:** build `TerrainGenParams` from the resolved candidate JSON **in memory**
  (factor a `json→TerrainGenParams` helper WITHOUT touching the shared `LoadTerrainPreset`; or, last
  resort, a single reused temp file — but the temp-file data-root resolves wrong, see gotchas).
- **UI integration:** simplest is the menu-backdrop trick — the main loop renders the preview FBO into
  the create-screen's preview rect under the transparent RmlUi panel; mouse events over that rect go
  to the orbit controller. Alternatively bind the FBO color texture to an RmlUi element via the GL3
  backend handle. (`Rml_UIManager` has no JobSystem and the texture upload is GL-thread-only — keep
  render + upload on the main thread.)
- **Real-time:** re-render the FBO each frame the camera/params/weather changed; pause when the create
  screen is hidden or nothing changed. Rebuild the world only on param change (debounced ~250 ms,
  latest-wins via a generation counter).
- **Slice framing + fallback:** vignette/clip the diorama edges so it reads as a model; on a build
  failure keep the last good frame + a non-blocking note.

**Tests (headless GL, in `ui_smoke_test` or a new `worldgen_preview_test`):** preview world builds +
renders to FBO without crashing; changing params (own fixed literal, **not** `default.json`) / weather /
tod changes FBO pixels; orbit changes the view; a per-frame render-budget assertion; rebuild is
debounced/latest-wins.

---

## 4. Item 2 — semantic knobs (separate persisted layer)

~6 outcome knobs (mountainousness, ruggedness, wetness, erosion/age, climate, feature density) as the
**default** create surface; the existing ~30-param panel becomes the **advanced** fold.
- **Knobs own a persisted layer.** apply = `applyKnobLayer(knobVector)` then overlay a **sparse
  raw-override diff**. Persist BOTH the knob vector and the override diff in the world save + user
  presets (extend `BuildCustomPreset` / the saver). Reopen must be **exact**.
- Knob→param relationships are **tuned response curves** (per-param splines, NOT 2-point lerps) with
  **ramped enable-flags** (no discontinuous feature pop). One engine-side **param-descriptor + knob-map
  table** is the single source of truth for ranges/labels/defaults (feeds the RML min/max AND the map).
- Curated presets carry **no** knob layer → knobs render **neutral ("preset/custom")**, never
  inverse-lerped; engaging a knob snapshots the preset as the override baseline so **authored splines
  survive** (NEVER flatten a curated preset — the critique's hardest finding).
- Bind through the `WorldgenOverride`/`WorldParamGetter` **bridge**, not raw RML elements.
- **Tests:** knob extremes hit expected params; **monotonicity** sweep (each knob 0→1 → Item 1's
  relief/roughness metric moves monotonically); reopen-exactness; e2e that a knob moves the controls +
  regenerates the preview.

## 5. Item 4 — constrained layer graph (LAST, flagged)

A **fixed-topology** `LayerGraph` (`world/LayerGraph.{h,cpp}`) mirroring the real pipeline stages
(base FBM → warp → splines → biome relief → rivers → lakes → cliffs → hydro); nodes expose the same
scalars as `TerrainGenParams`; edges are the fixed order. `compile(graph)` → `generation_params` JSON
**bit-exact** to the equivalent flat preset (so `world_hash`/run==replay/far-LOD are unchanged).
Serialize to `generation_params.graph`. Render it in the **existing ImGui `WorldGenViewer`** behind a
`--worldgen-graph` flag — a flagged **internal authoring tool**, NOT the shipping RmlUi create surface.
Reuse Item 1's preview as the viewport. **Non-goal:** a free-topology evaluator (that's a worldgen-core
rewrite + determinism minefield; separate future epic). **Tests:** graph round-trip; `compile` bit-exact
to a curated preset's params field-for-field.

---

## 6. Build & test gotchas (READ before touching code)

- **Toolchain PATH:** prepend ucrt64 every build/ctest: `export PATH=/c/msys64/ucrt64/bin:$PATH`
  (KiCad/mingw on PATH → silent `cc1plus exit 127`).
- **Build the tree you test:** `cmake --build build/debug --target <x> -j 8`. Reconfigure
  (`cmake --preset debug`) only after adding a source to `sources.cmake` / `test/CMakeLists.txt`.
- **Stale-obj / ODR hazard (HIT TWICE):** a concurrent agent edits the worldgen headers
  (`SHIELD_WorldSystem.h` / `TerrainGenParams`). Symptom: a link "undefined reference" to an old
  signature, or a **SIGSEGV (exit 139)** destructing a vector/string on float-bit-pattern pointers
  (e.g. `0x41600000`=14.0f). Fix: `find build/debug -name 'GameSession.cpp.obj' -delete` (and any
  named stale `.obj`), rebuild. **Additive seams only — never refactor `TerrainGenParams` /
  `LoadTerrainPreset`.**
- **Concurrent agent owns worldgen:** `TerrainPresetLoaderTest.LoadsShippedDefaultPreset` was RED at
  one point because they enriched `default.json` — *their* test, not ours; don't pin new tests to
  `default.json` (use a fixed literal).
- **Gate honesty (3a, done):** the UI gate now **fails on GTEST_SKIP**, so new GL-backed UI tests must
  genuinely run + pass; pin them in `required_ui_tests` (`test/CMakeLists.txt`).
- **RmlUi images are TGA-only**; for generated images use the GL3 backend's `GenerateTexture`
  (raw bytes), NOT file+`LoadTexture` (it serves the stale cached texture; "toggle src" does NOT bust it).
- **Commits:** a background process commits with `git add -A` and has swept staged files twice — stage
  ONLY your files. Standing mandate: commit when green, no push.

---

## 7. File map / seams

- UI: `data/ui/*.rml` + `game_theme.rcss` / `pages.rcss`; `src/luminumbra_client/ui/Rml_UIManager.{h,cpp}`
  (`SettingsBridge`, `WorldCreationCallback(name,seed,type,params)`, `WorldParamGetter`,
  `WorldPresetSaver/List`, `SeedWorldGenParams`, `PopulateUserPresets`).
- Worldgen authoring: `src/luminumbra_client/world/WorldgenOverride.{h,cpp}` (`WorldGenParam`,
  `BuildCustomPreset`); host wiring + `start_world_creation` merge + `SetWorldParamGetter`/saver/list
  in `src/luminumbra_client/main_client.cpp`.
- Worldgen core (DON'T refactor): `src/luminumbra_common/systems/SHIELD_WorldSystem.{h,cpp}`
  (`GetTerrainHeightAt`, the fixed pipeline), `world/TerrainPresetLoader.{h,cpp}`,
  `world/GameSession.{h,cpp}` (`CreateWorld` embed, `ClampTerrainParams`), `world/FarLodStore.cpp`
  (`ComputeTerrainParamsHash` = the determinism key).
- Render/preview infra: `rendering/RenderPipeline.{h,cpp}` (`render_frame`, `set_weather`,
  `set_time_of_day`, `set_cloud_state`), the F4 menu-backdrop block in `main_client.cpp`,
  `debug/WorldGenViewer.{h,cpp}` (ImGui world build + render-to-texture).
- Tests: `test/ui/ui_smoke_test.cpp` (+ `test/CMakeLists.txt` `required_ui_tests` ~L966),
  `test/common/GameSessionHeadlessWorld_test.cpp` (`HeadlessRoot` harness), `tools/ppm_to_tga.py`.
- Presets: `worlds/atlas/presets/*.json` (schema: `generation_params.{terrain{shaping,hydro},biomes,
  features,materials}`), user presets `user_*.json`.

---

## 8. The workflow (rewritten — see `WORKFLOW.md` in this folder)

A sequential build+test+commit pipeline (one heavyweight agent per item, each on top of the prior).
 To run it: `Workflow({ scriptPath: "<session>/workflows/scripts/create-world-deferred-ultracode-*.js" })`
or paste `WORKFLOW.md`'s script. The pipeline: spike+Item 1 (diorama) → Item 3b → Item 2 → Item 4 →
integration verify. **Note:** the live-diorama Item 1 is a meatier piece than a thumbnail — give it its
own milestone; it's the highest-value lever and the on-ramp to Item 4's viewport.

## Open questions
- Diorama UI: blit FBO into the create-screen rect (menu-backdrop style) vs bind FBO texture to an
  RmlUi element? (spike/prototype both; blit is simpler.)
- Do `SHIELD_WorldSystem` `set_params`/`set_seed` setters exist for cheap world rebuild, or must we
  reconstruct? (spike answers.)
- Streaming radius / FBO size that holds the create-screen frame budget (spike answers).
