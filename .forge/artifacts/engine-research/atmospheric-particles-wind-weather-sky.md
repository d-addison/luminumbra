# Engine Iteration 5a Research — Atmospheric Core (particles, wind, weather, sky, clouds, lightning)

Scope: the technical-choice resolutions feeding the iteration-5a design-decisions
doc. Inherits the determinism contract landed in iteration 4
(`src/luminumbra_common/core/DeterministicMath.h`, `SimDeterminismLint`, pinned
`-ffp-contract=off`/no-fast-math on `luminumbra_common` + `luminumbra_server`,
per-system `world_hash` sub-hashes, LREC1 replay, delay-based lockstep). Every
sim-side addition here lives under that contract. Render-side state does NOT.

The governing axis for this whole iteration: **what is in `world_hash`
(sim-authoritative, deterministic, replicated) vs what is render-only (visual,
client-local, never hashed).** Get this line wrong and lockstep desyncs or the
heavy-mode resim oracle fails. It is restated per system below.

---

## 1. Sky model — physically-based atmospheric scattering

**Choice: Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere")
precomputed LUT approach, NOT full Bruneton 2008.**

Rationale:
- Bruneton's 2008 precomputed transmittance + multiple-scattering (4D) tables are
  the gold standard but heavy: a 4D scattering LUT (~32×128×32×8) and a
  non-trivial precompute. Overkill for a terrain camera that never leaves the
  lower atmosphere.
- Hillaire 2020 reduces to three modest 2D/LUT passes computed once at startup
  (and only re-derived when atmosphere params change, which for us is never at
  runtime unless a preset declares it):
  1. **Transmittance LUT** (~256×64, RG/RGB16F) — view-zenith × altitude.
  2. **Multiple-scattering LUT** (~32×32) — isotropic multi-scatter approximation.
  3. **Sky-view LUT** (~200×100, lat-long of the sky hemisphere) recomputed
     per-frame-ish when the sun moves; cheap.
- Aerial perspective is a small camera-frustum 3D LUT (~32×32×16) OR a cheap
  analytic fog term derived from the transmittance LUT along the view ray. For
  iteration 5a we take the **analytic per-pixel term in the lighting pass**
  (extend `lighting_pass.frag` / wire the dormant `volumetric_lighting.frag`)
  rather than a froxel volume — froxel volumetrics are a tier-2 / iteration-6
  upgrade (shares SHIELD-RT froxel infra).

Integration with the existing chain:
- Replaces the authored gradient in `enhanced_skybox.frag`. The current
  `u_skyDayFactor` seam (smoothstep of sun elevation, from the tod-sky-balance
  fix) stays as the **night-darkening multiplier**; the scattering LUTs supply
  the *color*, `u_skyDayFactor` supplies the *brightness envelope* down to a
  near-black night dome with the star/aurora layer unchanged.
- Sun/sky/ambient/fog become **coherent** because all four read the same
  transmittance: sun disc color = transmittance toward the sun; ambient =
  integral of the sky-view LUT; fog/aerial = transmittance along the view ray.
  This is the headline "sunrise palettes emerge from Rayleigh/Mie" deliverable —
  pinks/purples/oranges fall out of the wavelength-dependent Rayleigh term at low
  sun elevation, not from an authored ramp.
- Exposure: the existing tonemap/exposure stage stays; the scattering output is
  HDR pre-tonemap. Calibrate atmosphere params (Rayleigh scale height ~8 km, Mie
  ~1.2 km, Mie g ~0.8, ground albedo) so noon luminance lands inside the current
  TimeOfDaySweep noon band — avoid a gratuitous exposure re-bless if achievable;
  if bands must move, it is a deliberate logged re-bless.

Determinism: **render-only.** Sun direction is already derived from the
sim-authoritative time-of-day; the scattering math runs client-side and never
enters `world_hash`. No DeterministicMath needed here.

Open knob for the design doc: LUT resolutions above are the recommended pins;
the precompute must be a one-time cost gated by RenderHealth resource-registry
entries + a startup-cost note in render telemetry.

---

## 2. GPU particle framework

**Choice: instanced draw of a fixed-capacity pool with CPU-side emitter
simulation on a worker, NOT a compute-shader transform-feedback pipeline — for
v1.** Compute pools are the stretch; instancing is the floor and is portable.

Rationale:
- Iteration-4 research already rejected bindless on this GL target (AMD/Intel
  fragility). The same conservatism applies to compute-driven particle systems:
  GL 4.3 compute is available but SSBO + atomic compaction across the target
  matrix is a portability risk for a v1. Instanced rendering of a persistent-
  mapped instance buffer (reusing the T-I4-16 persistent-mapped pool pattern in
  `Mesh.cpp` / `RenderPipeline`) is the safe, already-proven mechanism.
- The dormant `magical_particles.{vert,frag,geom}` shaders **re-home onto this
  framework** — the geometry-shader billboard expansion is replaced/kept as the
  instanced-quad expansion; the fragment shading (emissive sprite) is reused for
  the crystal-glow consumer.
- Soft particles: sample the G-buffer depth (already produced by `GBufferPass`)
  and fade alpha by depth delta — standard soft-particle term. Depth-buffer
  "collision" for splash/leaves is the same depth read with a spawn/clip test.
- Lit-by-existing-path: cheap forward light accumulation for particles (a few
  nearest point lights + the sun/ambient from §1) rather than deferring particles
  — transparent, so they composite after the lighting/water passes.

**Pass slotting:** a new `ParticlePass` between `LightingPass`/`WaterPass` and
`SkyboxPass` is wrong for transparency ordering against the sky; the correct slot
is **after `SkyboxPass`** (sky is the backmost opaque), reading the depth buffer
for soft fade, blending into the lit HDR target before the final blit. (Lightning
bolt flash is a *lighting-pass* light-pulse, separate — see §6.)

**Determinism line (critical):**
- The **emitter schedule** is deterministic and sim-owned: which emitters exist,
  their spawn cadence, and the *seed* for each emitter's RNG are a function of
  world state (e.g. a rain emitter exists because the weather state at this
  tick/region says "raining"). This scheduling is what the
  ParticleEmitterDeterminism gate pins — same tick + same world state → same
  emitter spawn descriptor snapshot.
- The **particle motion** (per-particle position/velocity/age integration, the
  visual jitter) is **render-only** and MUST NOT enter `world_hash`. It runs at
  render rate, can use ordinary `std::` math and `Math::random`, and differs
  harmlessly between machines. Storing it in the sim would be a determinism
  liability for zero gameplay benefit (particles are cosmetic).
- Practical rule for the gate: snapshot the *emitter descriptors* (id, type,
  origin region, spawn-rate, RNG seed, enable flag) at a fixed tick on the sim
  side; assert byte-equality across runs. Do not snapshot particle arrays.

Emitter format (game data): per-emitter curves (rate/lifetime/velocity/size/color
over normalized life), texture refs into the `.ltex` array atlas (the T-I4-6
texture-array path), and a blend mode. Lives in `data/common/` (e.g.
`data/common/particles/`). Engine knows only the curve/atlas schema.

---

## 3. Wind grid

**Choice: a coarse 3D (or 2.5D layered) vector field on the 30 Hz tick, stored in
a generalized `Field<T>` container that the Aetheric scalar-field stack reuses in
iteration 6.** Build it as a small grid of cells (region-scale, e.g. 16–32 m
cells over the streamed area), advanced by a deterministic update: a base
large-scale wind direction (slow-varying, derived from a `seed+11` low-frequency
noise sampled by tick-time) + storm-cell perturbations injected by the weather
system (§4). 2.5D (a few stacked layers) is sufficient for surface gameplay
visuals; full 3D is unnecessary mass.

Reuse mandate: today the field machinery is `fields/ScalarFieldDiffusion`
(scalar). The wind grid is the **first production vector-field consumer** — factor
the storage/iteration/budget so a scalar Aetheric field and a vector wind field
share the container, the tick-budget slot, and the snapshot/hash plumbing. This
is an explicit iteration-6 enabler; the design doc pins the shared interface.

**Determinism: sim-side, in `world_hash`.** The wind field is read by weather
advection, precipitation slant, and (5b) foliage displacement — anything that
feeds visuals consistently across clients must agree, and storm advection feeds
the weather *state* which is authoritative. Therefore:
- Update uses `DeterministicMath` wrappers only (no libm `sin`/`exp` in the tick
  path); noise sampling via the existing FastNoise batch path with `seed+11`.
- Snapshot-gated: WindFieldDeterminism gate = run N ticks twice from a seed,
  hash the field, assert equal; add a `wind` sub-hash to the `world_hash`
  artifact.
- Seed-registry offset `+11` appended FIRST (before any wind code), continuing
  the append-only convention (terrain, +1 caves, +2 island, +3..+7 shaping/warp,
  +8 temp, +9 humidity, +10 rivers).

Cost: the grid is coarse and the update is a cheap per-cell blend — pin a
per-tick budget in the release lane (target negligible vs FastNoise generation,
e.g. << 0.2 ms/tick at streamed extent).

---

## 4. Weather system

**Choice: a server-authoritative storm-cell model layered over a biome-aware
base climate.** Base "weather pressure" per region from a slow `seed+12` field
modulated by the biome table's temperature/humidity (iter-4 climate channels) →
selects clear/overcast/rain/snow/fog probability; discrete **storm cells** are
spawned on a seeded schedule, advected by the wind grid (§3), and carry
local rain/snow/fog intensity + (for storms) a lightning strike schedule (§6).
The dormant `weather_system.frag` graduates from the iteration-2 beautification
overlay to a *sim-driven* overlay: its `u_rainIntensity`/`u_snowIntensity`/
`u_fogDensity`/`u_stormIntensity`/`u_windDirection`/`u_windStrength` uniforms are
now fed from the replicated weather state, not a debug `set_weather`.

**Determinism line (critical): weather STATE is in `world_hash`; the visual
overlay is render-only.**
- In `world_hash`: the per-region weather category, storm-cell positions/
  intensities/velocities, the precipitation intensity field, the strike schedule.
  This is what makes weather "server-authoritative, replicated via lockstep
  inputs/seeded schedule — NOT client-local" — every client and every replay sees
  the same storm in the same place at the same tick. Updated with DeterministicMath,
  snapshotted, `weather` sub-hash added.
- Render-only: the `weather_system.frag` streaks, the precipitation *particles*
  (§2), wetness material darkening. These read the authoritative state but
  compute pixels client-side.
- Wetness response in materials: a render-side material modulation (roughness/
  albedo shift) keyed by the local precipitation/accumulation value — NOT a
  persisted terrain edit in 5a (snow/season accumulation as persisted terrain
  state is a roadmap "committed expansion", later).

Seed offset `+12` appended FIRST. Storm scheduling RNG is a deterministic stream
seeded from `seed+12` + region + tick-epoch (no wall-clock, no `std::random` in
the tick path).

Gate: WeatherVisual extends to (a) a **state-hash** assertion (weather sub-hash
stable across resim/replay) and (b) the existing baseline-vs-weather luminance/
streak visual checks now driven by real state. Endurance300Storm forces an active
storm for 300 ticks to catch perf cliffs and state-growth leaks.

---

## 5. Cloud layer — tier 1

**Choice: wind-advected 2.5D coverage clouds with a projected cast-shadow term —
NOT volumetric raymarch (that is tier 2 / iteration 6, sharing SHIELD-RT).**

- Representation: a scrolling 2D coverage field (cloud density f(noise, wind
  offset, weather)) rendered as a sky-dome cloud layer + landscape-distance
  fluffy imposters. The coverage field scrolls by integrating the wind grid's
  large-scale direction — clouds visibly move with the wind, tying §3 to the
  sky.
- **Real cast shadows:** project the coverage field onto the world as a
  cloud-shadow attenuation sampled in the **lighting pass** — multiply direct
  sun contribution by (1 − cloudShadow(worldPos)). This yields crawling terrain
  shadows as clouds drift, the cheap-but-dramatic "real place" cue. Sample is one
  texture lookup per lit fragment; pin its cost in the release lane.
- Weather/biome-aware: coverage amplitude driven by the weather state (overcast →
  high coverage) and biome (deserts clearer). Reads the authoritative weather
  category but the cloud field itself is render-only (it is a deterministic
  function of weather state + time, so it agrees across clients without being
  hashed — but it must NOT feed anything sim-side).

Determinism: render-only. The coverage field is a pure function of (replicated
weather state, sim time, wind direction) so all clients see the same clouds, but
nothing reads clouds back into `world_hash`.

Gate: CloudShadow — assert a moving cast-shadow signature on terrain (luminance
delta in a fixed ROI between two times as a shadow edge crosses it), plus the
coverage layer present in SkyboxVisual.

---

## 6. Lightning

**Choice: deterministic strike events scheduled sim-side from storm-cell state;
fully cosmetic render + audio response.**

- Sim-side (in `world_hash`): a storm cell with intensity above a threshold
  schedules strike events — `(tick, world position, magnitude)` — from its
  deterministic RNG stream (`seed+13`). Strikes are **world events** (replayable,
  identical for every player), not client effects. The schedule is part of the
  weather/storm state hash. Iteration-6 hook: a strike writes ignition into the
  fire scalar field — **noted, not built** (no fire field in 5a).
- Render-side:
  - **Bolt geometry:** a branching polyline generated client-side (midpoint-
    displacement / L-system) seeded from the strike event's deterministic seed so
    the bolt *shape* is also identical across clients if desired (cheap to make
    deterministic; keep the generator seeded from the event).
  - **Light pulse:** a 1-to-few-frame full-scene illumination injected through the
    **lighting pass** (a transient bright directional/point contribution at the
    strike position) — this is the "THE timing shot" the gate asserts.
  - **Scorch/impact:** emitters (§2) at the strike point.
- Audio: **thunder via the existing `AudioPropagationSystem`** with distance-
  correct delay — the strike position + speed-of-sound gives a delay of
  distance/343 m·s⁻¹, so thunder arrives seconds after the flash for free. Reuse,
  do not build new audio machinery; `EnvironmentalAudioSystem` already exists.

Determinism: the **schedule** is sim/hashed; the bolt render, light pulse,
emitters, and thunder playback are render-only (the playback *trigger* is the
deterministic event, the pixels/samples are local).

Gate: WeatherVisual asserts a captured strike frame shows the full-scene
luminance pulse (frame-mean luminance spike vs neighbors) + bolt pixels (a
bright thin high-gradient structure) — the photography timing-shot contract.

---

## 7. Cross-cutting: the `world_hash` mega-bump

Wind (§3) + weather (§4) + lightning schedule (§6) + season time (sky/celestial,
render-derived but the *time scale* is tick-derived) all add sim sub-hashes →
`world_hash` changes deliberately. Per the iteration-4 contract this is an
**orchestrator decision**, executed in the SAME commit as: the new sub-hashes,
the heavy-mode save/load/resim oracle re-validation, and the LREC1 replay +
lockstep loopback gates re-blessed. Particle motion, sky scattering, cloud
fields, bolt geometry, and all `*_system.frag` overlays stay OUT of the hash.

Recommended sub-hash registry after 5a:
`world_hash = fnv(terrain, entities, water, fields[=scalar+wind], weather, rng)`
— `wind` folded under a generalized `fields` sub-hash or its own slot (design-doc
decision; prefer its own slot for debuggability), `weather` (incl. storm cells +
strike schedule) as a new slot.

---

## 8. Scope guards (rejected / deferred — record so the critique can enforce)

- **Volumetric (Nubis) raymarch clouds** — iteration 6 (SHIELD-RT froxel infra).
- **Fire as a spreading simulation** (Aetheric scalar field) — iteration 6; only
  the lightning→ignition *hook point* is noted.
- **Froxel volumetric fog / god-ray volume** — 5a takes the analytic aerial term;
  froxel volume is a later upgrade.
- **Compute-driven particle pools** — stretch; v1 is instanced.
- **Persisted snow/season terrain accumulation** — roadmap committed expansion,
  not 5a.
- **3D splash fluids (FLIP/SPH)** — permanently rejected (non-deterministic,
  lockstep-hostile); particles fake the visible part.
- **Foliage, ecology channels, atmosphere audio ambience, waterfalls** — these
  are iteration **5b**, not 5a (5b consumes 5a's wind grid + particle framework +
  weather state).
