# Engine Iteration 5a Design Decisions (binding ultimate-plan)

Binding for all 5a waves. Resolves the spec + research + critique into pinned
values, the gate list with thresholds, the file-ownership/executor map, and the
operating rules. Inherits the iteration-4 determinism contract verbatim.

**T-I5a-0 status (2026-06-14): FINALIZED.** All formerly "pin at review" values
are now concrete (marked **PINNED** below): particle pool 65,536 instances / 256
emitters; wind grid 24 m × 3 layers, ≤ 0.15 ms/tick; sky LUTs 256×64 / 32×32 /
192×108 with Earth-like atmosphere params; all render GPU-timer budgets and
bounded-state caps in §7. These are targets; each is validated against actuals by
its gate, and the render budgets are re-checked once a quiet-machine release
baseline is captured (see perf-baseline note below). No remaining placeholders —
Wave A may start.

Sources: `ENGINE-ITERATION-5A-2026-06-14.md`,
`engine-research/atmospheric-particles-wind-weather-sky.md`,
`engine-iteration-5a/critique.md` (findings F1–F10 are binding).

---

## 1. Seed-offset registry (append-only, collision-free)

Existing (do not change): `seed`=terrain, +1 caves, +2 island, +3..+7
shaping/warp, +8 temperature, +9 humidity, +10 rivers.

**Appended for 5a (FIRST, before any sim code in the owning task):**
- **+11 — wind field** (large-scale base direction low-freq noise). A2.
- **+12 — weather / storm-cell** (base pressure field + storm spawn schedule). B1.
- **+13 — lightning strike schedule** (per-storm strike RNG stream). B3.

Convention preserved: disjoint, append-only, batch-path == sample-path parity.
No reuse of +3..+10. Season/celestial (C2) is tick-derived and takes NO seed
offset (render-derived; see §6).

## 2. The sim/render line + `world_hash` sub-hash registry (F1, F2)

**In `world_hash` (sim-authoritative, deterministic, replicated):**
- `wind` — the wind field cell values (new sub-hash slot). A2.
- `weather` — region weather category + storm-cell set (pos/intensity/velocity) +
  precipitation intensity field + lightning strike schedule (new sub-hash slot).
  B1 + B3.

Post-5a registry:
`world_hash = fnv(terrain, entities, water, fields, wind, weather, rng)`
(`fields` = the existing scalar-diffusion slot; `wind` kept as its own slot for
debuggability per research §7; `weather` folds storm + strike schedule).

**OUT of `world_hash` (render-only, client-local, never hashed):**
- All particle motion / arrays (only the emitter *descriptor set* is snapshotted
  for its gate — see §3).
- Sky scattering LUTs + output, aerial-perspective term.
- Cloud coverage field + cast-shadow texture + imposters.
- Lightning bolt geometry, the light-pulse, scorch emitters, thunder playback.
- `weather_system.frag` overlay streaks, wetness material modulation.

**One-way rule (F2):** no render subsystem may write into any sim/`world_hash`
input. Data flows sim → render only. This is a review-enforced rule on A1, B2,
B3, C1, C3.

**Mega-bump protocol (F1):** the FIRST commit that changes `world_hash` (A2's
`wind` slot, then B1's `weather` slot) must, IN THE SAME COMMIT: add the sub-hash;
re-run + re-bless the heavy-mode save/load/resim oracle, ReplayRoundtrip,
ReplayDivergence, LockstepLoopback, LockstepFaultInjection; log old→new
`world_hash` in the commit message + handoff. Each sim system proves
DeterministicMath-clean + SimDeterminismLint-pass BEFORE folding into the hash.
Unexpected non-neutrality or replay break → STOP and report (orchestrator
decision). Old hash for the record: `2fa007951a21e140`.

## 3. GPU particle framework (A1)

- **Mechanism:** instanced draw of a fixed-capacity, persistent-mapped instance
  buffer (reuse the T-I4-16 persistent-mapped pool pattern in `Mesh.cpp` /
  `RenderPipeline`). NOT compute / transform-feedback in v1 (research §2, F-stack).
- **Pool capacity (PINNED):** global pool **65,536** particle instances (fixed,
  ring-recycled — oldest evicted on overflow); **256** concurrent emitters max.
  Both asserted bounded by Endurance300Storm (F9). Instance stride: pos(3·f32) +
  size(f32) + color(rgba8) + atlas-layer(u16) + rotation(f16) = 24 B → 1.5 MB
  pool VRAM.
- **Pass slot:** new `ParticlePass` AFTER `SkyboxPass`, reads G-buffer depth for
  soft-particle fade + collision, blends into the lit HDR target before final
  blit.
- **Lighting:** forward — sun/ambient from the §5 sky + N nearest point lights.
- **`magical_particles.{vert,frag,geom}` re-homes** onto this framework.
- **Emitter format (game data, `data/common/particles/`):** per-emitter curves
  (rate, lifetime, velocity, size, color over normalized life), `.ltex` array
  texture refs (T-I4-6 atlas), blend mode. Engine knows only the schema.
- **Determinism surface (F2):** the ParticleEmitterDeterminism gate snapshots the
  *emitter descriptor set* ONLY — `{id, type, origin-region, spawn-rate, RNG
  seed, enable}` — at a fixed tick; byte-equal across two runs. Particle arrays
  never snapshotted, never hashed. Emitter RNG seed derived deterministically from
  world state so descriptors reproduce without motion being deterministic.
- **Downstream readers (F10):** B2 precipitation, B3 scorch; 5b waterfall spray,
  5b foliage (none) — the emitter-creation API is public + stable at 5a close.

## 4. Wind grid (A2)

- **Shape (PINNED):** coarse 2.5D vector field — **24 m cells**, **3 layers**
  (ground 0–32 m / mid 32–128 m / high 128–512 m AGL), over the streamed extent,
  on the 30 Hz tick. Each cell holds a 2D horizontal wind vector per layer
  (vertical wind ignored in 5a). Grid follows the streamed region; out-of-region
  samples clamp to the base large-scale direction.
- **Container:** a generalized `Field` storage (cell grid + stride) factored so a
  scalar Aetheric field reuses the storage/iteration/snapshot/sub-hash plumbing in
  iteration 6 (F5: minimum shared surface only — no finished generic template,
  no second consumer in 5a). Refactor/extend `fields/ScalarFieldDiffusion`
  neighborhood as needed without changing its behavior/hash.
- **Update:** base large-scale direction from `seed+11` low-freq noise sampled by
  tick-time (FastNoise batch path) + storm perturbations injected by B1; cheap
  per-cell blend, DeterministicMath only.
- **Budget (PINNED):** per-tick wind update **≤ 0.15 ms** at streamed extent
  (the tick budget is the gated number; WindFieldDeterminism gate measures it).
- **Determinism:** `wind` sub-hash (§2); WindFieldDeterminism gate; offset +11
  FIRST.
- **Downstream readers (F10):** B1 advection, B2 rain slant; 5b foliage
  displacement — the wind-sampling API (`SampleWind(worldPos)`) is public + stable.

## 5. Sky / scattering (C1) + seasons (C2) + clouds (C3)

**Sky (C1) — PINNED LUTs:** Hillaire 2020 — transmittance **256×64** (RGB16F),
multi-scatter **32×32** (RGB16F), sky-view **192×108** (RGB16F, recomputed when
the sun moves past a small threshold). Analytic aerial-perspective term in the
lighting pass (extend `lighting_pass.frag` / wire `volumetric_lighting.frag`);
NO froxel volume (deferred). Replace the authored gradient in
`enhanced_skybox.frag`; keep `u_skyDayFactor` as the night-darkening envelope +
star/aurora layers. Sun/sky/ambient/fog all read the same transmittance.
**Atmosphere params (PINNED, Earth-like):** Rayleigh scattering coeff at sea
level β_R = (5.802, 13.558, 33.1)·10⁻⁶ m⁻¹, scale height 8000 m; Mie β_M =
3.996·10⁻⁶ m⁻¹, scale height 1200 m, phase g = 0.76; ground albedo 0.1; planet
radius 6360 km, atmosphere top 6460 km. Tune only ground albedo / an exposure
scalar if needed so noon luminance lands inside the existing TimeOfDaySweep
noon band where achievable; any band move = single deliberate logged re-bless
(F6). RenderHealth resource-registry entries for the LUTs; startup precompute
cost recorded in render telemetry.

**Seasons (C2):** season phase = pure function of **tick count** (integer epoch
math; DeterministicMath for sun-path trig). **Render-derived** — adds nothing to
`world_hash` (F7). Drives sun path, day length, palette selection. No wall-clock,
no free-running float accumulator. TimeOfDaySweep extends to a season sweep.

**Clouds (C3):** 2.5D wind-advected coverage field (f(noise, wind offset,
weather, biome)) scrolled by the A2 large-scale direction; landscape-distance
imposters. Cast shadow = projected coverage sampled in the lighting pass,
`directSun *= (1 - cloudShadow(worldPos))`. Prefer a low-res coverage/shadow
texture + cheap sample (F3). Render-only, pure function of replicated state, one-
way (F2). TIER 2 volumetric raymarch = iteration 6.

## 6. Weather (B1) + precipitation (B2) + lightning (B3)

**Weather core (B1):** base pressure `seed+12` × biome temp/humidity → region
category (clear/overcast/rain/snow/fog); discrete storm cells on a seeded
schedule, advected by A2, carrying precip intensity + (storms) a strike schedule.
`weather_system.frag` uniforms now fed from replicated state (not `set_weather`).
State (category + storm cells + precip field + strike schedule) in `world_hash`
`weather` slot; overlay + wetness render-only. Offset +12 FIRST. State bounded
(F9): storm-cell count cap, precip-field flat memory.

**Precipitation (B2):** rain/snow via A1 emitters, wind-advected (slant from A2);
splash/spray on depth-buffer impact. Descriptors deterministic; motion render.

**Lightning (B3):** strike events `(tick, pos, magnitude)` scheduled by storm
state from `seed+13` (sim, in `weather` hash). Render: seeded branching bolt
(midpoint-displacement, seeded from the event) + 1-to-few-frame full-scene light
pulse through the lighting pass; scorch emitters. **Thunder via existing
`AudioPropagationSystem`** — delayed positional one-shot at distance/343 s;
B3 reads `AudioPropagationSystem.h` FIRST to confirm the capability; thin additive
hook only if needed; null-audio gates stay green; the visual gate does NOT depend
on audio (F8). Offset +13 FIRST. Iteration-6 fire-ignition hook NOTED, not built.

## 7. Gate list + thresholds (all wired into validate-engine-frontier.ps1)

**Render GPU-timer budgets (PINNED, at the baseline PlayerView at 2560×1440;
release build; measured via the per-pass GPU timers, NOT the worldgen perf
lane — F3).** Each is a hard PerfRegression ceiling; a task that exceeds it
either optimizes or falls back (e.g. coarser cloud-shadow) rather than ships a
regression:

| Render addition | Budget (ms) |
|---|---|
| ParticlePass (steady visual load) | ≤ 0.8 |
| ParticlePass (active storm + precipitation) | ≤ 1.2 |
| Aerial-perspective term (folded into lighting pass) | ≤ 0.3 |
| Cloud-shadow sample (added to lighting pass) | ≤ 0.4 |
| Sky-view LUT refresh (per qualifying frame) | ≤ 0.2 |
| Sky LUT full precompute (startup one-shot, not per-frame) | ≤ 8.0 |
| Lightning light-pulse frame (transient) | ≤ 0.5 |

**Sim per-tick budgets (PINNED):** wind update ≤ 0.15 ms; weather update
≤ 0.20 ms (both at streamed extent, gated by their determinism modes).
**Bounded-state caps (PINNED, F9):** ≤ **16** active storm cells; emitter pool
fixed at 256 emitters / 65,536 instances; precip-intensity field flat memory.

| Gate | Kind | Assertion |
|---|---|---|
| **ParticleEmitterDeterminism** | determinism + visual + perf | emitter-descriptor snapshot byte-equal across 2 runs at fixed tick; visual capture shows particles; ParticlePass GPU-timer ≤ 0.8 ms |
| **WindFieldDeterminism** | determinism | seed → N-tick wind-field hash equal across 2 runs; `wind` sub-hash present + stable |
| **WeatherVisual (extended)** | determinism + visual | `weather` state-hash stable across resim/replay; baseline-vs-weather sky-luma drop + streak gradient (real state); strike frame: frame-mean luminance spike vs neighbors + bolt high-gradient pixels |
| **TimeOfDaySweep (extended)** | visual | existing noon>dusk>night ordering (clear sky) + NEW dawn/dusk hue-band (sky r/b ratio bands from scattering) + season-sweep per-season sun-path/palette bands |
| **SkyboxVisual (extended)** | visual | existing monotonic horizon brighten + sun-disc localize (clear sky) + NEW low-sun-angle scattering palette emergence (warm horizon band) |
| **CloudShadow** | visual | partly-cloudy fixture: moving cast-shadow signature — luminance delta in a fixed terrain ROI between two times as a shadow edge crosses |
| **Endurance300Storm** | endurance | 300 ticks active storm: no frame-time cliff AND bounded state (storm cells ≤ 16, emitter pool fixed 256/65,536, precip-field memory flat); record peak mem + cell count |
| **PerfRegression (render budgets)** | perf | the PINNED GPU-timer ceilings above (not the worldgen lane, F3), each measured at the baseline PlayerView |

Premise-conflict guard (F4): storms run ONLY in dedicated weather scenarios with
clear-sky control phases. SkyboxVisual + TimeOfDaySweep keep clear-sky atmosphere.
CloudShadow uses partly-cloudy (not overcast). Closeout verifies PlayerView +
FarLodHorizon stay green on a weather-enabled-but-clear world.

**Perf-baseline status (T-I5a-0, 2026-06-14).** The release perf lane was
re-verified HEALTHY post-iteration-4 (builds the release preset clean, runs the
9 scenarios, writes a baseline — the closeout stderr-trap fix holds). A
provisional capture on this session-loaded machine came back NOISE-CONTAMINATED
(idle_horizon p99 1.81→3.67 ms — the IDLE scenario with unchanged code, i.e. pure
contention; pan_camera 1.31→2.31; while enter_spawn 2.61→2.56 fell, proving
noise not regression), so it was NOT blessed and the enforced blessed baseline
(retained T-I3-20, 2026-06-11) is unchanged. The real `-Bless` is DEFERRED to a
genuinely quiet window (single command: `.forge/scripts/run-release-perf-lane.ps1
-Bless`). This does NOT block Wave A: the 5a render budgets above are NEW per-pass
GPU timers measured independently of this worldgen/streaming baseline, and the
sim tick budgets are gated by their own determinism modes + Endurance300Storm.
NOTE for any agent: drive the lane from a normal terminal / the PowerShell tool,
NOT the Bash tool — the Bash tool's environment lacks the msys2 ucrt64 DLL PATH,
so `c++.exe` spawns but its children die with no diagnostic (`FAILED: [code=1]`).

## 8. File-ownership + executor map

| Task | Executor | Primary modifies (illustrative; agent contract is authoritative) | Creates |
|---|---|---|---|
| T-I5a-0 design | opus-agent (inline) | — | design-decisions.md (this file) |
| T-I5a-1 particles | opus-agent | `RenderPipeline.{h,cpp}`, `passes/` (new ParticlePass), `Mesh.cpp`, `res/shaders/magical_particles.*`, `RuntimeScenarioHarness.{h,cpp}`, validator script | `passes/ParticlePass.{h,cpp}`, `data/common/particles/`, particle shaders |
| T-I5a-2 wind | opus-agent | `fields/`, `systems/`, `world/GameSession.cpp`, `server/ServerWorldRunner.cpp`, validator script | `systems/WindFieldSystem.{h,cpp}` (or `fields/`), wind snapshot test |
| T-I5a-3 weather core | opus-agent | `systems/`, `world/GameSession.cpp`, `server/ServerWorldRunner.cpp`, `res/shaders/weather_system.frag`, `RuntimeScenarioHarness.{h,cpp}`, validator script | `systems/WeatherSystem.{h,cpp}`, weather test |
| T-I5a-4 precip | opus-agent | `RenderPipeline.cpp`, `passes/ParticlePass.cpp`, `systems/WeatherSystem.cpp`, validator script | precip emitter data |
| T-I5a-5 lightning | opus-agent | `systems/WeatherSystem.{h,cpp}`, `passes/LightingPass.{h,cpp}`, `res/shaders/lighting_pass.frag`, `audio/AudioPropagationSystem.*`, `RuntimeScenarioHarness.{h,cpp}`, validator script | bolt geometry util |
| T-I5a-6 sky | opus-agent | `res/shaders/enhanced_skybox.frag`, `res/shaders/lighting_pass.frag`, `res/shaders/volumetric_lighting.frag`, `passes/SkyboxPass.{h,cpp}`, `passes/LightingPass.cpp`, `RenderPipeline.cpp`, `RuntimeScenarioHarness.{h,cpp}`, render-health-baseline.json, validator script | sky LUT precompute |
| T-I5a-7 seasons | opus-agent | `RenderPipeline.cpp` (update_time_of_day), `RuntimeScenarioHarness.{h,cpp}`, validator script | — |
| T-I5a-8 clouds | opus-agent | `res/shaders/enhanced_skybox.frag`, `res/shaders/lighting_pass.frag`, `passes/SkyboxPass.cpp`, `passes/LightingPass.cpp`, `RuntimeScenarioHarness.{h,cpp}`, validator script | cloud coverage util |
| T-I5a-9 closeout | opus-agent (inline) | `.forge/artifacts/engine-frontier/handoff.md`, `perf-baseline-release.json` | 5b planning inputs |

## 9. Dependency DAG

```
0 design
├─ A1 particles ─┐
├─ A2 wind ──────┤
│                ├─ B1 weather ─ B2 precip
│                │             └ B3 lightning
└─ C1 sky ─ C2 seasons
        └─ C3 clouds (needs A2 + C1)
A1,A2 → B1; A1,A2,B1 → B2; B1,A1 → B3; A2,C1 → C3
all leaves → D closeout
```

A1 ∥ A2 ∥ C1 can start after Wave 0. B-wave after A1+A2. C2 after C1. C3 after
A2+C1. The `world_hash` mega-bump is sequenced: A2 lands the `wind` slot
(+re-bless), then B1 lands the `weather` slot (+re-bless) — adjacent, deliberate.

## 10. Operating rules (repeat of spec §Operating rules — binding)

1. Sim/render line per §2; one-way render→sim ban.
2. Determinism: DeterministicMath + SimDeterminismLint; offsets +11/+12/+13
   FIRST; mega-bump protocol §2/F1.
3. Perf: GPU-timer budgets §7 (not the worldgen lane); deliberate logged
   RenderHealth re-bless; quiet-machine baseline captured in Wave 0 FIRST.
4. Scope: out-of-5a list per spec §4 (no tier-2 clouds, fire-sim, froxel,
   compute particles, persisted season terrain, 3D fluids, any 5b system).
5. Worktree base hazard: `git rev-parse HEAD` + scope-file check before any
   agent worktree work; prefer main tree (stale `main`/`972c133`).
