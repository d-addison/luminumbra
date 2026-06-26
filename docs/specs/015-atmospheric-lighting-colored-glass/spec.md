# Spec 015: Atmospheric Lighting Beauty + Colored-Glass Light Transmission

> Status: DRAFT (created 2026-06-26). Output of `/forge-brainstorm` on atmospheric lighting +
> colored glass for the zen-photography game where **light is the subject**. Owner decisions locked
> this turn: **(1) atmosphere spine first** (couple light INTENSITY to the scattering model before
> building the visible beauty); **(2) colored glass goes all the way to full layered/OIT +
> refraction** (not just colored shadows). Grounded in the deferred GL pipeline + the existing
> Hillaire-2020 scattering LUT + the determinism contract.

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
- The hemisphere irradiance integral already exists (`SkyAtmosphereLut.ipp:~550`,
  `m_sky_ambient`) but its **magnitude is discarded** — only its hue is used.

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

## Phasing (sequenced by photographic-payoff-per-effort, owner: spine first)

1. **Pillar A — atmosphere→intensity coupling + exposure.** Foundation; retires night hand-tuning;
   makes all times-of-day correct. Triggers the visual-golden re-bless. *(cheap-high-impact)*
2. **Pillar C-1 — colored shadow maps.** Small, determinism-safe, instant "light is the subject"
   stained-glass moment. *(cheap-high-impact)*
3. **Pillar B — froxel volumetrics.** The big beauty unlock; half-res + temporal first. Colored
   shadows from step 2 immediately become colored god-rays.
4. **Pillar C-2 — OIT + screen-space refraction.** See-through layered colored glass/crystals. Most
   awkward in deferred; lands last.

## Blocking gates (per phase)

1. **Determinism:** `--smoke == 6f008a9f637c40b7`, run==replay (render-only; must never move).
2. **Perf:** `--render-benchmark` `frame_wall`/`gpu` p50 within budget; froxel volumetrics half-res +
   temporal must hold the forest/render budget (RTX 5070 Ti @ target fps); colored-shadow attachment
   and OIT pass each measured.
3. **Visual parity / re-bless:** FLIP-diff (`tools/flip_diff.py`) vs the prior golden; Pillar A's
   re-bless is reviewed + intentional (heatmap shows global tone shift, expected). New goldens for
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

## Key files
- `src/luminumbra_client/rendering/SkyAtmosphereLut.{h,ipp}` — LUTs; expose sun + hemisphere
  irradiance MAGNITUDE getters (hue already used).
- `src/luminumbra_client/rendering/RenderPipeline.cpp::update_time_of_day` (~4446 sun color, ~4464
  transmittance, ~4506 moon, ~4531 ambient, ~4545 scatter-hue) — replace intensity ramps with
  LUT-derived irradiance.
- `res/shaders/lighting_pass.frag` — sun/moon/ambient terms already present; consume new irradiances;
  crystal/aether emissive (~540-558) is the colored-light precedent.
- `res/shaders/volumetric_lighting.frag` — existing analytic aerial; extend to froxel (Pillar B).
- `src/luminumbra_client/rendering/passes/ShadowPass.*` + `get_light_space_matrices` — colored shadow
  attachment (Pillar C-1).
- new `passes/VolumetricPass.*`, `passes/GlassPass.*` — Pillars B and C-2.
- `tools/flip_diff.py` — the visual-parity gate for every phase.

## Verification (end-to-end)
1. Build both trees (prepend `C:\msys64\ucrt64\bin`); `--smoke` byte-identical each phase.
2. `--render-benchmark` within budget per phase (half-res froxel; OIT; colored-shadow attachment).
3. Capture goldens at midnight / golden hour / blue hour / a colored-god-ray-through-glass scene;
   FLIP-diff + re-bless intentionally.
4. Photo-mode exposure still works (manual overrides the new auto-exposure).
5. Send MP4/PNG showcases at each phase boundary (light is the subject — show it).
