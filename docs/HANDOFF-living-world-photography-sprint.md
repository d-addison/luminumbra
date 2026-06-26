# Handoff — next sprint: "A Living World Worth Photographing"

Branch: `feat/polyglot-audit-roadmap`. All work below is **committed, not pushed**. Branch is green:
`--smoke` run==replay at `world_hash = 6f008a9f637c40b7`, the ecology/config/worldgen/preview test
suites pass. This handoff hands you a brainstormed sprint **plus** a devil's-advocate critique that
already corrected it — read both, then run the spec → plan → task → execute → verify lifecycle.

- Sprint plan (draft): `.forge/sprint-plan-living-world-photography.md`
- **Critique of the plan (READ THIS): `.forge/critique-sprint-plan-20260626-082729.md`** — verdict
  **GO-WITH-CHANGES**; the "REVISED SPRINT SHAPE" + "RISKS TO TASK" sections below are distilled from it.
- The system that makes all of this tunable: `.forge/fullcontrol-master-plan.md`.

---

## 1. Build / run / validate (read first)

- **Toolchain:** prepend `C:\msys64\ucrt64\bin` to PATH on EVERY build/test (KiCad/mingw PATH
  contamination → `cc1plus` exit 127 otherwise). Use the **PowerShell** tool for builds/client/gates
  (the Bash tool is sandboxed: exit 127 on the GPU client, silent compile fails).
- **Two build trees:** `cmake --build build` → `build/bin` (server smoke + tests);
  `cmake --build build/release` → `build/release/bin` (client perf/visual). Build the tree you test.
- **Determinism smoke (the gate you run after EVERY sim change):**
  `build\bin\luminumbra_server_app.exe --smoke` → must print `world_hash == world_hash_replay
  (6f008a9f637c40b7)`. The headless gate (`ServerWorldRunner::SpawnEcologyRoster`) never sets tuning
  / never spawns client ambient creatures, so client-activated features stay byte-identical here.
- **Headless visual capture:** `client_app --frame-scan out.json --no-audio` (self-implies the
  auto-world); `--ui-screenshot world_creation` drives the create-world preview; `--world-preset
  <name>` tours a preset. Convert PPM→PNG via `python tools/ppm_to_png.py`.
- **Tests:** `frontier_gates_test` (ai/), `common_tests` (sim/config), `worldgen_layer_snapshot_test`
  (worldgen byte-identity), `worldgen_preview_test` (async preview).

---

## 2. What this session delivered (committed)

- **Spec 011 (Living Creatures) — partial:** energy need + circadian-gated **Sleep** landed and
  ACTIVE on ambient creatures (they bed down at their off-phase, breathe softly). A **forager colony**
  forages live (Deneubourg double-bridge proven by `foraging_test`) — but it is **INVISIBLE** (the
  cell→transform mirror is wired at `main_client.cpp` ~the forager-mirror block; nothing renders the
  ants yet). `nocturnal` is now a per-species DATA flag (`creatures/species/*.json`).
- **Ark-style FULL CONTROL config-drive:** every creature system is data-tunable from
  `data/common/systems.json` with a byte-identical default-OFF baseline — `sim.ecology`
  (energy/sleep/hunger/stamina/herd + full flocking geometry/catch), `sim.wildlife_foliage`,
  `sim.thirst`, `sim.scavenging`, `sim.reproduction`, `sim.foraging`, `render.circadian`,
  `render.creature_spawn` + `render.foraging_colony` (counts/speeds). Pattern + determinism rules:
  `.forge/fullcontrol-master-plan.md` and memory `system-config-substrate`.
- **Audio:** the "everything maps to sound" audit is CLOSED (footsteps/per-material, per-species
  creature calls, wingbeats, sleep breathing, wind-gust swell, in-game music with a live volume bus,
  farming/terraform/discovery/objective, water/thunder). Doc: `docs/AUDIO-everything-maps-to-sound.md`.
  ENGINE FIX landed: `MiniaudioManager::PlayOneShot` (3D) use-after-free (`0xC0000005`).
- **Create-world crash FIXED:** the "lake" preset crashed `0xC0000005` — `WorldgenPreview` built its
  candidate world with a null `WaterSystem`; now linked (memory `create-world-lake-preview-null-water-crash`).
- **Critique (preview + ecology) fully executed:** knob_layer warning, forager OOB clamp, foliage in
  the preview, async background preview rebuild, preset/form toggle parity, perf (forager height cache).
- **Photo gallery** shows real captures; **tree impostors default-ON** (NOT motion-sign-off'd — see below).

---

## 3. The next sprint — REVISED per the critique

The brainstorm proposed living-world-first with photography "woven in." The critique's load-bearing
correction: **the photo loop has ZERO mechanical depth** (exposure is a hardcoded `scene_luminance =
0.6f` at `main_client.cpp:568`; aperture/focus are trivial nudges in `PlayerController`), so finishing
the sim makes a world that's lovely to *watch* and unchanged to *play*. Take these changes:

### Spine A — Camera depth (PROMOTED to a co-equal week-1 spine; the thing that must be proven fun)
- **Spike ONE concrete, determinism-safe camera mechanic** before building wide: e.g. a manual
  **exposure dial (±2 stops) + histogram**, or **focus-distance lock / focus-stacking**. Client-side
  optics are already proven hash-safe (`PhotoSession.h`). 30-min fun-validation spike first.
- **Wire scene luminance to time-of-day** (replace the constant `0.6f`) so lens/exposure changes have
  visible feedback and golden-hour is a *reward*. This is the unlock for "depth" being real.

### Spine B — Living world (KEEP, but RESEQUENCE: A → E/F → C → D → G; nests do NOT block sleep)
1. **Ant MARKER rendering** (~0.5 day true reuse of the octahedron/marker path; `BakeCreatureMarkers`
   ~`main_client.cpp:580`). Ship the colony VISIBLE.
2. **Sleep + full needs arbiter (Phase E/F)** — substrate is landed; Sleep already works circadian-
   gated, creatures sleep anywhere when tired — nests are only a *destination*, not a prerequisite.
3. **Foragers + their nests together (Phase C/D)**; then **large-creature nests (D)**.
4. **Atmosphere (Phase G)** — rest-pose render (survey species JSON: marker-swap vs particle, NO rig
   overhaul) + the night-quiet hook (audio dusk/dawn already swaps).

### Secondary — Codex depth (was "behavior objectives"; relabeled honestly)
- Species-trait/temporal-rarity unlock objectives. NOTE: behavior objectives like "photograph a
  sleeping creature" require a **capture→sim telemetry bridge** (`ObservationMetadata` through
  `PhotoSubjectView`) that **couples photo capture to sim and breaks the render-only contract**
  (`PhotoMode.h:31-34`) — 1-2 days, and it depends on Phase C+rest-poses landing first. Defer unless
  those land; otherwise score on sim-data temporal rarity instead.

### CUT / DEFER (explicitly out this sprint)
- **Pheromone ground-overlay trail** — there is NO ground-decal pass in `RenderPipeline` (passes at
  `RenderPipeline.h:805-813`); it's a 1.5-2.5 day new pass (CPU-quad vs compute→G-buffer). Split from
  ant markers; defer, or ship a simple unoccluded glyph trail with a measured ≤0.5ms GPU budget.
- **FR-C3 large-creature carry-food-home steering** → multiplayer sprint.
- **BF4 texture micro-detail** → tracked separately.

---

## 4. The three decisions to make BEFORE coding (do not defer to "implementation time")

1. **Nests: anchor-only vs server-authoritative (the hidden multiplayer landmine).** FR-D2 return-home
   + FR-E1 "sleep at nest" imply *steering*. Ambient creatures are client-only; the server roster
   never stamps Circadian/Forager/Territory. If clients steer creatures home and the server doesn't,
   **multiplayer worlds diverge** — invisible to `--smoke` (local-only). → **Take Option A
   (anchor-only):** nests are render metadata, creatures rest *near* them, no steering gate. Byte-
   identical, unblocks Sleep, matches the "client-activated" intent. Write it into FR-D2/E1/C3. Option B
   (server-auth, folded into the ecology sub-hash) is a major refactor — defer to the MP sprint.
2. **Photo-depth design doc** (1 page): the item-A mechanic (goal / mechanic / what it teaches /
   progression) + the scene-luminance→time-of-day curve + the aperture/focus reward rebalance. Without
   this, "deepen the photo loop" ships as busywork.
3. **Explicit perf-budget gate (not "opportunistic").** The forest is **fill-bound, not triangle-
   bound** — every new pixel-touching pass (24 markers, any trail) directly costs the already-over-
   budget worst-frame tail (p90 ~20ms vs the 300fps target). Make a `--render-benchmark` forest_dense
   threshold + a `--smoke` sim-tick delta (with/without a colony) BLOCKING tasks. **Task #0: tree-
   impostor live MOTION sign-off** (it's default-ON but never validated for popping) — gate the
   forager-render ship behind it.

---

## 5. How to run the lifecycle (the way we did this session)

1. **`/forge-spec`** — update **spec 011** (docs/specs/011-living-creatures-daily-life/spec.md) with the
   REVISED sequencing + the Option-A nest decision (rewrite FR-D2/E1/C3); draft a NEW **"Photography
   Depth"** spec for Spine A (the camera mechanic + scene-luminance wiring). Bake the §4 decisions in.
2. **`/forge-plan`** — break each into tasks with the §5 critique risks as explicit task lines
   (render-path decision, perf gate, impostor sign-off, ObservationMetadata schema, per-phase re-pin).
3. **Execute** — implement per phase. Client-activated/render-only = byte-identical (no re-pin). Any
   sim-roster change = `--smoke` + re-pin the gate literals (seeds +38..+42 are reserved per the spec;
   keep empty-roster neutrality). Commit per phase with the test/`--smoke` evidence in the message.
4. **Verify** — run the perf gate + `--smoke` + the relevant test suite after each phase. Send a
   screenshot/short clip at phase boundaries (the colony rendered, a creature sleeping at a nest, a
   golden-hour photo) — visual progress is a standing owner expectation.

Parallelize the disjoint render/data work via worktree-isolated subagents (as this session did for the
preview foliage + preset parity), then review + apply + verify their diffs in the cached tree. NEVER
`git worktree remove --force` carelessly (a past hazard wiped vendor/ via junctions; vendor is now
FetchContent so it's currently safe, but prefer a clean remove).

---

## 6. Determinism discipline (sacred — applies to everything)

- Procedural geometry (creatures, ants, plants, impostors) is VISUAL-ONLY; sim is integer/fixed-point.
  Render/UI/audio-only changes never touch `world_hash`.
- Client-activated ambient features stay byte-identical ONLY while the gate roster doesn't spawn/steer
  them. The moment a feature becomes server-authoritative (multiplayer), it must be deterministic +
  fold into the ecology sub-hash + re-pin. The nest decision (§4.1) is exactly this fork.
- Config: `sim.*` blocks hash when enabled (so shipped tuned values stay deterministic); render.* never
  hash; all-OFF baseline keeps `world_hash` byte-identical. Don't ship a `sim.*` block enabled in the
  canonical `systems.json`.
- Validate with `--smoke` (run==replay) BEFORE assuming determinism. Re-pin = hand-edit the gate
  literals + re-run; keep ≤2 ordered writes per sub-hash.

---

## 7. Pick-up list (TL;DR for the next dev)
1. Read `.forge/critique-sprint-plan-20260626-082729.md` (the corrected plan).
2. Make the 3 §4 decisions; write them into the specs (`/forge-spec`).
3. Spike the camera mechanic + scene-luminance (prove photography fun) — Spine A.
4. Ant MARKER render (Spine B #1) — the cheapest visible win; gate behind the impostor motion sign-off.
5. Sleep/needs arbiter → foragers+nests → atmosphere, resequenced; `--smoke` + re-pin per sim phase.
6. Stand up the explicit perf gate; don't let "opportunistic" become "ignored."
