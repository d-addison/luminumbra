# Research Brief — Volumetric Clouds Tier-2 (Nubis raymarch) + Froxel Fog

- **Slug:** `volumetric-clouds`
- **Wave:** iteration-6 Wave 0 (research-gate) → implemented in Wave B (RENDER), reusing Wave-A raymarch infra
- **Side of the line:** **RENDER-ONLY. No `world_hash` impact.** Wind advection is a *read* of the existing sim-side `FieldGrid` wind field; clouds write no sim state.
- **Closes debt:** flat/milky storm-cloud-depth (the tier-1 2.5D dome layer from `fa1e08b` / C3 has no internal lighting or depth).

---

## Recommended approach

Replace the tier-1 2.5D coverage dome with a **raymarched volumetric cloud layer following Nubis (Schneider/Vos, HZD SIGGRAPH 2015; Nubis-Evolved SIGGRAPH 2017)**, and **build the froxel/raymarch infrastructure ONCE, shared with SHIELD-RT (Wave A)**. Concretely:

### 1. Density model (Nubis)
- **Low-frequency base shape:** one `128³` RGBA 3D texture. Channel R = Perlin-Worley; G/B/A = Worley noise at increasing frequencies. This builds the billowy base cloud shape. [Schneider 2015; Meteoros; Sachan]
- **High-frequency detail/erosion:** one `32³` RGB 3D texture, 3 channels of Worley at increasing frequencies, used to *erode* the base-shape edges into wispy detail. [Schneider 2015; Meteoros]
- **Weather/coverage 2D texture:** controls coverage, cloud-type (stratus→cumulus→cumulonimbus), and precipitation per-column. In luminumbra this is **driven by the existing weather system** (storm-cell field, `weather_system.frag`, `f36fa3a`) rather than a static authored texture — the storm cells set local coverage/type so storm clouds get cumulonimbus verticality.
- **Height-gradient remap** by cloud type (stratus low/flat, cumulus mid, cumulonimbus tall) shapes density vertically inside the cloud slab. [Schneider 2015]

### 2. Raymarch + lighting
- **Primary ray:** ~**128 steps** through the cloud slab, with early-out on accumulated transmittance and an empty-space skip using the low-freq texture as a cheap occupancy test. [Sachan; Schneider 2015]
- **Light march:** ~**6 cone-sampled steps** toward the sun per primary sample (cone sampling softens self-shadowing). [Sachan; Schneider 2015]
- **Transmittance:** **Beer-Lambert** `T = exp(-σ·d)`; Nubis uses a **dual-lobe Beer** ("Beer-Powder") to push light deeper and recover the dark-edge/bright-silhouette look. [Maxime Heckel; Sachan; Schneider 2015]
- **Powder effect:** the `1 - exp(-2σd)` dark-edge term for the sugar-powder look on lit cloud edges. [Schneider 2015]
- **Phase:** **dual Henyey-Greenstein** (forward + back lobe blend) to approximate Mie scattering — strong forward scatter gives the silver-lining around the sun. `HG(g,μ) = (1-g²) / (4π·(1+g²-2gμ)^1.5)`. [Maxime Heckel; Sachan]

### 3. Temporal amortization (the cost lever — load-bearing)
- **Render 1/16 of pixels per frame** (4×4 reprojection grid) and reproject the other 15/16 from the previous frame, jittered per the sub-pixel pattern. This is the single biggest cost reducer and is how HZD hit <2 ms on PS4. [Sachan: "Ray marching for 1/16th of the pixels … other 15 of 16 filled by reprojection"; Schneider 2015 reports <2 ms PS4]
- Combine with **blue-noise dithered ray-start offset** (`fract(blueNoise + frame%32 …)`) to break up banding, and a history-rejection clamp on reprojection failures (disocclusion at cloud silhouettes / fast camera turns). [Maxime Heckel]

### 4. Render at half-resolution + bilateral upsample
- Raymarch into a half-res (or quarter-res) RGBA16F target, composite over the scene using scene depth, then bilateral-upsample. Clouds are low-frequency so the resolution loss is nearly invisible and it quarters the per-pixel raymarch count before temporal amortization even applies.

### 5. Froxel fog as the SHARED participating-media path (Hillaire 2015)
Add a **froxel (frustum-aligned voxel) volumetric integration** stage following Frostbite's "Physically Based and Unified Volumetric Rendering":
- A `~160×90×64` froxel volume (frustum-aligned, **exponential depth slicing** near→far) stores per-froxel **in-scattered RGB + extinction (RGBA)**. [Hillaire 2015; Wronski; Godot/Eevee derivations]
- Three compute passes: **(a) material/density estimation** (write extinction+scattering coeffs per froxel from fog + cloud-shadow + future Aetheric emission), **(b) light scattering** (evaluate sun + shadow per froxel, apply phase), **(c) final integration** — a single front-to-back scan along the depth axis accumulating scattering + transmittance. [Hillaire 2015]
- Temporal reprojection between froxel frames removes the noise from low sample counts. [Hillaire 2015; Godot]
- This **replaces the current analytic-only aerial-perspective pass** (`m_aerial_shader`, Hillaire 2020 analytic, no froxel — see `RenderPipeline.h` `Aerial` pass) with a unified froxel path that does aerial perspective AND ground fog AND god-rays AND cloud-shadow volume in one buffer. The clouds composite into the same depth-resolved buffer.

### SHARED RAYMARCH/FROXEL INFRA — build ONCE (the key architecture point)
Wave-A SHIELD-RT (SDF sphere-trace far-field) and Wave-B clouds/fog are different *march kernels* but share a large reusable substrate. Factor these into a common `render/volumetric/` module both consume:

| Reusable component | Used by SHIELD-RT (Wave A) | Used by clouds/fog (Wave B) |
|---|---|---|
| **Ray setup from depth/camera** (reconstruct world ray per pixel, near/far from G-buffer depth) | yes (far-field rays) | yes (cloud slab entry/exit, fog along view ray) |
| **Half-res render target + bilateral depth-aware upsample** | yes | yes |
| **Temporal reprojection + history buffer + disocclusion reject** | yes | yes (the 1/16 amortization rides on this) |
| **Blue-noise / interleaved-gradient jitter LUT** | yes | yes |
| **Froxel volume (frustum voxel) alloc + exponential depth mapping + RGBA16F storage** | optional (far-field aerial) | yes (fog + cloud composite) |
| **3D-texture sampling + min-filtered conservative mips** | yes (SDF mips — already a spike success criterion) | yes (Perlin-Worley / Worley mips) |
| **Per-pass GL timestamp timers** (already exist) | yes | yes |
| Kernel-specific (NOT shared) | SDF sphere-trace / heightfield march | density-noise sampling + Beer-Powder-HG lighting |

**Build order:** Wave A lands the ray-setup + half-res + temporal-reproject + froxel substrate as its SHIELD-RT delivery; Wave B clouds/fog plug a second march kernel into that substrate. This is exactly the handoff's stated intent ("lays the froxel/raymarch infra that Wave-C volumetric clouds reuse"). Freeze the substrate's interfaces alongside the `FieldGrid` freeze.

---

## Alternatives considered (+ why rejected)

1. **Keep tier-1 2.5D coverage dome, add fake parallax/bump-lit shading.** Rejected: the debt is *depth* — a 2.5D sheet has no internal scattering, so storms read flat/milky no matter the shading. Owner visual-debt item explicitly calls for tier-2.
2. **Full per-frame full-res raymarch (no temporal, no half-res).** Rejected: 128 primary × 6 light steps per full-res pixel is ~10–15 ms class on our shared budget — blows the contended GPU budget that SHIELD-RT + GPU grass also draw from. Temporal 1/16 + half-res is what makes it affordable (HZD <2 ms PS4 baseline). [Schneider 2015; Sachan]
3. **Separate bespoke raymarcher for clouds, independent of SHIELD-RT.** Rejected by the handoff's explicit "build it ONCE" mandate — duplicates ray-setup, temporal reprojection, jitter, and upsample, doubling maintenance and the GPU-state churn, and fragments the shared budget accounting.
4. **Mesh-shader / analytic single-scatter clouds (no march).** Rejected: cannot produce cumulonimbus self-shadowing/silver-lining; defeats the purpose.
5. **Froxel-only clouds (march clouds inside the fog froxel volume).** Rejected as the primary path: froxel depth resolution (64 slices) is too coarse for the high-frequency cloud silhouette; clouds need their own dedicated march. Froxels handle the *low-frequency* media (fog, aerial, cloud-shadow shafts); clouds composite into them.

---

## Perf budget (GPU ms + VRAM, concrete)

Targets are **1080p, with temporal 1/16 amortization + half-res raymarch** (the regime that makes this affordable; numbers degrade ~linearly without amortization).

**GPU time (amortized, per frame):**
- Volumetric clouds raymarch (half-res, 1/16 reprojected): **~1.2–1.8 ms** target. Reference: HZD <2 ms PS4 full-screen; Meteoros <3 ms full-HD on a mobile GTX 1070 with simplified lighting and 1/16 reprojection. Our half-res + better-than-1070 desktop GPU should sit at the low end. [Schneider 2015; Sachan]
- Froxel fog (160×90×64, 3 compute passes + reproject): **~0.4–0.7 ms**. Reference: Frostbite reported froxel volumetrics in the sub-millisecond-to-~1 ms class at console resolutions. [Hillaire 2015 — **flag: I could not extract the exact ms figure from the slides (PDF was image-only); treat 0.4–0.7 ms as an engineering estimate to be measured against the existing per-pass GL timers**]
- **Combined tier-2 target: ~1.6–2.5 ms GPU.** Note this **partly replaces** the existing analytic aerial pass (`aerial_gpu_ms`), so net add is less than the gross.

**VRAM:**
- Low-freq Perlin-Worley `128³` RGBA8 = 8 MB (BC-compressible ~2 MB).
- High-freq detail `32³` RGB8 = ~0.1 MB.
- Cloud half-res RGBA16F target (960×540) = ~4 MB; + history buffer for reprojection = ~4 MB.
- Froxel volume RGBA16F 160×90×64 = ~7.4 MB; + history copy = ~7.4 MB.
- Blue-noise LUT negligible.
- **Total: ~30 MB uncompressed, ~24 MB with BC on the noise textures.** Well inside headroom; note this is *separate* from FarLodStore's 64 MB residency budget.

**Shared-budget split (Wave A must ratify):** of the contended GPU-ms pool across {SHIELD-RT, GPU grass, volumetric clouds+fog, Aetheric render}, this topic claims **~2.0–2.5 ms**. Wave A should allocate the total explicitly; the shared raymarch substrate means SHIELD-RT and clouds amortize their temporal/upsample cost on the *same* history buffers, so the marginal cost of adding clouds after SHIELD-RT is lower than a standalone build.

---

## Determinism implications (sim vs render; `world_hash` impact)

- **RENDER-ONLY. Zero `world_hash` impact.** No deliberate bump. The cloud/fog system runs entirely in `luminumbra_client` rendering passes (RGBA16F floats, GL compute, temporal history) — none of it is in a sim TU, so SimDeterminismLint, DeterministicMath, and the FP-pin contract do not apply.
- **Wind advection is a READ, not a write.** Clouds scroll/advect by *sampling* the existing wind `FieldGrid` (the sim-authored field that already bumped the hash in `0eac…` / A2). Reading it for render does not affect the hash. Today's tier-1 already does this via `advance_cloud_phase` + `wind_xz` (see `RenderPipeline.cpp` ~L3569–3595, `kCloudDriftMetersPerSec = 60`); tier-2 inherits the same read path.
- **Caveat — keep advection deterministic-of-render-state but NOT sim-coupled:** the cloud scroll must derive from tick-derived deltaTime (as tier-1 does) so two clients at the same tick see the same sky, but this is a *visual* consistency nicety, not a determinism requirement. Do NOT let any cloud/froxel computation feed back into sim (e.g. cloud-shadow → temperature → ecology) without routing that through the sim side and bumping the hash deliberately.
- **Gate:** verify under `WorldVisualSweep` (the storm-depth cells) and confirm `HeadlessServerTick` world_hash is **unchanged** (`d950a6afc12a5cdc`) after the cloud work lands — proving it's render-only.

---

## Integration notes (luminumbra files/systems touched)

- **`src/luminumbra_client/rendering/RenderPipeline.{h,cpp}`** — pass ordering. Tier-2 clouds + froxel fog land **after skybox, replacing/extending the analytic `Aerial` pass** (`RenderPipeline.h` enum `Aerial`, `m_aerial_shader`, `execute_aerial_pass`, `aerial_gpu_ms`). The froxel composite must precede foliage/particle so they're fogged correctly. Add `cloud_gpu_ms` / `froxel_gpu_ms` to `m_last_render_pass_stats` alongside the existing `aerial_gpu_ms` / `cloud_shadow_gpu_ms`.
- **`CloudRenderState` + `set_cloud_state` + `advance_cloud_phase`** (`RenderPipeline.cpp` ~L3548–3595) — extend the existing tier-1 state struct (coverage_amount, biome_variation, scroll_offset, plane_height, sun_travel_dir) with cloud-type/precip and the 3D-noise bindings; keep the wind-driven `scroll_offset` accumulation.
- **`src/luminumbra_client/rendering/passes/SkyboxPass.{cpp}`** — tier-1 dome clouds live here; tier-2 either subsumes this or runs as a new dedicated volumetric pass that the skybox renders behind.
- **`src/luminumbra_client/rendering/passes/LightingPass.cpp`** (~L1399–1405) — the existing **cloud cast-shadow sample** (`cloud_shadow_gpu_ms`, CloudShadow gate). Tier-2's volumetric density can drive a more accurate shadow; the froxel volume can carry cloud-shadow shafts (god-rays) into the scene.
- **`weather_system.frag` + storm-cell weather state** (`f36fa3a`) — drives coverage / cloud-type per column so storm cells become cumulonimbus. This is the determinism-safe coupling: weather is already sim-authored (bumped in `0857…`); clouds *read* it.
- **`src/luminumbra_common/fields/FieldGrid.h`** — the wind field consumed for advection. **API is being FROZEN for iter-6** — clouds are a read-only consumer of `FieldGrid<WindCell>`; rely only on the frozen surface (`at`, `index`, origin, `cell_size_m`). No new requirement on FieldGrid.
- **NEW shared module `src/luminumbra_client/rendering/volumetric/`** — the reusable ray-setup / half-res+upsample / temporal-reproject / froxel substrate co-owned with Wave-A SHIELD-RT. **Freeze its interface during Wave A** so Wave B plugs in cleanly. SHIELD-RT lives near `GPUSDFSystem` (currently `kEnableExperimentalGpuSdfIntegration=false`) — keep the new volumetric substrate behind a feature flag until its gate is green, per the no-deferral rule.
- **Materials LUT emissive path** (row 2, `kEmissiveLutScale=8.0`) — Aetheric emission can be injected into the froxel scattering pass later (Wave A Aetheric render). Design the froxel material pass to accept an additive emissive-scattering source so Aetheric glow becomes volumetric for free.
- **Per-pass GL timestamp timers** (existing) — reuse for the new passes; feed the shared-budget split.
- **Gates:** `WorldVisualSweep` storm/cloud-depth cells (cloud-structure variance flag), `CloudShadow` (re-bless if shadow sharpens), `SkyboxVisual` / `TimeOfDaySweep` (must stay green — silver-lining at dawn/dusk, dark night clouds), `HeadlessServerTick` (hash unchanged), `PerfRegression` (GPU-ms budget).

---

## Open risks

1. **Shared-budget contention is real (handoff critique #2).** Clouds + fog + SHIELD-RT + GPU grass all draw the same GPU-ms pool. If SHIELD-RT overruns, clouds get squeezed. **Mitigation:** Wave A ratifies the split BEFORE fan-out; clouds have a coverage/step-count quality knob to trade ms for fidelity.
2. **Frostbite froxel ms figure unverified** — slides were image-only PDF; the 0.4–0.7 ms is an estimate. Measure against the real GL timers early; if froxel fog is too costly, ship clouds-only first and fold froxel fog in later (it's the *shared* substrate so it's reused regardless).
3. **Temporal reprojection ghosting** at cloud silhouettes during fast camera turns / lightning flashes (the storm scenario specifically has bright transients). Needs robust history rejection; the existing storm-visual DR already fought camera-float artifacts, so the visual-critique pipeline will catch regressions.
4. **Half-res + reproject vs the visual-critique objective flags** — cloud-structure variance and luminance bands are scored by `tools/visual_critique.py`. Low-res or over-smoothed clouds could trip the "milky/flat" flag the same way tier-1 did. Tune so the objective cloud-structure-variance metric clears.
5. **Substrate co-ownership coordination** — two waves (A render-SDF, B clouds) editing one `volumetric/` module risks merge churn. **Mitigation:** Wave A owns and freezes the substrate; Wave B is a pure consumer adding a kernel. Worktree base-check + no `--force` removal per the carried hazard.
6. **HZD exact noise-channel layout** is reconstructed from the 2015 slides via secondary implementations (Meteoros, arxiv optimisations paper) because the primary PDFs would not text-extract here. The `128³`/`32³` resolutions and 128/6 step counts are corroborated across two independent implementations, but exact channel packing should be confirmed against the slide images during implementation.
7. **Replacing the analytic aerial pass** (Hillaire 2020) with a froxel path is a visual change to the sky/far-terrain tint that already had three scattering bugs fixed in 5a (`413694a`). Re-verify `SkyboxVisual` + `TimeOfDaySweep` warm-dusk bands don't regress.

---

## Citations

| Source | URL | Specific finding |
|---|---|---|
| Schneider & Vos, "The Real-time Volumetric Cloudscapes of Horizon Zero Dawn," SIGGRAPH 2015 Advances in Real-Time Rendering | https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf | Primary source: Perlin-Worley low-freq + Worley high-freq erosion, Beer-Powder, dual-HG phase, weather/coverage + height gradient, **<2 ms on PS4**. (PDF is image-based; parameters cross-confirmed via implementations below.) |
| Schneider, "Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima Engine," SIGGRAPH 2017 | https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf | Successor / authoring + lighting-model refinements (Nubis-Evolved). |
| Sachan, "Meteoros" (Vulkan HZD/Decima cloud impl) | https://github.com/AmanSachan1/Meteoros | **Corroborates exact params:** low-freq `128³` RGBA Perlin-Worley+Worley; high-freq `32³` RGB Worley; **1/16 pixels raymarched + 15/16 reprojected (5.97× speedup)**; dual Beer-Lambert; dual Henyey-Greenstein; `<3 ms` full-HD on GTX 1070. |
| "Optimisations for Real-Time Volumetric Cloudscapes" (arxiv 1609.05344), extends HZD | https://arxiv.org/pdf/1609.05344 | HZD optimisation follow-up; ~128 primary steps + ~6 light steps per pixel (PDF image-based; step counts cross-confirmed via search snippet + Meteoros). |
| Maxime Heckel, "Real-time dreamy Cloudscapes with Volumetric Raymarching" | https://blog.maximeheckel.com/posts/real-time-cloudscapes-with-volumetric-raymarching/ | Concrete shader: Beer `exp(-dist*absorption)`, HG formula `(1-g²)/(4π(1+g²-2gμ)^1.5)`, blue-noise temporal dither `fract(blueNoise + frame%32 …)`, bicubic upscale. |
| Hillaire, "Physically Based and Unified Volumetric Rendering in Frostbite," SIGGRAPH 2015 | https://www.ea.com/frostbite/news/physically-based-unified-volumetric-rendering-in-frostbite | Froxel (frustum voxel) approach: cascaded extinction volume, per-froxel scattering+extinction, scattering pass + final integration, temporal reprojection, volumetric shadow. (Exact ms not extractable from page.) |
| GameDev.net / Godot docs (Frostbite-derived froxel fog) | https://www.gamedev.net/forums/topic/683409-question-about-froxel-volume-rendering-tech-used-by-frostbite/ ; https://github.com/godotengine/godot-docs/blob/master/tutorials/3d/volumetric_fog.rst | Confirms froxel = frustum-aligned voxels (Hillaire & Wronski); per-froxel radiance accumulated along view ray; temporal reprojection blends current+previous frame; depth slices configurable. |
| luminumbra `RenderPipeline.cpp` / `RenderPipeline.h` | (repo) | Existing tier-1 2.5D cloud state, `advance_cloud_phase` wind-advect (`kCloudDriftMetersPerSec=60`), cloud cast-shadow in LightingPass (`cloud_shadow_gpu_ms`), analytic `Aerial` pass (`aerial_gpu_ms`, no froxel) — the integration seam. |
| luminumbra `fields/FieldGrid.h` | (repo) | Wind field container (read-only consumer for cloud advection); API frozen for iter-6. |
| luminumbra `engine-frontier/handoff.md` (iter-6 program) | (repo) | "build it ONCE": clouds reuse SHIELD-RT froxel/raymarch infra; shared+contended GPU budget; RENDER-only; Wave-B placement. |
