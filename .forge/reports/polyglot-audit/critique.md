# Adversarial Critique of the Consensus Roadmap

Generated: 2026-06-07

## Posture

Read the roadmap as if every claim is wrong until it earns its place. Look for: hidden coupling, sequencing dependencies the roadmap glosses over, scope creep dressed up as a "stage," tasks too vague to dispatch, tests that aren't there, hand-waved time estimates, missed prerequisites, and assumptions the roadmap inherits from the panels without questioning.

## C-level Objections

These are the items where, if I'm right, the roadmap is materially wrong and needs change.

### C1 — Stage 0 ("Lock the Contract") is under-specified and may not converge in 1–2 days

The roadmap says: flip the SDF sign, normals follow, caves fix follows, done. This compresses three load-bearing decisions into one bullet:

- **Which sign convention?** The roadmap picks inside-negative ("matches tests, matches MC default"). But MC tables aren't universal — Bourke's tables, Paul Bourke's vs. Geometric Tools', and the embedded `MarchingCubesTables.inl` may not all agree on winding order. If the existing `MarchingCubesTables.inl` was authored against an inside-positive convention (whoever wrote the engine first), flipping the sign also requires flipping every triangle winding by hand — or every face normal will invert again. The roadmap does not budget for the possibility that the tables themselves are inverted relative to standard MC.

- **`get_density_at()` is used by the GPU SDF path AND the CPU path.** S0 only edits the CPU path. The GPU SDF compute shader (`RenderPipeline::generate_chunk_sdf_gpu`, `res/shaders/` compute shaders) computes its own density. If the GPU and CPU diverge by a sign, the GPU path silently produces correct geometry while the CPU tests produce broken geometry, or vice versa. **The roadmap explicitly notes this in "Risks" but does not put the GPU/CPU parity test on the critical path before S0.** It should.

- **Cave fix may not be a one-line change.** `BUG-SHIELD-CAVES` says cave-on and cave-off produce identical vertex counts. The test sets `cave_threshold = 0.4f` and `cave_frequency = 0.04f`. If, after the sign flip, the carve term `cave_density = cave_carve_value * (cave_val - cave_threshold)` is now larger than `terrain_density` for every point above ground, `std::min(terrain_density, cave_density)` is a no-op (it stays equal to `terrain_density`). Fixing this is a parameter-tuning + tests-update task that has no upper bound on iteration count. **The roadmap should acknowledge that S0.3 may force test edits, not just code edits.**

**Required change.** Add **S0.0 — Add a GPU/CPU SDF parity test** as the first sub-step. Split S0.3 into "fix code to match test" and "if no fix exists, refactor the cave contract." Budget Stage 0 as 2–4 days, not 1–2.

### C2 — Stage 2 (concurrency) ships behind a feature flag, OR is a breaking change that needs migration plan

The roadmap rewrites `JobSystem` counters from a ring of `atomic<int>` to per-handle `shared_ptr<atomic<int>>`. This:

- changes `JobHandle`'s ABI (now holds a `shared_ptr` instead of a raw pointer);
- changes the dispatch path's allocation cost (one heap allocation per batch);
- changes the semantics of "what does a `JobHandle` keep alive."

If the renderer or any other consumer holds a `JobHandle` past the lifetime of the workers, the `shared_ptr` keeps the counter alive after worker exit. That's the intent, but the implication is: **every place that currently held a `JobHandle*` raw pointer needs to be audited for capture-by-value.** The roadmap does not list this.

Similarly, S2.4 wraps `m_chunks` in a `shared_mutex` — but render code reaches into `Chunk` itself (mesh vertices, water buffers) under a raw pointer. A shared lock on the map doesn't protect the chunk's internal vectors from a parallel write by a meshing job. The roadmap correctly defers this to REF-CHUNK-DOMAIN, but **the intermediate state (post-S2.4, pre-REF-CHUNK-DOMAIN) still has the data-race**, only the map-level race is fixed.

**Required change.** Either:
1. Acknowledge that Stage 2 is a partial fix and that the chunk-internal race persists until REF-CHUNK-DOMAIN lands, OR
2. Pull REF-CHUNK-DOMAIN's snapshot/handle work into Stage 2.

Option 1 is honest but ships a known-buggy intermediate. Option 2 makes Stage 2 a 7–10 day work item, not 3–5.

### C3 — Stage 1.5 (sources.cmake) and Stage 1.8 (.gitignore) bury risk

Stage 1.5 deletes `src/luminumbra_client/CMakeLists.txt` ("stale but still present"). This file *defines `luminumbra_client_app`*. The roadmap says the active build is in `src/CMakeLists.txt`. The architecture panel confirms this. **But if any developer's local CMake cache references the stale file's `luminumbra_client_app` target via the second-definition path, deleting it may break their build until they delete `build/`.** The roadmap doesn't say "force-clear the build dir after this change." Cheap to add, but it's the kind of thing that turns a 10-minute fix into a 2-hour debugging session for whoever pulls next.

Stage 1.8 deletes `build_error.log` and `imgui.ini` from the index. **`imgui.ini` is per-developer UI state.** If David has a customized ImGui layout that he relies on for debug viewer panels, deleting it from git deletes his state on the next pull (he'd need to re-rebuild it). The roadmap should note this and confirm the user wants the loss.

**Required change.** Add explicit "developers must `rm -rf build/` after pulling" to the S1.5 acceptance. Confirm with the owner that `imgui.ini` deletion is intentional before doing it.

### C4 — Stage 4 (UI decision) is the only owner-gated step and the roadmap pretends it's optional

The roadmap lists S4 as one of the "must-fix" stages but also says "S4.1 — Owner decision (not Claude's call)." This is internally inconsistent: the roadmap can't simultaneously require S4 and require an owner decision before S4 starts, then sequence S5 (runtime coordinator extraction) after S4. If the owner doesn't decide for 2 weeks, the whole back half stalls.

**Required change.** Either:
1. Make S4 explicitly *not* a blocker for S5/S6 — the runtime coordinator can be extracted while UI direction is undecided, as long as it consumes UI through an interface, OR
2. Acknowledge that the roadmap may stall on the UI decision and surface this as a top-line risk.

Option 1 is engineering. Option 2 is honesty. Pick one. The current text equivocates.

### C5 — Stage 6 (RenderPipeline split) is a 5–8 day estimate for a 1,184-line file with no test coverage

The roadmap says "no single file in `src/luminumbra_client/rendering/` exceeds 600 LOC" as the acceptance bar. Splitting 1,184 LOC into modules without breaking rendering, on a code path that *has no rendering tests* in the test suite, is not a 5–8 day task. The acceptance bar requires "Add a snapshot test for each pass (render a known scene, hash the framebuffer, compare)." That is **a workstream of its own** — headless rendering on Windows + Linux CI, framebuffer hash comparison tolerance for floating-point determinism, etc.

**Required change.** Either:
1. Pull the snapshot-test infrastructure work into its own stage (S5.5 — "Establish headless render snapshot harness") before S6, OR
2. Accept that S6 cannot be confidently verified without the harness and gate it as "experimental until snapshot tests land."

Estimate doubles either way. 5–8 days is fantasy.

### C6 — Stage 7 ("rolling") is not a stage

S7 lists six bullets — domain types, material registry, sound bank validator, archetype definition, LMesh V1, per-module tests. Each of these has a different risk profile, different stakeholders, different test surface. Calling them one stage hides the fact that *the roadmap does not actually sequence the rest of the work*. S7 ends and S8 (specs) is listed as "parallel," which means there is no defined endpoint.

**Required change.** Either:
1. Decompose S7 into S7a (SdfGrid + ChunkLodSelection — couples to S0), S7b (MaterialRegistry — couples to S6), S7c (SoundBankV1 — independent), S7d (LMeshHeaderV1 — couples to S1 if asset paths move), S7e (per-module tests — couples to S1.3 CI), S7f (ArchetypeDefinition — spec-first, defer), OR
2. Drop the pretense of staging and treat the post-S6 work as a backlog the team picks from as bandwidth allows.

Option 1 makes the roadmap longer but actually plans. Option 2 is honest about uncertainty.

## H-level Objections

These are issues that don't kill the roadmap but should be fixed before dispatch.

### H1 — "Lock the SHIELD contract" doesn't address GPU SDF authoring

Stage 0 fixes the CPU SDF path. The GPU compute shader (`RenderPipeline::generate_chunk_sdf_gpu`) implements the same logic in GLSL. **Whoever changes the sign in C++ must change it in GLSL.** The roadmap doesn't say this. Easy miss; add to S0 acceptance.

### H2 — Stage 2.5 removes the inline `wait()` from `dispatch_generation_jobs` without specifying what replaces it

If `dispatch_generation_jobs` no longer waits, the post-wait debug scan races. The roadmap says "the post-wait debug scan moves into a `forge`-style diagnostic command or behind `LUMINUMBRA_DEBUG_WORLDGEN`." But the *consumers* of the generation result — `update_chunk_activation` and downstream meshing — assume the chunk is populated. If `dispatch_generation_jobs` returns immediately, the meshing pass in the same frame may try to mesh an empty chunk. The chunk state machine prevents this (`ChunkState::Loading` blocks meshing), but the roadmap doesn't validate this protocol explicitly.

**Required change.** Add to S2.5: "Verify `ChunkState::Loading` → `ChunkState::Idle` transition is the only entry to meshing eligibility, and write a regression test that proves the meshing pass skips `Loading` chunks."

### H3 — Stage 3.1 ("replace `std::cout`/`std::cerr` with `LUMINUMBRA_CORE_*`") doesn't define what happens when `Log` isn't initialized

`main_server.cpp` has a single `std::cout` call. If we replace it with `LUMINUMBRA_CORE_INFO`, we need `Log::Init()` to have been called first. `main_client.cpp:74` does this; `main_server.cpp` does not. The roadmap mentions "Run `Log::Init()` from `main_server.cpp` if it exists; otherwise add the init" — but `Log::Init()` may have OpenGL or filesystem dependencies that don't make sense for a future server-only binary. Worth flagging.

### H4 — Stage 5 (runtime coordinator) assumes GLFW callbacks can capture `this` via `glfwSetWindowUserPointer`

GLFW callbacks are C-style function pointers. The "capture this" pattern through user pointer works, but the existing global pointers in `main_client.cpp` (`g_camera`, `g_playerController`, `g_uiManager`, `g_loading_visualizer`) suggest the original author tried capturing context another way. The roadmap doesn't acknowledge that this is a known migration pattern with edge cases (`window` parameter mismatch, late destruction). Add a callout.

### H5 — No mention of Forge dispatch caching

The polyglot-audit workflow ran twice today (once by Codex, once by Claude). Each panel cost meaningful tokens. The roadmap says "the polyglot-audit workflow runs on a cron." It doesn't say "cache the deterministic phases" or "skip the panels if structure hasn't changed." Without caching, every cron tick burns the full audit budget.

**Required change.** Add a hygiene line item to S1: "Add Forge cache invalidation policy for `structure` and `index`, so the audit workflow re-runs panels only when the underlying structure changes."

### H6 — "vcpkg manifest mode" is named in the roadmap with no acknowledgment that this is a Windows-first project

vcpkg works on Windows. The CI workflow includes Linux. **vcpkg on Linux requires either bootstrapping vcpkg in CI or using vcpkg-installed packages.** The bootstrapping path adds 3–5 minutes per CI build (vcpkg builds dependencies from source). For a project with 15 vendored libs, this is a real cost. The roadmap doesn't budget it.

**Required change.** Either pick a different package manager (Conan, CPM, FetchContent) or explicitly budget vcpkg bootstrap caching as a sub-task.

### H7 — Test coverage targets are absent

Multiple stages add tests ("snapshot tests for each pass," "save/load round trip," "1000-batch stress"). None of these specify *coverage targets* or *flake budgets*. Per-module GTest binaries with no coverage gate are easy to slip — adding one test per binary technically satisfies "per-module test binary." The roadmap should set, e.g., "Stage 2 acceptance requires JobSystem branch coverage ≥85%."

### H8 — `forge tasks validate` is the dispatch gate but the dispatch JSON shape is not specified

The roadmap names tasks (BUG-SHIELD-SDF, REF-RENDER-SPLIT, etc.) but doesn't specify what the Forge task graph v2 schema requires. The dispatch.json must be `forge tasks validate`-clean. If we generate it with wrong field shapes, the workflow step `validate-dispatch` fails and the whole consensus phase has to re-run.

**Required change.** Inspect Forge's `tasks validate` schema before writing dispatch.json. Cite the schema explicitly in the final-roadmap document. If the schema is documented in Forge's own repo (D:/Coding/forge-new), read it; otherwise inspect an existing validated dispatch.json for shape.

## M-level Objections

Smaller issues. Fix in passing.

- **M1.** The roadmap's "1–2 days" / "3–5 days" estimates are not calibrated against any historical data. A line item should say "estimates are unbacked rough sizes; track actuals."
- **M2.** Stage 8 (specs catch up with code) is "parallel" but spec writing requires deep knowledge of the systems being specified. If `INSTINCT-ENGINE.md` is written before S5 lands, the spec describes the old architecture.
- **M3.** The "Recommended Verification Commands" section duplicates content from the tooling panel. Reference, don't restate.
- **M4.** S6.4 ("Move static mesh instancing into a `StaticMeshRenderer`") — there is no existing static mesh instancing in the renderer that I observed. Verify before treating this as in-scope.
- **M5.** S7.5 says LMesh loader should validate `crc32`. The current `LMeshHeader` doesn't carry one. The roadmap should call out that this is a *format change*, not a *validator change* — existing `.lmesh` files in `data/` will need to be reprocessed.
- **M6.** "Decide submodules vs vcpkg. Pick one. Document the choice in `docs/`." This is a meta-decision that should happen *before* S1 (since S1 includes "reconcile `external/` vs `vendor/` duplicates"). Reorder.
- **M7.** The data-models panel proposed `WorldManifestV1` includes `engine_version`, `content_hashes`. The roadmap pulls this into S3.2 but doesn't say *how* `engine_version` is computed. Git SHA? CMake-injected version? Build-time string? Pick one. If undecided, the implementation will pick the wrong one.

## What's Right

For balance, the roadmap gets these things right:

- **Sequencing.** S0 → S1 → S2 → S3 is correct: contract first, build trust, then concurrency, then persistence. Anyone tempted to skip ahead will create more rework.
- **The "lock then extract" framing.** No big-bang rewrite. Each stage produces a working binary.
- **Explicit must-fix vs optional.** The reader can decide where to stop.
- **Owner-gated decisions called out.** S4 is flagged. The UI direction was not pretended to be Claude's call.
- **Forge integration retained, not replaced.** The roadmap uses Forge's verify/validate/dispatch primitives instead of bypassing them.

## Concrete Changes Required

For the final-roadmap revision (before dispatch generation):

1. Add S0.0 — GPU/CPU SDF parity test, prerequisite to S0.1.
2. Acknowledge GPU shader sign-flip in S0 acceptance (H1).
3. Add chunk-internal race acknowledgement to Stage 2 OR pull REF-CHUNK-DOMAIN into Stage 2 (C2).
4. Add "verify ChunkState::Loading gates meshing" to S2.5 (H2).
5. Add explicit "force-clean `build/` after pull" to S1.5 acceptance (C3).
6. Confirm `imgui.ini` deletion intent before S1.8 (C3).
7. Decouple S5 from S4: have the runtime coordinator consume UI through an interface (C4).
8. Add S5.5 — snapshot-test harness — as a prerequisite to S6 (C5).
9. Decompose S7 into S7a–S7f with explicit sequencing (C6).
10. Add Forge cache invalidation policy to S1 (H5).
11. Replace vcpkg with CPM/FetchContent OR budget vcpkg bootstrap (H6).
12. Add coverage targets to each test-shaped acceptance bar (H7).
13. Inspect Forge `tasks validate` schema before writing dispatch.json (H8).
14. Move submodules-vs-package-manager decision to a S0/S1 prerequisite (M6).
15. Specify `engine_version` source for `WorldManifestV1` (M7).

## Bottom Line

The roadmap is directionally correct and well-grounded in the panel reports. It is **not yet dispatch-ready** because:

- Three load-bearing technical questions are unresolved (GPU/CPU SDF parity, table winding, chunk-internal race in the intermediate state).
- Two stages (Stage 2 concurrency, Stage 6 renderer split) are materially under-budgeted in days.
- One stage (Stage 4 UI) creates a hard dependency on an owner decision that the roadmap doesn't account for.
- The Forge dispatch JSON schema isn't cited.

Apply the 15 concrete changes above before generating `dispatch.json`. The revised roadmap will be longer and less optimistic — that is the point.
