# Iteration 6 — Wave A Spec (REVISED post-critique)

**Status:** spec / pre-dispatch — **revised** to clear the 4 BLOCKs + scope split
from `.forge/critique-wave-a-spec-20260615.md`. **Branch:**
`feat/polyglot-audit-roadmap` (tip after spec v1 = `bb3a5fc`). **Inputs:** the 5
Wave-0 briefs + `_synthesis.md`; owner decisions 2026-06-15; the critique report.
**Determinism baseline:** `world_hash d950a6afc12a5cdc`.

> **What changed from v1 (critique dispositions):** the wave is **split** (heavy
> SHIELD-RT substrate moved to **Wave A.2**; froxel volume + SDF brick clipmap are
> **explicit non-goals**); erosion is correctly modeled as a **non-local baked grid**
> with a single-point lookup contract (not a "pure additive offset"); the shaping-
> spline hash fix is its **own attributable commit**; the heightfield tracer is
> **PROVISIONAL** pending a **GPU profile that now gates** the substrate; the perf
> budget is **PROVISIONAL** pending Phase 0.3; fire is **deferred** out of A1. Full
> objection→disposition table at the end.

## 0. Owner constraints (pinned)
- **Hardware:** RTX 5070 Ti, 16 GB. **Goal:** ~300 fps, beautiful (Frostbite/Crysis
  caliber or stylistic). Caps may be raised.
- **Render API:** stay **OpenGL 4.5**; Vulkan/DLSS/hardware-RT deferred to a future
  render iteration (memory `render-api-and-target`). "Vulkan-aware" is **demoted to a
  non-binding coding-style note for the new shaders only** (optional `glslangValidator`
  CI check over `res/shaders/*.compute`); an opaque-handle abstraction over `GLuint`
  is a **NON-GOAL** this wave (no SPIR-V path exists; it would tax the wave for an
  unvalidatable deferred payoff).
- **Far-field quality:** nicest path welcome, but SDF GI / soft-shadows and the SDF
  brick clipmap are **NOT** Wave A/A.2 scope (recorded for later).

### Frame-budget honesty
300 fps (3.3 ms) native at 6× view distance + volumetrics on GL **without DLSS** is
not achievable; **300 fps is the post-Vulkan+DLSS target.** Wave A/A.2 interim target
≈ **120–144 fps native** on the 5070 Ti. **§4 numbers are PROVISIONAL** — they cannot
be "ratified" until **Phase 0.3 (quiet-machine baseline) lands**, which is an **A.2
entry-gate** (a binding split needs the measured cost of the existing 6× passes, not
an assumed one).

## 1. Wave structure & the ordered, attributable hash changes
The wave is split to avoid bundling ~4 independently-hard subsystems behind one
serialization.

**Wave A (this spec, dispatchable after the BLOCKs are closed):**
- **A0** — Nsight markers + **standalone GPU tracer micro-profile spike** (the BLOCK
  #3 gate, pulled to the front).
- **A1** — Aetheric **aether** scalar field (fire deferred). **Hash change #1 (sim).**
- **A1.5** — shaping-spline params-hash fix. **Hash change #2 (worldgen params).**
- **A2** — hydraulic/thermal relief (baked grid). **Hash change #3 (worldgen params).**

**Wave A.2 (separate spec, after Wave A merges + Phase 0.3 lands):**
- **A3a** substrate skeleton + tracer profile review → select tracer.
- **A3b** freeze substrate (ray-setup/half-res/temporal/upsample/empty-skip — **NOT**
  froxel) + SHIELD-RT heightfield primary path + parity/seam/temporal gates.

**Explicit NON-GOALS of Wave A & A.2:** froxel volume (built in **Wave B** with its
only consumer, clouds); SDF brick clipmap (only if the A3a profile or measured
far-field 3D-content density demands it); SDF GI/soft-shadows; the `GLuint`-opaque
portability layer.

**Three ordered, attributable hash-affecting commits** (was mislabeled "exactly two
bumps" in v1). Each is its **own commit** with in-commit replay (LREC1) + lockstep
re-bless, and a **CI guard** asserts each commit changes **exactly one** sub-hash term
(squash-proof, MINOR fold):
1. **#1 A1 aether** — sim composite term (see A1).
2. **#2 A1.5 shaping-spline fold** — `ComputeTerrainParamsHash`, gated on
   `shaping_enabled`; re-keys shaped presets only. **Lands before #3.**
3. **#3 A2 hydraulic relief** — `ComputeTerrainParamsHash`, gated on the new
   `hydro_enabled`. **Lands before any SHIELD-RT parity bless (Wave A.2).**

**Serialization (BLOCK/MAJOR #20):** A1, A1.5, A2 all rewrite the same pinned-hash
literal in `validate-engine-frontier.ps1` (≥4 sites) → **NOT file-disjoint.** Author
in parallel worktrees but **land serially** (#1→#2→#3), rebasing each onto the prior so
its expected hash is computed against the post-prior baseline. `validate-engine-
frontier.ps1` is the named ordering chokepoint. Worktree base-check + real-copy-vendor
+ no-`--force`-removal are **fail-closed harness preconditions**, not prose (memory
`stale-main-worktree-hazard`).

---

## A0. Nsight markers + GPU tracer micro-profile (do first; gates A.2)
**Render-only, no hash.**
- Add `push_debug_group`/`pop_debug_group` to `passes/PassGlHelpers.h` (guard on
  `GL_VERSION_4_3` + non-null fn, like `glObjectLabel`); bracket each
  `RenderPipeline::render_frame` pass. Debug context already on under
  `LUMINUMBRA_DEBUG`. **Add a push/pop balance assertion** (counter==0 at frame end)
  so Nsight captures aren't garbled (MINOR fold).
- **GPU tracer micro-profile spike (BLOCK #3):** a *thin standalone* bench that traces
  the existing `FarLodStore` tiles two ways — heightfield max-mip march vs SDF
  sphere-trace — on the **5070 Ti**, measuring real GPU ms (Nsight/GL timers) for a
  **flat far view AND a grazing low-camera-over-relief view** (the spike's 5× blowup
  case, MAJOR #5). Emits a committed `shieldrt-tracer-profile.json` (measured ms per
  case). **This is a hard gate that MUST be reviewed before any Wave A.2 substrate
  commit.** Until then the heightfield-primary choice is **PROVISIONAL**, not
  "CONFIRMED" (a CPU-Debug spike with 88% variance does not confirm GPU cost).
- **Verify:** `RenderHealth` byte-stable; capture shows named groups; profile json
  present with both cases measured.

---

## A1. Aetheric AETHER scalar field (SIM — hash change #1)
Engine knows only "emissive scalar fields" (`engine-game-decoupling`).
**FIRE IS DEFERRED out of A1** (MAJOR #16): a single 2.5D scalar per column cannot
represent rising flame/plumes without likely violating the `FieldGrid` freeze. Wave A
ships **only the `aether` channel** (clear 2.5D justification: a slowly-diffusing
horizontal energy field) + its render tap. Fire returns only with a **written
2.5D-sufficiency justification artifact** (or a 3D design) in a later wave; the
lightning→fire hook is **descoped** from A1.

### Data
- New `src/luminumbra_common/systems/AetherFieldSystem.{h,cpp}`, **one
  `FieldGrid<float>`** (aether), wind geometry (24 m, 64×64 = 1536 m), region-following
  integer origin. Double-buffer + hoisted scratch, zero per-tick heap alloc.
- **Wind is cited for FieldGrid plumbing + sub-hash pattern ONLY, not as the solver
  template** (MAJOR #8): wind is stateless noise resampling; aether is a **stateful PDE
  with cross-tick float accumulation** and origin-scroll re-anchoring — a different
  beast.

### Update (deterministic, 30 Hz, after weather)
- Canonical z-major / x-ascending iteration.
- **Diffusion:** fixed-iteration **Gauss-Seidel** (no `DeterministicMath::Exp`
  exists). **The iteration count is selected by a Wave-A visual spike, ratified, and
  PINNED before the #1 bump commit** (changing it later = a forced re-bless; explicit
  non-goal for the rest of iter-6). Record the integer + the decay form/coefficients
  in the spec before dispatch (decision artifact).
- **Advection:** semi-Lagrangian backtrace; **bilinear interpolation applies to the
  scalar field at the backtraced point** (fixed op order, IEEE basic ops). **Velocity
  comes from `WindFieldSystem::SampleWind` which is NEAREST-cell** (not bilinear — v1
  conflated these, MINOR fold). `LocalCell` uses `std::floor`, the **sanctioned
  integer-exact libm exception** (like `Sqrt`) — confirm SimDeterminismLint's allowlist
  covers `std::floor` before dispatch.

### Determinism / hashing (corrected — MINOR/Determinism #1)
- Seed offset **+14** (append-only; +11 wind, +12 weather, +13 lightning).
- Add `AetherFieldSystem::ComputeAetherSubHash()` mirroring `ComputeWindSubHash`
  (fnv1a-64 over IEEE bits via `MixFloat`/`BitsOf`, canonical order).
- `ComposeWorldHash` in `ServerWorldRunner.cpp` is **3-arg `(chunk,wind,weather)`**;
  the v1 "append like the lightning slot" precedent was **wrong** (lightning lives
  *inside* the weather sub-hash). The real edit: **add a 4th arg** `aether`, append
  `"|aether:"+aether`, **update BOTH call sites (≈lines 212, 281)**, add
  `GetAetherFieldSystem()` threading through `GameSession`, and a `SubHashes.aether`
  field. The wind→weather composite-string append IS the correct precedent.
- SimDeterminismLint stays green (new TU under `src/luminumbra_common/systems`).

### Render coupling (render-only)
- Pack aether → R16F texture from `cells()` in canonical order; sample in the lighting
  pass → materials-LUT emissive path (row 2, `kEmissiveLutScale=8.0`). One-way bridge.
  Budgeted as the **flat 2D LUT tap only**; any froxel/volumetric Aetheric emission is
  a future budget item (MINOR fold).

### A1 gates
- `AetherFieldDeterminism` mode: field differs across fixtures, identical on double-run.
- **Coupling-assertion leg (MAJOR #17 — avoid the iter-5 dead-system trap):** assert
  the aether emissive tap **measurably changes lighting-pass output** at a seeded
  source; hash-stability alone must NOT pass.
- **Origin-scroll + long-horizon replay (MAJOR #8):** a multi-tick replay **with region
  motion** (cells entering/leaving) AND a **10k-tick** replay to surface cross-tick
  float accumulation, asserted before the #1 bump is blessed.
- `HeadlessServerTick` advances to the new canonical **in the #1 commit**; heavy oracle
  + LREC1 + lockstep re-bless there. ctest + an `AetherFieldSystem` diffuse/advect
  golden.

---

## A1.5. Shaping-spline params-hash fold (WORLDGEN — hash change #2)
**Split out of erosion (BLOCK #4).** `ComputeTerrainParamsHash`
(`FarLodStore.cpp:70-113`) currently hashes base/cave/island/biome/river/structure
params but **NOT** the shaping fields (`shaping_enabled`, `*_frequency`, `peaks_*`, the
three splines). Shipped archipelago/mountains presets enable shaping → folding the
splines **re-keys those presets** (a real, separate change from erosion).
- Own commit, **marker `0x05`**, gated on **`shaping_enabled`** (NOT `hydro_enabled`).
  **Lands before** the A2 hydro bump.
- **Pinned canonical encoding:** `FnvMixValue(count)` then each `[input,output]`
  control point in **stored order** via the raw-IEEE-bits `FnvMixValue` path; input
  order taken as-stored (not re-sorted). **Unit test: a known spline → a known u64.**
- Acceptance: shaping-OFF legacy fixture `params_hash` **byte-unchanged**; shaping-ON
  preset `params_hash` **changes exactly once**.

---

## A2. Hydraulic/thermal RELIEF (WORLDGEN — hash change #3)
Renamed **`hydro_*` / "hydraulic relief"** to avoid collision with the existing
analytic *erosion* channel (seed +4, `erosion_spline`) (MINOR fold).

### Architecture (BLOCK #1 — the core correction)
Erosion is a **non-local iterative grid simulation**, NOT a per-point analytic term.
So `ErodedHeightOffset` is a **baked offset grid**, and the spec must define the
**storage + single-point lookup contract**:
- **Bake:** offline/generation-path, per region tile, thermal (talus) + hydraulic
  (Mei-style) over the tile **with an overlapping halo**; cropped interior is the
  product. Fixed iteration/step count **baked into the params hash**. The
  `C=Kc·sin(tilt)·|v|` term uses `DeterministicMath::Sin`.
- **Storage:** persist the baked hydro offset alongside the far-LOD/region data
  (quantized), keyed by the params hash so it invalidates correctly.
- **Single-point consistency — DECISION (a), owner-confirmed 2026-06-15:** the baked
  hydro offset grid is the **authoritative** terrain source; ALL single-point callers
  (collision, spawn, telemetry, water, render) sample it so the player **walks the
  eroded surface**. The lookup is a **deterministic bilinear sample** of the baked grid
  at the query XZ, in a fixed op order using IEEE basic ops + the sanctioned
  `std::floor` exception (NO libm transcendentals) — it runs on the sim/lockstep path,
  so it is part of the determinism contract (SimDeterminismLint-clean) and is exercised
  by the heavy oracle + replay + lockstep re-bless. The grid is streamed/loaded with
  the region data, quantized, params-hashed (marker `0x04`) so it invalidates with the
  bump. Wave A includes the runtime grid-lookup path + its **perf budget** (a per-query
  bilinear fetch on the streamed grid — cache the resident region; bound the cost) and a
  **collision-walks-eroded-surface acceptance test** (an entity placed on a hydro-eroded
  slope rests on the eroded height, not the analytic one).
- Add `hydro_enabled` (+ params) to `TerrainGenParams`, default **OFF**; shipped
  presets opt in. Slot the offset application after shaping+island, before the river
  carve, mirrored in `ComputeShapedHeightGrid`. Mix into `ComputeTerrainParamsHash`
  (marker `0x04`).

### Re-bless surface (BLOCK #2 — enumerate the FULL set)
Terrain shape feeds many consumers; A2 gates must cover all, not just SHIELD-RT:
- **WaterSystem** terrain-height caches; **WaterfallDetect** + its `waterfall_visual_test`
  (steep-drop site detection drifts);
- **RuntimeScenarioHarness** vantage / grass / rim / water **selectors** (selection
  drifts, not just pixels) → **pin WorldVisualSweep vantages to fixed world coords** for
  the hydro preset, or accept+document re-selection;
- **MarchingCubes** meshing; **spawn** placement; **FarLodHorizon** re-derive.
- Add a gate that steep-rim/waterfall detectors **still find content** post-hydro.

### Edited-tile seam (MINOR fold)
Edited far tiles retain pre-hydro geometry while neighbors bake hydro → reintroduces a
shoreline-seam at edited/pristine borders. **Decide + gate:** force-regenerate edited
far tiles on the hydro bump (one-time invalidation), OR document the carve-out + a
`FarLodHorizon` check that no edited tile borders a hydro pristine tile in shipped
presets.

### Halo-independence acceptance test (MAJOR #19 — make it concrete, a ctest not prose)
Bake region R with halo widths **H1 (+N)** and **H2 (+2N)** (state N, e.g. N=16
cells); compare the cropped **interior** `height_q` arrays; require **byte-identical**
post-quantization equality across **archipelago + mountains** presets **AND
region-corner tiles** (worst case for cross-edge flow).

### A2 gates
- Worldgen atlas/snapshot deltas re-blessed; preset height-hash bumped (logged);
  slope-histogram normal-land floor holds; relief is hydro-shaped (drainage present);
  halo-independence ctest green; the full re-bless surface above green.
- `HeadlessServerTick` advances **in the #3 commit**; heavy + LREC1 + lockstep re-bless.
- **WorldVisualSweep as a terrain-scoped DELTA (MAJOR #18):** acceptance = no NEW
  objective flags in terrain-shape ROIs vs the blessed iter-5b baseline; the
  pre-existing **foliage/aurora OPEN BLOCKs are carried forward** (their discharge is a
  **Wave B** exit criterion, not A2's gate). Pin hydro-attributable flags before
  dispatch so attribution survives.

---

## Wave A.2 (summary — full spec authored after Wave A merges + Phase 0.3 lands)
SHIELD-RT substrate + heightfield far-field. Key constraints carried from the critique:
- **A3a:** substrate skeleton + review the A0 GPU tracer profile → **select** the
  primary tracer (heightfield-primary is PROVISIONAL until then). **Frozen API
  enumerated** (ray-setup, half-res alloc, temporal-resolve, upsample) with each taking
  **output-target + field-sampler as parameters** (not hardcoded G-buffer/MAX-mip),
  proven via a **Wave-B-consumer dry-run review** as an exit criterion (MAJOR #10).
  Froxel volume scoped OUT.
- **A3b:** heightfield max-mip march over `FarLodStore` tiles (derived structure, not
  persisted); insert in the G-buffer slot after live chunks (`GBufferPass.cpp:233`);
  add a `ShieldRT` GPU timer; near↔far dither blend.
  - **Parity gate gains a ground-truth leg (MAJOR #9):** mesh-vs-raymarch alone only
    proves *consumer agreement* (both read the same `height_q`); add a leg diffing the
    raymarcher hit-depth against analytic `ComputeShapedHeightSample` at the hit XZ
    within a quantization-aware tolerance — that is the load-bearing correctness
    assertion.
  - **Temporal-stability/ghosting gate (MAJOR #11):** fast-pan + seam-crossing +
    lightning-flash cases; reprojection uses the **render-interpolated** camera (not
    tick-snapped) + a seam disocclusion-reject policy. Distinct from static parity.
  - **Grazing-ray sizing (MAJOR #5):** size the ceiling against the grazing case with
    empty-space-skip + ray-cone mip implemented; add a per-ray max-step clamp + distance
    fade as a degrade lever.
- **GPUSDFSystem reuse (MAJOR #15):** ONLY the feature-flag gating *shape* is reused;
  the mip pyramid, clipmap, indirection table, sphere-trace, incremental update are
  **net-new**. **Forbid** reusing the `glClientWaitSync` synchronous readback + the
  placeholder sin/cos noise. The SDF brick clipmap stays a flag-dark non-goal unless
  the profile/3D-density demands it.

---

## §4. GPU budget — PROVISIONAL (gated on Phase 0.3; for Wave A.2)
Interim ≈7–8 ms frame. **Not ratifiable until Phase 0.3 gives the measured cost of the
existing 6× passes** (A.2 entry-gate). Add explicit line items for **substrate fixed
cost** (half-res upsample + temporal resolve — these persist regardless of march cost,
MAJOR #12) and for **max-mip build/rebuild + any clipmap update** (MAJOR #13). Verify
and pin the actual `FarLodStore` resident-region count the VRAM estimate depends on (if
uncapped in code, max-mip memory is unbounded). Clouds/grass figures are **Wave B**, to
be re-ratified against the measured A.2 split — not silently pre-cut here.

| Consumer | Allocated | Ceiling | Notes |
|---|---|---|---|
| SHIELD-RT march | PROVISIONAL | PROVISIONAL | size vs grazing case post-profile |
| Substrate fixed (upsample+temporal) | PROVISIONAL | — | own line item (persists) |
| Max-mip build/rebuild | PROVISIONAL | — | frame-spike cap on stream/hydro-dirty |
| Aetheric render tap | 0.1 ms | 0.2 ms | flat 2D LUT tap only |

## §5. Go/no-go gates must emit artifacts (MINOR fold)
Each go/no-go emits a committed decision artifact + a trivial validator that fails if
absent/unset: `aether-2.5d-decision.json` (aether 2.5D justification; fire deferred),
`shieldrt-tracer-profile.json` (measured GPU ms, both tracers, both view cases),
`hydro-single-point-contract.json` (**decision (a) confirmed**: authoritative baked-grid
bilinear lookup for all callers; records the resident-grid caching + per-query cost
bound). No honor-system preconditions.

## §6. Execution model
Agent teams (worktree-isolated Opus 4.8) author A1/A1.5/A2 in parallel but **land
serially** at the hash-gate chokepoint (#1→#2→#3). Workflow for the per-leg verify
pipeline + the find→adversarially-verify visual critique. Each hash commit is its own
commit with in-commit re-bless + the one-term-changed CI guard.

## §7. Verification (Wave A exit)
ctest green (+ AetherField golden/coupling, spline-hash known-u64, hydro halo-
independence); engine-frontier gates incl. AetherFieldDeterminism, SimDeterminismLint,
FarLodHorizon (re-derived), WorldVisualSweep (terrain-scoped delta; foliage/aurora
BLOCKs carried to Wave B), RenderHealth; HeadlessServerTick shows **three** new
canonical hashes across the three ordered commits (one term each, CI-guarded); forge
verify clean (modulo documented brace false-positives). Wave A.2 is a separate spec.

---

## Objection → disposition (critique `.forge/critique-wave-a-spec-20260615.md`)
- **BLOCK #1 erosion non-local / single-point** → §A2 Architecture: baked grid +
  single-point lookup contract; **owner chose (a)** — authoritative baked-grid bilinear
  lookup for ALL callers (player walks eroded surface), on the deterministic sim path;
  decision artifact `hydro-single-point-contract.json`.
- **BLOCK #2 re-bless surface** → §A2 Re-bless surface enumerated (WaterSystem,
  WaterfallDetect+test, RuntimeScenarioHarness selectors, MarchingCubes, spawn,
  FarLodHorizon); vantages pinned to world coords.
- **BLOCK #3 unmeasured "CONFIRMED" tracer** → §A0 GPU micro-profile spike is a hard
  gate before any A.2 substrate commit; heightfield-primary marked PROVISIONAL; A3
  split into A3a/A3b.
- **BLOCK #4 hidden third hash** → §A1.5 spline fold is its own commit (marker 0x05,
  `shaping_enabled`-gated, canonical encoding + known-u64 test), landed before #3; §1
  reframed to three ordered attributable changes.
- **Scope split (strong rec)** → §1: SHIELD-RT substrate → Wave A.2; froxel + brick
  clipmap → explicit non-goals.
- **MAJORs 5,8,9,10,11,12,13,14,15,16,17,18,19,20** → folded into A0/A1/A1.5/A2/A.2/§4
  as cited inline.
- **MINORs** → folded (ComposeWorldHash precedent, std::floor exception, iteration-count
  pin, decay-coeff pin, edited-tile seam, squash-proof CI guard, render-tap budget note,
  hydro naming, go/no-go artifacts, push/pop balance, fail-closed worktree harness).
