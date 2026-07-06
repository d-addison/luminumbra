# HANDOFF 2026-07-06 — post-A1 aether arc · 3 Wave-H reds fixed · the NVIDIA/GPU modernization runway

**Supersedes** `HANDOFF-2026-07-05-post-wave-H-instinct-band.md`.
**Branch:** `feat/polyglot-audit-roadmap` (NEVER push; commit per green gate only).
**HEAD after this session:** `36222b8b`. **Backlog: 142/190 done.**
**No canonical hash moved this session** — the A1 band is byte-identical by construction
and every red-fix was test-only.

---

## 0. TL;DR

This session **closed the A1 aether completion arc** and **fixed 3 pre-existing test reds**
that the first full serial gtest lane in a while surfaced. All committed; tree clean; the
canonical baselines are untouched (Bump B: debug `a66ab4d049ba9228`/`a91098d71d742567`,
release `045f7c2f0645bcce`/`85c0842944f86563`, populated `d281053b8de4b891`).

The next big work is **Waves I–K = the NVIDIA/GPU modernization band (M0–M6)**: render-scale
seam → HLSL/slang port → native Vulkan (Diligent) → IUpscaler + DLSS (Streamline) → DX12 →
hardware ray tracing (BLAS/TLAS, RT-GI/AO, RT reflections), plus file-disjoint
NET/UI/AUDIO/OPS/FOLIAGE/sim-tail clusters. Full inventory in §3. **Start at M0 → OPS-05.**

---

## 1. What landed this session (commits, in order)

| Commit | What |
|---|---|
| `4319e980` | AETHER-05 — spec 024 (research-first; Codex NO-GO r1 → GO r2). |
| `35c41429` | Wave H record → priority-ranking.md (@921) + the orphaned release ecology-budget bless. The **Event P bundle was re-run to completion** (prior session's runner died on a scriptblock-`exit` bug): matrix 26/26 at both canonicals, heavy oracle, LREC1 roundtrip+divergence, lockstep loopback+fault, ReplicationSmoke, PopulatedWorldReplay — all green. |
| `aca7e56e` | **A1 sim/field core** — AETHER-06/07/12 + AETHER-08 sim side (see below). |
| `16ed59ee` | **A1 render taps** — AETHER-11 + AETHER-08 render side. RenderParityFrame EXACT 0.0. |
| `a23d6799` | test fix — ResidencyContract (Bump B water-mirror scope). |
| `236316c1` | test fix — HuntWish (INSTINCT-06 sprint degradation). |
| `36222b8b` | test fix — Starvation fixture (lone predator, post-graze-arbiter). |

**A1 aether band** (all default-OFF behind `sim.aether_state`; OFF ⇒ no allocation/tick/hash
bytes, saves unchanged; **seed +38** — the chartered +35 was TAKEN by PhotoFilters, +28/+36
too, grep-verified):
- **AETHER-06** keystone — NEW `src/luminumbra_common/fields/EnergyFieldState.{h,cpp}`:
  world-anchored sparse 16×16 pages (ordered map), uint16 fixed-point (256 raw/unit,
  saturating adds), two-phase id-sorted deposits, pinned integer kernel (truncating outflow
  residue-in-source = number-conserving; exponential shift decay with exact-zero; cadence 8),
  sealed 64×64 window, **sequential** catch-up decay (not pow-by-squaring), save-epoch
  persistence + load rebase. `aether_state:v1:` folds **conditionally into the existing
  `|aether:` ComposeWorldHash slot** (no 8th term) + a **state-only `sub_hashes.aether_state`**
  for the heavy oracle/LREC1 (Codex finding A). 11-test `AetherEmitterDeterminism`.
- **AETHER-07** — `FieldEmitterComponents.h`, header-only `FieldEmitterSystem.h` gather
  (id-sorted, Chebyshev `>>`-falloff), the engine's **first live sol2 Lua VM**
  (`sample_energy_field`, no stdlib, manifest + both gate baselines). **sol2 v3.3.0 needs
  `-Wno-template-body` on GCC 15** (confined to LuaState.cpp). Game alias
  `scripts/common/api/aetheric_field.lua`.
- **AETHER-11** — `u_aetherMaterialModulation` (default 0.0 = ×1.0 = pixel-identical) through
  RenderContext/RenderPipeline/LightingPass/`lighting_pass.frag`; monotonic RenderSmokeTest.
- **AETHER-12** — `StimulusChannel::Aether=5` (append-only, count→6), GameSession stimulus
  fill, PhotoScoring `aether_glow` bonus axis (zero-input ⇒ bit-identical totals).
- **AETHER-08** — channel B (polarity) = channel 1 of the same layer (2 channels from day one
  so v1 covers polarity forever); RG32F dual tap + `u_aetherPolarityActive`-gated glow tint
  (default OFF = pixel-identical).

## 2. The 3 pre-existing reds (fixed) + the coverage-gap lesson

The full serial lane showed 4 fails / 1720. `ForestPerfBudget` = the chartered RED
(FOLIAGE-05 retires it in M1). The other three were **pre-existing** (proven not-A1 by an
exact-bit match — `2.0000004768 = 3.0f × 0.66667f`, where `0.66667` is INSTINCT-06's
`starve_degrade` for a hunger-0.95 predator on the un-modified compiled brain). **Design
call (owner-delegated): keep the shipped INSTINCT-06 behavior** (a starving animal is
genuinely weaker — a deliberate "needs have consequences, FR-A3" model, and the hash-baked
canonical behavior), so all three were test-only fixes (zero baseline movement):
1. **ResidencyContract** (`a23d6799`) — Bump B moved 3 water float-mirror fields out of hash
   scope; the test's `legacy_excluded` literal now records it (16→19 names).
2. **HuntWish** (`236316c1`) — recovers/asserts the 1.5× sprint multiplier by dividing out
   the known `starve_degrade`; honors the INSTINCT-06 degradation.
3. **Starvation** (`36222b8b`) — samples a lone predator (no ambient food ⇒ Wanders when
   starving; prey now correctly Graze rather than move, so they can't sample locomotion
   degradation). `stamina_move_drain=0` tuning keeps it wandering till it starves.

**Lesson (recorded in memory):** the prior session closed Wave H on the Event P **validator**
bundle, which does **not** run these gtest cases; the last full gtest lane predated the
instinct band. **A validator-bundle pass ≠ a full-lane pass — run serial `ctest` after every
sim band.**

---

## 3. Remaining work — Waves I–K = the NVIDIA/GPU modernization band (M0–M6)

Full detail: campaign plan `C:\Users\David\.claude\plans\ultracode-land-all-of-joyful-matsumoto.md`
§"WAVES I–K" (L394-520); backlog `docs/audit/021/backlog.json`;
`docs/audit/021/gpu-modernization-plan.md`.

### 3.0 Landed GPU foundation (do NOT redo)
`src/luminumbra_client/rendering/rhi/` — `RhiBackend`/`Device`/`RhiResource`: backend
selector via `LUMIN_RHI` (default gl), headless Diligent device bring-up, opaque 64-bit
handles beneath the spec-016 handles; **backing slots exist but are null** until M3 drives a
pass through Diligent. **RhiNoReexport** gate keeps Diligent headers inside rhi/.
`cmake/diligent.cmake` — **Diligent v2.5.6**, GL+Vk only, ucrt64 GCC recipe
(`MINGW_BUILD=TRUE`, `-include cstdint` scoped, `VULKAN_SDK`, `core.longpaths`); **ctest-only
linkage so `--smoke` stays byte-identical**. Slang: `slangc` via `VULKAN_SDK/Bin`,
**pilot-only** wiring in `test/CMakeLists.txt:589-658` (PilotShaderReflectionParity);
**`cmake/shaders.cmake` does NOT exist yet** (M2 prep). Pilot HLSL = 2 files under
`res/shaders/pilot/`; the other ~53 shaders are still GLSL. `user.render_scale` knob exists
but has **no rendering seam** (M1 unstarted). Landed backlog: GPU-01..06, GPU-09, **GPU-12
(registry ownership = the P09 precondition)**, GPU-14, GPU-P01..P05.

**Chains:** U: P09→P10→P11 · B: P06→P07→P08 · R (after P07): P12→P13→P14.
**Shared-file serialization** (one track at a time; orchestrator merges at sub-wave
boundaries): `RenderContext.h`, `RenderResourceRegistry.*`, `RenderPipeline.*`,
`main_client.cpp`, `validate-engine-frontier.ps1`, `test/CMakeLists.txt`,
`PassShaderLayouts.cpp`.

### 3.1 The M0–M6 GPU spine

| Sub-wave | GPU spine (id) | What + the HARD PINS |
|---|---|---|
| **M0** hygiene | — | **OPS-05 FIRST** (retire root `build/`, `-Strict` preflight, NEW BuildTreeStrict gate — protects the whole band; may pull forward), OPS-06, NET-10, SHIELD-10. No GPU spine. |
| **M1** scale seam | **GPU-P09** (≡GPU-07) | render_scale into registry descriptors; scale G-buffer/SSAO/lighting/water; shadow atlas/TAAU history/backbuffer stay output-res; `RenderContext.internal_extent`/`output_extent`; LOD bias `log2(scale)`; `taau_resolve.frag` becomes a real upsampler. **scale=1.0 byte-identical = the hard gate** (NEW UpscaleSeamParityGpu; 0.67× under calibrated FLIP). P09 **before** P06's TAAU batch. |
| **M2** HLSL port | **GPU-P06** (≡GPU-08) | Build `cmake/shaders.cmake` (slangc → GLSL+SPIR-V+reflection into `build/shaders_gen/`, cached; Shader.cpp loads emitted GLSL; hot-reload watches `.hlsl`) → NEW **ShaderSingleSourceInventory** gate → dead-shader census (~53→~43: `bloom_*`, `rml.*`, `crystal_field_effect`, `loading_visual.*`, `screen_space_reflections`, `basic.frag`). **Batches F1-F6** (per batch: HLSL → reflection-parity vs PassShaderLayouts → whole-frame in-process A/B emitted-vs-original → delete originals → inventory green). **INLINE-only** (do NOT fan out): enhanced_skybox (42KB), g_buffer, lighting, water, particles, grass_scatter.comp, sdf_generation.compute. FOLIAGE-09 **before** the compute batch. |
| **M3** native Vulkan | **GPU-P07** | rhi/ gains PSO/command/texture backing behind 016 handles; per-pass backend routing (one pass Vulkan, rest GL). Port order (increasing coupling): blit→ssao→aerial+god-rays→skybox→gbuffer→lighting→shadow→water/waterfall→particles(GS risk)→foliage+grass_scatter→plant-procgen(post-FOLIAGE-07)→decals→overlays→**TAAU last (=DLSS fallback)**. Per pass: GL-via-Diligent baseline → native-Vulkan in-process FLIP + frame-health (**Wave-F A/B harness; `-Mode RenderParityFrame`==0.0**). NEW VulkanPassParityGpu. Milestone: `LUMIN_RHI=vulkan` default-capable, GL fallback. |
| **M4** upscaler + DLSS | **GPU-P10** (≡GPU-11), **GPU-P11** (≡GPU-13) | P10: `rendering/upscale/` `IUpscaler{color,depth,motion,jitter,exposure,extents}→color`; **TAAU = provider #1**; FSR2/XeSS **stubs compile only** (R4). NEW UpscalerSelectSmoke (off default byte-identical). P11 (DLSS): `cmake/streamline.cmake` FetchContent (doesn't exist yet); **RUNTIME-loaded DLLs only** — `sl.interposer`+`sl.dlss`+`nvngx` staged next to the binary, **NO import-lib linkage**; Vulkan manual-hooking via Diligent native handles; RTX detect + TAAU fallback; **`LUMIN_DLSS=0` override**; NEW **DlssSwapSmoke** RTX-gated, **excluded from -Mode All**. Streamline auto-exposure interim until F6 metered-exposure ratified (R5). **VISUAL PAUSE #3** (DLSS presets + FOLIAGE-10 morphology). |
| **M5** DX12 + nightly | **GPU-P08** | `DILIGENT_NO_DIRECT3D12=OFF` (currently ON) — **TIMEBOXED, mingw risk**; fallback = parse-only + residual R3 (nothing downstream depends on DX12). `slangc -target dxil` reuses single-source. NEW Dx12PassParityGpu. + OPS-08, OPS-09 (nightly full-gate, after FOLIAGE-05), FOLIAGE-10. |
| **M6** hardware RT + tail | **GPU-P12/P13/P14** | P12 BLAS/TLAS: `rendering/rt/` per-chunk BLAS from near-field MC mesh at render-mesh upload — **render residency ONLY, never sim hash**; refit-vs-rebuild heuristic; **amortized per-frame build budget** (EnsureSurfaceReadyNear lesson — never block streaming); far-field SDF stays analytic; **`LUMIN_RT=0`=zero work**; NEW BlasTlasStreamGpu. P13 RT-GI/AO: slangc **lib-profile HLSL (no GLSL RT ever)**; additive in LightingPass **alongside** the untouched software ShieldRtFarFieldPass (**augments-never-replaces**); **NFR-4 carve-out — frame-health + visual review, NOT strict FLIP**; NEW RtGiCaveGpu + **ShieldRt* trio stays green**. P14 RT reflections in WaterPass (`LUMIN_RT_REFLECTIONS=1` OFF; screen-space fallback preserved); NEW RtReflectionsWaterGpu. **VISUAL PAUSE #4** (RT-GI cave + RT water reflections) + campaign final report. |

### 3.2 Non-GPU parallel clusters (file-disjoint; fan out via Workflow/Agent — agents never build, never touch main_client·GameSession)
- **NET:** NET-06 delta-ON variants (before NET-07) · NET-07 N=32 over-the-wire soak (freeze net_soak.v1) · NET-08 GNS matrix (ENABLE_GNS OFF today) · NET-09 backpressure (ThrottledFrames is a hard-0 stub) · NET-10 doc (M0) · NET-11 single-port multi-connection accept · **NET-12 HARDWARE-BLOCKED** · NET-13 POSIX TcpTransport #else (Docker-validated).
- **UI:** UI-06 world-select from `worlds/saves/` + thumbnails (M1) · UI-09 unhide settings rows · UI-10 codex photo thumbnails · UI-11 first-session tutorial · **UI-08 fidelity baseline @3840×1600, LAST in UI (bless once)**.
- **AUDIO:** AUDIO-11 real occlusion (`SetPhysicsSystem` ~main_client.cpp:3025-3040, Jolt raycasts) (M1) · AUDIO-12 prune 7 dead banks/5 orphan headers · AUDIO-14 photography SFX (.mp3 only; ElevenLabs key = R7) (M3) · AUDIO-13 doc (M6).
- **OPS:** OPS-05 (M0, FIRST) · OPS-06 doc (M0) · OPS-07 config `mirrors:` drift check (M4) · OPS-08 generated C++/shader constant header (M5) · OPS-09 nightly ScheduledGateRun (M5, after FOLIAGE-05) · OPS-16 EngineGameSplitLint allowlist retire (INSTINCT-12 done ⇒ unblocked) (M6) · OPS-15 harness operator doc (dead last).
- **FOLIAGE:** **FOLIAGE-05 EARLY in M1** (re-task ForestPerfBudget GREEN — retires the chartered RED) · FOLIAGE-07 PlantMeshCache (before M3 plant-procgen) · FOLIAGE-09 drop int64 ext from grass_scatter.comp (before M2 compute batch) · FOLIAGE-10 morphology (M5).
- **Sim tails:** INSTINCT-09 GOAP+IAUS unification (also the chartered answer to the ecology O(N²) hot spot) · INSTINCT-14 **BLOCKED on spec-019** (reserve seed ≥39 — the +36 charter is stale/taken) · ATMO-13 region-follow grids (deferred moving-baseline bump) · WATER-15 river fidelity (render-only channel detail first) · SHIELD-08 far-field SDF fidelity (XL; design M4/M5, land M6) · SHIELD-10 sdf-contract doc (M0).

### 3.3 Hardware / residual register
- **Needs a 2nd machine / non-NVIDIA GPU (cannot validate on the dev box):** NET-12 (Steam SDR → GNS loss-matrix proxy) · FSR2/XeSS full providers (stubs only, R4) · NET-13 (Docker gcc container proxy, R2).
- **Needs RTX (dev box has RTX 5070 Ti ⇒ CAN validate locally, but gated / excluded from -Mode All):** DlssSwapSmoke, RtGiCaveGpu, RtReflectionsWaterGpu, BlasTlasStreamGpu.
- **Timebox/process:** DX12 mingw (R3, parse-only fallback) · DLSS Streamline auto-exposure interim (R5) · Streamline ship-signing follow-up (R9) · AUDIO-14 assets need `ELEVENLABS_API_KEY` (R7).

### 3.4 Open owner menus (nothing blocks engineering; current defaults stand)
- **PAUSE #1 visual menu** (still open): AC-A-001 night floor `u_moonWrapFloor` 0.25 vs 0.0; GPU metered auto-exposure adopt-or-keep; froxel default tier.
- **Activation menu** (each = ONE deliberate hash bump + evidence bundle + re-pin):
  `sim.hydrology_weather` ON · `sim.weather_events` ON · a THIRSTING server roster · ATMO-13
  region-follow ON · full spec-010 finite hydrology ON · **`sim.aether_state` ON** (the A1
  layer's activation — the first stateful-field bump).

---

## 4. Source-of-truth pointers
1. `docs/audit/021/backlog.json` (+ `validate_backlog.py` → "OK: 190 items valid").
2. `docs/audit/021/priority-ranking.md` (Wave A–H records; append I–K next).
3. Campaign plan `ultracode-land-all-of-joyful-matsumoto.md` (M0–M6 detail, serialization,
   per-commit sequences).
4. `docs/audit/021/gpu-modernization-plan.md`; `cmake/diligent.cmake`; `cmake/streamline.cmake`
   (to create, M4); `src/luminumbra_client/rendering/rhi/`; `res/shaders/pilot/`.
5. Auto-memory `MEMORY.md` (baselines + gotchas; updated through this session).

## 5. Standing law + new lessons
**Unchanged:** ucrt64 PATH prepend on every build/ctest/validator; **ctest SERIAL only**;
full-tree rebuild after header changes; shaders INLINE; commit per green gate, NEVER push;
`git commit -F <file>`; visual changes need a looked-at capture; validate backlog after every
flip; Codex advisor = gpt-5.5 high, read-only, `-o`. Fan-out agents never build, never touch
`main_client.cpp`/`GameSession`; `git status` sweep after every generation pass.
**New this session:** (1) **A validator-bundle pass ≠ a full-lane pass** — run serial `ctest`
after every sim band. (2) sol2 v3.3.0 needs per-file `-Wno-template-body` on GCC 15
(`optional<T&>::emplace`). (3) stateful-field sub-hashes fold into an EXISTING world_hash term
(never add a term literal — it moves the empty composite) and need a SEPARATE state-only slot
for the heavy oracle (the folded term is recompute-excluded). (4) page-in catch-up decay must
be the sequential per-step loop, not pow-by-squaring. (5) the Event P runner bug: never `exit`
inside a `& { }` scriptblock — read `$LASTEXITCODE`. (6) since the Wave H graze arbiter
(INSTINCT-04/05), a starving PREY grazes (zero-velocity) rather than moving — a lone predator
is the way to sample pure locomotion behavior in tests.

## 6. Immediate next actions
1. Append the **Wave-close note** (A1 + the 3 reds) — or start Waves I–K directly.
2. **M0 → OPS-05** (retire root `build/`, `-Strict` preflight, NEW BuildTreeStrict gate).
   Then M1 with **FOLIAGE-05 early** (retires the ForestPerfBudget chartered RED so the lane
   is 100% green from M1 on).
3. Execute M1–M6 per the campaign plan; the GPU spine is INLINE (orchestrator-owned shared
   seams), the clusters fan out file-disjoint.
