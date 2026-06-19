# HANDOFF — Hardening campaign complete + the deferred roadmap (critique/spec/plan me)

Date: 2026-06-19 · Branch: `feat/polyglot-audit-roadmap` (luminumbra, **LOCAL-ONLY — never push**)
Head at handoff: `6b79f66` · Prior handoff: `2f2b021` (2026-06-18, forge-driven roadmap driver)
Forge repo for forge bugs/PRs: `D:\Coding\forge-new` (push allowed; gh = `rv-daddison`).

> **Purpose.** This is the next-session driver. It (1) records EVERYTHING landed since the
> 2026-06-18 handoff so nothing is re-done, and (2) lays out the deferred backlog as a set of
> options for the next dev to run through Forge's full **critique → spec → plan** lifecycle
> (`/forge-critique`, `/forge-spec`, `/forge-grill`, `/forge-plan`, `/forge-pre-mortem`) BEFORE
> executing. Method is unchanged: SDD+TDD, RED→GREEN→REFACTOR, gated, commit-per-slice, no push.

---

## 0. How to build / test (every session — read first)

- **PATH:** prepend `C:\msys64\ucrt64\bin` to PATH for EVERY build/ctest/gate call
  (memory `toolchain-path-contamination`: KiCad/mingw64 otherwise shadow ucrt64 → silent cc1plus
  exit 127).
- **TWO build trees** (memory `build-tree-gotcha`): `cmake --build build` → `build/bin`; the gate
  tree `cmake --build --preset debug` → `build/debug`. Build the tree you test. NOTE: `build/debug`
  compiles tests with `-Werror=unused-function` (the `build` tree does not) — an unused test helper
  fails the debug build. You can run two independent builds (one per tree) concurrently without
  collision — used this session to keep authoring while a gate ran.
- **Gates:** `.\.forge\scripts\validate-engine-frontier.ps1 -Mode <X>` then for visual slices
  `python tools/visual_critique.py analyze build/debug/test-artifacts/runtime/world-visual-sweep --strict`.
- **Determinism contract:** sim = float-but-deterministic (`DeterministicMath` Sin/Cos/Sqrt/Atan2 only,
  NO exp/log/pow; `DeterministicRng` splitmix64 seeded from INTEGERS only; `-ffp-contract=off`);
  id-ordered traversal; no wall-clock/libm-transcendentals on the sim path. New sim systems are
  per-entity OPT-IN (gated by a participant component) → empty roster is byte-identical, so the
  canonical hash holds. RNG seed-offset registry is at the bottom of this doc.

---

## 1. LANDED since the last handoff (DONE — committed local-only, gated green)

> ~100 commits. Grouped by theme; representative commit hashes in parens. **Do not re-do.**

### 1a. SystemConfig substrate + settings/controls (the feature-flag layer)
- **§1a data-driven feature-flag + tuning registry** (`a6a5ea7`): `core/SystemConfig.{h,cpp}` +
  `data/common/systems.json`; `enabled(key)` / `param(key,name,default)`. **Config sub-hash is
  ADDITIVE / sim-only and hash-NEUTRAL at defaults** (append-chain finding `bc89254`) → ZERO re-pin;
  new sim systems plug in default-OFF (memory `system-config-substrate`).
- **user.* settings layer + per-user overlay persistence** (`936e43a`, `f6e3989`).
- **Settings + controls**: VSync/look-sens/FOV (`54b3069`), rebindable keyboard `InputAction`
  action-map + F8 settings menu (`e857dd4`), window-mode + resolution (`18aa714`, `eeeab91`),
  master volume real apply (`32f01fa`), `settings.rml` + `SettingsBridge` round-trip (`bf8d63d`,
  `5b220e6`) (memory `settings-and-controls-system`).
- **Consolidated `docs/STANDARDS.md`** (`f6e3989`).

### 1b. Living-World Foliage pillar A (procgen forests + farming)
- **Deterministic procedural plant geometry** (genetic + atmospheric) + pipe-model branches +
  sun-facing leaves (`b0a68b7`, `e257bb9`, `3b26f28`); procgen REPLACES baked models behind
  `render.plant_procgen` (`face7cb`, `ca4a045`); fuller canopy (`41a6f70`, `98c99ca`).
- **VAST programmatic forest via instanced procedural-tree PALETTE** (no baked model) (`a8f4f16`),
  per-instance tree LOD mesh swap by camera distance (Track-B) (`db09f45`), **far-field LOD3
  cross-billboard → vast 16k-tree forest to the horizon** (`fb518d8`), per-instance genetic
  tint green→autumn-gold + bark (`7e9558c`), wind sway (`bd0212e`, `30d0347`) (memory
  `vast-procgen-forest-optimization`).
- **Live-growth render bridge** sapling→tree (`488d399`), 2nd species conifer/broadleaf (`e1bf33d`).
- Farming substrate: soil-moisture irrigation diffusion (`e155794`), soil nutrient field (`91194af`).

### 1c. §4 configurable Living-World systems (all opt-in, gated OFF)
fire spread (wind-biased) (`3a11448`, `2903fb1`, `de56a3a`), soil nutrients (`91194af`),
pollination/genetic-drift (`b3c768f`), plant disease/blight (`e9784f6`), irrigation (`e155794`),
wildlife×foliage grazing/trampling (`f2ba748`), weather-event scheduler clear/storm/drought/snow
(`cd3d92b`), one-scalar difficulty profile (`f3b35be`). Wired LIVE into the GameSession tick, gated
(`c68f61f`).

### 1d. Emergent ecology pillar E (the research-directed AI core)
- **Utility-AI (IAUS) decision arbiter** (`0354b7c`); creature brain perception→IAUS→predator/prey
  action (`54f44ba`), wired live (`0001fe0`); boids herd flocking blended into the brain (`0c95989`);
  true-physics Jolt avatar locomotion (`a2573f7`).
- **Predator-prey loop**: catch closes the loop (`674f1d0`); **emergent pack hunting via flanking**
  (`6f47dce`, `b219690`); seasonal migration toward a moving target (`d34e26a`); herd-alarm collective
  vigilance (`a2cc411`, `7d981a9`); thirst→nearest water (`cd6207a`); scavengers eat carcasses
  (`0e76b68`).
- **Life cycle**: sexual reproduction (mate-seek + courtship) + nameplates + offspring physics
  (`92c8b02`); generational evolution (`351badd`); lifespan/old-age/starvation death (`8e4fbc9`);
  decomposition death→soil (`85c3465`); circadian diurnal/nocturnal (`ba96a67`); territory home-range
  homing (`6571155`) (memory `emergent-ecology-ai-direction`, `engine-pillars-cores-landed`).

### 1e. Photography pillar G — pure-sim DE-RISK only (NOT the game loop; engine-first honored)
Pure deterministic scorers: capture-composition (`885c084`), light-quality (`59aa1e0`), camera/lens
DoF/EV/isolation (`c94845f`), post-capture filter/grade (`fedd287`); photo-session star-verdict glue
(`996d1fe`); creature/subject CODEX + rarity (`37c5820`, `56646db`). These are pure fixtures — the
actual camera/capture GAME LOOP remains LAST (binding owner directive, memory
`iteration-4-priorities`).

### 1f. Networking pillar F — seam + codec only (single-PC constraint honored)
Deterministic network-condition sim (latency/jitter/loss) over `ILockstepTransport` (`15a62db`);
delta-vs-acked snapshot codec P3.1 (`ca9afbd`); ack-driven delta-vs-acked replication loop (`4a8566d`).
Over-the-wire / Steam SDR still blocked on single-PC (memory `single-pc-testing-constraint`).

### 1g. Timelapse / visual-progress system
Engine time-scale (host_timescale-style) (`b284659`), `--timelapse` capture mode (`0074dd5`),
frame→GIF/MP4 assembler (`7fcf638`) (memory `timelapse-and-timescale`).

### 1h. ⭐ THE HARDENING CAMPAIGN (this session's main thrust) — ~601 new deterministic tests
Owner directive: *"deterministic unit tests on as many layers as we can so that by the time we get
to e2e we have 100% in all the systems."* Driven via parallel adversarial-agent workflows
(worktree/`build`-tree isolated; each authors NEW test files only; orchestrator integrates + fixes).

- **Pack-hunt glitch FIXED** (`ee30ae6`): the steering consumer read `PackHunterComponent.coord` (a
  unit STEER DIRECTION) as a world POINT → packs steered toward the origin. Extracted the inline blend
  into testable `ai/SteeringConsumer.h` + layered tests (consumer → composed-pipeline → e2e).
- **5 batches, 11 real producer→consumer / determinism bugs fixed**, all the same class:
  | Commit | Domain | Tests | Bugs |
  |---|---|---:|---:|
  | `cd1ea7e` | ecology / movement / photo / §4 | ~181 | 7 (ODR `Clamp01`, empty-frame star, predator double-satiation, wildlife feed-from-nowhere, migration comment, seed-offset collision) |
  | `a8b689c` | emergent-AI substrate + fields | 162 | 2 (**order-dependent boids float-sum → latent run==replay break**; NaN-unsafe `utility_clamp01`) |
  | `bed7217` | replication / lockstep-replay / streaming / persistence | 140 | 2 (**delta-mode despawn dropped → stale client ghost**; replay LREC1 version gate unenforced) |
  | `8cef665` | worldgen / meshing / SHIELD layers | 118 | 0 (3 design hazards pinned, see §3) |
- **Determinism baseline reconciled** (`1600f77`): the server composite world_hash
  `f17726d44054d133 → 8a6b7bb6795da912` — root-caused to commit `0cb9ce8` (T-I7-ECO-RENDER) appending
  the `|scents:` term to `ComposeWorldHash` (NOT the ecology hardening, which is a no-op in these
  creature-less gate scenarios). Re-pinned ReplayRoundtrip / LockstepLoopback / LockstepFaultInjection.
  **NetworkedSession** proven a LEGITIMATE measurement difference (it pins the CLIENT bare streamed-chunk
  hash, no sub-hash fold → its own stable pin `5b316f81a0c72a71`, host==client across 3 runs), not a
  desync. HeadlessServerTick asserts run==replay only (no literal).
- **Final state:** common_tests **817/817**, frontier_gates_test **304/304**, world_generation_test
  **103/103**, worldgen_layer_snapshot_test **66/66** — all green in `build/debug`. Gates
  HeadlessServerTick / ReplayRoundtrip / LockstepLoopback / LockstepFaultInjection / NetworkStateHash
  pass.

---

## 2. DEFERRED ROADMAP — the options for the next dev (critique → spec → plan each)

> Revised dependency order from `.claude/plans/use-forge-to-land-vast-kernighan.md` (the forge-critique
> output). For each: `/forge-critique` the approach, `/forge-spec` with testable ACs + the standing
> feature-flag AC + a mandatory `Compute<X>SubHash` determinism AC for sim items, then plan/execute.

| Pillar | State | First slice to spec | Hard dependency |
|---|---|---|---|
| **B — tree LOD + octahedral impostors** | not started | per-instance distance→LOD bucket swap in `geometry_pass_static_meshes` + hemi-octa impostor atlas for far trees + per-frame static-instance dirty-cache. **Fold in the coarse-LOD-ignores-SDF fix (§3).** RED: tri/draw-call budget gate that FAILS at today's load; LOD-pick-per-distance; flag OFF → full-res unchanged. | — (pure render; owner's standing "vast forest" priority, memory `vast-procgen-forest-optimization`) |
| **D — erosion (hydraulic/thermal, integer-baked)** | core built+gated OFF | Mei-2007 virtual-pipe + Musgrave thermal, CPU single-thread, bake ONE finest-res offset field + apron → downsample. RED: baked field is a PURE fn of (seed,region) byte-exact; seam-continuity; LOD == area-average; `enabled:false` → terrain byte-identical. | **Must land BEFORE C** (changes the geometry C renders; re-blessing C twice is the trap) |
| **C — SHIELD-RT GPU far-field** | partial | CDLOD/geometry-clipmaps + Distant-Horizons cached LOD + Transvoxel near band + max-mip raymarch + Hillaire aerial perspective + sky-stencil horizon fix. RED: `FarLodHorizon` gate fails on today's over-blue-cap false-positive, passes on depth/stencil sky fix; AP froxel parity; `mode:off` → byte-identical. | **D first** |
| **A — foliage polish (finish)** | partial | space-colonization (Runions 2007) procgen baked per (genome bucket, growth stage); season/ToD light into the plant env sampler; 2nd+ species data table. RED: procgen PURE fn of (genome,stage); flag OFF → scatter byte-identical. | — |
| **E — emergent AI (deepen)** | foundations landed | make Utility-AI the PRIMARY arbiter / demote GOAP; fixed-point Lotka-Volterra population governor; distance-dependent vision cone; self-adaptive σ. RED: IAUS picks higher-utility action; governor stays bounded over N ticks; per-subsystem flag OFF is a no-op + hash-stable. | — |
| **F — networking (deepen)** | seam+codec landed | decouple physics tick (60-120Hz) from snapshot (~30Hz); adaptive server input buffering; grid AOI w/ cached relevancy. **Gate execution to the seam + a deterministic network-sim harness** (injected loss/latency) — DEFER prediction/reconciliation landing until a 2nd test box exists. | net-sim harness FIRST (memory `single-pc-testing-constraint`, `multiplayer-scale-testing`) |
| **G — photography (the GAME LOOP)** | de-risk fixtures only | camera/lens DoF/exposure/focus live + capture scoring + Codex + light/shadow tools, coupled to atmosphere/aether/foliage/ecology. RED: capture-scoring determinism fixture + photo-mode perf budget. | **LAST** — binding owner directive (engine-first; memory `iteration-4-priorities`) |

---

## 3. Smaller parked technical items (each a candidate slice)

- **coarse-LOD-ignores-SDF** (NEW this session; memory `coarse-lod-ignores-sdf`): `MarchingCubes.cpp
  GenerateCoarseHeightfieldTerrain` (step>1) re-derives terrain from the heightmap/analytic sampler
  instead of reducing the SAME SDF the fine mesh extracts → **caves / runtime-edits VANISH at distance.**
  A real distance-LOD fidelity gap (not a determinism break). **Belongs in pillar B/C** — coarse must
  reduce the SDF (area/octave downsample + apron, mirroring the erosion-field discipline).
- **all-solid chunk → empty mesh** (worldgen batch): standard MC interior culling, so cross-seam
  watertightness relies on the neighbour's zero-crossing. Fine at equal LOD (shared-plane SDF is
  bit-identical); a LOD mismatch across that seam can open a crack — guard when doing B/C LOD seams.
- **#5 — wire `ComputeConfigSubHash` into `WorldStreamingStateSubHashes`**: deferred under lazy-migration
  (no `sim.*` flag consumers move the hash yet). Do it when the first sim flag actually gates behavior.
- **§1b lazy config migration**: migrate existing toggles (LUMIN_GRADE/ATMOS, moonlight/wind/palette,
  scatter, LOD) into SystemConfig ONLY when already touching that subsystem, each with a before/after
  determinism + visual diff proving zero change. Never big-bang.
- **client `NetSessionCaptureHashes` sub-hash fold** (follow-up from the gate investigation): if you
  later want the NetworkedSession gate to assert the client world == the FULL canonical server world,
  wire wind/weather/aether/scent sub-hash folding into the client capture path
  (`RuntimeScenarioHarness.cpp:7849`). Not required for green today.
- **GPU-driven forest cull**, **weather→sim coupling**, **decomposition→soil-nutrient routing**,
  **circadian move-speed** — small ecology/render couplings noted but not pulled.

---

## 4. Resume pointers / gotchas

- **Namespaces:** components `Luminumbra::Components` (CAPITAL L); systems `luminumbra::ai`/`::foliage`/
  `::core` (lowercase); net `Luminumbra::Net`; worldgen/systems `Luminumbra::Systems`; persistence
  `Luminumbra::Persistence`; replay `Luminumbra::Replay`. Alias `namespace Comp = ::Luminumbra::Components;`
  inside lowercase ns. Mixing the two L's is the #1 compile bite.
- **RNG seed-offset registry** (claim the next free, record in design-decisions): wind+11, weather+12/13,
  aether+14, plant+15, creature-repro+16, fire+17, soil+18, pollination+19, disease+20, irrigation+21,
  lifespan+22, wildlife+23, photo-scoring+24, weather-events+25, herd-alarm+26, decomposition+27,
  light-tools+28, circadian+29, territory+30, pack+31, migration+32, thirst+33, scavenging+34,
  photo-filters+35. **Next free: 36.**
- **world_hash lineage** (server composite, asserted by HeadlessServerTick/Replay/Lockstep gates):
  …→ d950a6afc12a5cdc [bump #3] → f17726d44054d133 [bump #4 aether] → **8a6b7bb6795da912** [bump #5,
  `0cb9ce8` `|scents:` term]. NetworkedSession pins its OWN client bare-chunk hash `5b316f81a0c72a71`.
  Local-dev hash bumps are OK (memory `local-dev-world-hash-bumps-ok`): keep run==replay, re-pin literal.
- **Visual-gate baselines** may be stale → re-bless on intentional look changes (memory
  `render-controls-and-rebless`); the SkyboxPass loads `enhanced_skybox.frag` (NOT `skybox.frag`,
  which is dead — memory `dormant-skybox-shader`).
- **`GameSession.cpp` is compiled at `-O1`** (CMake `set_source_files_properties`) to dodge a GCC-15
  vague-linkage link error at -O0 (many entt storage instantiations in one TU). Determinism unaffected
  (`-ffp-contract=off` stays).
- **Hardening method that worked** (reuse it): extract inline glue into a testable function to expose
  producer→consumer contract bugs; cover every layer (per-system run==replay → consumer → composed
  pipeline → e2e gate); parallel agents author NEW files only, orchestrator cherry-picks/registers/
  builds/fixes; for a flagged "bug", ship the test asserting CORRECT behavior (xfail until fixed).

---

## 5. Recommended next move
**Pillar B (tree LOD + octahedral impostors)** — owner's standing "vast hyper-optimized forest"
priority, pure engine (honors engine-first), the natural home for the coarse-LOD-ignores-SDF fix, and
it precedes the D→C far-field chain. Start with `/forge-critique` on the B approach, then `/forge-spec`
the first slice (the tri/draw-call budget RED gate). Alternatively D→C if the far-field horizon is the
priority. G (photography) stays last.
