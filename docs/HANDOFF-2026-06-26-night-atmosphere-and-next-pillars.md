# Handoff — 2026-06-26: Night lighting, cave completion, atmosphere insight + the next pillars

Session owner: David. Branch: `feat/polyglot-audit-roadmap` (commit, no push). This is the
**start-here** doc for a fresh session. It covers (1) what landed, (2) everything surfaced to
address, (3) what is NOT done, and (4) the sequenced next pillars. Companion docs:
`HANDOVER-2026-06-26-caves-debug-suite.md` (the prior cave catalog, now mostly resolved — see §2)
and the specs under `docs/specs/014-*` and `docs/specs/015-*`.

> **THE ONE BIG IDEA for next session:** our atmosphere is a **COLOR authority, not an INTENSITY
> authority**. A real Hillaire-2020 scattering model colors the sun/sky/fog, but authored ramps set
> brightness — decoupled. Every "magic light" moment (golden hour, blue hour, moonlit night, fog
> catching a sunbeam) is hand-tuned instead of falling out of the physics. **Spec 015 Pillar A
> couples intensity to the LUT and is the highest-leverage next move** — it retires the night
> hand-tuning we did this session and is the foundation volumetrics + colored glass ride on.
> (memory: `atmosphere-color-not-intensity`)

---

## 1. What landed this session (all committed, render-only, determinism-clean)

`--smoke` default world stays `6f008a9f637c40b7`, run==replay, verified after every change. The sim
is render-agnostic; nothing below feeds `world_hash`.

| Commit | What |
|---|---|
| `2f3c8eec` | No underground foliage (roof-probe gate) + crystals light REAL enclosed caves (`FindEnclosedCave`) |
| `f3e1082b` | Cave sky-visibility ambient term — **default-OFF scaffold** (`LUMIN_CAVE_AO`); honest caveat below |
| `4ca57167` | FLIP golden-image visual-regression harness (`tools/flip_diff.py`, `golden_update.py`, `docs/visual-regression.md`) — selftest passes |
| `d081d53a` | Spec 014: RHI Vulkan/DX12 migration (Diligent / Streamline-DLSS / HLSL / RT) |
| `0e4eb975` | Spec 015: atmospheric lighting beauty + colored-glass transmission |
| `080e18a7` | **Moonlit nights** (moon casts light + cast shadows), foliage night-darkening, emissive markers, far-tree leaf caps |
| `d53c99c5` | Cave/terrain polish: macro albedo variation (#6) + analytic MC normals (#7) |

### Night/moon (the headline user-facing win)
- Moon is now a real night key: the shadow cascade **re-keys onto an overhead moon direction when the
  sun is below the horizon** (`RenderPipeline::get_light_space_matrices`), so moonlit terrain casts
  real shadows. `lighting_pass.frag` moon term samples that cascade + a wrapped Lambert
  (`NdotL*0.6+0.25`) that fills camera-facing slopes an overhead moon leaves black.
- Midnight went **mean-luma 8 (pitch black) → 18 (dim, cool, navigable)**. Verify recipe:
  `--timelapse-tod 0.5` = midnight, `0.0` = noon.
- **This is a hand-tuned INTERIM** (`kMoonKeyScale`, `nightAmbient`, wrap floor all dialed by hand).
  Spec 015 Pillar A re-derives these from physics and retires the hand floors.

### Emissive markers (works great)
- Creature/forager/crystal beacon octahedra pack emissive into the unused `gNormalMaterial.b`;
  `lighting_pass.frag` adds an albedo-tinted glow faded by daylight. They glow as colored beacons in
  dark caves; non-marker geometry is pixel-identical (all G-buffer writers put `.b = 0`).

---

## 2. Everything SURFACED this session (findings + their status)

1. **Atmosphere drives hue not intensity** (the big one — see top banner). STATUS: documented; the fix
   is spec 015 Pillar A. memory `atmosphere-color-not-intensity`.
2. **Foliage cards full-bright at night** (handover §2 #10). STATUS: **FIXED** (`080e18a7`) — foliage is
   forward-lit (`foliage.frag`, drawn after lighting); re-keyed to the deferred sun-colour nightFactor
   + shared moon fill.
3. **Moon didn't cast light/shadows; nights pitch-black.** STATUS: **FIXED (interim)** (`080e18a7`).
4. **Cave #6 "flat untextured walls" was MISDIAGNOSED.** It is NOT cave-specific and NOT missing
   textures — stone/soil/deepslate all have triplanar rock textures that LOAD FINE; they mip to flat
   brown at vista distance (surface hides it under grass; bare cave rock shows it). STATUS: addressed
   with **macro albedo variation** (`d53c99c5`) — subtle; a real win needs higher-contrast detail
   textures / close-range detail (terrain-art task). A material-id triplanar branch was tried +
   reverted as dead code.
5. **Cave #7 jagged MC normals**: the SDF density gradient (true normal) was computed only for triangle
   winding then discarded. STATUS: **addressed** (`d53c99c5`) — gradient now blended into vertex
   normals; subtle in wide views, helps thin features.
6. **Cave-AO (#1/#2) screen-space scaffold could not be shown to work** in blind headless captures —
   the upward probe only darkens fragments whose ceiling is on-screen. STATUS: default-OFF scaffold
   (`f3e1082b`); the REAL fix is world-space — spec 015 Pillar A intensity coupling + RT-GI (spec 014).
7. **Hemispheric ambient is keyed to `Normal.y`** and world-up is effectively `-Y` in the lighting
   convention; raising `nightAmbient` barely lit the visible up-faces (the moon wrap did the work).
   Worth auditing when Pillar A reworks ambient. (Noted, not changed.)
8. **NEW finding for follow-up — blind headless captures are unreliable for cave/night framing:** the
   auto-world randomizes; reachable framings are water-grottos, outward/horizontal, or night-black.
   Use FIXED scenes: `--world-preset caverns --debug-goto <cave|doline|spawn>` + `--debug-view
   <albedo|normal|material|position>` + `--timelapse-tod`. The albedo-vs-lit luminance compare (a
   tiny python mean-luma over the PPM) was the decisive diagnostic all session — keep using it.

---

## 3. What is NOT done (open / deferred)

- **Spec 015 (atmospheric lighting + colored glass) — NOT STARTED.** This is the main next body of
  work. Owner decisions locked: **atmosphere spine first**, colored glass to **full OIT + refraction**.
  See §4.
- **Spec 014 (RHI Vulkan/DX12) — SPEC ONLY, not started.** Big migration; volumetrics + OIT (015)
  should be authored compute-shaped so they port behind it. RT-GI (014) is the eventual real
  cave/GI lighting.
- **Cave-AO world-space fix** — the screen-space scaffold is a placeholder; the proper fix is Pillar A
  + RT-GI. The `LUMIN_CAVE_AO` env knob stays default-OFF.
- **#6 high-contrast terrain detail** — macro variation is subtle; a dramatic fix (detail textures /
  close-range relief) is a focused terrain-art task, not done.
- **Spec 013 POI pillars beyond caves** — biome-aware landmark placement, hero meshes (hybrid
  WFC/authored) per research `wf_ae777895`. Not started.
- **Night exposure / eye-adaptation** — folded into Pillar A; the hand-tuned night floors want
  replacing with physical auto-exposure that respects the manual photo-mode shutter/ISO/EV.
- **Untracked junk in the tree** (NOT mine; pre-existing): lots of `?? *.txt`, `?? crop_*.png`,
  `?? assets/models/`, `?? .tmpcrops/`, `?? critique*.md`, plus the always-dirty `build/` and
  `.forge/` logs. Leave them unless asked; don't `git add -A`.

---

## 4. THE NEXT PILLARS — spec 015 (do these next, in order)

Full spec: `docs/specs/015-atmospheric-lighting-colored-glass/spec.md`. Sequenced by
photographic-payoff-per-effort. All render-only; gate is **FLIP visual parity + intentional re-bless**
(`tools/flip_diff.py`), NOT a hash. The sim is render-agnostic.

### Pillar A — Couple light INTENSITY to the atmosphere (DO FIRST, the spine)
Derive sun/moon irradiance + sky-ambient **magnitude** from the same LUTs that already drive hue, and
add a physical exposure/eye-adaptation. Retires the night hand-tuning (`kMoonKeyScale`, `nightAmbient`,
moon wrap floor) and makes golden hour / blue hour / moonlit night correct "for free."
- Files: `SkyAtmosphereLut.{h,ipp}` (expose sun + hemisphere irradiance MAGNITUDE — the integral
  `m_sky_ambient` at `~SkyAtmosphereLut.ipp:550` exists but its magnitude is discarded);
  `RenderPipeline.cpp::update_time_of_day` (~4446 sun color, ~4464 transmittance, ~4506 moon,
  ~4519-4521 ambient ramp — replace the intensity ramps); `lighting_pass.frag` (sun/moon/ambient
  terms already present — consume new irradiances); a tonemap/exposure stage.
- ⚠️ **Re-bless storm**: every lit frame shifts. `world_hash` untouched (render-only) but ALL visual
  goldens move — review the FLIP heatmaps and re-bless intentionally (local-dev re-bless is fine).
- ⚠️ Reconcile auto-exposure with the manual photo mechanic (manual overrides auto in photo mode).

### Pillar C-1 — Colored shadow maps (cheap, do right after A)
Shadow pass stores a TINT where light crosses colored glass → light beyond is colored (stained-glass
cathedral). Determinism-safe; instant "light is the subject" moment; becomes colored god-rays once B
lands. Files: `ShadowPass.*` + a color attachment; a glass material/flag.

### Pillar B — Froxel volumetric participating media (the big beauty unlock)
Camera-frustum froxel volume; per-froxel sample the (now moon-keyed) shadow cascade + scattering LUT
in-scatter; raymarch-integrate; composite. God-rays, fog that catches light, colored shafts.
**Half-res + temporal reproject** (proven by the half-res cloud win). EXTENDS the existing analytic
aerial pass (`res/shaders/volumetric_lighting.frag`), not a duplicate. Budget via `--render-benchmark`.

### Pillar C-2 — Full OIT colored glass + screen-space refraction (last)
Owner chose full layered OIT + refraction (see-through colored crystals). Weighted-blended OIT or
per-pixel linked list; Beer–Lambert absorption per layer; normal-driven refraction offset sampling the
lit scene behind. Forward transmissive pass (deferred can't do this natively). Files: new
`passes/GlassPass.*` + glass shaders. Keep game content (LuminCrystal stained panes) out of the engine
seam — engine exposes a generic colored-transmissive material.

**The money shot all three compose into:** a colored god-ray through stained glass landing in fog.

---

## 5. Build / test / verify quick reference
- Toolchain: **prepend `C:\msys64\ucrt64\bin` to PATH** every build/run (KiCad/mingw64 contaminate it).
  Run the GPU client + builds via the **PowerShell** tool (Bash tool is sandboxed; exit 127 / silent
  compile fails).
- Build: `cmake --build build/debug --target luminumbra_client_app` (and `luminumbra_server_app`).
  TWO build trees — the engine-frontier gate uses **build/debug**. Shaders **hot-load at runtime** (no
  rebuild for `.frag`/`.vert` edits — just relaunch); C++ needs a rebuild. Don't rebuild while the
  client holds the `.exe` (Windows lock).
- Determinism: `luminumbra_server_app --smoke` (must stay `6f008a9f637c40b7`, run==replay).
- Visual capture (use FIXED scenes, not blind auto-world):
  `luminumbra_client_app --world-preset caverns --debug-goto <cave|doline|spawn> --debug-view
  <albedo|normal|material|position> --timelapse-frames 2 --timelapse-tod <0.0 noon|0.5 midnight>
  --auto-create-world --auto-enter-world --no-audio --timelapse-dir <d>`, then `tools/ppm_to_png.py`.
- Quick luminance read (the decisive diagnostic): python mean-luma over the PPM bytes; albedo-view vs
  lit-view compares "geometry present?" vs "lit black?".
- FLIP visual gate: `python tools/flip_diff.py --selftest` (luma + stdlib backends; `pip install
  flip-evaluator` for true FLIP). Bless goldens with `tools/golden_update.py` (refuses overwrite
  without `--force`).
- Debug suite: `--debug-view`/F6, `--frame-scan <out.json>` (+ `.health.json` verdict), `--debug-goto`,
  `LUMIN_GL_DEBUG=1` (KHR_debug callback).

## 6. Key gotchas / standing rules (memory-backed)
- `atmosphere-color-not-intensity` — the spine of spec 015 (see top).
- Mesh GEOMETRY + normals are EXCLUDED from `world_hash` (render mesh non-deterministic) — normal-only
  changes are hash-safe by construction; still `--smoke` to be rigorous.
- Local-dev `world_hash` bumps are fine (no shipped game) but these render-only changes kept it stable.
- Audio: miniaudio here has NO ogg decoder → use mp3; ElevenLabs key from `$env:ELEVENLABS_API_KEY`
  ONLY, never written to a tracked file.
- `dispatch-shaders-inline` — intricate shader/lighting work (Pillar A/B, OIT) is best done INLINE
  with GPU verification; delegate only additive/mechanical drafting to workflows (which can't
  build/GPU-verify — they draft exact patches, the parent integrates + builds + verifies).
- Single-PC: no two-box LAN; Steam transport can't be locally validated.

## 7. Suggested first move next session
Start **spec 015 Pillar A** (atmosphere→intensity coupling + exposure). It is the foundation, retires
this session's night hand-tuning, fixes all times-of-day from the physics, and unblocks the
volumetrics + colored-glass beauty. Expect + sequence the visual-golden re-bless deliberately. Then
Pillar C-1 (colored shadows) for a fast stained-glass win, then B (volumetrics), then C-2 (OIT glass).
