# Spec 015: Atmospheric Lighting Beauty + Colored-Glass Light Transmission

> Status: SPEC — execution-ready (formalized 2026-06-26 in place from the `/forge-brainstorm`
> draft; all four pillars carried to full depth with numbered FR/NFR/AC + open questions).
> **Re-sequenced 2026-06-26** per the engine-infrastructure critique
> (`.forge/critique-engine-infrastructure-framework-20260626-200421.md`, finding **F2**): Pillar A
> proceeds now (it fixes the lighting authority and only needs two narrow slices of spec 016), but
> **Pillars B (froxel volumetrics) and C-2 (OIT/refraction) are GATED behind the render seam** —
> spec 016 (render framework) + spec 014's RHI pilot — so they are built once as first-class
> graph/RHI clients instead of twice (GL now, re-port later). See **Phasing** and **Dependencies**.
> Subject: atmospheric lighting + colored glass for the zen-photography game where **light is the
> subject**. Owner decisions locked: **(1) atmosphere spine first** (couple light INTENSITY to the
> scattering model before building the visible beauty); **(2) colored glass goes all the way to full
> layered/OIT + refraction** (not just colored shadows). Grounded in the deferred GL pipeline + the
> existing Hillaire-2020 scattering LUT + the determinism contract, with the code anchors below
> verified against the current tree.

## The framing insight (why this spec exists)

A code audit this session established the load-bearing fact: **the atmosphere is a COLOR authority,
not an INTENSITY authority.** The Hillaire-2020 scattering LUTs (`SkyAtmosphereLut.h/.ipp`) decide
the *hue* of sun/sky/fog, but authored ramps in `RenderPipeline::update_time_of_day()` decide *how
bright* everything is:

- Sun color = `mix(horizonColor, noonColor, smoothstep(sun_up))*intensity` then ×LUT transmittance
  (hue coupled) — but **`m_sun.intensity` is a hardcoded `smoothstep`**, not LUT-derived.
- `m_skyAmbientColor` = hardcoded `mix(nightAmbient, dayAmbient, intensity)`, then hue-rotated ≤50%
  toward the sky-view irradiance integral — but the **magnitude is the authored vector**.
- Aerial fog opacity = hardcoded exponential, gated to **vanish at night**.
- The hemisphere irradiance integral already exists and **does carry full RGB magnitude in the LUT**
  (`SkyAtmosphereLut.ipp:549-550`, `m_sky_ambient` = `ambient_accum / ambient_weight * π`, declared
  `SkyAtmosphereLut.h:126`). Its magnitude is **not** discarded in the LUT — it is discarded
  *downstream* in `update_time_of_day`: the integral is pulled into `m_skyScatterAmbient`
  (`refresh_sky_view`, ~`:4459-4461`) and then only **hue-blended ≤50%** while the authored
  `m_skyAmbientColor` luminance is preserved (~`:4545-4552`). Pillar A's job is to consume that
  magnitude instead.

Every "magic light" moment (golden hour, blue hour, moonlit night, fog catching a sunbeam) is
currently a hand-tuned ramp fighting a physical model that already knows the answer. The night-moon
work landed this session (`kMoonKeyScale`/`nightAmbient` hand-dialed to a navigable mean-luma ~18)
is a symptom. This spec **couples intensity to the atmosphere**, then builds two beauty payloads on
that spine: **volumetric participating media** and **OIT colored glass**. They compose into the
signature shot — a colored god-ray through stained glass landing in fog.

**Determinism:** every pillar is render-only. The sim is render-agnostic; nothing here feeds
`world_hash` (legacy default stays `6f008a9f637c40b7`, `--smoke` run==replay). The gate is **FLIP
image parity / visual re-bless** (the `tools/flip_diff.py` harness from spec-adjacent work), not a
hash. Coupling intensity (Pillar A) WILL move every lit-frame visual golden — that re-bless is
expected and sequenced deliberately.

## Goals

- **G-1** — Drive sun / moon / sky-ambient *intensity* (magnitude) from the same scattering LUTs
  that already drive hue, so golden hour, blue hour, twilight, and moonlit night fall out of one
  physical model instead of hand-tuned ramps.
- **G-2** — Add a physically-sane exposure / eye-adaptation stage so the day↔night irradiance range
  maps to a viewable, "dim but navigable" image **without hand-floors**.
- **G-3** — Make light *visible in the air* (Pillar B): god-rays, light shafts, fog that catches a
  sunbeam — via froxel volumetric participating media.
- **G-4** — Make colored glass/crystals **tint light, cast colored light, and be see-through**
  (Pillar C): colored shadow maps → colored god-rays → full OIT + screen-space refraction.
- **G-5** — Compose all of the above into the signature shot: **a colored god-ray through stained
  glass landing in fog.**
- **G-6** — Preserve determinism end-to-end: every pillar is render-only; `world_hash` never moves.

## Non-Goals

- **NG-1** — The RHI / Vulkan / DX12 port and hardware ray tracing (that is **spec 014**). This spec
  authors Pillars B and C **compute-shaped** so they port cleanly behind 014, but does not depend on
  or perform that migration.
- **NG-2** — RT global-illumination cave lighting (also spec 014). The screen-space cave-AO scaffold
  (`LUMIN_CAVE_AO`, default-OFF) stays as-is; the real cave-GI fix is 014.
- **NG-3** — Game-content stained-glass / LuminCrystal assets. The engine exposes a **generic
  colored-transmissive material**; game content stays out of the engine seam.
- **NG-4** — Any sim / worldgen / streaming change. Nothing here feeds `world_hash`.
- **NG-5** — Terrain detail-texture / close-range relief work (handover #6; a separate terrain-art
  task).

## Pillars

### Pillar A — Couple light INTENSITY to the atmosphere (the spine) — DO FIRST

Derive sun/moon irradiance and sky-ambient *magnitude* from the same transmittance + sky-view LUTs
that already drive hue.

- **Sun irradiance** = LUT transmittance toward the sun × a top-of-atmosphere solar constant, instead
  of the authored `smoothstep` intensity. Golden hour reddens *and dims* from one model; civil/
  nautical twilight falls out automatically.
- **Sky ambient magnitude** = the hemisphere irradiance integral (`m_sky_ambient`) used at FULL
  magnitude (currently only its hue is taken), so blue hour and overcast read correctly without a
  ramp.
- **Moon** = the night key already added this session, but its *intensity* re-derived from a (dim,
  cool) lunar transmittance term so it scales with moon elevation/phase instead of `kMoonKeyScale`.
- **Exposure / eye-adaptation:** add a physically-sane auto-exposure (or fixed-stop night/day) so the
  far larger day-vs-night irradiance range maps to a viewable image — this is what lets night read
  as *dim but navigable* without hand-floors. Photography-appropriate: ties into the existing
  manual-exposure photo mechanic (shutter/ISO/EV).

Files: `RenderPipeline.cpp::update_time_of_day` (replace the intensity ramps), `SkyAtmosphereLut.*`
(expose sun_irradiance + hemisphere irradiance magnitude getters), `lighting_pass.frag` (consume the
new irradiances; the sun/moon/ambient terms already exist), a new/extended tonemap-exposure stage.
**Retires** the moon/night hand-tuning. Cheapest effort, deepest payoff.

### Pillar B — Volumetric participating media (god-rays / light shafts / fog that catches light)

A froxel volume (camera-frustum-aligned 3D texture, e.g. 160×90×64, half-res + temporal reproject
like the existing half-res cloud win): per froxel, sample the (now moon-keyed) shadow cascade + the
scattering LUT in-scatter, raymarch-integrate front-to-back, composite into the lit HDR target. This
makes light *visible in the air*: god-rays through cave mouths/canopy, dust in a sunbeam, glowing
dawn fog. It **extends/replaces** the existing analytic aerial pass (`volumetric_lighting.frag`) —
not a duplicate. Two quality tiers behind a knob (cheap screen-space radial vs. full froxel).

Files: new `passes/VolumetricPass.*` + froxel compute/raster shaders, reuse `ShadowPass` cascades +
`SkyAtmosphereLut` in-scatter; slot after lighting, before TAAU; budget via `--render-benchmark`.

### Pillar C — Full OIT colored glass + colored shadows + refraction

Colored glass / colored crystals that **tint and cast colored light** and are **see-through**:
- **Colored shadow maps:** the shadow pass stores a *tint* (RGB attenuation) where light passes
  through colored glass, so light landing beyond is colored — the stained-glass-cathedral look.
  Combined with Pillar B → **colored god-rays**. Highest beauty-per-effort; lead the pillar with it.
- **Order-independent transparency (OIT)** for stacked/layered colored panes (per owner decision):
  weighted-blended OIT or per-pixel linked-list, so overlapping glass composites correctly in any
  order. Each layer applies **Beer–Lambert absorption** (thickness × material color).
- **Screen-space refraction:** sample the lit scene behind the glass with a normal-driven offset so
  crystals refract what's behind them. A forward transmissive pass (deferred can't do this natively).

Files: `ShadowPass.*` (+ color attachment + glass occluder pass), new `passes/GlassPass.*` (forward
OIT + refraction), a glass material/flag in the G-buffer or a separate glass draw list, glass
shaders. Game content (LuminCrystal stained panes) stays out of the engine seam — engine exposes a
generic colored-transmissive material.

## Functional Requirements

IDs are grouped by pillar. Each is anchored to a verified file/line in the current tree (see
**Key files**). "Replace" means the listed hand-tuned ramp/constant is retired by the new behavior.

### Pillar A — intensity coupling + exposure

- **FR-A-001 — Sun irradiance from transmittance.** Sun *magnitude* shall be `LUT transmittance
  toward the sun × a top-of-atmosphere solar constant`, replacing the authored intensity
  `glm::smoothstep(-0.1f, 0.15f, sun_up_factor)` at `RenderPipeline.cpp:~4426`. Golden hour must
  redden **and** dim from the one model; civil/nautical twilight must fall out automatically (no
  separate twilight ramp).
- **FR-A-002 — Sky-ambient magnitude from the hemisphere integral.** Sky-ambient magnitude shall
  consume `SkyAtmosphereLut::m_sky_ambient` at **full magnitude** (currently only its hue is used),
  replacing the authored `dayAmbient=(0.1,0.15,0.2)*π` / `nightAmbient=(0.06,0.09,0.165)*π`
  constants and the ≤50% hue-only blend at `RenderPipeline.cpp:~4530-4552`.
- **FR-A-003 — Moon intensity from a lunar transmittance term.** Moon *intensity* shall be
  re-derived from a dim, cool lunar transmittance term that scales with moon elevation/phase,
  replacing the hand constant `kMoonKeyScale=1.3` (`lighting_pass.frag:462`) and the wrapped-Lambert
  floor `moonWrap = NdotL*0.6 + 0.25` (`:470`). The moon must remain a real cast-shadow key (the
  cascade re-key on `get_light_space_matrices` from this session stays).
- **FR-A-004 — Exposure / eye-adaptation stage.** A physically-sane exposure stage (auto-exposure
  or fixed day/night stops) shall map the far larger day↔night irradiance range to a viewable
  image, feeding the existing `u_exposure` uniform (today a single global constant `1.12` / `LUMIN_GRADE`,
  set in `passes/LightingPass.cpp:~149-170`; tonemap at `lighting_pass.frag:~650-681`). It must make
  night read **dim but navigable without hand-floors**. **Any GPU→CPU luminance metering for
  auto-exposure MUST use the frame-delayed async readback ring from spec 017 (`FR-A-*`) — a
  synchronous readback is forbidden** (critique F4; reading last frame's average luminance is
  acceptable and is the standard eye-adaptation pattern).
- **FR-A-005 — LUT magnitude getters.** `SkyAtmosphereLut` shall expose getters for sun irradiance
  and hemisphere-irradiance **magnitude** (e.g. `sun_irradiance()` / `sky_irradiance_magnitude()`)
  so the pipeline consumes physical magnitudes, not just `sun_transmittance()` hue
  (`SkyAtmosphereLut.h:105`) + the discarded-magnitude `sky_ambient()`.
- **FR-A-006 — Photo-mode reconciliation.** The new auto-exposure shall be **overridable by manual
  photo-mode exposure**; in photo mode, manual settings win. (Today photo mode is time-of-day *hold*
  only — `set_time_of_day_hold`, `RenderPipeline.h:613-615`; see Open Questions OQ-1.)

### Pillar C-1 — colored shadow maps (lead Pillar C with this)

- **FR-C1-001 — Glass material flag.** The engine shall expose a generic colored-transmissive
  material/flag (RGB absorption tint) on geometry that participates in shadow casting.
- **FR-C1-002 — Shadow-pass color attachment.** The shadow pass shall store an RGB *attenuation
  tint* where light crosses glass-flagged geometry (add a color attachment + glass-occluder pass to
  `passes/ShadowPass.*` / `get_light_space_matrices`).
- **FR-C1-003 — Tinted light consumption.** `lighting_pass.frag` shall multiply direct light by the
  sampled shadow tint so light landing beyond colored glass is colored (the stained-glass-cathedral
  look). The crystal/aether emissive at `lighting_pass.frag:~540-558` is the colored-light precedent.

### Pillar B — froxel volumetric participating media

- **FR-B-001 — Froxel volume.** A camera-frustum-aligned froxel volume (3D texture, target ~160×90×64,
  **half-res + temporal reproject** per the proven half-res-cloud pattern) shall be built each frame.
- **FR-B-002 — Per-froxel in-scatter.** Each froxel shall sample the (moon-keyed) shadow cascade +
  the `SkyAtmosphereLut` scattering in-scatter, then raymarch-integrate front-to-back.
- **FR-B-003 — Composite.** The integrated volume shall composite into the lit HDR target after
  lighting and before TAAU, making light visible in air (god-rays through cave mouths/canopy, dust
  in a sunbeam, glowing dawn fog).
- **FR-B-004 — Extend, not duplicate.** Pillar B shall **extend/replace** the existing analytic
  aerial pass `res/shaders/volumetric_lighting.frag`, not run a parallel duplicate.
- **FR-B-005 — Quality tiers.** Two quality tiers shall sit behind a knob: a cheap screen-space
  radial mode and the full froxel mode.
- **FR-B-006 — Colored shafts.** With C-1 landed, the volume shall pick up the shadow tint so god-rays
  through colored glass render as colored shafts (the C-1×B compose).

### Pillar C-2 — OIT colored glass + screen-space refraction (lands last)

- **FR-C2-001 — Forward transmissive pass.** A forward transmissive pass (`passes/GlassPass.*`) shall
  render colored glass/crystals (deferred cannot do transmission natively); glass uses a separate
  draw list or a G-buffer glass flag.
- **FR-C2-002 — Order-independent transparency.** Stacked/layered colored panes shall composite
  correctly in any order via OIT (weighted-blended OIT **or** per-pixel linked list — choice pending
  OQ-5).
- **FR-C2-003 — Beer–Lambert absorption.** Each glass layer shall apply Beer–Lambert absorption
  (thickness × material color).
- **FR-C2-004 — Screen-space refraction.** The pass shall sample the lit scene behind the glass with
  a normal-driven offset so crystals refract what's behind them.

## Non-Functional Requirements

- **NFR-001 — Determinism (hard gate).** Every pillar is render-only. `luminumbra_server_app --smoke`
  must stay `6f008a9f637c40b7`, run==replay, after each pillar. Nothing feeds `world_hash`.
- **NFR-002 — Performance budget.** `--render-benchmark` `frame_wall` / `gpu` p50 must stay within
  the RTX 5070 Ti target budget per pillar. Froxel volumetrics (B) must hold the forest/render budget
  via half-res + temporal reproject; the colored-shadow attachment (C-1) and the OIT pass (C-2) are
  each measured independently.
- **NFR-003 — Visual-parity gate.** Each pillar is gated by FLIP image parity (`tools/flip_diff.py`,
  `--threshold` + `--json` + heatmap) vs. the prior blessed golden, with **intentional re-bless**
  (`tools/golden_update.py`, refuses overwrite without `--force`). Pillar A's re-bless is a reviewed,
  expected global tone shift.
- **NFR-004 — Hot-reloadable shaders.** All new `.frag`/`.vert`/compute shaders must hot-reload at
  runtime (no rebuild for shader edits), consistent with the existing pipeline.
- **NFR-005 — RHI-portable authoring.** Pillars B and C shall be authored **compute-shaped** so they
  port behind the future RHI (spec 014) rather than fighting it.
- **NFR-006 — Photo-mechanic preservation.** The exposure work must not break the manual photo loop;
  manual exposure overrides auto in photo mode (paired with FR-A-006).

## Acceptance Criteria

Each is verifiable via the existing harness; capture recipe / measurable signal is named inline.
Capture flags: `--world-preset caverns --debug-goto {cave|doline|spawn} --debug-view
{albedo|normal|material|position} --timelapse-tod {0.0=noon|0.5=midnight} --timelapse-frames N
--timelapse-dir <d> --auto-create-world --auto-enter-world --no-audio`, then `tools/ppm_to_png.py`.

### Cross-cutting
- [ ] **AC-001** — `--smoke` stays `6f008a9f637c40b7` (run==replay) after **every** pillar.
- [ ] **AC-002** — `--render-benchmark` p50 within budget after every pillar (report the JSON).
- [ ] **AC-003** — Blessed goldens exist for the scene set {midnight, golden-hour, blue-hour,
  colored-god-ray-through-glass}; each re-bless reviewed via FLIP heatmap.

### Pillar A
- [ ] **AC-A-001** — Midnight (`--timelapse-tod 0.5`) reads "dim but navigable" (mean-luma in the
  navigable band, ≳ pitch-black 8) **with `kMoonKeyScale` / `nightAmbient` / wrap-floor removed**.
- [ ] **AC-A-002** — Golden hour both reddens **and** dims from the single LUT-coupled model (no
  authored twilight ramp); verify across a `--timelapse-tod` sweep.
- [ ] **AC-A-003** — `SkyAtmosphereLut` exposes magnitude getters (FR-A-005) and the pipeline
  consumes them (grep shows `dayAmbient`/`nightAmbient`/`smoothstep` intensity ramp removed from
  `update_time_of_day`).
- [ ] **AC-A-004** — Photo-mode manual exposure overrides the new auto-exposure (FR-A-006 / OQ-1).
- [ ] **AC-A-005** — Pillar A re-bless is intentional: FLIP heatmap reviewed, global tone shift
  confirmed expected, goldens updated with `--force`.

### Pillar C-1
- [ ] **AC-C1-001** — Light landing beyond a colored-glass occluder is visibly tinted that color
  (capture a glass-occluder scene; albedo-view vs lit-view luminance/hue compare).
- [ ] **AC-C1-002** — Non-glass geometry is pixel-identical to pre-C1 (FLIP ~0 off the tinted region).

### Pillar B
- [ ] **AC-B-001** — A god-ray / light shaft is visible in air through a cave mouth or canopy
  (capture `--debug-goto cave`); fog catches the sunbeam.
- [ ] **AC-B-002** — Froxel mode holds budget at half-res + temporal (`--render-benchmark`).
- [ ] **AC-B-003** — With C-1 present, the shaft through colored glass renders as a **colored** shaft
  (the signature compose, G-5).
- [ ] **AC-B-004** — The analytic aerial pass is extended, not duplicated (one volumetric path).

### Pillar C-2
- [ ] **AC-C2-001** — Stacked colored panes composite order-independently (rotate camera; no
  layer-order popping).
- [ ] **AC-C2-002** — Each layer shows Beer–Lambert absorption (thicker / stacked = more saturated).
- [ ] **AC-C2-003** — Crystals refract the scene behind them (screen-space offset visible).

## Dependencies (added 2026-06-26 per critique F2)

This spec no longer sits on the raw GL deferred pipeline alone. The render-architecture pillars are
sequenced against the **render seam**:

- **Pillar A** depends only on **two narrow slices of spec 016**: `FR-F-001` (split
  `update_time_of_day` so the intensity coupling lands on a clean surface) and `FR-E-001` (the async
  readback seam for exposure metering, from spec 017). It does **not** wait for the full framework.
- **Pillar C-1** (colored shadow maps) shall be built **through spec 016's pass/resource contract**,
  not as another raw-GL side channel — it doubles as a validation of the seam.
- **Pillars B and C-2** are **gated** behind spec 016 (render framework) **and** spec 014's RHI pilot:
  froxel volumes, temporal history, OIT, and refraction are designed once around explicit resources,
  history buffers, barriers, and backend command semantics — built as first-class graph/RHI clients,
  not legacy GL payload that must be re-ported during 014. ("Compute-shaped so it ports" is treated
  as *insufficient on its own* — the seam must exist first.)

## Phasing (re-sequenced 2026-06-26 per critique F2)

1. **Pillar A — atmosphere→intensity coupling + exposure.** Foundation; retires night hand-tuning;
   makes all times-of-day correct. Build after spec 016 `FR-F-001`+`FR-E-001` land; exposure metering
   via spec 017's async ring. Triggers the visual-golden re-bless. *(cheap-high-impact, do now)*
2. **Pillar C-1 — colored shadow maps.** Small, determinism-safe, instant "light is the subject"
   stained-glass moment — built **through the spec 016 pass/resource contract** (validates the seam).
   *(cheap-high-impact)*
3. **Pillar B — froxel volumetrics.** *Gated behind spec 016 + spec 014 RHI pilot.* The big beauty
   unlock; half-res + temporal first; authored as a graph/RHI client. Colored shadows from step 2
   immediately become colored god-rays.
4. **Pillar C-2 — OIT + screen-space refraction.** *Gated behind spec 016 + spec 014.* See-through
   layered colored glass/crystals; first-class transmissive pass on the new seam. Lands last.

## Blocking gates (per phase)

These are the per-phase gates; the **Acceptance Criteria** above are the per-pillar pass list they map to.

1. **Determinism (AC-001):** `--smoke == 6f008a9f637c40b7`, run==replay (render-only; must never move).
2. **Perf (AC-002, AC-B-002):** `--render-benchmark` `frame_wall`/`gpu` p50 within budget; froxel
   volumetrics half-res + temporal must hold the forest/render budget (RTX 5070 Ti @ target fps);
   colored-shadow attachment and OIT pass each measured.
3. **Visual parity / re-bless (AC-003, AC-A-005):** FLIP-diff (`tools/flip_diff.py`) vs the prior
   golden; Pillar A's re-bless is reviewed + intentional (heatmap shows global tone shift, expected).
   New goldens for
   golden-hour, blue-hour, midnight, a colored-god-ray scene.

## Risks / unknowns
- **Pillar A is a re-bless storm** — every lit frame shifts. `world_hash` is untouched (render-only),
  but all visual-critique baselines move; sequence + review deliberately (local-dev re-bless is fine).
- **Froxel volumetrics are the GPU sink** — needs half-res + temporal reproject (proven pattern via
  the half-res clouds); validate on a GPU-free + on-target pass.
- **Deferred ≠ transmission-native** — glass refraction/OIT is a forward bolt-on; colored *shadows*
  sidestep this, which is why they lead Pillar C.
- **RHI/Vulkan migration (spec 014) is coming** — froxel volumetrics + OIT are compute-shaped and
  should be authored so they port cleanly behind the future RHI, not fight it.
- **Auto-exposure interaction with the photo mechanic** — exposure coupling must not break the manual
  shutter/ISO/EV photo loop; reconcile (manual overrides auto in photo mode).

## Open Questions
- **OQ-1 (blocks FR-A-006 design)** — Does a manual photo-exposure mechanic exist, or must Pillar A
  introduce it? Today photo mode is **time-of-day hold only** (`set_time_of_day_hold`,
  `RenderPipeline.h:613-615`); there is **no shutter/ISO/EV control**. Decide: add a minimal manual
  EV control now, or have auto-exposure-with-photo-override land first and wire manual later.
- **OQ-2 (drives the AC-A re-bless target)** — What top-of-atmosphere solar constant and day/night
  exposure-stop calibration produce the intended look? This sets the absolute brightness the re-bless
  freezes; pick before mass re-blessing or the goldens move twice.
- **OQ-3** — Hemispheric ambient is keyed to `Normal.y` while world-up is effectively `-Y` in the
  lighting convention (`lighting_pass.frag:~578-601`; handover §7). Audit/correct during Pillar A so
  up-faces receive sky correctly under the new magnitudes.
- **OQ-4** — Auto-exposure metering mode (full-frame average vs. center-weighted vs. histogram) and
  adaptation speed (instant vs. temporal eye-adapt). Affects photographic feel and temporal stability.
- **OQ-5 (blocks FR-C2-002 method)** — OIT method: weighted-blended OIT vs. per-pixel linked list.
  Decide after the C-2 perf measurement (`--render-benchmark`); WBOIT is cheaper, linked-list is exact.
- **OQ-6** — Froxel resolution + temporal-reproject jitter vs. the **visual** gate: the sim
  `world_hash` is unaffected, but temporal jitter can perturb FLIP frame-to-frame. Decide the capture
  protocol (settle N frames / fixed jitter phase) so the gate is stable.

## Key files
- `src/luminumbra_client/rendering/SkyAtmosphereLut.{h,ipp}` — LUTs. `m_sky_ambient` (decl
  `:h:126`, computed `:ipp:549-550`) is full-RGB hemisphere irradiance; `sun_transmittance()`
  `:h:105`. Add sun + hemisphere irradiance **magnitude** getters (FR-A-005).
- `src/luminumbra_client/rendering/RenderPipeline.cpp::update_time_of_day` (lines **4375-4578**):
  sun intensity smoothstep ~`:4426`, sun color ~`:4446`, normalized transmittance ~`:4464-4476`,
  moon dir ~`:4506-4523`, ambient constants ~`:4530-4539`, scatter-hue ≤50% blend ~`:4545-4552`,
  `refresh_sky_view` → `m_skyScatterAmbient` ~`:4459-4461`. Replace intensity ramps with LUT-derived
  irradiance (FR-A-001/002/003).
- `res/shaders/lighting_pass.frag` — `SUN_IRRADIANCE_SCALE=PI` `:205`; sun radiance ~`:411-424`;
  moon block ~`:426-486` (`kMoonColor=(0.40,0.52,0.92)`, `kMoonKeyScale=1.3` `:462`, wrap
  `NdotL*0.6+0.25` `:470`); hemispheric ambient ~`:578-601`; tonemap/`u_exposure` ~`:650-681`;
  crystal/aether emissive ~`:540-558` (colored-light precedent).
- `src/luminumbra_client/rendering/passes/LightingPass.cpp` ~`:149-170` — `u_exposure`/grade setup
  (`LUMIN_GRADE`); the exposure stage (FR-A-004) plugs in here / a new tonemap-exposure stage.
- `res/shaders/volumetric_lighting.frag` — existing analytic aerial; extend to froxel (Pillar B).
- `src/luminumbra_client/rendering/passes/ShadowPass.*` + `get_light_space_matrices` — colored shadow
  attachment (Pillar C-1).
- new `passes/VolumetricPass.*`, `passes/GlassPass.*` — Pillars B and C-2.
- `tools/flip_diff.py`, `tools/golden_update.py`, `tools/ppm_to_png.py` — the visual-parity gate +
  capture conversion for every phase; goldens under `build/debug/showcase/`. Capture/debug flags
  parsed in `src/luminumbra_client/core/RuntimeScenarioHarness.cpp` + `main_client.cpp`
  (`debug/DebugCamera.h` for `--debug-goto` locators).

## Verification (end-to-end)
1. Build both trees (prepend `C:\msys64\ucrt64\bin`); `--smoke` byte-identical each phase.
2. `--render-benchmark` within budget per phase (half-res froxel; OIT; colored-shadow attachment).
3. Capture goldens at midnight / golden hour / blue hour / a colored-god-ray-through-glass scene;
   FLIP-diff + re-bless intentionally.
4. Photo-mode exposure still works (manual overrides the new auto-exposure).
5. Send MP4/PNG showcases at each phase boundary (light is the subject — show it).
