# HANDOFF — Forge-driven roadmap driver (configurable systems edition)

Date: 2026-06-18 · Branch: `feat/polyglot-audit-roadmap` (luminumbra, **LOCAL-ONLY — never push**)
Forge repo for forge bugs/PRs: `D:\Coding\forge-new` (push allowed; gh = `rv-daddison`).

> **Purpose.** Drive EVERY remaining roadmap pillar through Forge **Spec-Driven (SDD) +
> Test-Driven (TDD)**: spec is the source of truth, the failing test is written from the
> spec's Acceptance Criteria FIRST, then code is implemented to green. Forge runs the
> lifecycle (brainstorm → spec → write-failing-tests → execute → verify), all on top of a
> new **cross-cutting feature-flag / system-config layer** so every system — old and new —
> can be turned **on/off and tuned from data**, no recompile. This doc is the next-session
> driver: pick a pillar, run the listed forge commands, RED→GREEN→REFACTOR. The method is §2.

---

## 0.5. CRITIQUE REVISIONS — owner-approved 2026-06-18 (THESE OVERRIDE §1–§6 BELOW)

A `/forge-critique` devil's-advocate pass (report:
`.forge/critique-handoff-roadmap-20260618-221947.md`) verified this doc's facts as sound but
found 3 blockers + structural issues. Owner adopted the revised scope. **Where the text below
conflicts with this section, this section wins.**

1. **Hash contract (fixes a contradiction — refined after grounding in the code).** Codebase
   check (`persistence/WorldPersistenceRoundtrip.h:100`) shows per-system sub-hashes are
   **ADDITIVE and SEPARATE** from the top-level `world_hash` ("unchanged byte-for-byte" by them),
   and each is **empty `{}` when its system has no participants** (wind/weather/aether/scent/
   plant). So model a `config` sub-hash on those: it is an additive field that **does NOT touch
   the top-level `world_hash`** (canonical `d950a6afc12a5cdc` holds — **zero re-pin** from
   introducing SystemConfig), and `ComputeConfigSubHash()` returns empty `{}` when every `sim.*`
   flag is at default (sub-hash set byte-identical too). A `sim.*` flag only moves the top-level
   hash when it actually changes sim STATE (its owning pillar's deliberate, intrinsic bump).
   Render flags never touch any hash. **Full contract: `.forge/specs/system-config/spec.md` P2.**
   (This is cleaner than the critique's original "one intentional re-pin"; that's superseded.)
2. **Split §1.** **§1a (FIRST, ~1 slice):** minimal `enabled(flag)` + typed `param()` over a
   static default table, implemented as a **resolved immutable per-tick snapshot** — packed
   bitset for flags, flat enum-indexed array for params; `enabled()` is an O(1) bit test
   (perf AC: zero alloc/locking per entity on hot paths, 36k-entity / 30Hz budget). New
   systems consume §1a. **§1b (LAZY, deferred):** migrate each *existing* toggle only when
   already touching that subsystem, each with a before/after determinism+visual diff proving
   zero behavior change. **No big-bang migration.**
3. **Reorder D before C.** Erosion (D) lands BEFORE SHIELD-RT (C) so C is blessed against final
   terrain. Only terrain-independent C parts (sky-stencil, aerial-perspective LUT) may precede D.
4. **Game-profile baseline.** Maintain a first-class **"game profile"** config (intended-ON set;
   seed = today's known-green behavior) gated ALONGSIDE the all-off baseline. Every slice keeps
   BOTH green. Bless named bundles ("ecology", "weather", "photography"), not just all-off.
5. **Tiered process.** **T1 full lifecycle:** §1, C, E, F. **T2 (spec+tests+verify):** §4
   self-contained systems (fire/soil/pollination/disease). **T3 (test+implement):** mechanical
   toggles (overlays, difficulty). Determinism tests mandatory for ALL sim items; visual
   re-bless only on render-touching slices.
6. **Scope F to the seam.** Land/keep `ILockstepTransport` + TCP local path for determinism +
   API shape only. DEFER delta-snapshot/prediction/reconciliation until a 2nd test box OR a
   deterministic network-sim harness (injected loss/latency) exists. Steam over-the-wire stays
   blocked on single-PC.
7. **Float-determinism AC for A/D.** Procedural geometry is VISUAL-ONLY; any value the sim
   consumes is quantized to fixed-point at the sim boundary; sim-read erosion offset field is
   baked to integers + committed + hashed as data (not recomputed in float per platform). Add a
   `-ffp-contract`/`-ffast-math` on/off hash-parity test.
8. **Rollback + prioritization.** Baseline break → **revert the slice by default**; re-pin only
   with a written justification of why the change is intended. Tag each commit with its
   baseline-state hash. Tag every item **game-critical / engine-foundational / gold-plating**;
   pull from the backlog by priority, not list order.

**Revised order:** §1a → §1b (lazy, opportunistic) → A → B → **D → C** → E → §4 (fire/soil/
pollination first, T2) → F-seam → G (pure-sim capture-scoring fixture only; full photo loop
stays LAST per the engine-first directive).

**G-ordering note:** the critique suggested pulling photography forward, but standing owner
directive (memory `long-range-roadmap`, `iteration-4-priorities`) is *engine first, photography
100% last*. Resolution: only a pure-sim capture-scoring determinism fixture is pulled forward;
the actual photo/camera loop stays last.

---

## 0. State at handoff (what's DONE — committed local-only, all gated green)

Living-World Foliage pillar is COMPLETE at sim/farming/atmospheric/visual level. 7 commits:
- `0a67935` ecology nodiscard build-fix.
- `273e769` I8 render: dusty palette tint (material LUT row 3), moonlight (terrain+grass),
  wind, tree decimation (2.06M→sane via asset_processor `--max-tris`/`--primitive` + meshopt
  SimplifySparse), full UV bark/leaf texturing (static-model texture lane), denser/fuller groves.
- `58b3caf` plant pillar foundation (genome/phenotype/growth/breeding + 4 tests).
- `8a110f4` tick-wire growth into the sim (atmospheric, opt-in via `PlantTag`, baseline-safe).
- `ff23c59` genetic+maturity grove size variation (sim→visual).
- `f0d5bd2` farming loop (plant/water/fertilize/harvest/breed + tests).
- `50d69b0` bind growth to terrain soil + altitude temperature.

Gates: WorldVisualSweep objective critique **0/48**, RenderSmokeTest **14/14**, plant/farming +
NetworkStateHash/world-hash baselines all green. Memory updated: `living-world-foliage-pillar`,
`render-controls-and-rebless`.

**Build/run (every time):** prepend `C:\msys64\ucrt64\bin` to PATH. Two trees — gate tree
`cmake --build --preset debug` (build/debug), main `cmake --build build` (build/bin). Build the
tree you test. Gates: `.\.forge\scripts\validate-engine-frontier.ps1 -Mode <X>` and
`python tools/visual_critique.py analyze build/debug/test-artifacts/runtime/world-visual-sweep --strict`.
Sweep: `luminumbra_client_app.exe --scenario world_visual_sweep --auto-create-world --auto-enter-world
--timed-run 20 --no-audio --no-ui --runtime-artifact-dir <dir>` then PPM→PNG (PIL) for the critique.

---

## 1. FOUNDATION FIRST — the system-config / feature-flag layer (do this before any pillar)

**Why:** the owner wants every system configurable + toggleable on/off. Today toggles are ad-hoc:
`PlantTag` opt-in, `LUMIN_GRADE`/`LUMIN_ATMOS` env vars, hardcoded moon/wind/palette constants,
scatter consts in `main_client.cpp`. Unify them under ONE data-driven registry so each pillar below
plugs into a consistent on/off + tuning seam.

**Forge it:**
```
/forge-brainstorm  "data-driven feature-flag + per-system tuning registry; sim vs render split; determinism (sim flags fold into world_hash, render flags don't); hot-reload?"
/forge-spec        "SystemConfig: data/common/systems.json registry of {system: {enabled, params{}}}, loaded at startup, queried by every system; render section (render-only, never hashed) vs sim section (flags that change sim behaviour bump world_hash deliberately)"
/forge-grill       <the spec>      # probe: determinism of toggles, default-on vs default-off, migration of existing env-vars/consts
/forge-plan        ; /forge-critique current ; /forge-pre-mortem ; /forge-execute ; /forge-verify
```
**Design seed (for the spec):**
- `src/luminumbra_common/core/SystemConfig.h/.cpp` — parse `data/common/systems.json`; API
  `bool enabled(key)`, `float param(key, name, default)`, `Vec3 param3(...)`. Two sections:
  `sim` (flags that alter sim behaviour → fold a `ComputeConfigSubHash` into world_hash so a
  changed flag is a deliberate, detectable bump) and `render` (render-only, never hashed).
- **Migrate existing toggles into it** (each becomes a flag + params): plant pillar
  (`PlantTag` stays the per-entity opt-in, but `sim.plant_growth.enabled` gates the system + carries
  growth/mutation/farming-difficulty params), moonlight (`render.moonlight.{enabled,color,strength}`),
  wind (`render.tree_wind.{enabled,strength}`), dusty palette (`render.albedo_tint` per material),
  foliage density (`render.grass.density_scale`, `render.tree_scatter.{count,cell,grove...}`),
  tree LOD/impostors, atmosphere (fold `LUMIN_ATMOS`), grade (fold `LUMIN_GRADE`).
- Convention for ALL pillars below: **every new system reads `SystemConfig::enabled("<key>")` in its
  init/tick and is OFF by default until its gate is green** (matching the plant-pillar opt-in
  discipline). Tuning params live beside the flag. **NOTE (§0.5.1):** "byte-identical baseline" is
  superseded — render flags never touch the hash; sim flags use a versioned default-constant
  sub-hash → one intentional re-pin at §1, zero at defaults after. See §0.5 for the corrected contract
  and §0.5.4 for the required game-profile ON baseline.
- Optional: a tiny in-game debug overlay / console to flip render flags live (render-only).

**TDD — write these tests RED first** (before `SystemConfig.cpp` exists): (a) a missing/empty
`systems.json` yields all-defaults (no crash); (b) a flag set OFF makes its system a no-op AND the
canonical `world_hash` / NetworkStateHash baseline stays byte-identical; (c) a `sim.*` flag change
moves `ComputeConfigSubHash` (deliberate, detectable) while `render.*` flags do NOT; (d) param parse +
defaults round-trip. Implement to GREEN, then migrate one existing toggle (e.g. moonlight) as the first
real consumer with its own RED test.

This layer is the substrate; everything below references it.

---

## 2. THE METHOD — SDD + TDD via Forge (NON-NEGOTIABLE; apply to every pillar in §3–§4)

This roadmap is driven **Spec-Driven (SDD)** and **Test-Driven (TDD)**. The forge spec is the single
source of truth; nothing is implemented that isn't in the spec, and nothing is "done" until the tests
derived from the spec's Acceptance Criteria are GREEN. The project already locks this discipline —
see `.forge/specs/iter6/TDD-LOCK.md` and enable forge's TDD mode (`/forge-setup`, TDD on). The two
loops interlock:

**SDD outer loop** — spec is the contract:
1. **Research → Brainstorm.** `/forge-brainstorm "<pillar> — approaches, trade-offs, what to toggle"`
   (or `/brainstorm`). Research FIRST from journals/papers/talks (memory `research-before-spec`);
   cited briefs are on file under `.forge/artifacts/engine-iteration-6/research/`.
2. **Spec.** `/forge-spec "<pillar>"` → spec.md. Every requirement gets a **testable AC** (no AC →
   it can't be TDD'd → rewrite it). Add the standing **feature-flag AC**: "system X is gated by
   `SystemConfig::enabled('x')`, OFF by default, params P; with the flag OFF the canonical
   world_hash / visual baselines are byte-identical."
3. **Sharpen.** `/forge-grill <spec>` + `/forge-clarify` — close underspecified areas until every AC
   is unambiguous and measurable. `/forge-critique <spec>`, `/forge-red-team`, `/forge-pre-mortem`
   BEFORE writing code.

**TDD inner loop** — RED → GREEN → REFACTOR, gated by the spec's AC:
4. **RED — write the failing tests/gates FIRST, straight from the AC.** Use
   `/forge-evaluate "<spec>"` to GENERATE the evaluators/gates from the spec (this is the SDD→TDD
   bridge), then add concrete unit tests (gtest in `test/`, registered in `test/CMakeLists.txt` —
   pattern: `test/ai/plant_growth_test.cpp`) and/or a visual/objective gate
   (`tools/visual_critique.py` + a fixture in `tools/test_visual_critique.py`) and/or a perf-budget
   gate. **Run them, watch them FAIL** (proves the test bites). Sim systems: a determinism test
   (`run==replay`, a `Compute<X>SubHash`) is mandatory.
5. **GREEN — `/forge-execute`** (or `/forge-dispatch` a single agent) to implement the MINIMUM that
   makes the tests pass. **VERIFY-GAP CAVEAT** (memory `forge-dispatch-verify-gap`): put the verify
   command in `agent_contract.verify_command` or a `## Verify With` block (NOT prose) so
   `complete_task()` actually gates on `verification_ok` (PR #1728); verify locally after every dispatch.
6. **REFACTOR** — `/simplify` or `/code-review` the green diff; tests stay green.
7. **Verify + re-bless.** `/forge-verify` (hygiene/docs/perf/security/testing gate) + re-bless visual
   gates if the look changed intentionally (passing `--strict` run; detector false-positives get a
   fixture-backed fix, never a reclassification).
8. **Commit (no push)** — split unrelated concerns (the critique caught a bundled build-fix this session).

Useful extras: `/forge-evaluate` (the SDD→TDD evaluator generator), `/forge-evolve` (optimize prompts/
params), `/forge-retro` after a pillar, `/forge-dashboard` / `/forge-cost` to watch.

**The bar:** no code before a spec (SDD); no implementation before a failing test derived from its AC
(TDD); no "done" before that test is green AND the gates/baselines hold. The plant pillar this session
was built this way (4→10 tests written alongside; baselines held) — keep that rhythm.

---

## 3. ROADMAP PILLARS — forge plan + config flag + RED tests for each

> For EVERY pillar: spec it (SDD), then write the listed RED tests FIRST and watch them fail (TDD),
> then `/forge-execute` to green. The "RED tests" lines below are the test-first starting point —
> expand them from the spec's AC. Every system is OFF by default behind its flag until its gate is green.

### A. Foliage polish (finish the pillar)  — flag group `render.foliage.*`, `sim.plant_growth.*`
- **Live-growth render bridge** — drive tree scale from the TICKED growth stage (visible growth over
  time), not just baked maturity. Flag `render.plant_live_growth.enabled`. A small dedicated ticked
  grove avoids the 36k-entity per-tick cost.
- **Procedural generator** — space-colonization (Runions 2007) baked per (genome bucket, growth stage)
  → mesh/impostor cache; TRUE structural genetic variation. Flag `render.plant_procgen.enabled`.
  Research: the proc-gen brief (visual-only/baked spine; integer-sim boundary).
- **Season/time-of-day light** into the plant env sampler (autumn color, winter dormancy). Flag
  `sim.plant_growth.season_coupling`.
- **2nd+ species** (data-driven species table → genome ranges + visual model). Flag `sim.plant_growth.species`.
- Forge: `/forge-brainstorm "live plant growth render bridge + space-colonization procgen; per-frame instance dirty-cache"` → spec → plan → critique → execute → verify (visual sweep + plant tests).
- **RED tests first:** procgen is a PURE function of (genome, stage) → identical mesh for identical input (hash the baked vertices); flag OFF → scatter byte-identical to today; live-growth bridge scales monotonically with stage. Then implement to green.
- Key files: `main_client.cpp` scatter, `GBufferPass.cpp`, `PlantGrowthSystem.h`, `tools/asset_processor.cpp` (procgen bake), `tree_textures.json`.

### B. Phase 1.3 — tree LOD + octahedral impostors (perf at scale)  — flag `render.tree_lod.{enabled,distances}`, `render.tree_impostors.enabled`
- asset_processor already emits LODs (`--max-tris`). Add LOD-distance swap in `geometry_pass_static_meshes`
  (per-instance distance → LOD mesh bucket) + hemi-octahedral impostor atlas for far trees + dithered
  LOD/seeded-TAA + per-frame static-instance dirty-cache (critique-flagged CPU waste). Research: foliage brief.
- Forge: brainstorm (impostor capture rig, LOD thresholds, popping) → spec → plan → red-team (determinism of dither/TAA) → execute → verify (sweep + a perf-budget gate via `/forge-evaluate`).
- **RED tests first:** a tri-count/draw-call budget gate (visible trees under N tris) that FAILS at today's 85k-leaf×12k load; LOD selection picks the right LOD per distance; flag OFF → full-res unchanged.

### C. SHIELD-RT GPU far-field (I7.2)  — flag `render.far_field.{mode,view_distance}` (mode: off|cdlod|raymarch)
- Spec scaffold: `.forge/artifacts/engine-iteration-6/WAVE-A-SPEC.md` (Wave A.2). Research verdict
  (far-field brief): spine = **CDLOD/geometry-clipmaps + Distant-Horizons cached-LOD + Transvoxel
  near band + max-mipmap raymarch finishing layer + Hillaire aerial perspective + sky-stencil horizon
  fix**; demote "SDF" to optional. Gate on the Phase-0.3 quiet-machine perf baseline.
- Forge: `/forge-brainstorm` (CDLOD vs clipmaps; raymarch acceleration; AgX vs ACES display transform)
  → `/forge-spec` (update WAVE-A-SPEC) → `/forge-grill` → plan → critique + pre-mortem (thin-feature
  shimmer, disocclusion) → execute → verify (FarLodHorizon gate; fix the `default` boundary-band
  false-positive structurally via depth/stencil sky classification).
- **RED tests first:** write the FarLodHorizon `default` gate to FAIL on today's over-blue-cap
  false-positive and pass only on the depth/stencil sky fix; a view-distance assertion; AP froxel
  parity; `mode:off` → far-LOD byte-identical to today.

### D. I7.6 erosion (hydraulic/thermal, baked)  — flag `sim.erosion.{enabled,iterations,...}` (DELIBERATE world_hash bump)
- Research/spec: `WAVE-A-SPEC.md §A2` + `research/erosion-far-lod-meshing.md`. Verdict: Mei-2007
  virtual-pipe + Musgrave thermal, **CPU single-thread, fixed CFL clamp, bake ONE finest-res offset
  field with an apron then downsample for all LODs** (seam-free by construction). Land BEFORE SHIELD-RT
  parity baselines bless (changes the geometry SHIELD-RT renders).
- Forge: brainstorm (droplet vs grid; apron size) → spec → plan → critique → execute → verify
  (world_hash bump is intentional — memory `local-dev-world-hash-bumps-ok`; re-pin baseline + keep run==replay).
- **RED tests first:** the baked offset field is a PURE function of (seed, region) → identical bytes
  on re-run (determinism); seam-continuity test (apron-cropped tiles match at borders); LOD downsample
  == area-average of the fine field; `enabled:false` → terrain byte-identical (no bump).

### E. I9 emergent AI / ecology  — flag group `sim.ecology.{boids,scent,perception,evolution,utility_ai}` (each toggleable)
- Spec: `.forge/specs/emergent-ecology/spec.md`. Foundations landed (boids, ScentField, perception,
  GA). Research change: **make Utility AI (IAUS) the primary decision arbiter, demote GOAP**; plus
  distance-dependent vision cone, self-adaptive σ, fixed-point Lotka-Volterra spawn governor, canonical
  sorted iteration. The plant-pillar GA (`ai/Evolution.h`) and creature evolution share one engine.
- Forge: `/forge-brainstorm "utility-AI arbiter over boids/scent/perception; predator-prey balance;
  what to toggle per-species"` → spec revise → plan → critique/red-team → execute → verify (ai tests + run==replay).
- **RED tests first:** IAUS picks the higher-utility action (scored fixture); boids neighbour-sum is
  order-stable (canonical sorted iteration → run==replay); each sub-system flag OFF is a no-op + hash-stable;
  Lotka-Volterra spawn governor stays bounded over N ticks.
- **Configurable systems to ADD here** (each a flag): predator/prey roles, herd alarm signalling,
  wind-borne scent trails, trait evolution on/off, population governor.

### F. I10 networking  — flag `sim.net.{mode,tick_hz,snapshot_hz,interest_radius}` (mode: single|listen|dedicated)
- Spec: `MULTIPLAYER-BLOCKER-SPEC.md` v2 (authoritative server + delta-snapshot + client prediction;
  rollback/lockstep correctly rejected). Transports behind `ILockstepTransport`: TCP / GNS-UDP / Steam-SDR.
  Research refinements: decouple physics tick (60-120Hz) from snapshot rate (~30Hz); adaptive server
  input buffering (consume 0/1/2); bound the authoritative-physics actor set + grid AOI with cached
  relevancy. Steam over-the-wire still blocked on single-PC (memory `single-pc-testing-constraint`).
- Forge: brainstorm → spec revise → plan → red-team (lag-comp, desync) → execute → verify
  (ReplicationScale ctest, `--replicate --avatars N`; memory `multiplayer-scale-testing`).

### G. I11 photography (the game loop, last)  — flag `game.photo_mode.enabled` + lens/film data
- Roadmap: `engine-roadmap/long-range-roadmap.md` Iter 7. Camera/lens (DoF/exposure/focus) + capture
  scoring + creature Codex + light/shadow tools. Couples to Atmospheric + Aetheric + the new
  Living-World Foliage + ecology state (rich subjects).
- Forge: `/forge-brainstorm` (capture scoring; composition/light metrics; zen pacing) → spec → plan
  → execute → verify (capture-scoring determinism fixture + photo-mode perf budget).

---

## 4. NEW configurable systems to brainstorm + add (owner's "more systems, on/off")

Each is a candidate to run through §2 (SDD+TDD: spec → RED determinism + flag-OFF-is-no-op tests →
green) and ships gated OFF behind a `SystemConfig` flag. Grouped by pillar they extend. Capture with
`/forge-idea`, expand with `/forge-brainstorm`:

- **`sim.fire`** — emergent fire/burn spread across foliage (cellular spread + wind + moisture
  resistance from plant genome); deterministic. Toggles drought→wildfire risk. Ties to weather + plants.
- **`sim.soil`** — DST-style soil nutrient grid (N/compost/manure) consumed/produced per plant stage →
  crop rotation + companion planting depth for farming. Toggle simple vs deep agronomy.
- **`sim.pollination`** — plants self-seed / cross-pollinate with neighbours (wind/insect-borne genome
  mixing), so a field genetically drifts over seasons. Reuses BreedPlants.
- **`sim.disease`** — configurable pest/blight that spreads via proximity + low plant quality; tending
  (fertilize) resists. Difficulty slider.
- **`sim.irrigation`** — water-flow / soil-moisture diffusion grid (player digs channels) feeding the
  plant moisture env. Toggle manual-watering vs irrigation.
- **`render.seasonal_color`** — foliage albedo shifts by season (autumn ochre/red, winter bare) driven
  by the sim season phase; render-only, flag + palette.
- **`sim.wildlife_interactions`** — creatures eat/trample/disperse plants (ecology × foliage coupling).
- **`render.weather_events`** — configurable storms/droughts/snow as scheduled or random events with
  intensity sliders (promote the existing weather into a tunable event system).
- **`render.debug_overlays`** — live flag/console overlay to flip render systems in-game.
- **`game.difficulty`** — a meta-config that scales farming difficulty, growth speed, disease, mutation
  rate (one slider fanning out to the per-system params).

**Brainstorm kickoff (paste into the next session):**
```
/forge-brainstorm  "Configurable Living-World systems on top of the plant pillar: fire spread, soil
nutrients, pollination/genetic-drift, disease/pests, irrigation, seasonal color, wildlife-plant
coupling, weather events. For each: deterministic sim design, what params to expose, default-off
flag, how it couples to existing weather/terrain/plant/ecology state, and the minimal first slice."
```

---

## 5. Resume pointers / gotchas (read before executing)

- **Component namespaces:** `Luminumbra::Components` (capital L), systems `luminumbra::foliage`/`::ai`/
  `::core` (lowercase). Mixing bites — alias `namespace Comp = ::Luminumbra::Components;` inside lowercase ns.
- **Determinism contract:** sim = float-but-deterministic via `DeterministicRng` + `DeterministicMath`
  + `-ffp-contract=off`; no wall-clock/libm-transcendentals on the sim path; id-ordered traversal.
  Plant RNG seed offset = **+15** (wind+11, weather+12/13, aether+14 taken). New sim systems: claim the
  next offset, record in `.forge/.../design-decisions`, add a `Compute<X>SubHash`, keep opt-in/OFF.
- **Tick order** (`GameSession::TickSimulation`): anim → instinct → perception → scent → locomotion →
  wind → weather → aether → **plant growth (slot 6)** → event-bus drain. New sim systems append after,
  gated by a participant/flag check so the canonical roster stays byte-identical.
- **Render-only vs hashed:** trees are client decoration (never hashed); render flags never touch
  world_hash; sim flags fold a config sub-hash (deliberate bump). Visual-gate baselines re-bless by a
  passing `visual_critique.py --strict` run (no golden files); detector false-positives get a
  **fixture-backed** fix in `tools/test_visual_critique.py` (precedent: FOLIAGE_SPARSE noon-gate,
  GREEN_SKY_SPECKLE), never a reclassification.
- **Asset gotchas:** tree binary assets (`.lmesh`/`.ltex`) are gitignored (locally generated via
  asset_processor `--max-tris`/`--primitive`); the `tree_textures.json` manifest IS tracked. Leaf cutout
  is luma-keyed in LINEAR space (SRGB8 array) at ~0.025; foliage decimation needs meshopt SimplifySparse.
- **Forge hygiene:** file forge bugs to `rossvideo/forge` proactively (memory `forge-issue-reporting`);
  `quality_gate:` belongs under `dispatch.quality:` (PR #1735 warns on the misplaced top-level key);
  put verify cmds in `agent_contract.verify_command`.
- **Execution model:** Fable gone; all planning + impl on Opus 4.8; dispatch specs self-contained
  (memory `execution-model-tiering`).

---

## 6. Suggested order for the next session(s)  — SUPERSEDED BY §0.5 (revised order)

> ⚠️ The numbered list below is the ORIGINAL order. §0.5 overrides it: §1 is split §1a→§1b,
> and **D (erosion) runs BEFORE C (SHIELD-RT)** — item 4 here had them backwards. Use the
> §0.5 "Revised order" line. Kept below for history.

1. **§1 SystemConfig feature-flag layer** (substrate) — now **§1a minimal first, §1b lazy** (§0.5.2).
2. **§3.A foliage polish** (live-growth bridge + procgen — finishes the headline pillar) and
   **§3.B tree LOD/impostors** (perf) — both plug into the flags.
3. Brainstorm + slice **§4 new systems** (fire/soil/pollination first — they deepen the farming game).
4. ~~**§3.C SHIELD-RT** then **§3.D erosion**~~ → **§3.D erosion THEN §3.C SHIELD-RT** (§0.5.3:
   erosion bumps hash + changes geometry before SHIELD-RT bless).
5. **§3.E AI/ecology**, **§3.F networking** (seam only, §0.5.6), **§3.G photography**
   (pure-sim capture-scoring fixture only; full loop LAST per engine-first directive).

Run each through the §2 forge loop. Commit per slice (no push), split unrelated concerns, keep the
gates green, re-bless on intentional look changes.
