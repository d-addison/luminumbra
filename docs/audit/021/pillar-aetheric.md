# Pillar audit: Aetheric Field (Spec 021, 2026-07-02)

**Verdict.** The Aetheric Field pillar is substantially MORE built than its reputation ("may be the
least-built"): iteration 6 (T-I6-A1) shipped a fully deterministic, hash-folded, gate-tested engine
scalar field (`src/luminumbra_common/systems/AetherFieldSystem.h:3`) plus a complete render emissive
tap that is plumbed end-to-end through RenderContext, LightingPass, and the lighting shader — but the
single client glue call that would make it visible was never written: `RenderPipeline::
update_aether_field` (`src/luminumbra_client/rendering/RenderPipeline.cpp:2675`) has **zero call
sites** anywhere in the tree, so `u_aetherActive` stays 0.0 and the glow term contributes nothing at
runtime. The sim side is a *procedurally animated* field (recomputed from noise each tick, no
persistent energy state), which deliberately buys save/load-free determinism at the cost of the
README's stateful vision (entity emitters/absorbers, Lumin/Umbra polarity, energy decay) — all of
which remains unbuilt, along with the Lua game-content layer that references a C++
`AethericFieldComponent` and `get_aetheric_value` API that do not exist. One genuine gate
inconsistency was found: the engine/game split lint unconditionally bans the noun `aetheric` under
`src/` while 11 engine files carry "Aetheric" in comments, so `-Mode EngineGameSplitLint` fails if
run against the current tree.

## Current state + evidence

### 1. Deterministic sim core — BUILT and hardened (T-I6-A1a)

- `AetherFieldSystem` is a coarse 2.5D scalar field: 24 m cells, 64x64 grid, pinned solver constants
  (`src/luminumbra_common/systems/AetherFieldSystem.h:49-58` — `kAetherCellSizeM=24.0f`,
  `kAetherExtentCells=64`, `kAetherDiffuseIterations=8`, `kAetherDiffuseRate=0.25f`).
- Update pipeline per tick: (1) seed+14 low-frequency FBm simplex emission noise sampled over cell
  world coords, scrolled by tick (`src/luminumbra_common/systems/AetherFieldSystem.cpp:103-125`);
  (2) semi-Lagrangian backtrace advection by the wind field, bilinear, edge-clamped
  (`AetherFieldSystem.cpp:127-167`); (3) 8 pinned Gauss-Seidel diffusion sweeps with Neumann
  boundaries (`AetherFieldSystem.cpp:169-189`). All arithmetic is `+,-,*,/` + `DeterministicMath::BitsOf`;
  no libm transcendentals, no RNG, no wall-clock (`AetherFieldSystem.h:25-29`).
- The field is a **pure function of (seed, tick, region anchor[, wind])** — the grid is never
  persisted; save/load re-creates the system and one `Update()` reproduces the exact state
  (`AetherFieldSystem.h:16-21`; load path `src/luminumbra_common/world/GameSession.cpp:805-809`).
- Storage reuses the shared `FieldGrid<float>` plumbing built for wind
  (`src/luminumbra_common/fields/FieldGrid.h:30-31`), giving grid identity with wind/weather —
  asserted by test (`test/common/fields_hardening_test.cpp:680-692`).
- Ticked every sim tick in `GameSession::Update`, after weather, advected by the fresh wind grid
  (`src/luminumbra_common/world/GameSession.cpp:368-377`); constructed on world create
  (`GameSession.cpp:690`) and on load (`GameSession.cpp:809`); accessors on the session
  (`src/luminumbra_common/world/GameSession.h:167-168`, member at `GameSession.h:256`).
- Public sampling API `SampleAether(world_pos)` returns the non-negative cell value, 0 outside the
  streamed region (`AetherFieldSystem.h:76`, `AetherFieldSystem.cpp:192-201`).

### 2. world_hash integration — BUILT (deliberate bump #4)

- Dedicated `aether` sub-hash slot in the per-system sub-hash struct
  (`src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h:124-129`).
- `ComputeAetherSubHash()` hashes raw IEEE bits of every cell in canonical order plus seed/tick/
  geometry, "so a one-ULP drift fails the gate loudly"
  (`src/luminumbra_common/systems/AetherFieldSystem.cpp:203-222`).
- Folded into the composite world_hash by the headless runner as **bump #4**
  (`d950a6afc12a5cdc -> f17726d44054d133`), append-only after chunk|wind|weather
  (`src/luminumbra_server/ServerWorldRunner.cpp:61-70`, fold-order contract at
  `ServerWorldRunner.cpp:88-99`; bump chain also recorded at
  `.forge/scripts/validate-engine-frontier.ps1:5177`). The current `--smoke` baseline
  `6f008a9f637c40b7` therefore already contains the aether field.
- The client capture context deliberately does NOT fold aether (client owns no independent field
  state) — guarded by the harness authority checks
  (`src/luminumbra_client/core/RuntimeScenarioHarness.cpp:7954`, `:8023-8032`).

### 3. Determinism gates and tests — BUILT

- Engine-frontier gate `AetherFieldDeterminism` (`.forge/scripts/validate-engine-frontier.ps1:4956-5011`,
  mode dispatch at `:7285`): drives `luminumbra_server_app --aether-bench --ticks 90 --seed 424242`,
  asserts sub-hash equality across two runs, evolution over ticks, and the pinned 24 m / 64-extent /
  8-sweep shape.
- Server bench driver `RunAetherBench` with the `luminumbra.aether_field_determinism.v1` artifact and
  a telemetry-only 0.40 ms/tick budget (`src/luminumbra_server/main_server.cpp:790-867`).
- Unit determinism/behavior suite: `test/common/AetherFieldSystem_test.cpp:38-111` (7 tests:
  pinned geometry, sub-hash determinism across runs, evolves with tick, seed-dependence,
  non-negative/non-trivial field, out-of-region zero, wind advection changes the field). Registered
  in the `common_tests` gtest target (`test/CMakeLists.txt:138`), discovered per-case via
  `gtest_discover_tests` (`test/CMakeLists.txt:926`, `:950-957`).
- Hardening suite `AetherHardening.*` (`test/common/fields_hardening_test.cpp:254-392`): null-wind
  deterministic no-op, every cell finite/non-negative/bounded, exact cell-centre sampling,
  out-of-region edge zero, flattened exact run==replay with wind coupling (`:345`), anchor
  quantization (no raw sub-cell anchor leak, `:375-383`), degenerate/inf sample safety (`:387-392`).
- The `aether` slot is a required artifact section in the world-hash gate
  (`.forge/scripts/validate-engine-frontier.ps1:4671`).

### 4. Render emissive tap — PLUMBED end-to-end, but INERT (T-I6-A1d, commit `bce99a5b`)

- Upload path: `RenderPipeline::update_aether_field(cells, origin, cell_size, extent)` uploads the
  grid as an R32F texture, sets origin/cell-size/active state
  (`src/luminumbra_client/rendering/RenderPipeline.cpp:2675-2703`; state members
  `src/luminumbra_client/rendering/RenderPipeline.h:1095-1103`).
- Pass-contract seam: RenderContext "Group G — aether field" carries the texture handle + geometry
  (`src/luminumbra_client/rendering/RenderContext.h:99-104`), populated from pipeline state each
  frame (`RenderPipeline.cpp:885-890`).
- Consumption: `LightingPass` binds the field at texture unit 10 and sets the uniforms, gated by
  `ctx.aether_active` (`src/luminumbra_client/rendering/passes/LightingPass.cpp:137-151`; uniform
  declared in the pass contract at `LightingPass.cpp:46`).
- Shader: `res/shaders/lighting_pass.frag:23-34` (uniforms; `u_aetherActive = 0.0` default,
  `u_aetherGlowColor = vec3(0.30, 0.55, 0.95)`, `u_aetherGlowIntensity = 2.0`), additive glow term
  sampled at the fragment's world XZ (`lighting_pass.frag:568-579`), composited at
  `lighting_pass.frag:630`.
- GPU shader-level proof exists: `RenderSmokeTest.AetherEmissiveTapBrightensLitOutput` renders the
  REAL lighting shader with the tap inactive vs a uniform active field and asserts measurable
  brightening + blue tint, and that the inactive path is the pixel-identical baseline
  (`test/rendering/render_smoke_test.cpp:2249-2273`, helper `:970`, `:1085-1110`).
- **The missing wire:** `update_aether_field` has NO call sites — grep over `src/` finds only the
  declaration (`RenderPipeline.h:419`) and definition (`RenderPipeline.cpp:2675`); grep over `test/`
  finds none. The client pulls the WIND field from the session (`src/luminumbra_client/main_client.cpp:4950`,
  `:6639`) but never pulls the aether field (`GetAetherFieldSystem` has zero client call sites). The
  read-only `grid()` accessor built for exactly this bridge (`AetherFieldSystem.h:88-91`) is unused.
  Net effect: `m_aetherFieldActive` is permanently false and the entire visual chain (README §2.4
  "Lumin emits soft glow", `README.md:79`) is dormant in every shipped path.

### 5. Generic conservative diffusion substrate — BUILT, used elsewhere (not by aether)

- `fields::ScalarFieldDiffusion` is a separate, engine-generic **conservative** solver with
  per-cell permeability, sealed cells, and impulse injection, plus an energy-conservation report
  (`src/luminumbra_common/fields/ScalarFieldDiffusion.h:37-53`), gate-tested
  (`test/fields/scalar_field_diffusion_gate_test.cpp:19-39`, runner
  `test/fields/scalar-field-diffusion.ps1`).
- Its one production consumer is the scent-stigmergy field
  (`src/luminumbra_common/ai/ScentField.h:31`, `:230` — one `ScalarFieldDiffusion` channel per
  scent species); the irrigation and soil-nutrient systems deliberately mirror its conservative
  model with their own integer kernels rather than consuming the class
  (`src/luminumbra_common/systems/IrrigationSystem.h:18` — its own conservative integer
  4-neighbour pass "like ScalarFieldDiffusion's"; `src/luminumbra_common/systems/SoilNutrientSystem.h:10`
  — "mirrors ... ScalarFieldDiffusion style", pure per-cell, no diffusion at all). The
  AetherFieldSystem does NOT use it — it has its own non-conservative Gauss-Seidel smooth. This
  solver is still the natural substrate for a future stateful aether model
  (sources/sinks/occlusion) without writing a new kernel.

### 6. Engine/game decoupling — enforced by gate; game layer is aspirational

- The engine deliberately knows only an "emissive scalar field"; game content assigns meaning
  (`AetherFieldSystem.h:5-8`). The `EngineGameSplitLint` gate bans game nouns (including `aetheric`)
  under `src/` (`.forge/scripts/validate-engine-frontier.ps1:5623-5645`, noun list `:5630-5640`).
- The game-content layer exists only as data/scripts and references engine API that does not exist:
  `system_aetheric_feedback.lua` expects an `AethericFieldComponent` and a
  `get_world_api():get_aetheric_value(pos)` returning `{lumin, umbra}`
  (`scripts/common/systems/system_aetheric_feedback.lua:12-18`); archetypes attach the component
  (`scripts/common/archetypes/glimmercap.json:13-15`, also `shadowstalker.json`,
  `grovestrider.json`); `directive_lunar_bloom.lua` mutates `aether.emission.strength`
  (`scripts/common/directives/directive_lunar_bloom.lua:22-32`). Grep over `src/` finds NO
  `AethericFieldComponent` and NO `get_aetheric_value` — none of this is executable today.
- "AethericField" appears as a component TYPE string only in the ECS snapshot test fixture
  (`test/support/EntitySnapshotFixture.h:48`) and its gate check
  (`validate-engine-frontier.ps1:2499`) — serialization-format coverage, not a live component.
- **Discrepancy found:** the lint's content scan lowercases file text and flags any occurrence of
  `aetheric` with an allowlist that always returns false
  (`validate-engine-frontier.ps1:5642-5645`, `:5666-5676`). A faithful replication of that scan
  against the current tree flags **11 engine files** (16 case-insensitive occurrences, all in
  comments — e.g. "Aetheric scalar field" at `AetherFieldSystem.h:3`,
  `GameSession.cpp:368`, `RenderPipeline.cpp:2677`). `-Mode EngineGameSplitLint` therefore FAILS if
  run against the current tree; either the T-I6 comments breached the lint or the gate has not been
  run since `fc8f476a`.

### 7. README §2.4 / §4.2 vision vs reality

| README claim | Status |
| --- | --- |
| "unified, persistent data field" updated "every tick via diffusion algorithm" (`README.md:72-77`) | Partial: field exists and diffuses every tick, but it is recomputed from noise per tick — `Update()` overwrites the field from the advected emission before diffusing (`AetherFieldSystem.cpp:171-174`); no energy persists across ticks. |
| "Energy flows from sources (crystals, sun, moon)... Entities absorb or emit energy" (`README.md:75-76`) | Unbuilt: emission is noise-only (`AetherFieldSystem.cpp:103-125`); no emitter/sink API exists. |
| "Lumin emits soft glow; Umbra creates deep shadows" (`README.md:79`) | Half-plumbed: single non-negative channel only; glow shader term exists but is inert (§4 above); no Umbra/absorption representation. |
| "Radiance Cascade Global Illumination" (`README.md:81`, `:134-137`) | Does not exist: no radiance-cascade code in the client (grep finds only the spec-015 `moon_radiance` channel, `src/luminumbra_client/rendering/RenderContext.h:112`). GI tech is RENDER/GPU-owned (software SHIELD-RT far-field is shipped; HW-RT RT-GI is the spec-014/021 GPU track). |
| "Subsurface scattering" (`README.md:80`, `:140`) | Does not exist: no subsurface/SSS in `res/shaders/` (grep: zero matches). RENDER-owned if pursued. |
| "Emissive materials (crystals, fungi) glow based on Aetheric Field strength" (`README.md:139`) | Not coupled: crystal glow scales by the static materials-LUT `emissive_intensity` (`res/shaders/lighting_pass.frag:541`, `:562-565`); the aether term is additive and independent (`:577`); no modulation of material emissive by field strength. |
| "Players can manipulate energy flow with tools" (`README.md:83`) | Unbuilt. The shipped "light tools" system is a pure photography light-QUALITY scorer, unrelated to aether (`src/luminumbra_common/game/LightTools.h:3-9`). |
| TDD §3.4 "Emission Decay: verify energy levels drop over time" (`docs/TDD.md:84`) | Untestable against the current model — there is no decaying state to observe (stateless-per-tick). The shipped tests correctly test what exists instead. |

### Shipped since the 2026-06-28 roadmap

**Nothing aether-specific.** `git log --since=2026-06-27` over `AetherFieldSystem.*`,
`src/luminumbra_common/fields/`, `res/shaders/lighting_pass.frag`, and `LightingPass.cpp` shows only
RENDER-pillar work touching shared lighting files: `3aa9740d` (spec-015 Pillar A moon radiance
channel), `6bb83b17`/`f3b31169` (spec-016 FR-D shader reflection), `66ac5459` (A-T04 moonlight),
`d22c0c81` (A-T01/A-T05b) — all owned by `pillar-render.md`. The aether pillar's own landings
(`fc8f476a` "iter-6 A1a: deterministic Aetheric scalar field (hash-neutral)", `bce99a5b` "iter-6 A1d
(1/2): aether render-tap plumbing (inert, pixel/hash-neutral)") predate the roadmap and are
reflected as `done` backlog items below. Notably, A1d was labelled "(1/2)" — the second half (the
client wiring) never landed.

## Gaps / debt

1. **The sim→render bridge is missing (the "(2/2)" that never landed).** Zero call sites for
   `update_aether_field` (`RenderPipeline.cpp:2675`); the wind field precedent shows exactly where
   the pull belongs (`main_client.cpp:4950`). Everything downstream is built and GPU-tested. This is
   the highest-value, lowest-effort item in the pillar.
2. **No sim/gameplay consumers.** `SampleAether` has no callers outside the system and its tests;
   the "(later) planner stimuli" hook is documented but unbuilt (`AetherFieldSystem.h:74-77`).
3. **No entity emitter/absorption API; stateless-per-tick model.** Emission is noise-only
   (`AetherFieldSystem.cpp:103-125`) and the field carries no state across ticks
   (`AetherFieldSystem.cpp:171-174`). The README's energy economy (sources, sinks, absorption,
   decay) requires a stateful layer — a deliberate world_hash bump and a persistence/replay story.
   The conservative `ScalarFieldDiffusion` solver (permeability + sealing + impulses) already exists
   as the substrate (`ScalarFieldDiffusion.h:37-53`).
4. **Single channel; no Lumin/Umbra polarity.** The game scripts expect `{lumin, umbra}`
   (`system_aetheric_feedback.lua:18`); the engine field is one non-negative scalar
   (`AetherFieldSystem.h:5-7`).
5. **The Lua game layer is dead content.** Three archetypes, a feedback system, and a directive
   reference a component and world API that do not exist in C++ (§6 above). Either charter the seam
   or mark the scripts as design documents.
6. **Glow color/intensity are not drivable.** `u_aetherGlowColor`/`u_aetherGlowIntensity` exist only
   as shader defaults (`lighting_pass.frag:33-34`); no C++ setter anywhere (grep: zero matches), so
   game content cannot theme the glow.
7. **Emissive materials are not aether-modulated** (README §4.2 claim; `lighting_pass.frag:541` is
   static LUT-driven).
8. **EngineGameSplitLint would fail on the current tree** — 11 engine files carry the banned noun in
   comments (`validate-engine-frontier.ps1:5639` vs e.g. `AetherFieldSystem.h:3`). Gate-vs-tree
   drift undermines trust in the split lint.
9. **No dedicated spec.** Specs 001–020 contain no aether spec (grep over `docs/specs/` finds only
   incidental mentions); the remaining vision has no charter, contradicting the research-before-spec
   standing rule for new systems.
10. **No SystemConfig flag.** The field is unconditionally on (`GameSession.cpp:690`; `SystemConfig.h`
   has only a comment mention) — acceptable because it is baseline-hashed, but future stateful
   features must follow the default-OFF plug-in convention.

## Risks

- **Determinism (high, for future work).** Every pinned constant is world_hash-bearing
  (`AetherFieldSystem.h:52-58`); emitters/dual-channel/stateful work each require a deliberate bump,
  heavy-oracle re-bless, and LREC1/lockstep evidence. The current `--smoke == 6f008a9f637c40b7`
  baseline embeds the aether sub-hash, so ANY solver change breaks the law of the audit.
- **Visual verification is blocked upstream.** Activating the tap needs a WorldVisualSweep re-bless,
  and headless IN_GAME capture currently hangs (RENDER pillar owns the fix; charter FR-A-003,
  `docs/specs/021-engine-framework-audit-charter/spec.md:90-92`). Sequencing AETHER-04 before that
  fix would leave the change unverifiable.
- **Stateful-model perf.** The bench budget is telemetry-only 0.40 ms/tick for 64x64x8 sweeps
  (`main_server.cpp:843`); an emitter-driven conservative layer roughly doubles field work on the
  main sim thread — needs the budget promoted to enforced before scaling.
- **Content rot.** The aspirational Lua layer silently references non-existent APIs; anyone wiring
  scripts to the engine will discover the seam is missing at runtime, not at review time.

## Opportunities

- **One-call visual win.** The entire glow chain lights up with a single client pull (mirror
  `main_client.cpp:4950`); the shader path is already proven by
  `RenderSmokeTest.AetherEmissiveTapBrightensLitOutput`. Highest beauty-per-line in the engine right
  now, and it directly serves the visual-fidelity mandate.
- **Reuse, don't rebuild.** The conservative `ScalarFieldDiffusion` (permeability, sealed cells,
  impulses, conservation reporting) is the ready-made kernel for the stateful energy model; the
  scent field proves it composes with the deterministic tick (irrigation/soil replicate the same
  conservative pattern independently with their own integer kernels).
- **Shared field-overlay path.** Aether/wind/weather share grid identity
  (`fields_hardening_test.cpp:680-692`) — one debug-overlay/upload path can visualize all coarse
  fields for free once the aether texture path is exercised.
- **Photography-loop synergy.** A high-aether zone is exactly the kind of "magical light" the photo
  loop rewards; an aether term in the pure `LightScene` scorer (`LightTools.h:62-68`) is a cheap,
  deterministic gameplay coupling once consumers land.
- **GI coupling later, not GI now.** When the GPU track lands RT-GI (spec 014/021 Group E), the
  aether field is a natural emissive-source injection; the engine seam (field texture + world
  mapping) already exists in RenderContext Group G.

## Backlog items

| id | summary | spec | effort | risk | deps | status | proving_signal |
| --- | --- | --- | --- | --- | --- | --- | --- |
| AETHER-01 | Deterministic Aether scalar-field sim core (T-I6-A1a): pinned 24m/64x64 grid, noise emission, wind advection, 8-sweep diffusion, every-tick GameSession update | new | L | low | — | done | validate-engine-frontier.ps1 -Mode AetherFieldDeterminism + ctest AetherFieldSystem.SubHashIsDeterministicAcrossRuns |
| AETHER-02 | world_hash `aether` sub-hash (bump #4, d950a6afc12a5cdc -> f17726d44054d133) + AetherHardening suite (run==replay exact, anchor quantization, bounds) | new | M | low | AETHER-01 | done | ctest AetherHardening.RunEqualsReplayWithWindFlattenedExact + --smoke run==replay 6f008a9f637c40b7 (heavy oracle) |
| AETHER-03 | Render emissive-tap plumbing, deliberately inert (T-I6-A1d 1/2): R32F upload, RenderContext Group G, LightingPass unit-10 bind, shader glow term, GPU shader test | new | M | low | AETHER-01 | done | ctest RenderSmokeTest.AetherEmissiveTapBrightensLitOutput |
| AETHER-04 | Wire the missing client sim→render bridge (T-I6-A1d 2/2): pull GameSession aether grid into RenderPipeline::update_aether_field per frame (mirror the wind pull), re-bless visual baselines | new | S | medium | RENDER:headless-IN_GAME-capture-hang-fix | todo | WorldVisualSweep rerun + ctest RenderSmokeTest.AetherEmissiveTapBrightensLitOutput stays green |
| AETHER-05 | Author the Aetheric-field completion spec (research-first): stateful energy model, emitters/absorbers, Lumin/Umbra polarity, gameplay coupling, engine/game component seam | new | M | low | — | todo | NEW: TDD-LOCK SDD-trace scenario — every AC in the new spec links one named ctest/gate per test/features/TDD-LOCK.md |
| AETHER-06 | Deterministic entity emitter/sink API + stateful field layer on the conservative ScalarFieldDiffusion solver, SystemConfig-gated default-OFF, deliberate world_hash bump when ON | new | L | high | AETHER-05 | todo | NEW: ctest AetherEmitterDeterminism — emitter-driven field bit-identical run==replay and deposits decay over ticks; plus heavy oracle --smoke + LREC1 replay re-bless |
| AETHER-07 | Engine ECS field-emitter component + Lua world-API sampling seam (makes system_aetheric_feedback.lua / glimmercap.json executable; engine-generic naming to satisfy the split lint) | new | M | medium | AETHER-06 | todo | NEW: ctest AetherScriptBinding — Lua get_aetheric_value returns SampleAether at the entity position in a fixture session |
| AETHER-08 | Lumin/Umbra dual-polarity: second scalar channel (engine-generic 2-channel field); game maps light/shadow attunement onto it | new | L | high | AETHER-05, AETHER-06 | todo | NEW: ctest AetherDualChannelDeterminism (both channels bit-stable run==replay) + heavy oracle --smoke + LREC1 replay re-bless |
| AETHER-09 | Reconcile EngineGameSplitLint: 'aetheric' noun ban vs 11 engine files carrying "Aetheric" in comments — reword comments to "Aether" or allowlist the engine field docs; gate must go green | new | S | low | — | todo | validate-engine-frontier.ps1 -Mode EngineGameSplitLint (green on the current tree) |
| AETHER-10 | Drive u_aetherGlowColor / u_aetherGlowIntensity from RenderContext (C++ setters + tuning hook); today they are shader-const defaults game content cannot theme | new | S | low | AETHER-04 | todo | NEW: render_smoke_test case — a non-default glow color propagates to the lit output; WorldVisualSweep on defaults stays pixel-identical |
| AETHER-11 | Aether-modulated emissive materials (README §4.2): scale materials-LUT crystal/fungi emissive by sampled field strength instead of leaving the terms independent | new | M | medium | AETHER-04, AETHER-06 | todo | WorldVisualSweep rerun + NEW render_smoke_test assertion — material emissive output scales monotonically with the bound field value |
| AETHER-12 | Sim/gameplay consumers: AI planner stimulus from SampleAether + an aether axis in the pure photo light-quality scorer (LightScene) | new | M | medium | AETHER-06, AETHER-07 | todo | NEW: ctest AetherStimulusDeterminism — stimulus values deterministic across runs; ecology sub-hash unchanged with the feature OFF |
