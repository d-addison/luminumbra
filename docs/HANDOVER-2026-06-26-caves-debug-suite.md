# Handover — 2026-06-26: Photography depth, living world, caves, AAA debug suite

Session owner: David. This handover covers a large multi-feature session and, importantly,
a **catalog of cave/underground rendering problems** surfaced by the new debug suite, plus
the prioritized next steps.

---

## 1. What landed (all committed on `feat/polyglot-audit-roadmap`, determinism-clean)

| Area | Commits (newest→) | State |
|---|---|---|
| **Spec 012 — photography depth** | `6add62d3`, `9b5ddb02` | Light→time-of-day, scoring rebalance, manual exposure (shutter/ISO + EV HUD), ObservationMetadata, `BehavioralMatch` objectives, rest poses. Verified (898 tests). |
| **Spec 011 — living world** | (earlier) `08eb4fad`, `0563bb42`, `8a1719af`, `63221c0c` | Rest poses, forager/nest markers (anchor-only), per-action audio (feed/drink/colony), pheromone ground-decal (compute/SDF), UI/HUD scale setting. |
| **Spec 013 — POI / caves** | `0dc0e780`, `3182fcda`, `73180673`, `c98e0f49`, `d5f52133`, `fc018900`, hero-crystal | Photo-mode TOD+weather scrub; `caverns` preset; **MC-1.18 noise-router caves** (cheese + spaghetti + Worley rooms); doline locator; lumin crystals; **far-camera streaming fix**. |
| **AAA debug suite (ultracode)** | `03619ac1`, `9db10a4c`, `35936fca` | Debug G-buffer view modes (`--debug-view` / F6), auto frame-health verdict (`--frame-scan`), GL debug-output callback (`LUMIN_GL_DEBUG`), `--debug-goto cave\|doline\|spawn` feature locator. **Verified + it found a real lighting bug.** |

**Determinism:** every change is render-only OR worldgen-gated. Legacy default preset is byte-identical
(`world_hash 6f008a9f637c40b7`). New `caverns` preset is run==replay (`43930c7ee05ef46f` with Worley on).
Verify: `luminumbra_server_app --smoke` and `--smoke --preset caverns`.

**Research briefs (cited, adversarially verified) produced this session:**
- POI/landmark design + procedural architecture (WFC/hybrid) → `wf_ae777895`.
- Minecraft-1.18 cave algorithms (cheese/spaghetti/noodle + Worley) → `wf_c5cdd173` (drove the noise-router caves).
- Engine-wide rendering observability → `wf_3ee0f93e` (drove the debug suite).
- RHI / Vulkan+DX12 migration landscape → `whmuyyyvg` (**recommends Diligent Engine**; see §4).

---

## 2. CAVE / UNDERGROUND RENDERING PROBLEMS (the priority)

Caught with the new debug suite (`--debug-goto cave` to frame an enclosed cave; `--debug-view
albedo/normal` to see geometry vs lighting). Ordered by severity.

1. **Point lights do not illuminate caves [CRITICAL, root-caused].**
   A lumin-crystal point light at **intensity 50, 14 m away, in range, correctly uploaded**
   (count ≤ 32 after the cull fix, world-space math correct) casts **zero visible light**.
   Root cause in `res/shaders/lighting_pass.frag`: `color = ambient*ao + Lo` (line ~497-498) —
   `Lo` (point contribution via `CalculateLightContribution`) returns ≈0 at the visible cave
   walls, and the ambient term floods the cave. Repro: `--debug-goto cave --debug-view albedo`
   shows full cave geometry while the lit frame is black. Images: `litcave_f3`, `litcave2_f3`.
   → **The whole deferred point-light path was dormant before crystals; this is its first real
   use and it's broken for environmental fill.** Fix needs investigation: BRDF facing, the
   ambient/`ao` flooding, and whether point lights need a different curve underground.

2. **Flat ambient + aerial fog flood caves [HIGH].** Cave interiors fill with a uniform
   dark-blue/teal with no surface detail, no light gradient, no occlusion. SSAO is screen-space
   and cannot occlude a large cave interior, so `u_skyAmbientColor` reaches everywhere
   underground. Deep caves should be near-black with crystals as the only light. Image: `litcave2_f3`.
   → Needs large-scale / cave ambient occlusion (or a "sky visibility" term) so interiors go dark
   and point lights read. This is the *paired* fix for #1.

3. **Foliage/grass grows UNDERGROUND in caves [HIGH].** The albedo debug view shows green
   grass/foliage material on ledges **deep inside cave chambers** (no sky/light). The foliage
   scatter does not reject roofed/underground positions. Image: `cavealbedo_f3`, `decal3_f47`.
   → Gate foliage/plant scatter on a surface/sky-visibility check (reuse the cave-air probe or
   a roof test like `DebugCamera::FindEnclosedCave`).

4. **Cave-air probe lands in open surface depressions [MEDIUM].** The lumin-crystal scatter +
   the earlier blind cave captures used a naive "first air below surface-cap" probe that can hit
   open valleys/depressions, so a camera placed there sees **night sky / aurora**, not an
   enclosed cave. Images: `crystal_f4`, `cryshot_f2`. `--debug-goto cave` (DebugCamera
   `FindEnclosedCave`, roof-checked) is correct; the crystal *scatter* still uses the naive probe.
   → Switch crystal scatter to the enclosed-cave test (the hero crystal already does).

5. **Markers render as black unlit spikes underground [MEDIUM].** Creature/forager/plant procgen
   octahedra appear as dark/black spiky shapes in caves (lit only by the broken point lights +
   flooded ambient). Image: `decal3_f47`. → Largely resolves with #1/#2; consider an emissive
   flag for in-cave markers.

6. **Flat untextured cave-wall albedo [MEDIUM].** Cave rock is uniform flat brown (the known
   `LOW_TEXTURE_DETAIL` terrain debt) — no triplanar/material detail underground. Image: `cavealbedo_f3`.

7. **Jagged / fragmented marching-cubes cave surfaces [LOW-MED].** Cave-air boundaries look noisy
   in normal+albedo views (thin-feature MC artifacts / normal discontinuities). Images:
   `cavealbedo_f3`, `worley_f3`. → Possibly tune the noise-router thresholds (spaghetti/worley)
   or MC normal smoothing.

8. **Trees render as bare red branch skeletons from distance [LOW].** Aerial caverns view shows
   trees as red/brown branches without leaf coverage. Image: `cavtest_f3`. → Likely LOD/impostor
   or leaf-material at distance; pre-existing, not cave-specific.

9. **Far-camera streaming void + white-triangle artifacts [FIXED this session].** A fixed
   `--cam-pos` far from spawn rendered an unstreamed black void (streaming anchored on the
   spawn-bound player, not the camera). Fixed in `fc018900` (streaming now anchors on the fixed
   camera). NOTE: this was a *capture-tooling* bug; the broader large-world float-precision class
   (per the RHI research) is separate and would want camera-relative rendering eventually.

**Recommended fix order:** #2 + #1 together (cave AO so interiors darken + point lights that
actually fill) — this is the single highest-impact cave fix and unblocks the lumin-crystal payoff.
Then #3 (no underground foliage), #4 (crystal scatter uses enclosed-cave test), then #6/#7 polish.

---

## 3. The debug suite (use these to fix the above)

All render-only, default-OFF, `world_hash`-neutral.
- `--debug-view <albedo|normal|depth|material|position>` (or **F6** to cycle live): overrides the
  final composite with a G-buffer channel. **Albedo = "is it dark night or a lighting bug?"**;
  Normal = geometry/LOD artifacts; Depth = streaming holes. Pass: `rendering/passes/DebugViewPass.*`
  + `res/shaders/debug_view.frag`.
- `--frame-scan <out.json>`: now also writes `<out>.health.json` + logs an anomaly **verdict**
  (black/unlit/blown/NaN/coverage) — CI-gateable. Module: `rendering/FrameHealth.*`.
- `--debug-goto <cave|doline|spawn>`: deterministically frames an **enclosed** cave / doline for
  reproducible capture (pair with `--timelapse`/`--frame-scan`). Module: `debug/DebugCamera.*`.
- `LUMIN_GL_DEBUG=1`: installs the KHR_debug callback (driver errors → log) + readable RenderDoc
  captures. Module: `rendering/GlDebugOutput.*`.

Example repro for the cave-lighting bug:
`luminumbra_client_app --world-preset caverns --debug-goto cave --debug-view albedo --timelapse-frames 4 --auto-create-world --auto-enter-world --no-audio`

---

## 4. Open follow-ups (not regressions — deliberate next work)

> **UPDATE 2026-06-26 (later same day): follow-ups 1, 2, 3, 5 LANDED on `feat/polyglot-audit-roadmap`.**
> Commits: `2f3c8eec` foliage+crystal fix · `f3e1082b` cave sky-visibility scaffold ·
> (FLIP harness) · (RHI spec 014). Determinism re-verified: `--smoke` default stays
> `6f008a9f637c40b7`, run==replay. See §6 for status + the HONEST cave-lighting caveat + a new finding.

1. **Cave lighting fix** (§2 #1+#2) — highest priority; the lumin-crystal payoff + cave
   photography depend on it. Iterate with `--debug-view`.
   **→ PARTIAL (`f3e1082b`).** Landed a default-OFF **sky-visibility ambient term** (`LUMIN_CAVE_AO`
   env knob; fades AMBIENT-only toward a floor when a fragment's ceiling is occluded, leaving
   sun/moon/point lights intact). It is **screen-space (SSR-class)** and I could **not** prove a
   visible win in blind headless captures — see §6. The proper fix is **world-space** (per-chunk
   sky-occlusion bake or RT-GI from the MC-mesh BLAS, spec 014). The scaffold + `u_projection`/
   `u_screenSize` plumbing a world-space version reuses is in place; it fails BRIGHT, never wrongly black.
2. **Underground foliage gate** (§2 #3) — cheap, clear bug. **→ DONE (`2f3c8eec`).** Roof probe in
   `FoliageSurfaceQuery` + tree scatter (reject if solid within 6 m overhead). Also fixed §2 #4:
   crystal scatter now uses `FindEnclosedCave` (16 deterministic enclosed-cave lights, not open dips).
3. **RHI / Vulkan+DX12 migration** — brainstorm done; research recommends **Diligent Engine**
   (Apache-2.0; only lib with Vulkan+DX12+**GL** backends + built-in RT — the GL backend lets GL
   and Vulkan run behind one RHI for in-process FLIP parity). DLSS via NVIDIA Streamline; shaders
   GLSL→single-source HLSL (DXC→DXIL + SPIR-V). Phased: RHI seam → GL-via-Diligent (FLIP no-change
   baseline) → **DebugViewPass on Vulkan, FLIP-diffed vs GL** (cheapest first step) → pass-by-pass
   FLIP-gated → DX12 → DLSS → **RT-GI (also fixes cave lighting)** → RT reflections. Next: `forge-spec`.
4. **POI pillars beyond caves** (spec 013) — biome-aware placement, hero landmarks (hybrid
   WFC/authored), per spec + research `wf_ae777895`.
5. **Visual-regression FLIP golden diffing** (debug-suite Phase 2) — the parity harness the RHI
   migration will lean on; not yet wired.

---

## 5. Build / test quick reference

- Toolchain: prepend `C:\msys64\ucrt64\bin` to PATH (KiCad/mingw64 contaminate it).
- Build: `cmake --build build/debug --target luminumbra_client_app` (and `luminumbra_server_app`).
- Determinism: `luminumbra_server_app --smoke` (default must stay `6f008a9f637c40b7`);
  `--smoke --preset caverns` (run==replay).
- Tests: `build/debug/bin/common_tests.exe` (898 green).
- Captures: `--timelapse-frames N --timelapse-dir <d>` + `tools/ppm_to_png.py`; `--render-benchmark`.
- Caves: `--world-preset caverns` (client) / `--preset caverns` (server); `cave_style:1` enables
  the noise-router; params `spaghetti_*`, `worley_*` in the preset `features` block.

---

## 6. Post-handover follow-up results (2026-06-26 later)

Landed the §4 follow-ups (commits in the §4 banner). Determinism held throughout
(`--smoke == 6f008a9f637c40b7`, run==replay; all four changes are render-only or planning docs).

**DONE & proven**
- **Underground foliage gate (§2 #3) + crystal-on-real-cave (§2 #4)** — `2f3c8eec`. Roof probe
  (solid within 6 m overhead ⇒ reject) at the single `FoliageSurfaceQuery` chokepoint + the tree
  scatter; crystals switched to `FindEnclosedCave`. Log confirms **16 enclosed-cave lights at
  identical coords across runs** (first `-217.2,-33.9,-213.9`), proving the deterministic locator
  finds real roofed chambers, not open dips.
- **FLIP golden-image harness** — `tools/flip_diff.py` (+ `golden_update.py`, `docs/visual-regression.md`).
  4-tier backend, heatmap, `--selftest` PASSES (luma + stdlib here; `pip install flip-evaluator` for true FLIP).
- **RHI spec 014** — `docs/specs/014-rhi-vulkan-dx12-migration/spec.md` (Diligent / Streamline-DLSS /
  single-source HLSL / RT from MC-mesh BLAS, FLIP-gated pass-by-pass).

**PARTIAL — HONEST caveat (read before trusting the cave-AO)**
- **Cave sky-visibility ambient term** — `f3e1082b`, **default OFF** (`LUMIN_CAVE_AO="en,maxDist,floor,steps,thickness"`).
  Mechanism is sound (upward view-space ray-march fades ambient-only toward a floor when roofed;
  point/sun/moon lights untouched), gated, determinism-neutral, builds & runs without crash. **BUT I
  could not demonstrate a visible improvement in blind headless captures.** It is screen-space: the
  probe only darkens a fragment whose ceiling is **on-screen above it**. Every reachable cave framing
  near spawn defeats that: the `--debug-goto cave` hero target is a **shallow water-grotto** (y=-3.2),
  `--cam-pos` into the deep cave looks **outward/horizontally**, and `--debug-goto doline` at the
  framed time is **night-black** (no ambient to remove). `dol_off` vs `dol_on` came out pixel-identical.
  **Next:** a world-space sky-occlusion bake per chunk (feed a real AO term) OR RT-GI (spec 014) — that
  is the real cave-lighting win; the committed scaffold is a safe placeholder + reusable plumbing.
  Capture set: `build/debug/test-artifacts/cave-lighting/{lit_off,lit_on,deep_off,deep_on,dol_off,dol_on}.png`.

**NEW finding surfaced by these captures (not yet fixed)**
- **Foliage billboard cards render full-bright at night.** In the `dol_*` top-down night captures the
  terrain is correctly black but grass/leaf cards float as **bright white/green/blue quads** — the
  foliage card path is not receiving the same darkening as deferred terrain (likely a flat/unlit or
  full-albedo foliage shade path, or missing ambient/sun attenuation on cards). Distinct from the
  underground-foliage bug (that was placement; this is shading). Worth a `--debug-view albedo` vs lit
  A/B on a surface field at night to localize. Add to the cave/rendering problem catalog (§2) as #10.

---

## 7. Night-lighting + foliage/marker/leaf round (2026-06-26 later) — LANDED

Commit `080e18a7` (render-only; `--smoke` stays `6f008a9f637c40b7`, run==replay).

- **Moon casts light + shadows at night** (was: very dark). The shadow cascade now re-keys onto a real
  overhead **moon direction** when the sun is below the horizon (`get_light_space_matrices`), and the
  `lighting_pass.frag` moon term samples that cascade (real cast shadows) + a brighter cool key + a
  **wrapped Lambert** (NdotL*0.6+0.25) that fills the camera-facing slopes an overhead moon leaves
  black. `nightAmbient` lifted to a real cool skylight. Midnight mean-luma **8 (black) → 18 (dim cool
  navigable)**; an early over-tune hit 73 (day-bright wash) — landed between. Capture set:
  `build/debug/test-artifacts/night-lighting/`.
- **Foliage cards darken at night** (§2 #10 FIXED) — re-keyed off the deferred sun-colour nightFactor
  (not u_sunIntensity) + a shared `u_moonDir` cool moon fill, so blades darken WITH the scene.
- **Emissive markers** (§2 #5 FIXED) — creature/forager/crystal beacons pack emissive into the unused
  `gNormalMaterial.b`; lighting adds an albedo-tinted glow faded by daylight. Green creature beacons
  glow clearly at night; non-marker geometry pixel-identical.
- **Far-tree leaf caps** (§2 #8 FIXED) — horizontal canopy cap on the far LOD3 leaf billboard so
  aerial views read green instead of bare trunks (+2 tris/far tree).

**KEY ARCHITECTURAL FINDING (memory: "atmosphere-color-not-intensity"):** the Hillaire scattering
atmosphere drives light **hue** but NOT **intensity** — sun/moon/ambient brightness are authored ramps.
So the night hand-tuning above is the *interim*; the real fix is to couple intensity to the LUT.

### Cave/terrain polish §2 #6 + #7 — LANDED (`d53c99c5`, render-only, --smoke `6f008a9f637c40b7`)
- **#6 flat cave walls = NOT cave-specific, NOT missing textures.** Investigation: stone/soil/deepslate
  all carry `has_texture` and DO enter the triplanar path; the textures load fine. They mip to a flat
  uniform brown at **vista distance**. The surface hides this under lit grass; underground bare-rock
  cave WALLS show it. Fix = **macro albedo variation**: a low-frequency world-space value noise
  (per-fragment, survives mip-blur) giving rock/soil ±22% brightness + warm↔cool hue mottle so rock
  reads as varied stone at any distance. (A material-id triplanar branch was tried + reverted as dead
  code.) Subtle but real; a deeper win would be higher-contrast detail textures / close-range detail.
- **#7 jagged MC surfaces** = the per-cell SDF density gradient (true normal) was computed only for
  triangle winding, then discarded. Now seeded onto each vertex, blended with the face-normal
  smoothing. Subtle in wide vistas; helps thin features. Mesh normals not hashed.

## 8. New spec from `/forge-brainstorm` — 015 atmospheric lighting + colored glass

`docs/specs/015-atmospheric-lighting-colored-glass/spec.md` (committed). Owner decisions:
**atmosphere SPINE first**, colored glass to **full OIT + refraction**. Three composable pillars:
(A) couple light INTENSITY to the scattering LUT + exposure [retires the night hand-tuning above];
(B) froxel volumetric god-rays / fog that catches light; (C) full OIT colored glass + colored shadow
maps + screen-space refraction [→ colored god-rays through stained glass]. Render-only; gate is FLIP
visual parity + intentional re-bless. Companion to spec 014 (RHI) — volumetrics + OIT port behind it.
**Next implementation step = Pillar A** (it retires the §7 night ramps and makes all times-of-day
correct from the physics; note it triggers a deliberate visual-golden re-bless storm).
