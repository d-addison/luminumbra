# Engine Frontier Handoff

## Iteration 5a CLOSEOUT (2026-06-15) — Atmospheric pillar (LEAD)

Branch `feat/polyglot-audit-roadmap`, tip **a971154**. **All 8 atmospheric-core
features of iteration 5a are implemented, integrated, and verified.** The full
iteration-5 spec round was authored first (split 5a/5b, owner 2026-06-14; Fable
unavailable so ALL tasks ran on Opus): spec, research, critique, design-decisions
(all values pinned), `engine-iteration-5a/dispatch.json` (10 tasks), and the 5b
skeleton — committed at `793e382`.

### Features landed (sim/render line held throughout)
- **A1 GPU particle framework** (`9c66385`): instanced 65,536-pool ParticlePass
  after skybox, deterministic emitter-descriptor snapshot, motion render-only.
- **A2 wind grid** (`8d87c6c`): 24 m × 3-layer deterministic field, new
  `FieldGrid` container, seed +11. **world_hash MEGA-BUMP #1: `2fa007951a21e140`
  → `0eac465289e7c88b`** (wind sub-hash `61e22348…`).
- **B1 weather core** (`f36fa3a`): storm-cell model over biome base, advected by
  wind, `weather_system.frag` sim-driven, seed +12. **MEGA-BUMP #2: `0eac…` →
  `0857e683b4b8c47e`** (weather sub-hash `a7d8f3d2…`).
- **C1 PBR scattering sky** (`413694a`): Hillaire-2020 LUTs replace the authored
  gradient; warm sunrise/sunset palettes EMERGE (SkyboxVisual horizon r/b 1.033,
  TimeOfDaySweep dusk warm-shift +0.846). Three real scattering bugs fixed
  (sun-relative azimuth frame, tonemap crushing chroma, aerial term tinting far
  terrain blue). Analytic aerial perspective; NO froxel. Render-only.
- **C2 seasons/celestial** (`a19443d`): tick-derived (NOT wall-clock) season time
  → sun path, day length, palettes; TimeOfDaySweep season sweep (summer sun-path
  1.249 vs winter 0.980). Render-derived, no world_hash.
- **C3 cloud layer tier-1** (`fa1e08b`): wind-advected 2.5D coverage + real
  lighting-pass cast shadows (CloudShadow moving-ROI delta 0.87, GPU 0.045 ms).
  Tier-2 volumetric deferred to iter 6.
- **B2 precipitation** (`92bbd4d`): rain/snow via the A1 framework, wind-advected
  (slant gain 1.68–1.83×), splash emitters. Render-only.
- **B3 lightning** (`d992190`): deterministic strike schedule from storm state
  (seed +13) folded into the weather sub-hash; seeded branching bolt + full-scene
  light pulse via a dedicated lighting-overlay pass (after skybox so the bolt
  composites over sky+terrain); thunder reuses `AudioPropagationSystem`
  (distance/343). Strike-frame gate: luminance pulse +0.110, 2701 bolt pixels.
  **MEGA-BUMP #3: `0857…` → `d950a6afc12a5cdc`** (weather sub-hash → `e3c7e0aa…`;
  the strike schedule replaced B1's reserved 0-slot, so the byte layout before it
  is unchanged). Iteration-6 fire-ignition hook noted, not built.

### Final sweep — ALL GREEN at a971154 (world_hash d950a6afc12a5cdc)
- **ctest 224/224** (+ wind/weather/particle/lightning/season tests).
- engine-frontier: HeadlessServerTick (d950a6afc12a5cdc + sub-hashes), Heavy,
  ReplayRoundtrip, ReplayDivergence, LockstepLoopback, LockstepFaultInjection,
  SimDeterminismLint, WindFieldDeterminism, WeatherVisual (+ strike frame),
  ParticleEmitterDeterminism, CloudShadow, Precipitation, SkyboxVisual,
  TimeOfDaySweep (+ hue + season), FarLodHorizon, PlayerView, RenderHealth — all
  pass. (FarLodHorizon flaked once under the 17-gate batch load; passes clean
  in isolation — sky-sliver 0px, far-water continuity OK.)
- runtime-stability: Smoke, WaterVisual, EnduranceStreamDrain, **Endurance300**
  green — the world survives 300 ticks with all atmospheric systems active.
- **forge verify**: only the SAME 3 documented `pre_review` brace false positives
  as the i4 close (ServerHeadlessHygiene_test / asset_processor / derive_dem_stats
  — C-brace heuristic miscounts string/dict braces; all compile clean under
  -Werror) + 1 trailing-whitespace + benign spec-drift. No iteration-5 findings.

### Determinism mega-bump chain (deliberate, each re-validated in-commit)
`2fa007951a21e140` (i4) → `0eac465289e7c88b` (A2 wind) → `0857e683b4b8c47e`
(B1 weather) → `d950a6afc12a5cdc` (B3 strike schedule). Each bump re-ran the
heavy oracle + LREC1 replay + lockstep with the new canonical asserted. Wind +
weather + strikes are recompute-and-excluded from the heavy oracle's
cross-phase compare (tick-phase-dependent), proven instead via same-tick paths.

### ENVIRONMENT RECOVERY (mid-iteration, important)
The on-disk `vendor/` third-party SOURCES were destroyed mid-session: agent
worktrees created Windows junctions into the main checkout's `vendor/`, and a
`git worktree remove --force` deleted THROUGH the junctions (the cached `.a`
libs survived, masking it until a reconfigure was needed). The sources were
never tracked in git (only `googletest` is a submodule). **Recovered + upgraded
to a reproducible standard (`2f5809e`): the 11 missing libs are now pinned
FetchContent declarations** (EnTT v3.15.0, glm 1.0.1, glfw 3.4, glad v0.1.36,
nlohmann_json v3.12.0, JoltPhysics v5.3.0, RmlUi 6.1, SOIL2 1.3.0, miniaudio
0.11.22, meshoptimizer v0.22, lua v5.4.8 + hand-built target, sol2 v3.3.0,
spdlog v1.15.1, imgui v1.92.1 backends, stb pinned-commit). Hardcoded vendor/
paths (imgui backends, meshoptimizer/src, soil2, stb) restore via file(COPY) at
configure; `entt`/`rmlui` link-name shims; `CMAKE_POLICY_VERSION_MINIMUM=3.5`
for CMake 4.0. Determinism PROVEN intact: clean rebuild reproduced world_hash
(then 0eac, later 0857/d950 with the features). **HAZARD CARRIED: NEVER
`git worktree remove --force` when a worktree may hold junctions into the main
tree — it deletes through them. Fresh worktrees still lack the STILL-on-disk
vendored libs (fastnoise, imgui core, googletest); agents restore them by
copying from the main checkout + `git submodule update --init vendor/googletest`
(NOT junctions).** fastnoise/imgui were kept on-disk (NOT FetchContent) because
fastnoise drives worldgen → determinism-critical.

### Iteration-5a carry-ins (for 5b spec round + closeout follow-ups)
- **Endurance300Storm** (storm-FORCED 300-tick variant) NOT built; bounded storm
  state IS proven (weather-bench: max_storm_cells ≤ cap over 300 ticks). Add the
  storm-forced endurance run in 5b/closeout polish.
- **Quiet-machine release perf re-bless** STILL deferred — the lane is verified
  healthy but a provisional capture on this session-loaded machine was
  noise-contaminated (idle p99 1.81→3.67). Blessed T-I3-20 baseline retained.
  Re-run `.forge/scripts/run-release-perf-lane.ps1 -Bless` on a quiet machine.
- **5b** (foliage + wind response, ecology stimulus channels, atmosphere audio
  ambience, waterfalls, folded water backlog) — spec skeleton at
  `.forge/specs/ENGINE-ITERATION-5B-2026-06-14.md`; consumes the 5a wind/
  particle/weather APIs. Author its full dispatch now that 5a's hash is final.

## Iteration 4 CLOSEOUT (2026-06-14, T-I4-19)

Branch `feat/polyglot-audit-roadmap`, tip **726ab8f**. All iteration-4 dispatch
tasks complete (T-I4-0..18) plus the T-I4-DR defect-resolution wave. The
networking and performance frontier landed this session arc (12 commits since
e811adb):

- Wave C networking: **T-I4-11** determinism contract (FP pin proven
  hash-neutral AND load-bearing - Jolt leaks `-mfma` onto sim TUs;
  DeterministicMath wrappers; SimDeterminismLint; per-system sub-hashes; heavy
  save/load/resim oracle) -> **T-I4-12** LREC1 tick-indexed replay (record/replay
  + checkpoint divergence dump) -> **T-I4-13** delay-based lockstep (adaptive
  horizon, hash-exchange desync oracle, LREC1 dump on divergence) -> **T-I4-14**
  client renders a server-owned world over loopback. Camera look stays
  render-side; world_hash 2fa007951a21e140 unchanged throughout.
- Perf trio (byte/pixel-neutral): **T-I4-18** reset-per-job meshing arena
  (~2941 malloc/free pairs/prep eliminated), **T-I4-17** pooled POD jobs +
  cache-aligned completion counter (~0 alloc/job steady state), **T-I4-16**
  persistent-mapped pool + glMultiDrawElementsIndirect (live chunks:
  **1361 glDrawElements -> 2 MDI calls, ~680x fewer draw calls**; shadow
  cascades 1191 -> 2). Far-LOD path untouched.
- Mid-iteration DR fixes that also landed: sliver-baseline-diff,
  live-needle-streak (+ skirt dangling-ref UB fix; the "needle" was legit
  terrain), far-water-exposure (sheet had never rendered - backface-wound),
  tod-sky-balance (night dome was daylit), and the FastNoise2 SIMD over-read
  (**T-I4-DR-server-streaming-race**, commit 09ac3bf) that had been crashing the
  headless server ~50% of 90-tick runs - a vendor buffer over-read, not a race.

### Final sweep (all GREEN at 726ab8f)
- **ctest: 200/200** (was 148 at i3 close; +replay/lockstep/networked/jobsystem
  /determinism tests).
- engine-frontier modes: HeadlessServerTick (hash 2fa007951a21e140 + sub-hashes),
  HeadlessServerTickHeavy, ReplayRoundtrip, ReplayDivergence, LockstepLoopback,
  LockstepFaultInjection, NetworkedSession, SimDeterminismLint, EngineGameSplitLint,
  RenderHealth, PlayerView (x3), FarLodHorizon (x3), **MaterialVisual (re-homed,
  now GREEN** - the i3 deferral is closed; it re-homed during T-I4-7),
  TimeOfDaySweep, SkyboxVisual, WeatherVisual, CreatureSlice, SkinnedMeshVisual,
  PerfRegression - all pass.
- runtime-stability: Smoke, LodGround, WaterVisual, LodSeamRisk,
  LodBoundaryHysteresis, EnduranceStreamDrain, **Endurance300** - all GREEN
  (terminal endurance run at the final tip).
- **forge verify**: the only blocking-shaped findings are 3 `pre_review`
  brace-balance false positives (test/common/ServerHeadlessHygiene_test.cpp,
  tools/asset_processor.cpp, tools/derive_dem_stats.py) - all untouched by
  iteration 4, all compile clean under -Werror, and ServerHeadlessHygiene is a
  passing ctest, so the braces ARE balanced (the C-brace heuristic miscounts
  string-literal / Python-dict braces). Plus soft PATH-002/004/005/006 infos.
  Same "findings outside iteration source" pattern as the i3 close (then vendor/).

### Perf re-bless log (deliberate decision)
**Release baseline NOT re-blessed; the previous T-I3-20 baseline
(perf-baseline-release.json, captured 2026-06-11) is RETAINED.** Rationale: the
perf harness (initial_world_loading_perf_test) measures worldgen/streaming, NOT
the render-submission MDI path, so T-I4-16's 680x win cannot appear in it, and
the arena/jobpool allocation wins are swamped by FastNoise-dominated generation
time. A -Bless capture was run but came back NOISE-CONTAMINATED on this
session-loaded machine (idle_horizon p99 1.81->3.42 ms - the IDLE scenario with
unchanged code, i.e. pure system contention; boot 0.89->1.19, enter_spawn
2.61->3.06). Blessing regressed noise as the enforced baseline would be
dishonest, so it was reverted. **PerfRegression -Preset release PASSES against
the retained T-I3-20 baseline** - confirming the trio introduced no real
wall-clock regression within the gate's margins. The trio's wins are
architectural (draw-call count, allocation count), verified analytically + by
byte/pixel-stable gates. ACTION FOR NEXT QUIET-MACHINE WINDOW: re-run
`.forge/scripts/run-release-perf-lane.ps1 -Bless` to capture the real post-trio
baseline - now a clean single command. **FIXED (2026-06-14):** the lane's
PS-5.1 stderr-as-error trap is resolved - an `Invoke-Native` helper relaxes
`$ErrorActionPreference` only around the native cmake calls and throws solely on
a non-zero `$LASTEXITCODE`, so benign cmake deprecation warnings no longer abort
configure/build. Verified: configure runs to exit 0 under `Stop` through the
helper. No manual workaround needed anymore.

### Iteration 5 planning inputs (Atmospheric pillar LEAD - owner-flagged IMPORTANT)
See [[iteration-4-priorities]] / long-range-roadmap.md. Carry-ins for the iter-5
spec/research round:
- **Atmospheric pillar is the iteration-5 lead.** The tod-sky-balance fix
  (84cf431: u_skyDayFactor driving the dome from sun elevation, warm dusk band,
  dark night) is the seam to build on - volumetric lighting/god-rays, aerial
  perspective/fog by distance, cloud layers, and the LuminCrystal night-glow tie
  (CreatureSlice already asserts night glow) are the natural next beats, each
  gate-first against TimeOfDaySweep's per-phase luma/color bands.
- **Water-domain backlog (deferred from far-water-exposure):** live-ring sea
  coverage (who renders the 0-512 m sea surface beyond the water-sim radius;
  bare sand seabed currently shows), seabed terracing stripes at the waterline
  (1/32 m height quantization banding), band-assertion premise on the walkable
  archipelago, sand-flat noon brightness. Atmospheric aerial-perspective work
  will interact with the far-water look - sequence accordingly.
- **Quiet-machine perf re-bless** (above) so iteration-5 perf work has an honest
  post-trio baseline.
- **Aesthetic carry-overs** (visual sweep): near-black shadowed slopes at noon,
  razor-straight shaped ridge crests - candidate Atmospheric/material polish.
- Lock-free JobSystem queue / work-stealing was REJECTED in T-I4-17 scope;
  recorded as a future candidate only if profiling demands it.

### NOTED — worktree base hazard (carry into any agent fan-out)
The repo's `main` / `origin/HEAD` points at the stale **project-capture**
predecessor commit `972c133`, while all real work lives on
`feat/polyglot-audit-roadmap`. On 2026-06-14 Agent `isolation: worktree`
provisioned all three perf worktrees from that stale default (not session HEAD);
the base-verification guards in the dispatch prompts caught it (two agents
refused, one self-recovered via `git reset --hard`), and the tasks were
re-dispatched in the main tree with no bad code landing. Mitigation going
forward: ALWAYS inject a `git rev-parse HEAD` + scope-file existence check
before agent worktree work, and prefer the main tree until `main` is repointed.
Recorded in auto-memory `stale-main-worktree-hazard.md`. Repointing/pruning
`main` + the ~20 orphan project-capture `feat/*` branches is an owner decision
(destructive; deferred).

## Iteration 4 Status (updated 2026-06-12, mid-iteration)

Branch `feat/polyglot-audit-roadmap`, tip `0af7ee3`. Landed from the
iteration-4 dispatch (`.forge/tasks/engine-iteration-4/dispatch.json`):
T-I4-0 through T-I4-10 (Wave A world identity + texture/material fidelity)
and T-I4-15 (SHIELD-RT spike, memo at
`.forge/artifacts/engine-iteration-4/shieldrt-spike-memo.md`). An ad-hoc
defect-resolution wave also landed: T-I4-DR-{terrain-realism, shaping-perf,
window-modes, albedo-calibration, lod-swap-atomicity, churn-perf,
far-water-sheet, horizon-sliver-render, river-seam-sliver, split-lint,
sliver-baseline-diff}.

T-I4-DR-sliver-baseline-diff (0af7ee3) closed the session that stalled
2026-06-11 night: the FarLodHorizon sliver gate is now far-attributable
(paired far-OFF render per station, per-pixel 3x3 cancellation, 64px budget;
raw 256px metric is telemetry-only). FarLodHorizon and PlayerView green on
all three presets.

T-I4-DR-live-needle-streak RESOLVED (2026-06-12): the "thin diagonal needle
blade" in the mountains eye_yaw_180 captures (raw sliver 91px) is NOT a
defect. A G-buffer probe (gPosition + material id at the blade pixels) pinned
the fragments to world (-0.3, ~40, 3.7) - the LEGITIMATE grass crest of the
hillock 8 m NW of spawn, whose surface (40 m) rises above the eye (38.5 m);
seen tangentially its shadowed north face collapses to a 1-2 px line sweeping
14 deg up across the sky (atan(2/8)). Every prior hypothesis (degenerate live
mesh, heightfield spike, structure stamp, water, far-LOD tile/mesh/upload,
GPU index corruption, skybox) was instrumented and exonerated - all
generation, upload, and draw paths verified clean along the way. The raw
sliver telemetry will keep reporting such crest silhouettes; the gated
far-attributable metric correctly cancels them. Two real items fell out:
(1) FIXED - dangling-reference UB in both skirt generators
(MarchingCubes.cpp: vertex refs invalidated by push_back reallocation);
(2) routed to the visual sweep - shadowed slopes render near-black
(~luma 31 at noon) and shaped ridge crests are unnaturally straight; both are
aesthetic, not geometric. Also noted: water chunk (4,1,-2) emits its
sea-level sheet at local y=-16 (world y=0) - benign but worth a look.

Remaining dispatch tasks: T-I4-11 (determinism contract) -> T-I4-12 (LREC1
replay) -> T-I4-13 (lockstep transport) -> T-I4-14 (client over transport),
T-I4-16/17/18 (perf O1-O3), T-I4-19 (closeout). Owner directive 2026-06-12:
another visual/defect DR sweep runs BEFORE resuming dispatch tasks.

T-I4-DR-far-water-exposure PARTIAL (2026-06-12): two real defects fixed.
(1) The far-water sheet had NEVER rendered a single pixel: its quads were
wound -Y and were 100% backface-culled (water_sheet_draws ~17 with ~1M
indices submitted per frame, zero rasterized). Winding fixed; the sheet now
renders. (2) Sheet shading: any specular setting turned the flat sheet into
a sun-colored mirror at grazing eye-level views (Fresnel -> 1); it now
shades pure-diffuse (explicit matte LUT row metallic 0 / roughness 1.0,
F0 zeroed for material 200 in lighting_pass, calibrated blue albedo
0.018/0.065/0.11 - the exposure chain clips albedo >= ~0.25 to white).
TRIED AND REVERTED: drawing the camera region's sheet (to cover the
live-disc sea) - the pale sheet behind live transparent water shifts the
water.frag blend enough to break the boundary-band blue-dominance
classifier (ratio 0.0071 -> 0).
REVISED understanding of the sweep's "flat white ocean": it is mostly
(a) the walkable archipelago's vast near-sea-level DRY sand flats rendering
sun-bright (albedo-calibration / preset-shaping territory), and (b) the
BARE SAND SEABED visible inside the live ring where the live water sim
does not reach and the far sheet correctly does not draw (<176 m discard +
live-disc ownership). REMAINING DESIGN WORK (next water session):
live-ring sea coverage (who renders the 0-512 m sea surface beyond the
water-sim radius); seabed terracing stripes where the gently-sloping
seabed crosses the waterline (1/32 m height quantization banding);
band-assertion premise review (the walkable archipelago has little deep
water at the band distance - same preset-conflict class that deferred
MaterialVisual); sand-flat brightness at noon. Gates after the landed
fixes: FarLodHorizon green x3 (band ratio 0.0071 restored), PlayerView
green x3.

VISUAL SWEEP COMPLETED (2026-06-12): 58 station images reviewed across
player-view/farlod-horizon/timeofday on all three presets. Defect backlog
(file as T-I4-DR-* in this order):
1. far-water-exposure (BLOCKER): the far-LOD water sheet (material 200,
   deep-blue albedo) renders flat near-white (~234,236,236) - upward-facing
   sheet takes max noon irradiance and clips through the exposure chain
   (flat sandy ground also reads ~234); live water.frag water is correctly
   cyan, leaving a hard live/far seam. Includes shoreline z-fighting of the
   sheet against waterline-grazing island slopes (depth bias insufficient
   there). Gate-first: extend far-water assertions with absolute on-screen
   sRGB bands per the T-I4-DR-albedo-calibration pattern.
2. tod-sky-balance (quality): night sky luma barely drops (252 noon -> 171
   night) while ground goes 234 -> 6; dusk has no warm tint. Tighten the
   TimeOfDaySweep per-phase sky bands so this fails, then fix.
3. farlod-pinholes: RE-TRIAGED CLOSED (2026-06-12), no geometry defect.
   The white speckles on archipelago shoreline slopes are legitimate
   sand-material patches (a post-triplanar material dye turned them green
   with the rest of the sand; true holes would have kept the background
   color). They read glaring white from the noon sand brightness - same
   aesthetic root as the sand-flat item above (albedo-calibration domain).
No structures were visible in any station capture (could not be judged);
no chunk seams/cracks/floaters/degenerate slivers observed. Aesthetic
carry-overs confirmed: near-black shadowed slopes at noon, razor-straight
shaped ridge crests. Sweep evidence PNGs:
%TEMP%\lumi-sweep\findings\01..07-*.png (sent to owner).

Housekeeping: the four merged agent worktrees were removed and their
branches deleted. The working tree carries uncommitted test-artifact churn
(audio/persistence/gpu-sdf/render-health JSONs under
build/debug/test-artifacts) left by interrupted-session test runs; per DR
commit convention it was NOT committed with sliver-baseline-diff - review
or re-bless at the iteration-4 closeout.

## Current Status (updated 2026-06-10, post-execution)

The dispatch has been executed. Of the 34-task graph: T-EF-1 (material visual
gate) was implemented directly after the pre-dispatch quality gate rejected it
as oversized; 27 tasks merged through the Codex dispatch onto
`feat/engine-frontier` and were merged back to the working branch; T-EF-32
(network state hash) was implemented directly after the Codex usage limit
exhausted mid-run; T-EF-33 and T-EF-34 were executed directly (wave-8 report,
endurance revalidation).

The full debug CTest lane passes 68/68 (`ctest --preset debug
--output-on-failure -E "_NOT_BUILT$"`), including the new frontier gate
executables. All 20 engine-frontier validator gate modes pass, plus
MaterialVisual, LodGround, WaterVisual, and Smoke.

Post-merge defects found and fixed during verification:
- GPU SDF callback-safety source needle vs the runtime-toggle-strengthened
  guard (cross-task interaction).
- `world/WorldStreamingState.cpp` missing from the common sources manifest
  (latent link break, hidden until the persistence fixtures were linked).
- Dispatched gate-test fixtures were never compiled by any CMake target; now
  built and registered with CTest (frontier_gates_test, eventbus and
  persistence gate programs).
- Endurance300 asserts visible water, but auto_world_smoke ran in the default
  preset which generates no water near spawn (height_offset 20, sea level 0);
  water-asserting scenarios now run in the archipelago world.

## Remaining Open Work (quality-gate-skipped tasks)

Three tasks were skipped by the pre-dispatch token-estimate ceiling (200k,
not configurable at the project layer) because their contracts read the large
engine translation units; the Codex usage limit (resets 2026-06-11 ~05:53)
prevented re-dispatching them via the shimmed runner:

- `T-EF-6-streaming-telemetry-schema` — EnduranceStreamDrain validator mode +
  no-behavior-change streaming telemetry.
- `T-EF-8-lod-boundary-hysteresis-gate` — lod_boundary_oscillation_smoke
  scenario + LodBoundaryHysteresis mode.
- `T-EF-9-lod-seam-arrival-gate` — lod_seam_arrival_smoke scenario +
  LodSeamRisk mode.

Run them via `.forge/scripts/run-codex-engine-frontier.ps1`-style direct
`codex exec` with their dispatch.json prompts once Codex credits reset, or
implement directly following the material_visual_smoke pattern.

## Immediate Next Step

Close the three skipped gate tasks above, then start the next iteration per
the Next Iteration Directives below (optimization pass, beautification pass)
and the staged roadmap in ultimate-plan.md (RenderHealth-gated render
extraction, persistence-first frontier sequencing).

Contract execution meters against a $30/day budget. Large waves, long runtime
gates, and `Endurance300` may need to span days rather than being forced into
one budget window.

## No-Deferral Rules

- Do not defer sand rendering as flat grey, invalid material IDs, wrong texture layers, missing material heatmaps, or missing visual screenshots.
- Do not accept draw counts as proof of final visual appearance.
- Do not start render extraction before `RenderHealth` exists and passes with `MaterialVisual`, `LodGround`, and `WaterVisual`.
- Do not start scheduler/LOD policy changes before streaming telemetry and boundary/seam gates exist.
- Do not enable persistence runtime paths, GPU SDF runtime integration, far-field SDF, Aetheric simulation, GOAP/Instinct planning, Lua hot reload, or networking unless the deterministic gate for that path exists and passes.
- Do not mark endurance closed until it runs after the visual gates and records the artifacts it depends on.
- Do not commit from task execution; integration is handled by the dispatch pipeline.

## Next Iteration Directives (owner, 2026-06-10)

Two additional work streams are mandated for the iteration after this dispatch
closes, both gate-first like everything else in this roadmap:

### Optimization pass

Goal: measured, regression-gated performance improvement — no optimization
lands without a baseline artifact proving the win and a gate preventing decay.

- Baseline first: capture frame-time P50/P95/P99, meshing throughput
  (cells/ms by step), upload drain rate, generation/meshing job latency, and
  peak memory across `auto_world_smoke 300`, `lod_ground_smoke`, and a
  camera-traversal scenario. Persist as
  `build/<preset>/test-artifacts/perf/perf-baseline.json` with schema and
  thresholds; add a `PerfRegression` validator mode that fails on >10%
  regression against the committed baseline.
- Candidate targets, in expected-leverage order: meshing hot path
  (MarchingCubes table walk and vertex cache), streaming drain and coalescing
  windows, RenderPipeline per-pass GPU timers (add timers first, optimize
  second), JobSystem priority lanes / work stealing, chunk SDF generation
  batching, water sim tick cost at distance.
- Every optimization task pairs with the gate run that proves end-state
  visuals unchanged (MaterialVisual, LodGround, WaterVisual stay green).

### Beautification pass

Goal: spend the renderer's existing-but-unwired feature set and tune the
world's look, with every visual claim backed by a screenshot-classification
or RenderHealth artifact.

- Wire and gate the dormant shader suite: volumetric_lighting,
  enhanced_skybox, weather_system, caustics_generator, magical_particles,
  screen_space_reflections. One feature per task; each adds a capture
  scenario plus pixel/structural assertions (e.g. god-ray luminance shafts
  present at dawn time-of-day; SSR reflections present on calm water ROI).
- Water beauty pass (explicitly deferred from Wave 1): depth-tint curve,
  caustics integration, SSR, shoreline foam; extends
  water-visual-analysis.json rather than replacing it.
- Terrain material richness: per-material texture layers validated by the
  material heatmap gate (extend the materials array beyond Sand: Grass,
  Stone, Soil ROI entries with their own thresholds).
- Atmosphere: time-of-day sweep capture (noon/dusk/night) with per-phase
  luminance and color-balance bands; LuminCrystal emission visible in night
  captures (ties to the Aetheric frontier stream).
- Sequencing: beautification tasks run AFTER RenderHealth exists and the
  optimization baseline is captured, so visual richness never silently buys
  frame-time regressions.

## Success Definition

The handoff is complete when the three file/graph gates pass:

```powershell
forge tasks validate .forge/tasks/engine-frontier/dispatch.json
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Files
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Sections
```

Feature success requires Wave 1 visual gates green first, all later frontier streams gate-first, and terminal `Endurance300` revalidation after the visual gates.

## Architectural Principle: Engine/Game Decoupling (owner, 2026-06-10)

Two decoupled deliverables: an optimized, powerful, generic ENGINE and a GAME
concept built on it. Game-world concepts must not be baked into engine code.

- LuminCrystal is game content; the engine feature is light-emitting material
  support (emissive materials in the registry + lighting integration). Gates
  assert "emissive material visible in night capture" generically; the
  LuminCrystal entry in data/common/materials.json is merely the fixture.
- Aetheric Field is a game concept; the engine piece is a generic scalar
  field diffusion system.
- Instinct/GOAP archetypes (grovestrider etc.) are game data; the engine
  planner API stays data-driven.
- New game concepts go in data/common/, worlds/, scripts/ (Lua) — never in
  src/luminumbra_common or src/luminumbra_client.
- Existing engine code carrying game names (AethericFieldDiffusion's naming,
  grovestrider fixture data inside InstinctPlanner.cpp) are rename/relocation
  candidates for a cleanup task — do not entrench further.

## Iteration 3 Directives (owner, 2026-06-10)

Mandated streams for the iteration after iteration 2, all gate-first:

0. **World scale & fidelity (LEAD STREAM)**:
   (a) 6x+ view distance, hyper-optimized — reference model: Minecraft
   Distant Horizons. Persistent far-LOD store (low-res region data built
   from live chunks / pre-generated, persisted via WorldSaveService)
   rendered as merged static meshes decoupled from live simulation; near
   field stays full-sim. GPU SDF far-field as complementary horizon path.
   Gated by PerfRegression/GPU timers/stream-drain/memory watermarks at the
   new distance.
   (b) Player-view coverage defect: unloaded chunks/faces visible in play —
   all current visual gates use elevated downward cameras (blind spot).
   New eye-level 360-degree player_view_smoke gate + vertical-band and
   horizon coverage fixes (neighboring columns' cliff faces, RENDER
   DISTANCE UP/DOWN audit). Includes the deterministic degenerate-geometry
   chunk near (-120..-160, 180..230) seed 424242 archipelago.
   (c) Terrain shaping for "normal land": continentalness/erosion control
   noises, domain warp, spline height remap so plains/hills/mountains
   coexist; slope-histogram gate (% area below slope threshold) in the
   worldgen atlas; preset params remain game data.

1. **Client/server decoupling**: simulation authority runs headless —
   luminumbra_server (currently a stub) must tick a real world with zero
   GL/GLFW/audio. Watch items: GameSession save/load hooks live in
   main_client; keep new sim features out of client code. Gate: headless
   server tick smoke (boot world, tick N frames, emit world hash). Precedes
   networking transport work.
2. **World generation improvements**: presets already declare unbuilt
   features (biome temperature/humidity params unused, rivers_enabled,
   structures_enabled). Stream: biome system driving material/vegetation
   variation, rivers, structures, richer 3D terrain beyond
   heightfield+capped-caves. Gated via the worldgen atlas/snapshot machinery.
   Biome/structure CONTENT is game data; generation systems are engine.
3. **Character models / animations**: engine side = skeletal mesh +
   animation sampling/blending through the extracted pass architecture, with
   the asset_processor/.lmesh pipeline as the import seam; game side = the
   actual models/clips. Gates: deterministic pose-sampling test +
   skinned-mesh render capture. Pairs with the GOAP runtime phase (brains +
   bodies).
4. (Carried) Engine/game split per the Architectural Principle above; cash
   in built gates (LOD hysteresis implementation, GPU SDF runtime
   enablement, release perf lane); gameplay systems runtime; vertical
   slice; networking last.

## Iteration 4 Directives (owner, 2026-06-11)

Priority order is a USER DECISION, binding for the iteration-4 planning
round (spec/research/critique/ultimate-plan happens after T-I3-22 closes,
inheriting this verbatim): **engine first; the gameplay loop is 100% last.**

1. **Wave A — World identity (engine, LEAD)**: biomes via temperature/
   humidity control noises (seed offsets +8/+9, already reserved in the
   registry), rivers (+10), surface structures. Unlocks audio reverb
   (recorded trigger: biomes), feeds material variety into far-LOD tiles
   (horizon stops being monochrome), gives creatures habitats. Biome/
   structure CONTENT is game data; generation systems are engine.
2. **Wave C — Lockstep transport (engine)**: input-based lockstep over a
   real socket against the headless server; world-hash exchange as the
   desync oracle (oracle + loopback authority gates already exist). Scope:
   loopback + LAN, one remote client, desync detection. NOT matchmaking.
   Sequencing note from the iteration-3 sketch: Wave A touches worldgen
   determinism/world_hash — run A first, C after A's hashes re-bless.
   Research amendments (2026-06-11, engine-research/worldgen-lockstep-
   sdfrt.md): (a) **session replay is a COMMITTED Wave C deliverable**,
   not a candidate — near-zero cost during lockstep construction vs
   ~1 engineer-year retrofitted (Riot), and it is the primary desync-repro
   tool (Factorio); (b) Wave C is PRECEDED by a determinism-prevention
   contract task: pinned FP flags (precise, contraction off) for
   common/server targets, deterministic trig wrappers, lint bans on libm
   transcendentals + unordered-container iteration in sim code (Factorio's
   first production desync was an ambiguous std::sort comparator, not
   floats); (c) world_hash oracle gains per-system sub-hashes + a
   heavy-mode save/load/resimulate/compare variant; (d) delay-based
   lockstep with adaptive input horizon (1500 Archers) — rollback
   REJECTED with rationale recorded; camera look stays render-side.
3. **SHIELD-RT spike**: timeboxed raymarch prototype against existing SDF
   data. Evidence-only, no gates, no production wiring — keeps the
   far-field end-state road open without gold-plating the waypoint.
   Research amendments: benchmark heightfield ray-marching of FarLodStore
   tiles AGAINST generic sphere tracing (terrain-only far fields likely
   favor heightfield marching — may flip iteration 6's "tiles retire"
   assumption into "tiles become the raymarch source"); conservative
   min-filtered SDF mips are a spike SUCCESS CRITERION (naive mips make
   sphere tracing silently overshoot); storage direction: camera-centered
   clipmaps of sparse 8^3 bricks, GPU-resident (UE-Lumen/Claybook
   convergent pattern).
4. Extras where owners have slack (engine): Lua bindings/hot-reload,
   StreamingProfile meshing-skip (server stops meshing chunks it never
   renders), GPU SDF enablement behind its parity gate.
5. **DEFERRED — photography/gameplay core loop (camera/lenses/DoF/shutter,
   capture scoring, Codex)**: explicitly LAST, after the engine waves.
   Also still deferred: seasons/wind/foliage, multi-anchor streaming, F3.

Long-range plan for iterations 5-7 (Atmospheric pillar leads iteration 5 —
owner-flagged IMPORTANT; Aetheric + SHIELD-RT productionization in 6;
Project Capture photography loop in 7, last by design):
see `.forge/artifacts/engine-roadmap/long-range-roadmap.md`.

## Iteration 2 Closeout (2026-06-10)

All five phases complete on feat/polyglot-audit-roadmap. Final state: 82/82
ctest; all 22 engine-frontier validator modes pass; runtime-stability gates
(Smoke, LodGround, WaterVisual, LodSeamRisk, LodBoundaryHysteresis,
EnduranceStreamDrain, Endurance300) green; forge verify clean.

Landed: scenario harness extraction; EnduranceStreamDrain/
LodBoundaryHysteresis/LodSeamRisk gates (closing T-EF-6/8/9); PerfRegression
with blessed median-of-3 baseline (+50%/+25% noise-honest margins, 20ms
noise floor); per-pass GPU timers; WorldSaveService + chunk dirty tracking +
runtime save/load with PersistenceRuntimeRoundtrip gate; marching-cubes
flat-edge-cache optimization (byte-identical, determinism-hash locked);
six-pass RenderPipeline extraction under empty RenderHealth diffs; streaming
drain optimization (streaming_walk p99 66->11ms, chunk_churn 77->7ms, +82%
frame rate under load); JobSystem High/Normal priority lanes; beautification
(animated caustics, improved SSR, depth tint + shoreline foam, atmospheric
skybox, weather overlay behind set_weather, noon/dusk/night sweep gate,
Grass/Stone/Soil material ROIs); scenario windows no longer steal focus.

Defects found and fixed by gate-first work: vertical-column LOD mismatch
(black seam slivers + bottom band — surface-band column LOD); corrupted
skybox cube array (106/108 floats); backface-culled skybox; sign-flipped
sun/moon directions (no sun disc had ever rendered); water tangent normals
added raw (permanent 45-degree tilt); SSR far-plane grey blotches; shoreline
foam multiplied by a black fallback (never rendered); WorldStreamingState.cpp
missing from the sources manifest; dispatched gate fixtures never compiled.

Open (carried to iteration 3, see Directives above): degenerate-geometry
chunk seed 424242 near (-120..-160, 180..230); player-view coverage
(eye-level gate + vertical-band fixes); terrain shaping; 6x+ view distance
per the Distant Horizons model. NOTE: no git remote is configured — add one
to push.

## Iteration 2 Kickoff (2026-06-10)

Execution model: Claude Code agent teams execute all implementation (Agent + Workflow, worktree isolation for parallel phases); Forge provides gates, bookkeeping (.forge/tasks/engine-iteration-2/dispatch.json, 19 tasks / 7 waves), and forge verify. Codex dispatch retired this iteration. Phase order: S1 gates || S2a perf infra -> baseline capture -> GPU timers || persistence core || meshing opt -> pass extraction || runtime persistence -> streaming/jobs optimization -> beautification tracks -> closeout. render-health-baseline.json committed as the extraction diff anchor.


## Iteration 3 Closeout (2026-06-11)

Branch `feat/polyglot-audit-roadmap`, tip at closeout includes T-I3-22
commits (slice-polish, alias-removal, perf-gpu-provenance, closeout).

### Final test count and validator sweep
- **Full ctest: 148/148 passed** (build/debug, `ctest --output-on-failure`,
  ~39s). Count rose 147 -> 148 with the new
  `CurrentShippedArchipelagoPresetHeightHash` gate.
- **engine-frontier**: `-Mode All` GREEN; build-dependent modes run
  individually all GREEN — Build, UnitTests, PerfRegression, PlayerView,
  FarLodHorizon, HeadlessServerTick, SkinnedMeshVisual, EngineGameSplitLint,
  CreatureSlice, PersistenceRuntimeRoundtrip, SkyboxVisual, WeatherVisual,
  TimeOfDaySweep, ScalarFieldDiffusionGate. **MaterialVisual: RED — see
  deferral below.**
- **runtime-stability-phase-1**: Smoke, LodGround, WaterVisual, LodSeamRisk,
  LodBoundaryHysteresis, EnduranceStreamDrain, Endurance300 — all GREEN.
- **forge tasks validate** on `.forge/tasks/engine-iteration-3/dispatch.json`:
  PASS (exit 0; 4 pre-existing duplicate-create warnings only).
- **forge verify**: completes (exit 0); the only blocking-shaped findings are
  in `vendor/` third-party code (jquery.js / glm docs / rmlui scripts) and a
  pre-existing trailing-whitespace hygiene warning — none in iteration-3
  source. `git diff HEAD~3 HEAD --check` is clean for the closeout commits.

### Success-definition checklist (ultimate-plan.md, item by item)
- **PlayerView green at both presets (eye-level 360, complete terrain)**:
  PASS — default 13 stations, mountains 13 stations, archipelago 14 stations
  (incl. the seed-424242 degenerate region); max_missing=0,
  min_renderable_ratio=1.0 everywhere.
- **FarLodHorizon green at 1536 m / <64 MB / <1.5 ms**: PASS — wanted=40
  resident=40 missing=0, resident_bytes=22.4 MB, gbuffer delta 0.49-0.65 ms.
- **Slope-histogram normal-land floor on shaped mountains**: PASS — mountains
  normal_land=0.629 (>0.25), cliff=0.072 (<0.08), bimodal relief.
- **HeadlessServerTick deterministic double-run**: PASS — world_hash ==
  world_hash_replay (4bc15e0cec4ebb3a), 90 ticks x 2 runs, 30 Hz, 4511 chunks.
- **Pose-determinism + skinned-capture**: PASS — AnimationRuntime G1 checksum
  test green in ctest; SkinnedMeshVisual draws a=1/b=1, changed_pixels ratio
  0.020.
- **Creature slice artifact shows stimulus-driven behavior**: PASS — plan
  graze(shore_grass) -> approach(glow_bloom), clips idle -> walk, skinned
  draws 1/1, plans replanned 3 -> 27. T-I3-22 added a composition gate
  (sky_ratio 0.304/0.199 in [0.05,0.6]; creature-vs-terrain color_delta
  55.4/103.0 >= 24) so a functionally-green-but-visually-broken capture fails.
- **Split-lint active**: PASS — EngineGameSplitLint 163 files, 0 violations,
  no alias allowlist note (aetheric alias removed, T-I3-22).
- **All prior tests (82+) and validator modes (22+new) green**: PASS except
  MaterialVisual (deferred below).
- **PerfRegression holds (deliberate re-blesses logged)**: PASS — debug lane
  green; release baseline blessed (T-I3-20). T-I3-22 added GPU/driver
  provenance (warn-on-drift), no baseline re-blessed.

### Deliberate contract bumps log
- **T-I3-11 preset hashes** (mountains schema_rev 2 shaping golden/hash bumps)
  — landed in T-I3-11.
- **T-I3-22 archipelago preset hash** (NEW current-shipped-preset gate):
  before (legacy, shaping-off) `0xc075cf55c182393c`; after (schema_rev 2
  shaping) `0x940d621a2e3c0436`. The LEGACY fixture (`0xc075cf55c182393c`,
  default-off shaping proof) is UNCHANGED.
- **WaterVisual settle 20 -> 40 s** — landed (commit b988c7a).
- **LodBoundaryHysteresis ratchet** (T-I3-19) — landed.
- **RenderHealth skinned program re-bless** (T-I3-16) — landed; NOT touched by
  T-I3-22 (the creature-slice glow uses the existing crystal glow path, no
  shader change).
- **Release perf baseline bless** (T-I3-20, 0fb3441) — landed; T-I3-22 perf
  provenance is additive, no re-bless.

### Deferred to iteration 4
- Far water sheet.
- Save-time far-tile rebuild wiring.
- StreamingProfile meshing-skip.
- Stimulus prop draw counter.
- **MaterialVisual gate re-homing (NEW, T-I3-22)**: the owner-priority slice
  polish made the archipelago deliberately rolling/walkable (whole-grid
  cliff 0.346 -> 0.018, dry-land walkable 0.82). The MaterialVisual scan needs
  a sand beach (height 0.25-12 m) within 112 m of a 38 m+ grass-capped,
  stone-rimmed highland — geometry that requires a steep flank (the old
  spiky archipelago had it at dry-land cliff ~0.30). These are irreducibly in
  conflict on one preset at the gate's current thresholds; re-homing to
  mountains hit the rim-band/grass-cap framing (the vantage + fixed
  top-quarter rim sub-ROI were authored for the short archipelago highland).
  Deferred to iteration 4: re-home material_visual to a dedicated
  material-diversity scenario / preset and re-derive the vantage + ROI bands
  for it. Material LUT rendering stays independently gated by RenderHealth's
  terrain-material diagnostics (texture array + material LUT required), so no
  coverage is lost in the interim.
