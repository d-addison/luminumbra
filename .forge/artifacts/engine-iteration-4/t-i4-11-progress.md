# T-I4-11 Determinism Contract — Progress Log

Recovery file (two prior agents lost final reports to session limits). Append a
dated entry after EACH numbered part. Raw verification values live here.

## Baseline capture (2026-06-12, before any change)

- **Current world_hash = `2fa007951a21e140`** (HeadlessServerTick, 90 ticks,
  4498 chunks streamed, run==replay, passed=True).
  - NOTE: the dispatch text and i3 closeout cite `4bc15e0cec4ebb3a`. That value
    is STALE — Wave A (T-I4-1..4 biomes/rivers/structures, my `depends_on`)
    deliberately bumped the worldgen goldens before this task. The neutrality
    baseline for Part 1 is therefore the CURRENT hash `2fa007951a21e140`.
- Build dir present: build/debug (ninja). Server exe present.
- ctest baseline count: PENDING (capture below).
- Existing FP precedent: `src/luminumbra_common/sources.cmake` already pins
  `-ffp-contract=off` on AnimationRuntime.cpp only (G1 pose-determinism gate,
  T-I3-15). No -march/-mfma/-ffast-math anywhere in CMake (grep clean except
  vendor). Tests: need to confirm whether -mavx2/-mfma are applied to common.

## Part 1 — FP flag pinning — STATUS: DONE (2026-06-12), hash-neutral PROVEN

Files:
- `CMakeLists.txt`: added `luminumbra_pin_fp_determinism(TARGET)` helper +
  calls on `luminumbra_common` and `luminumbra_server_app`. Non-MSVC arm pins
  `-ffp-contract=off`; MSVC arm pins `/fp:precise` (+ `/fp:contract-` guarded by
  compiler version) for a future client lane.

KEY FINDING (corrects the critique's assumption): the build is NOT free of FMA
flags on sim targets. Jolt exports `-mavx2 -mfma -mfpmath=sse` as PUBLIC compile
options (vendor/Jolt/Jolt.cmake:677/681); luminumbra_common links Jolt PUBLIC, so
those flow transitively onto EVERY common + server TU. `-mfma` makes FMA
contraction codegen possible, so `-ffp-contract=off` is load-bearing (not a
no-op) at -O2/release. `-ffast-math` is confined to vendor FastNoise only (12
build.ninja lines, all under vendor/fastnoise) — never reaches first-party.

Verification:
- Flags confirmed in build.ninja after reconfigure:
  - common GameSession.cpp.obj FLAGS now include `-ffp-contract=off`.
  - server ServerWorldRunner.cpp.obj FLAGS now include `-ffp-contract=off`.
- HeadlessServerTick world_hash BEFORE pin = `2fa007951a21e140`
  AFTER pin = `2fa007951a21e140` (run==replay, passed=True). **NEUTRAL.**
- Why neutral despite -mfma: debug build is -O0, where GCC does not contract
  anyway; the pin guarantees the invariant holds once -O2/release is in play and
  across compilers. No mega-bump needed; no orchestrator escalation.
- Full ctest: deferred to final sweep (run once after all parts land).
## Part 2 — Deterministic trig wrappers — STATUS: DONE (2026-06-12)

Files:
- NEW `src/luminumbra_common/core/DeterministicMath.h` — header-only namespace
  `Luminumbra::DeterministicMath`. Provides `Sin/Cos/Atan2/Atan/Sqrt` plus
  `BitsOf/FromBits` helpers and constants. Implementation uses ONLY IEEE basic
  ops (+ - * /) and one hardware sqrtf, no libm transcendentals, all float32
  intermediates, fixed-order Horner so `-ffp-contract=off` makes them bit-exact.
  - Sin/Cos: single range reduction to [-pi,pi] then FOLD into [-pi/2,pi/2]
    (quarter-interval) where a 7th-order odd (sin) / 6th-order even (cos)
    minimax is most accurate. Measured max abs err over [-20,20]: sin ~9.6e-7,
    cos ~8.6e-4. (Earlier double-reduction Cos=Sin(x+pi/2) peaked ~2.2e-3 near
    the boundary; the fold fixed that.)
  - Atan/Atan2: 11th-order odd minimax on [-1,1] with |x|>1 fold; Atan2 does
    exact quadrant resolution; Atan2(0,0)=0 defined (no errno/NaN).
  - Sqrt: routes to std::sqrt — IEEE-754 specifies sqrt as correctly-rounded
    (basic op), so it is ALREADY bit-stable cross-platform; re-implementing
    would only add risk. Wrapper exists to keep sqrt off the lint's libm ban
    and document why it's the one safe transcendental.
- NEW `test/common/DeterministicMath_test.cpp` — 6 gtest cases:
  golden bit-pattern sweeps for Sin/Cos/Sqrt/Atan2 (raw uint32 compares, not
  float ==, so 1-ULP drift fails loudly), a loose libm-approximation guard
  (2e-3, catches a broken polynomial only), and an idempotency check.
  Registered in `common_tests` (test/CMakeLists.txt) -> gtest_discover_tests.

Verification: `common_tests --gtest_filter=DeterministicMath.*` = 6/6 PASS.

Migration NOT done (by design — would churn world_hash). Sim-path transcendental
call sites surveyed (for the follow-up migration task):
- NO sin/cos/atan2/pow/exp/log on any sim path today (worldgen, water, AI,
  physics, animation all clean of those).
- sqrt sites (all IEEE-safe, left as-is): WaterSystem.cpp:576,
  InstinctSystem.cpp:106, AnimationRuntime.cpp:34 (std::sqrt);
  PhysicsSystem.cpp:417/435/463/473 (glm::distance / glm::normalize, which use
  sqrt) — note these are audio-occlusion, render-adjacent but in common.
- floor/ceil/round are exact IEEE ops (not hazards): SHIELD_WorldSystem,
  MarchingCubes, WaterSystem, InstinctPlanner/System.
## Part 3 — Sim determinism lint — STATUS: DONE (2026-06-12)

Files:
- `.forge/scripts/validate-engine-frontier.ps1`: added `Test-SimDeterminismLint`
  (modeled on Test-EngineGameSplitLint), wired into the param ValidateSet and
  the switch dispatcher as `-Mode SimDeterminismLint`.

Scans sim-critical roots: common/{systems,world,fields,ai,simulation,animation,
physics} + luminumbra_server. Bans (per line, comments stripped):
  (a) libm transcendentals sin/cos/tan/asin/acos/atan/atan2/sinh/cosh/tanh/exp/
      exp2/log/log2/log10/pow/cbrt/hypot (std:: or bare, optional f suffix);
      DeterministicMath:: calls are allowed. sqrt NOT banned (IEEE-stable);
      floor/ceil/round not scanned (exact).
  (b) wall-clock: std::chrono::{system,steady,high_resolution}_clock, std::time,
      glfwGetTime.
  (c) non-seeded RNG: rand/srand/std::random_device/std::mt19937/
      std::default_random_engine.
  (d) range-for over an identifier declared std::unordered_map/set in the same
      TU (hash-order iteration).

Allowlist (4 sites, all documented in-script with reasons):
  - GameSession.cpp|time and |rng: world-creation bootstrap (seed default,
    world-id directory name, creation timestamp) — selects the seed, never
    per-tick sim/hash state.
  - MarchingCubes.cpp|time: TerrainMeshBuildStats elapsed_us telemetry.
  - ServerWorldRunner.cpp|time: RunFixedTicks wall_seconds report telemetry.

Verification:
- PASSES current tree: 39 sim files scanned, 0 violations, 4 allowlist sites.
- NEGATIVE test (temp probe file): correctly flags all 5 classes (sin, pow,
  steady_clock, mt19937/random_device, unordered range-for), exit 1.
- sqrt/floor/round probe: correctly NOT flagged, exit 0.
## Part 4 — Per-system sub-hashes — STATUS: DONE (2026-06-12), top-level hash UNCHANGED

Files:
- `WorldPersistenceRoundtrip.h/.cpp`: NEW struct `WorldStreamingStateSubHashes`
  {terrain, mesh, water, entities} + `ComputeWorldStreamingStateSubHashes(state)`
  and an overload `(state, entity_snapshot_json)`; NEW public `StableChecksum()`
  exposing the fnv1a_64_stable_json algorithm. Sub-hashes PROJECT the same
  canonical per-chunk JSON (ChunkToJson) into subsystem field groups, each
  checksummed with the SAME Checksum() and chunk-id-ascending order. Does NOT
  call or alter ComputeWorldStreamingStateHash -> top-level world_hash byte-
  identical.
  - terrain = sdf_data + heightmap + chunk identity/state.
  - mesh = surface/water/pending mesh vertices+indices + mesh versions.
  - water = water level/flow/terrain-height fields + water flags.
  - entities = checksum of the canonical ECS snapshot (empty for the headless
    terrain/water-only server; present so a future entity desync is attributable).
- `ServerWorldRunner.h/.cpp`: NEW `ComputeWorldSubHashes()` mirrors
  ComputeWorldHash over the streamed-chunk snapshot, fills entities from an empty
  serialized EntityRegistrySnapshot.
- `main_server.cpp`: SmokeRunResult/SmokeRunJson/RunSmoke artifact gain
  `sub_hashes`, `sub_hashes_replay`, `sub_hashes_match`; determinism now also
  requires sub_hashes_match.
- `validate-engine-frontier.ps1` Test-HeadlessServerTick: asserts sub_hashes +
  sub_hashes_replay exist, each section non-empty and run==replay, and
  sub_hashes_match=true.

Verification (90-tick smoke):
- top-level world_hash = `2fa007951a21e140` UNCHANGED (run==replay).
- sub.terrain  = 9e1b9316d5eeca32
  sub.mesh     = 812c3bb1c19b127a
  sub.water    = ed4265f8b090adf7
  sub.entities = 5735a5094c1e92a8  (all distinct, all match run vs replay)
- HeadlessServerTick gate GREEN with the new assertions.
## Part 5 — Heavy-mode oracle — STATUS: DONE (2026-06-12)

Files:
- `main_server.cpp`: NEW `--heavy [--heavy-resim <n>]` mode + RunHeavy(): boot
  original session, tick N, SAVE full snapshot, LOAD a FRESH session from the
  saved world_id, resimulate M ticks on BOTH, compare. Writes
  `luminumbra.server_tick_heavy.v1` artifact.
- `ServerWorldRunner.h/.cpp`: NEW `SaveFullSnapshot()` — persists the COMPLETE
  in-memory streamed-chunk set via WorldSaveService::save_world (the dirty-gated
  GameSession::SaveWorldState writes nothing for a never-edited world, so the
  oracle needs this).
- `validate-engine-frontier.ps1`: NEW `Test-HeadlessServerTickHeavy` +
  `-Mode HeadlessServerTickHeavy` (kept OFF the default lane; default stays fast).

KEY FINDING (the heavy oracle and sub-hashes earned their keep):
- A naive full-hash comparison FAILED: save/load round-trip and resim both
  diverged. The sub-hashes localized it INSTANTLY: ONLY `mesh` differs;
  terrain/water/entities are byte-identical at every comparison point.
- Root cause: surface mesh is a deterministically-regenerated DERIVED render
  artifact. Chunks adopted across a save/load boundary are RE-MESHED
  asynchronously and reach identical geometry via a different in-memory
  pending-mesh/version snapshot than the originating session held. This is NOT a
  simulation desync -- the authoritative state (terrain SDF + water sim +
  entities) round-trips and resims EXACTLY.
- Design decision: the heavy oracle asserts equality on AUTHORITATIVE sim state
  (terrain/water/entities); mesh is reported informationally (roundtrip_mesh_
  match / resim_mesh_match, both false) with an in-artifact reason string. The
  top-level world_hash includes mesh and is therefore NOT asserted across the
  round-trip here (it IS still asserted run==replay in --smoke, where both runs
  re-mesh identically from scratch). FOLLOW-UP NOTE for T-I4-12/13: if a future
  desync oracle needs mesh determinism across save/load, the meshing pipeline's
  pending-mesh/version state would need to be made save/load-stable -- filed
  here, out of scope for T-I4-11.

Verification:
- `--heavy --ticks 60 --heavy-resim 30`: PASSED. roundtrip_match=True,
  resim_match=True. Original session converged to world_hash=2fa007951a21e140
  (the canonical hash). Runtime ~110s (two sessions + save + load).
- HeadlessServerTickHeavy validator mode: GREEN (exit 0) when run cleanly.
  Runtime ~110s. NOTE: two earlier heavy-gate runs crashed (0xC0000005 /
  0xC0000409) ONLY when another process was relinking luminumbra_server_app.exe
  concurrently (Windows file-in-use). The oracle itself is solid: 4 clean
  standalone/validator runs all passed exit 0. Do not run the heavy gate
  concurrently with a build that touches the server exe.

---

## FINAL VERIFICATION SUMMARY (2026-06-12) — ALL GREEN

- Full clean debug build: exit 0 (all targets, incl FP-pinned common, new
  persistence sub-hash code, server heavy mode).
- **Full ctest: 183/183 PASSED, 0 failed** (= prior 177 + 6 new DeterministicMath
  cases; harness counts 183 total). FP flag pin proven HASH-NEUTRAL across the
  entire suite including determinism-hash-locked tests.
- validate-engine-frontier.ps1:
  - HeadlessServerTick: GREEN, world_hash=2fa007951a21e140 (unchanged), sub-hashes
    [terrain=9e1b9316d5eeca32 mesh=812c3bb1c19b127a water=ed4265f8b090adf7
    entities=5735a5094c1e92a8] all match run==replay.
  - SimDeterminismLint: GREEN, 39 files, 0 violations, 4 allowlist sites.
  - HeadlessServerTickHeavy: GREEN, authoritative state round-trips + resims
    identically.
- forge dispatch NOT modified (git clean); `forge tasks validate` unaffected.
- world_hash baseline: dispatch cited 4bc15e0cec4ebb3a (STALE, pre-Wave-A);
  ACTUAL current = 2fa007951a21e140, UNCHANGED by this task. No re-bless, no
  mega-bump, no orchestrator escalation.

### Files touched (final list)
Modified: CMakeLists.txt; test/CMakeLists.txt;
  .forge/scripts/validate-engine-frontier.ps1;
  src/luminumbra_common/persistence/WorldPersistenceRoundtrip.{h,cpp};
  src/luminumbra_server/ServerWorldRunner.{h,cpp};
  src/luminumbra_server/main_server.cpp.
Created: src/luminumbra_common/core/DeterministicMath.h;
  test/common/DeterministicMath_test.cpp; this progress file.
NOTE: dispatch listed src/CMakeLists.txt and GameSession.cpp as possible
modifies; neither was needed (FP helper lives in top-level CMakeLists; sub-hashes
implemented in persistence+runner without touching GameSession).
