# Pillar audit: UI — cinematic RmlUi (spec 001) + create-world (spec 002) (Spec 021, 2026-07-02)

The UI pillar is in strong shape and materially AHEAD of the 2026-06-28 roadmap's assumptions: the
Wave-0.3 create-world punch-list the roadmap listed as remaining **landed in full on 2026-06-28**
(flex slider rows + advanced tab panes + slider-drag fix `adbff49d`, knob→preview rebuild proof
`main_client.cpp:9115`, preview precipitation `WorldgenPreview.cpp:464`, headless live-diorama
capture `860230b1`), and two KNOWN-CONTEXT facts are contradicted by the tree — `settings.rml` IS
built and fully wired (`data/ui/settings.rml:44`, `src/luminumbra_client/main_client.cpp:3481`), and
a v1 creature-codex browse overlay already exists (`data/ui/codex.rml:8`,
`src/luminumbra_client/main_client.cpp:7090`). The one large functional hole left is that the
**world-selection screen is not wired to real saves**: the list is hardcoded RML placeholders
(`data/ui/world_selection.rml:43`), `SetLoadWorldCallback` is never called by the client
(`src/luminumbra_client/ui/Rml_UIManager.h:141` — only tests wire it), and spec-001 FR-040 per-world
save thumbnails were never implemented. Secondary debt: `--ui-fixtures` is a parse-only no-op
(`main_client.cpp:2817`), the fidelity baseline covers only 3 of 8+ screens at 800x600
(`.forge/scripts/validate-engine-frontier.ps1:1647`), and a handful of settings are persisted-only.

## Current state + evidence

### Spec 001 — cinematic RmlUi overhaul (substantially delivered)

- **GL3 reference backend** (FR-001, the spec's linchpin): the hand-rolled `RmlRenderer` is replaced
  by RmlUi 6.1's reference GL3 renderer — `src/luminumbra_client/ui/Rml_UIManager.h:4` includes
  `gl3/RmlUi_Renderer_GL3.h` ("real blur/box-shadow/layers") and holds it as the manager's render
  interface at `src/luminumbra_client/ui/Rml_UIManager.h:188`; the backend source is vendored at
  `src/luminumbra_client/ui/gl3/RmlUi_Renderer_GL3.cpp`. Frosted glass / box-shadow / blur therefore
  actually render (spec 001 goal, `docs/specs/001-cinematic-rmlui-overhaul/spec.md:43`).
- **All screens authored** in one design language: `data/ui/` ships `main_menu.rml`,
  `world_selection.rml`, `world_creation.rml`, `settings.rml`, `hud.rml`, `photo_mode.rml`,
  `pause.rml`, `gallery.rml`, plus `codex.rml` and the shared `base.rcss` / `game_theme.rcss` /
  `pages.rcss` (directory listing of `data/ui/`).
- **`--ui-screenshot` fidelity-capture path** (FR-010, `spec.md:51`): batched multi-screen capture
  parsed at `src/luminumbra_client/main_client.cpp:2807-2831` (comma-separated screens, one window
  session, `<dir>/ui-<screen>.ppm`), captured after a settle at `main_client.cpp:9207-9275`;
  `tools/ppm_to_png.py` exists for PNG conversion. The RENDER-pillar headless **IN_GAME** capture
  hang does NOT block this path — `--ui-screenshot` runs in menu state and works (it was used to
  visually confirm the Wave-0.3 preview work, commit `56f40396`).
- **Hot reload** (FR-020, `spec.md:57`): `UIHotReload` instantiated at `main_client.cpp:1181`,
  enabled opt-in via `--ui-hot-reload` (`main_client.cpp:3040`) and pumped per frame at
  `main_client.cpp:4077`.
- **Live menu backdrop** (FR-030, F4): a golden-hour world with slow yaw oscillation renders behind
  the transparent menu bodies at `main_client.cpp:9082-9091`, suppressed while the create-world
  preview owns the world render (`main_client.cpp:9066-9076`).
- **Gallery thumbnails are real** (FR-041, `spec.md:67`): photo capture drops a TGA thumbnail
  (`main_client.cpp:7446`), and `PopulateGallery` replaces the placeholder grid with the player's
  real captures newest-first (`src/luminumbra_client/ui/Rml_UIManager.cpp:414`, `:874-915`), with an
  authored empty state (`Rml_UIManager.cpp:897`).
- **Settings screen built and wired** — CONTRADICTS the stale "settings.rml is NOT built" memory:
  `data/ui/settings.rml:44-115` ships sensitivity/FOV/UI-scale/master-volume sliders, a vsync
  toggle, a window-mode stepper, and six click-to-rebind keybind rows; the `SettingsBridge` in
  `main_client.cpp:3481-3540` live-applies FOV/sensitivity/vsync/UI-scale/master+music volume and
  begins keybind capture (`sb.BeginRebind`), persisting via `SaveUserOverlay`;
  `Rml_UIManager::BindSettingsListeners` (`Rml_UIManager.cpp:1036`) wires the widgets and the rebind
  chips (`Rml_UIManager.cpp:1129-1144`). Still true from the old memory: resolution, window-mode and
  sfx volume setters are **persist-only** (`main_client.cpp:3486`, `:3488`, `:3513`), and the
  resolution / sfx / music controls sit in a hidden div (`data/ui/settings.rml:13-36`).
- **Codex browse overlay (v1) exists** — CONTRADICTS "codex browse UI = next": `data/ui/codex.rml`
  (24 lines: completion header + row list), toggled by the codex key at `main_client.cpp:7088-7096`
  and populated live from `BuildCodexView(g_creatureSpecies, g_photoCodex)` (name + discovery stars,
  sig-throttled DOM writes) at `main_client.cpp:7114-7146`.
- **Tutorial hint (minimal) exists**: `data/ui/hud.rml:20-23` ships a 3-line "find a creature → P →
  Enter → C" hint block, shown while the codex is empty (`main_client.cpp:7175-7188`).

### Spec 002 — create-world (all four items delivered; polish landed 06-28)

- **Item 1, live diorama**: `src/luminumbra_client/world/WorldgenPreview.h:59` is the controller —
  real `SHIELD_WorldSystem` candidate world, orbit/turntable camera, debounced latest-wins rebuild
  on a background worker (GL-thread swap, `WorldgenPreview.h:20-27`), rendered full-screen through
  the real pipeline (`render_to_backbuffer`, `WorldgenPreview.h:136`). Host wiring:
  `main_client.cpp:9093-9199` (candidate sig diff → `set_candidate`, weather/tod, drag-orbit,
  scroll-zoom, reset-view). Water is linked so lake/ocean presets render (and don't crash):
  `WorldgenPreview.h:179-184` (fix `e1fee9ff`). Weather pills + tod slider + reset control:
  `data/ui/world_creation.rml:19-31`.
- **Item 2, semantic knobs**: engine-side canonical knob/param table
  `src/luminumbra_common/world/KnobLayer.h:1-27` (separate persisted layer, response curves); the
  host resolves the diorama through `BuildKnobResolvedPreset` when knobs are present
  (`main_client.cpp:9126-9131`); knob seeding from the persisted layer + neutral-on-curated-preset
  at `Rml_UIManager.cpp:725-739`.
- **Item 3a, gate honesty**: `GTEST_SKIP` is promoted to ctest FAILURE for the three GL-required UI
  targets via `FAIL_REGULAR_EXPRESSION "SKIPPED"` (`test/CMakeLists.txt:949-957`), and the ctest
  manifest pins 20 required UI tests (`test/CMakeLists.txt:1114-1136`).
- **Item 3b, e2e coverage**: 16 tests in `test/ui/ui_smoke_test.cpp` driving the REAL headless RmlUi
  manager (`Element::Click`), including preset save/overwrite-confirm/rename/delete
  (`ui_smoke_test.cpp:897`), pause actions (`:756`), world-select empty-guard + hot reload (`:830`),
  knobs (`:1234`); 6 preview tests in `test/ui/worldgen_preview_test.cpp:177-292` (pixels change on
  params/weather/tod, orbit, debounce latest-wins, render budget); document parse coverage in
  `test/ui/ui_page_load_test.cpp:112`. Preset overwrite modal + rename flow in
  `Rml_UIManager.cpp:602-639`.
- **Item 4, constrained layer graph**: `src/luminumbra_common/world/LayerGraph.h:1-15` (fixed
  topology over the existing `generation_params`), enabled by `--worldgen-graph`
  (`main_client.cpp:3603-3609`).
- **Gate**: `-Mode UiTestBaseline` (`.forge/scripts/validate-engine-frontier.ps1:1559`) asserts the
  ctest manifest, coverage subsystems, and a 3-view `ui_screenshots.json` baseline.

### Shipped since the 2026-06-28 roadmap

Verified via `git log` (the roadmap's "Wave-0.3 punch-list remaining" assumption is now stale):

- **`adbff49d` (2026-06-28 14:12) — the entire Wave-0.3 create-world punch-list**: (a) flex
  [label][slider][value] rows (`data/ui/game_theme.rcss:855-889`); (b) advanced params split into
  terrain/water/biomes/features TAB PANES, which also removed the `overflow-y:auto` scroll container
  that intercepted slider drags (`game_theme.rcss:922-928`, C++ tab handler
  `Rml_UIManager.cpp:542-564`, RML `data/ui/world_creation.rml:128`); (c) knob→preview rebuild
  proof — the host logs the resolved candidate sig per rebuild (`main_client.cpp:9112-9115`);
  (d) preview precipitation — camera-followed rain/snow/splash emitters in the shared ParticlePass
  (`WorldgenPreview.cpp:464-521`, cleared on deactivate at `main_client.cpp:9197`). Test extended:
  `UiSmokeTest.RedesignedControlsAreFunctional` (`test/ui/ui_smoke_test.cpp:528`).
- **`e79c1312` (2026-06-28 15:48)** — stable distant far-field anchored to the diorama centre.
- **`860230b1` (2026-06-28 17:00)** — `--preview-live` / `--preview-weather` headless live-diorama
  capture: bounded wait for `world_ready()` + post-ready settle + self-diagnosing readiness report
  (`main_client.cpp:9211-9246`, flag parse `:2823-2829`).
- **`a21e088e` (2026-06-28 17:04)** — full-SDF near-field LOD for the create-world preview.
- **`f5840346` (2026-06-28 10:19)** — slider-knob track centring (+ the punch-list capture that the
  same-day commits then closed).
- **Discrepancy vs the audit brief**: the create-world lake-preview null-water crash fix
  (`e1fee9ff`) landed **2026-06-25**, i.e. BEFORE the 2026-06-28 roadmap, not since it. It is filed
  `done` either way (`WorldgenPreview.h:179-184`; regression test
  `test/shield/test_worldgen_layer_snapshots.cpp:712`).

## Gaps / debt

1. **World-selection is not wired to real saves (the biggest functional gap).** The list-items are
   hardcoded placeholders with fake ids (`data/ui/world_selection.rml:43-58`) and generic landscape
   thumbs; there is no runtime population from `worlds/saves/` (real saves exist on disk);
   `SetLoadWorldCallback` has NO caller in the client (`Rml_UIManager.h:141`; only
   `test/ui/ui_smoke_test.cpp:350` and `:841` wire it) — the create flow is wired
   (`main_client.cpp:3327`) but the load flow dead-ends. Spec-001 FR-040 (save emits
   `worlds/<id>/thumbnail.png`, card shows it — `docs/specs/001-cinematic-rmlui-overhaul/spec.md:66`)
   is unimplemented.
2. **`--ui-fixtures` is a no-op** (spec 001 FR-011, `spec.md:52`): the flag is parsed and logged but
   never consumed (`main_client.cpp:302`, `:2817`, `:2830`). Consequence: gallery/world-select
   captures and the gallery e2e depend on on-disk state —
   `UiSmokeTest.GalleryBackNavigationAndContentArePresent` (`ui_smoke_test.cpp:799`) is RED on a
   machine with 0 saved photos (`PopulateGallery` swaps in the empty state,
   `Rml_UIManager.cpp:897`), documented as pre-existing/environmental in `adbff49d`'s message.
3. **Fidelity baseline covers 3 of 8+ screens at the wrong resolution**: the `ui_screenshots.json`
   contract pins exactly `main_menu` / `world_creation` / `world_selection` at the 800x600 hidden
   window (`.forge/scripts/validate-engine-frontier.ps1:1647`, `:1656`), vs spec 001 FR-012's
   3840x1600 reference aspect (`spec.md:54`) and the 8-screen AC-002 critic loop (`spec.md:83`).
   settings/hud/photo/pause/gallery/codex have no pixel-level regression coverage.
4. **Settings persist-only rows**: resolution, window-mode and sfx-volume setters only write config
   (`main_client.cpp:3486`, `:3488`, `:3513`); resolution + sfx/music controls are hidden markup
   (`data/ui/settings.rml:13-36`) with no visible row.
5. **Codex v1 is minimal**: a name + discovery-stars list (`data/ui/codex.rml:15-20`,
   `main_client.cpp:7128-7143`) — no captured-photo thumbnails, no per-species detail, and
   `codex.rml` is absent from `ui_page_load_test`'s page list
   (`test/ui/ui_page_load_test.cpp:129-138`), so it has zero automated coverage.
6. **Tutorial is one static hint block** (`data/ui/hud.rml:20-23`, gated only on "codex empty" at
   `main_client.cpp:7175`); there is no sequenced onboarding (find → photograph → codex → objective).
7. **Gate-honesty tail**: the validator asserts only 3 of the 20 manifest-pinned UI tests
   (`.forge/scripts/validate-engine-frontier.ps1:1591-1597`), and the configure-time manifest
   hardcodes `"passed": true` (`test/CMakeLists.txt:1139`) — honesty rests on the
   `FAIL_REGULAR_EXPRESSION` promotion (`test/CMakeLists.txt:949-957`), which is real but the
   validator-side pin list has drifted below the manifest.

## Risks

- **Preview ↔ render-pipeline coupling**: the diorama shares the live `RenderPipeline` (far-LOD
  drain-before-free just fixed a UAF, `WorldgenPreview.h:193-199`, commit `4922583a`). The spec-016
  pass decomposition (RENDER pillar) will refactor exactly the passes the preview drives
  (`render_to_backbuffer`, ParticlePass emitters, foliage scatter) — the preview tests
  (`worldgen_preview_test.cpp:177-292`) must stay pinned in the 016 gate or a conversion can
  silently break the create screen.
- **Environmental test RED normalizes failure**: a known-RED `GalleryBackNavigationAndContentArePresent`
  teaches the team to ignore the UI suite; combined with the validator only pinning 3 names
  (`validate-engine-frontier.ps1:1591`), a real regression in the other 17 could ride along unnoticed.
- **Fake world-select ids look functional**: clicking a placeholder card enables the load button
  (`Rml_UIManager.cpp:481-486`) but the callback is null in the client, so "load world" silently
  does nothing — a ship-blocking UX trap that no current test can catch (tests inject their own
  callback).
- **Determinism**: near-zero — the whole pillar is render/UI-side; `WorldgenPreview` explicitly never
  reimplements worldgen sampling (`WorldgenPreview.h:16-18`) and preview particles are one-way
  render-only (`WorldgenPreview.h:158-161`). The main hash-adjacent surface is UI-06 touching the
  world-save path (metadata/thumbnail emit must stay out of `world_hash`).

## Opportunities

- **Close spec 001 end-to-end** with the world-select saves wiring + FR-040 thumbnails: it converts
  the last mostly-cosmetic screen into the actual load-game flow and finishes AC-004.
- **`--ui-fixtures` unlocks honest visual gating**: deterministic gallery/world-select/settings data
  makes all-screen native-res captures reproducible, enabling the 8-screen critic-loop baseline
  (spec 001 AC-002) and de-flaking the gallery e2e in one move.
- **Codex v2 is cheap leverage**: `PhotoCodex` + captured TGA thumbs already exist
  (`main_client.cpp:7446`); surfacing the player's best capture per species in the codex list is
  client-only DOM work with an existing data source, and directly serves the photography-game core
  loop (polish-roadmap Phase 1 follow-on).
- **Preview as a marketing/QA surface**: `--preview-live --preview-weather` already produces
  self-diagnosing headless captures of the full diorama (`main_client.cpp:9236-9245`) — trivially
  extensible into a per-preset visual regression matrix for worldgen changes.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
|----|---------|------|--------|------|------|--------|----------------|
| UI-01 | Wave-0.3 create-world polish landed: flex value-beside-slider rows, advanced tab panes (slider-drag fix), knob→preview rebuild sig proof, preview precipitation | 002 | M | low | — | done | `UiSmokeTest.RedesignedControlsAreFunctional` (test/ui/ui_smoke_test.cpp:528; pinned in test/CMakeLists.txt:1120) |
| UI-02 | Headless live-diorama capture (`--preview-live`/`--preview-weather`) + stable far-field anchor + full-SDF near-field LOD for the preview | 002 | M | low | — | done | `WorldgenPreviewTest.RendersCandidateDioramaToTargetWithoutCrashing` (test/ui/worldgen_preview_test.cpp:177) + the `--ui-screenshot world_creation --preview-live` capture path (main_client.cpp:9211) |
| UI-03 | Lake-preview null-water crash fixed by linking a real WaterSystem to the preview world (landed 2026-06-25, pre-roadmap — brief discrepancy recorded) | 002 | S | low | — | done | `WorldGenLayerSnapshotTest.LakePreviewBuildWithNullSystemsDoesNotCrash` (test/shield/test_worldgen_layer_snapshots.cpp:712) |
| UI-04 | Settings screen built + fully wired (sliders, vsync toggle, window-mode stepper, keybind rebind, live apply + persisted overlay) — corrects stale "not built" memory | 001 | L | low | — | done | `UiSmokeTest.SettingsScreenRoundTripsThroughTheBridge` (test/ui/ui_smoke_test.cpp:425; pinned in test/CMakeLists.txt:1119) |
| UI-05 | Creature-codex browse overlay v1 (C toggle, live completion + discovery-stars rows from PhotoCodex) | new | M | low | — | done | NEW: extend `UiPageLoadTest.EveryShippedDocumentLoadsWithAPopulatedBody` (test/ui/ui_page_load_test.cpp:129) with `{codex.rml, codex}` — asserts the codex document parses with a populated body |
| UI-06 | Wire world-selection to real saves: runtime list population from `worlds/saves/`, client `SetLoadWorldCallback` wiring, per-world `thumbnail.png` emit on save (FR-040) | 001 | M | medium | — | todo | NEW: `UiSmokeTest.WorldSelectPopulatesFromSavesFixtureAndFiresLoad` — a fixture saves dir yields matching list-items with per-world thumbnails, and clicking load fires the callback with the real world id |
| UI-07 | Implement `--ui-fixtures` (FR-011) + hermetic gallery e2e (kill the environmental 0-photos RED) | 001 | S | low | — | todo | `UiSmokeTest.GalleryBackNavigationAndContentArePresent` (test/ui/ui_smoke_test.cpp:799) green on a clean checkout, still pinned skip-as-fail |
| UI-08 | Extend the UI fidelity baseline to all shipped screens at native reference aspect (currently 3 views @ 800x600 vs FR-012's 3840x1600) | 001 | M | low | UI-07 | todo | `validate-engine-frontier.ps1 -Mode UiTestBaseline` (.forge/scripts/validate-engine-frontier.ps1:1559) with the view list extended to all shipped documents + capture-resolution assertion |
| UI-09 | Settings completeness: expose resolution/sfx/music rows, live-apply (or restart-note) for resolution/window-mode/sfx (setters currently persist-only) | new | S | low | — | todo | `UiSmokeTest.SettingsScreenRoundTripsThroughTheBridge` (test/ui/ui_smoke_test.cpp:425) extended to round-trip resolution/window-mode/sfx through visible controls |
| UI-10 | Codex browse v2: per-species best-capture photo thumbnails + detail pane (photography-loop payoff; polish-roadmap follow-on) | new | M | low | — | todo | NEW: `UiSmokeTest.CodexPopulatesRowsFromPhotoCodexFixture` — an injected PhotoCodex fixture yields the expected discovered rows, stars, completion string, and photo `<img>` sources |
| UI-11 | Sequenced first-session tutorial/onboarding (beyond the single HUD hint block): find → photograph → codex → first objective | new | M | low | — | todo | NEW: `UiSmokeTest.TutorialStepsAdvanceThroughFirstCaptureFlow` — the hint sequence advances as fixture codex/objective state changes, and disappears when complete |
| UI-12 | Gate-honesty tail: validator pins all 20 manifest UI tests (not 3) and the manifest `passed` field stops being a configure-time constant | 002 | S | low | — | todo | `validate-engine-frontier.ps1 -Mode UiTestBaseline` negative test — a manifest missing any pinned name (or with a skipped pinned test) FAILS the gate |
