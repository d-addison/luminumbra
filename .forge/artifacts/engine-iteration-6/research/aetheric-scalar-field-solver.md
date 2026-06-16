# Aetheric scalar-field solver (advection-diffusion on the 30 Hz tick)

**Slug:** `aetheric-scalar-field-solver`
**Iteration:** engine-iteration-6, Wave 0 research
**Side:** SIM-AUTHORITATIVE. This is a deterministic per-tick field that MUST fold into
`world_hash` via a new `aether` sub-hash. Landing it is a deliberate `world_hash`
mega-bump in its own commit + LREC1 re-bless (same protocol as the wind/weather
mega-bumps #1–#3 already documented in `WeatherSystem.h` lines 28–31). The
render-side sampling/visualization of the field is RENDER-only and must NOT touch
`world_hash`.

---

## Recommended approach

Build an `AethericFieldSystem` (sim TU under `src/luminumbra_common/systems/`)
that mirrors the existing `WindFieldSystem` / `WeatherSystem` structure exactly, and
solve a **Jos Stam semi-Lagrangian advection + Gauss-Seidel implicit diffusion**
step per scalar channel, advected by the **existing A2 wind field** (no independent
velocity solve — see "Alternatives"). Concretely:

1. **Storage.** Reuse `luminumbra::fields::FieldGrid<float>` (the same container
   `WeatherSystem::m_precip` already uses) at the pinned wind geometry: 24 m cells,
   64×64 extent over the 1536 m streamed region (`kWindCellSizeM`, `kWindExtentCells`).
   FieldGrid is a 2-D-per-payload store with integer indexing and a canonical
   z-major iteration order [FieldGrid.h:16, 61–66]. The "3 layers" are a *consumer*
   concern as in `WindFieldSystem` (it stores `WindCell{ Vec2 layer[3] }`); for the
   Aetheric stack, store one `FieldGrid<float>` PER scalar channel (e.g. `aether`,
   `fire`) on a single layer to start — vertical Aetheric is out of scope for the
   first land, matching how 5a "ignored vertical wind" [WindFieldSystem.h:8–9, 35].
   Need a double-buffer (`m_field` + `m_field_prev`) per channel because
   semi-Lagrangian advect reads `prev` and writes `cur`; allocate both as hoisted
   per-tick scratch, no tick-path heap alloc (the `WindFieldSystem` scratch pattern
   [WindFieldSystem.h:125–130, WindFieldSystem.cpp:85–89]).

2. **Per-tick step (per channel), in fixed canonical order.** Per Stam's
   `dens_step`: add sources → diffuse (implicit) → advect.
   - **Source/emission/absorption.** Add deterministic per-cell source `S * dt`
     (emission), then a linear decay `c -= absorb * c * dt` (absorption). `dt = 1/30`
     is the fixed tick. Emission/absorption come from game content (LuminCrystal /
     Glimmer) but the engine sees only "a per-cell source/sink float."
   - **Diffuse.** Gray-Scott-style or plain Fickian diffusion via Stam's `lin_solve`:
     `c[i] = (c0[i] + a*(sum of 4 neighbors)) / (1 + 4a)`, a = dt·diff·N², run with a
     **FIXED iteration count** (Stam uses an `iter` parameter; the canonical
     real-time value is 20 — but 4–8 is enough for a smoke field at this cell count;
     pin the count, do not converge-to-tolerance, because a tolerance loop is a
     determinism + budget hazard) [Stam GDC2003; Stanford slides]. Gauss-Seidel is
     unconditionally stable here, so dt is unconstrained by the diffusion step.
   - **Advect** by the wind field: for each cell center, trace the cell-center
     position **backward** through the wind velocity (`x = pos - dt*v`), clamp the
     backtraced coordinate to grid bounds, and **bilinearly interpolate** the
     previous field at that point [Stam99; Stam GDC2003]. This is unconditionally
     stable for ANY dt because the interpolated value is a **convex combination
     (weights ≥ 0, sum = 1) of the four surrounding old cell values, so it is bounded
     by their min/max and cannot blow up** — verified across the original paper and
     the games paper [Stam99; Stam GDC2003 lin/advect].

3. **Velocity source = `WindFieldSystem::SampleWind`.** Do NOT solve Navier-Stokes.
   The Aetheric field is a *passive scalar* advected by the already-deterministic A2
   wind grid (the wind grid is "consumed by B1 advection" per its own design notes
   [WindFieldSystem.h:48–49]). This removes the entire pressure-projection /
   divergence-free machinery (the most expensive and most determinism-fragile part of
   Stam) and reuses an existing sim-blessed velocity field. `SampleWind` is already
   deterministic (all DeterministicMath) [WindFieldSystem.cpp:216–234].

4. **Lightning → ignition hook.** The fire channel is seeded by the EXISTING
   deterministic strike schedule. `WeatherSystem` already produces `StrikeEvent`s
   (`strike_tick`, `world_x`, `world_z`, `magnitude`) in a canonical, world-hashed
   order, and the header ALREADY reserves this exact hook: "Iteration-6 fire-ignition
   hook (NOTED, not built): a future ecology system will read `StrikeSchedule()` at
   `strike_tick` and probe terrain flammability at `world_{x,z}` to seed a fire"
   [WeatherSystem.h:105–114, 162–173]. Implementation: in `AethericFieldSystem::Update`,
   call `weather->StrikesThisTick()`, map each strike's `world_{x,z}` to a cell via the
   same `LocalCell` integer-floor math, and add a deterministic `magnitude`-scaled
   source into the `fire` channel at that cell. Because the strike set is already
   world-hashed and replay-stable, the ignition is automatically deterministic.

5. **Emissive coupling (render-side, NO world_hash).** The lighting pass already
   reads `emissive_intensity` from materials-LUT row 2, normalized by
   `kEmissiveLutScale = 8.0`, applied in `LightingPass` [LightingPass.cpp:117–118;
   materials.json]. The Aetheric field's render representation (a sampled texture or
   the field uploaded to a 3-D/2-D texture) modulates emissive output by sampling the
   replicated `aether`/`fire` value — a RENDER-only read of replicated sim state
   (same one-way contract `WeatherSystem.h:32–34, 99–104` mandates for the lightning
   overlay). The sim writes the field; the renderer multiplies emissive by it. The
   render read NEVER feeds back into the sim or `world_hash`.

6. **Sub-hash.** Add `ComputeAetherSubHash()` copied from
   `WindFieldSystem::ComputeWindSubHash` [WindFieldSystem.cpp:236–262]: fnv1a-64 over
   seed, tick, geometry, origin, then every cell of every channel in FieldGrid
   canonical order, hashing raw IEEE float bits via `DeterministicMath::BitsOf`. Fold
   into a new `aether` `world_hash` slot.

---

## Alternatives considered (+ why rejected)

- **Full Stam stable-fluids with its own velocity + pressure projection.** Rejected:
  the projection step (another Gauss-Seidel Poisson solve) doubles the linear-solve
  cost and is the most numerically delicate part for cross-machine bit-determinism,
  for ZERO gameplay benefit — the design only needs a scalar advected by *wind we
  already have*. Stam himself separates `dens_step` (what we want) from `vel_step`
  (what we don't) [Stam GDC2003].
- **Explicit (forward-Euler) diffusion instead of Gauss-Seidel.** Rejected on a
  hard numerical constraint: explicit diffusion has the **parabolic CFL bound
  `dt ≤ Δx²/(2D)` per axis** [MDPI 2021; arXiv 2409.15552 stability-function analysis].
  At Δx = 24 m and dt = 1/30 s, that caps the diffusion coefficient at
  `D ≤ Δx²/(2·dt) = 576/(2/30) = 8640 m²/s`, which is actually generous — BUT the
  bound is a *trap*: any future cell-size shrink, sub-stepping, or higher D silently
  goes unstable and corrupts `world_hash` on only *some* machines/parameter sets.
  Stam's implicit Gauss-Seidel is unconditionally stable and removes the footgun for
  a few extra adds. Use it.
- **Gray-Scott reaction-diffusion as the primary model.** Partially adopted, mostly
  deferred. Gray-Scott (`u,v` with feed `F` and kill `k`, Pearson's 12 pattern
  classes) is the canonical way to get organic spot/stripe/labyrinth emissive
  patterns [Pearson 1993 via MROB; ScienceDirect S1468121803000208]. BUT (a) its
  reaction term `uv²` is fine (only `* +`, deterministic), yet (b) the *interesting*
  Pearson regimes sit right next to a saddle-node bifurcation [uni-frankfurt
  Gray-Scott project] — i.e. they are intentionally near-chaotic, which is the worst
  possible behavior for a bit-deterministic system that must survive a one-ULP delta
  across machines. RECOMMENDATION: ship plain Fickian diffusion + advection first
  (visually a glowing, wind-blown haze), and treat Gray-Scott reaction as a LATER,
  separately-blessed channel with conservatively-chosen, well-inside-stable `F`/`k`
  — never the bifurcation-edge values. Flag as open risk.
- **Fixed-point (Q16.16) field instead of float32.** Rejected as unnecessary. The
  luminumbra contract already establishes that IEEE basic ops (`+ - * / sqrt`) under
  the pinned FP flags (`-ffp-contract=off`, precise, no FMA) are bit-stable across
  machines/compilers/libc [DeterministicMath.h:7–28; Dawson via Gaffer]. Semi-Lagrangian
  advect + Gauss-Seidel use ONLY those ops plus `floor` for indexing — no
  transcendentals, no `exp`. So float32 is deterministic *provided iteration order is
  fixed* (FieldGrid canonical order) and the wind sample is the existing deterministic
  one. Fixed-point would add precision-management cost for no determinism gain. (NOTE:
  if a future reaction term needs `exp` for an Arrhenius-style rate, it must route
  through a NEW `DeterministicMath::Exp` wrapper — none exists today; the file has
  Sin/Cos/Atan/Atan2/Sqrt only [DeterministicMath.h]. Flag as open risk.)
- **GPU compute solve.** Rejected for the SIM field: GPU reductions/atomics are a
  notorious cross-vendor determinism hazard and `GPUSDFSystem` compute is already
  DISABLED in this engine. The SIM solve stays on CPU. The render-side *sampling* can
  live on GPU since it is non-authoritative.

---

## Perf budget (concrete)

**CPU (sim, the authoritative cost).** Grid is 64×64 = 4096 cells/channel.
Per channel per tick: source (1 pass), diffuse (`iter` Gauss-Seidel passes × 4096 × ~6
flops), advect (4096 × ~20 flops + 1 `SampleWind`). With `iter = 8`:
diffuse ≈ 8 × 4096 × 6 ≈ 0.20 Mflop; advect ≈ 4096 × 20 ≈ 0.08 Mflop. Two channels
(`aether`, `fire`) ≈ 0.55 Mflop/tick — trivially under **0.20 ms/tick** on the sim
thread, in line with the wind (≤0.15 ms) and weather (≤0.20 ms) gated budgets
[WindFieldSystem.h:72; WeatherSystem.h:139]. **Pin a `AethericFieldDeterminism` /
budget gate at ≤ 0.20 ms/tick for the 2-channel 64² extent.** Memory: 4096 × 4 B × 2
buffers × 2 channels = 128 KB resident — negligible.

**GPU (render-side sampling/visualization — shared, contended budget).** The field
must be uploaded for the renderer to modulate emissive. Options + budget so Wave A can
split the shared GPU total explicitly:
- Upload the 2 channels as a single `R16F`×2 (or `RG16F`) 64×64 texture per frame:
  ~32 KB VRAM, upload cost negligible.
- Emissive modulation is a few texture taps folded into the EXISTING lighting pass
  (no new full-screen pass): budget **≤ 0.10 ms GPU**, **≤ 0.1 MB VRAM**.
- If a dedicated volumetric/billboard "glow" visualization is added later (NOT
  required for the first land), budget it SEPARATELY and explicitly against the shared
  pool that also feeds SHIELD-RT raymarch, GPU grass, and volumetric clouds. First
  land claims only the **≤ 0.10 ms / ≤ 0.1 MB** lighting-pass tap.

---

## Determinism implications (sim vs render; world_hash impact)

- **SIM side, world_hash-affecting.** The field state is authoritative and folds into
  a new `aether` sub-hash. Landing it = a deliberate `world_hash` mega-bump in its own
  commit, LREC1 replay re-bless, lockstep re-bless (the protocol already used for the
  wind/weather mega-bumps [WeatherSystem.h:28–31]). State this explicitly in the bump
  commit.
- **Determinism is achievable with float32** because: (1) only IEEE basic ops + floor
  are used (no libm transcendentals → passes SimDeterminismLint); (2) FieldGrid's
  canonical z-major order is the fixed iteration order for diffuse, advect, AND the
  sub-hash [FieldGrid.h:16, 61–66]; (3) the advecting velocity is the already-blessed
  deterministic `WindFieldSystem::SampleWind`; (4) the bilinear-interp weights are a
  fixed `* +` sequence under `-ffp-contract=off`. **Pure function of (seed, tick,
  region origin)** like wind/weather, so save/load/resim reproduces it.
- **HARD CONSTRAINTS to enforce:** fixed Gauss-Seidel iteration count (NO
  converge-to-tolerance loop); no `std::unordered_*` iteration in the TU; no
  wall-clock/RNG on the tick path; if Gray-Scott reaction is added, its rates must use
  only `* + -` (no `exp` until a `DeterministicMath::Exp` exists). The advect backtrace
  clamp must be integer/float-deterministic (use the same floor-division pattern as
  `WindFieldSystem::LocalCell` [WindFieldSystem.cpp:106–118]).
- **RENDER side, NEVER touches world_hash.** Emissive modulation, texture upload,
  any glow pass are one-way reads of replicated state (the `WeatherSystem.h:32–34`
  one-way contract).

---

## Integration notes (luminumbra files/systems touched)

- **NEW:** `src/luminumbra_common/systems/AethericFieldSystem.{h,cpp}` — model on
  `WindFieldSystem` / `WeatherSystem` (constructor takes `world_seed`, uses next free
  seed-offset in the registry; `Update(tick, region_anchor, const WindFieldSystem*,
  const WeatherSystem*)`).
- **REUSE:** `src/luminumbra_common/fields/FieldGrid.h` (`FieldGrid<float>`, frozen
  for iter-6) — one grid per channel.
- **REUSE:** `WindFieldSystem::SampleWind` for the advecting velocity
  [WindFieldSystem.h:79–81].
- **REUSE:** `WeatherSystem::StrikesThisTick()` / `StrikeSchedule()` for the ignition
  seed [WeatherSystem.h:162–173]; honor the explicit iter-6 hook note
  [WeatherSystem.h:105–114].
- **REUSE:** `core/DeterministicMath.h` for any trig (advect needs none; reaction
  defer); fnv1a sub-hash helpers (copy `MixFloat`/`MixU64`/`Fnv1a64` pattern from
  `WindFieldSystem.cpp:44–69`).
- **RENDER:** `src/luminumbra_client/rendering/passes/LightingPass.cpp` +
  `res/shaders/lighting_pass.frag` — sample the uploaded Aether texture, multiply into
  the `emissive_intensity`/`kEmissiveLutScale` path [LightingPass.cpp:117–118].
  Material content (LuminCrystal/Glimmer) lives in `data/common/materials.json` row 2.
- **WIRING:** the sim runner that already ticks wind+weather
  (`ServerWorldRunner.cpp` / `main_server.cpp`) ticks Aetheric AFTER weather (so
  `StrikesThisTick()` is current) and folds its sub-hash into `world_hash`.
- **GATE:** add `AethericFieldDeterminism` (sub-hash stability + ≤0.20 ms/tick budget)
  alongside the existing `WindFieldDeterminism` / `WeatherVisual` gates.

---

## Open risks

1. **Gray-Scott near-bifurcation chaos vs determinism.** The visually-rich Pearson
   regimes sit beside a saddle-node bifurcation — exactly where a 1-ULP cross-machine
   delta could diverge over many ticks and desync `world_hash`. MITIGATION: ship
   plain diffusion first; if reaction is added, use conservatively stable `F`/`k`,
   and stress it with the Endurance/replay gates over 300+ s before blessing.
   [uni-frankfurt Gray-Scott; ScienceDirect S1468121803000208].
2. **No `DeterministicMath::Exp`.** Any Arrhenius/exponential reaction or decay term
   needs a new bit-locked `Exp` wrapper (golden-tested like Sin/Cos). Until then,
   restrict to polynomial/linear rates. [DeterministicMath.h scope].
3. **Numerical dissipation of semi-Lagrangian advect** — the field smears/damps over
   time (the documented cost of the convex-combination stability) [Stam99; Stam
   GDC2003]. For a slowly-emitted glow this is acceptable/desirable; for crisp fire
   fronts it over-smooths. BFECC would sharpen it but adds cost AND another
   determinism surface — defer. UNVERIFIED whether the visual result is "crisp enough"
   for fire; needs a Wave-A visual spike.
4. **Fixed iteration count vs visual quality.** Too few Gauss-Seidel passes = under-
   diffused; the canonical 20 is for full fluids. UNVERIFIED what count looks right at
   64² — tune in Wave A, then PIN it (changing it later is a world_hash bump).
5. **FieldGrid is frozen for iter-6.** If the per-channel/per-layer storage needs a
   shape the frozen API can't express (e.g. true 3-D), that's a contract conflict —
   confirm single-layer-per-channel suffices before committing.
6. **Shared GPU budget.** The ≤0.10 ms render tap is small, but any later volumetric
   glow pass contends with SHIELD-RT/grass/clouds — must be budgeted explicitly by
   Wave A, not absorbed silently.

---

## Citations

1. **Jos Stam, "Stable Fluids", SIGGRAPH '99** — semi-Lagrangian backward trace +
   linear interpolation; unconditional stability; implicit (Gauss-Seidel) diffusion;
   numerical dissipation as the cost.
   https://en.wikipedia.org/wiki/Fluid_animation (paper summary) and original PDF
   (people.computing.clemson.edu/~dhouse/courses/817/papers/stam99.pdf — 301-redirects,
   mirror currently 404; claims cross-verified against the games paper below).
2. **Jos Stam, "Real-Time Fluid Dynamics for Games", GDC 2003** — `dens_step` (add
   source → diffuse → advect) separable from `vel_step`; `lin_solve` Gauss-Seidel
   relaxation with an `iter` loop parameter; `advect()` backtrace `x = i - dt0*u`,
   clamp to grid, bilinear interp; **unconditional stability because the interpolated
   value is a convex combination of the four surrounding old values, bounded by their
   min/max.**
   http://graphics.cs.cmu.edu/nsp/course/15-464/Fall09/papers/StamFluidforGames.pdf ;
   mirror https://www.readkong.com/page/real-time-fluid-dynamics-for-games-9900894
   (convex-combination stability + backtrace/clamp confirmed here).
3. **Stanford CS468 slides, "Stable Fluid" (An Nguyen)** — confirms semi-Lagrangian
   backward trace + interpolation + Gauss-Seidel implicit diffusion + unconditional
   stability regardless of timestep.
   http://graphics.stanford.edu/courses/cs468-05-fall/slides_2/an_stable_fluid_fall_05.pdf
4. **Explicit diffusion parabolic CFL bound `Δt ≤ Δx²/(2D)`** — MDPI, "Explicit Stable
   Finite Difference Methods for Diffusion-Reaction Type Equations" (2021),
   https://www.mdpi.com/2227-7390/9/24/3308 ; eigenvalue/stability-function derivation
   `Δt ≤ 2·Δx²/C`, arXiv 2409.15552 (PIROCK), https://arxiv.org/pdf/2409.15552 .
   FINDING: explicit diffusion is conditionally stable → footgun; use implicit.
5. **Gray-Scott model, Pearson's parameters & 12 pattern classes; near-bifurcation
   chaos** — MROB Gray-Scott talk https://mrob.com/sci/talks/20101209.html ;
   uni-frankfurt Gray-Scott project (fixed points / saddle-node separatrix)
   https://itp.uni-frankfurt.de/~gros/StudentProjects/Projects_2020/projekt_schulz_kaefer/ ;
   ScienceDirect "Pattern formation in the Gray–Scott model"
   https://www.sciencedirect.com/science/article/abs/pii/S1468121803000208 .
   FINDING: interesting patterns sit beside a saddle-node bifurcation → determinism
   risk.
6. **Floating-point determinism for lockstep** — Gaffer On Games, "Floating Point
   Determinism" https://gafferongames.com/post/floating_point_determinism/ ; cross-
   platform RTS FP indeterminism
   https://www.gamedeveloper.com/programming/cross-platform-rts-synchronization-and-floating-point-indeterminism .
   FINDING: IEEE basic ops are reproducible cross-machine under controlled FP flags —
   matches luminumbra's existing DeterministicMath contract, so float32 advect/diffuse
   is deterministic without fixed-point.
