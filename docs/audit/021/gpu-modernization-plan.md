# GPU Modernization Deep Plan — RHI / Vulkan+DX12 / DLSS / Hardware RT

> Spec 021 Group E deliverable (charter FR-E-001..004, AC-005). Date: 2026-07-02.
> Deepens **spec 014** (`docs/specs/014-rhi-vulkan-dx12-migration/spec.md` — decisions already made:
> Diligent thin seam, DLSS via NVIDIA Streamline, single-source HLSL, HW-RT from the MC mesh) into a
> phased, dependency-honoring execution plan. Built on the audit of record
> `docs/audit/021/pillar-gpu.md` (2026-07-02) — this plan does not contradict it; it sequences it.
> Owner emphasis: "get all the DLSS/RT support" — honored *within* the hard gate below, never by
> inverting it.

---

## 0. The hard gate (FR-E-002) — stated first because everything hangs off it

**The 014 pilot may not start until the 016 seam legs are done**: the `RenderContext` pass
contract on the pilot surfaces + **FR-D shader reflection** coverage + a **resource registry** that
owns (not merely adopts) the pilot's targets. This is merged-roadmap Codex change #8, restated as
charter FR-E-002 (`docs/specs/021-engine-framework-audit-charter/spec.md:151-153`) and as the
charter's hard-dependency line `016-seam + FR-D reflection + registry → 014 pilot`
(charter `spec.md:130-131`). Spec 014 itself demands it: passes are expressed "against the **016
seam**, not against Diligent's API directly" (014 `spec.md:49-50`, FR-A.5 `spec.md:131-137`).

Current gate status (verified in `pillar-gpu.md`, re-verified against the tree 2026-07-02):

| Gate leg | State | Evidence |
|---|---|---|
| 016 `RenderContext` seam | 10/13 passes converted; **the designated pilot pass DebugViewPass is OFF the seam** (`execute(const GBuffer&, int)`), so are GroundDecalPass and the inline TAAU/aerial/god-rays pipeline methods | `src/luminumbra_client/rendering/RenderContext.h:15-27`; `passes/DebugViewPass.h:52`; `passes/GroundDecalPass.h:38`; `RenderPipeline.h:1025-1028` |
| FR-D shader reflection | Substrate LANDED (commit 6bb83b17), coverage **1/13** (only LightingPass declares an `ExpectedLayout`) | `ShaderReflection.h:21-23`; `passes/LightingPass.cpp:37-54` |
| Resource registry | Exists but **adopt-only**, per-frame-cleared GL-name map; no ownership (016 FR-B-003), no barrier/layout tracking (016 FR-B-002) | `RenderResourceRegistry.h:9-21,53-54`; `RenderResourceHandles.h:15-17`; 016 `spec.md:101-106` |
| In-process FLIP harness | Does not exist; `tools/flip_diff.py` is offline file-based and run-to-run noisy (~0.057 same-tree) | `tools/flip_diff.py:2-15`; memory "Render FLIP gate run-to-run noise"; 014 FR-C.3 `spec.md:173-175` |

**Consequence for scheduling:** Phase 1 below has a *pre-pilot gate-closure wave* (Wave-2-compatible
per the merged roadmap) that burns these legs down (auditor items GPU-04, GPU-05, GPU-12, GPU-09,
tooling GPU-06). The pilot itself (auditor GPU-03) is Wave 3 and is scheduled strictly after the
gate closes. Nothing in Phases 2–4 starts before the pilot's go/no-go passes.

**Downstream gating (what the pilot unblocks):** the 014 pilot in turn gates **015 Pillars C-1 → B
→ C-2** (froxel volumetrics + OIT/refraction are clients of the proven framework, 016 NG-4
`spec.md:72-74`; `pillar-gpu.md:85`), the Group-F shader port (GPU-08), DLSS (GPU-13), and HW-RT
(GPU-10). The pilot is the single keystone: everything the owner wants (DLSS, RT) is downstream of
it, which is *why* the fastest path to DLSS/RT is closing the 016 legs now, not skipping them.

---

## 1. Phase 1 — RHI SEAM (engine-owned, thin, above Diligent)

### Scope

Stand up spec 014 Groups A + A0: vendor Diligent Engine, bring up device/swapchain, introduce the
`Rhi*` backend type set **beneath** the existing 016 handles, and prove the full stack on the
two-pass pilot (DebugViewPass + lighting/SSAO) with dual-backend in-process FLIP parity.

**The seam is engine-owned and thin — no re-export.** This is 014's own constraint, twice over:
the F3 revision ("Diligent is the backend, not the framework... 014 expresses passes against the
016 seam, not against Diligent's API directly", 014 `spec.md:47-50`) and FR-A.2 ("ONE abstraction,
two layers: spec 016 defines the engine-/pass-facing `RenderContext` + typed handles + resource
registry; this spec backs those handles with Diligent resources... They are not a second, competing
engine-facing abstraction", 014 `spec.md:117-124`). 016 NFR-005 says the same from the other side:
"engine-owned thin abstraction *above* Diligent (F3), not a re-export of Diligent's API" (016
`spec.md:159-161`). The seam already exists in embryo: `RenderContext.h:22-23` self-documents as
"the seam spec 014's RHI backend implements BEHIND: the handles here are backend-agnostic; today
they wrap GL names, later a Diligent backend", and `AsyncReadbackRing.h:17-21` is the first landed
proof of the pattern (backend-agnostic public API, GL confined to the .cpp).

Concretely the layering is:

```
passes/*             — see RenderContext + TextureHandle/FboHandle/BufferHandle ONLY
RenderContext.h      — engine-owned pass contract (exists, RenderContext.h:33-163)
RenderResource*.h    — engine-owned typed handles + registry (exist; gain ownership in P1)
rhi/                 — NEW: RhiDevice/RhiBuffer/RhiTexture/RhiPipeline/RhiCmd (014 FR-A.2/A.3)
vendor: Diligent     — implementation detail of rhi/ only; no Diligent header escapes rhi/
```

Enforced mechanically: a `RhiNoReexport` gate greps that no file under `rendering/passes/`, nor
`RenderContext.h`/`RenderResourceHandles.h`/`RenderResourceRegistry.h`, includes a Diligent header
(the 017-A recipe: interface + ctest + frontier gate, `pillar-gpu.md:64`).

### Prerequisites

1. **Gate-closure wave (pre-Diligent, Wave-2-compatible, all GL-side, pixel-neutral):**
   - **GPU-04** — put DebugViewPass on the `RenderContext` contract (it is the pilot pass and it
     is off-seam, `DebugViewPass.h:52`); convert GroundDecalPass (`GroundDecalPass.h:38`); extract
     TAAU/aerial/god-rays out of `RenderPipeline` into contract passes (`RenderPipeline.h:1025-1028`)
     — TAAU extraction is *load-bearing for Phase 3*: an inline pipeline method cannot be swapped
     per-backend (014 FR-G.3).
   - **GPU-05** — reflection `ExpectedLayout` coverage 1/13 → 13/13 (`LightingPass.cpp:37-54` is
     the template). Without declared layouts there is nothing to validate SPIRV-Cross output against
     in Phase 2 (014 FR-F.5).
   - **GPU-12** — registry ownership: G-Buffer + intermediate targets become registry-ALLOCATED
     (016 FR-B-003) with layout/state metadata slots (016 FR-B-002) — the minimum Vulkan-shaped
     surface 014 FR-A.4's explicit-state targets need (`RenderResourceRegistry.h:9-21` documents
     "later 016 phases" ownership; this is that phase).
   - **GPU-09** — the in-process FLIP harness (see §5, FR-E-003) — built and calibrated on raw-GL
     vs raw-GL first, so it exists before there is a second backend to diff.
   - **GPU-06** — capture hooks SDK-linked (see §6) — practically required to debug the Vulkan
     bring-up itself.
   - Shared-seam serialization: all of these touch `RenderContext.h`/registry — serialize them
     (charter FR-F-003; memory "Ultracode parallel-pass divergence").
2. **Then and only then** the pilot (GPU-03).

### Deliverables

| # | Deliverable | Plan item |
|---|---|---|
| 1.1 | `PilotReadiness` frontier gate — machine-checks all four gate legs of §0, fails listing unmet legs; flips green only when the hard gate is closed | GPU-P01 |
| 1.2 | Diligent vendored via **FetchContent** (014 FR-A.1 `spec.md:114-116` — deliberate deviation from the local-`vendor/` norm at `cmake/dependencies.cmake:3-4` because of the worktree/junction hazard; record in CMake comments) + `rhi/` device/swapchain wrapper + `LUMIN_RHI=gl|vulkan|dx12` flag (default `gl`, 014 FR-B.2 `spec.md:160-162`); **no pass ported yet** | GPU-P02 |
| 1.3 | `Rhi*` type set beneath the 016 handles (014 FR-A.2/A.3) + `RhiNoReexport` gate; registry handles gain an optional RHI backing so a pass cannot tell GL-name from Diligent resource | GPU-P03 |
| 1.4 | Pilot-pair shader port: `debug_view.frag` + lighting (or ssao) to single-source HLSL, DXC→SPIR-V, SPIRV-Cross reflection **diffed against the GL-introspected `ReflectedLayout`** (the currency already landed, `ShaderReflection.h:21-23`) — Group F pulled early for exactly two passes (014 FR-A0.1 `spec.md:139-143`) | GPU-P04 |
| 1.5 | **The pilot go/no-go** (completes auditor GPU-03): DebugViewPass + lighting/SSAO through the 016 seam on **GL-via-Diligent AND native Vulkan**, in-process FLIP parity (014 FR-A0.2 `spec.md:144-147`); GL-via-Diligent goldens registered as the parity references (FR-B.3); recorded go/no-go decision on Diligent GL-backend fidelity (OQ-6 `spec.md:348-350`) | GPU-P05 |

### Per-phase proving signals

- `PilotReadiness` gate green (NEW, GPU-P01).
- `RhiDeviceBringupGpu` ctest: Diligent GL + Vulkan devices/swapchains created headless;
  `LUMIN_RHI` parses; `--smoke == 6f008a9f637c40b7` unchanged (NEW, GPU-P02).
- `RhiNoReexport` frontier gate + WorldVisualSweep byte-identical (NEW, GPU-P03).
- `PilotShaderReflectionParity` ctest: SPIRV-Cross layout == GL-introspected layout for the pilot
  pair; ported-HLSL-on-GL FLIP-matches the GLSL golden (NEW, GPU-P04; proves 014 FR-F.5 early).
- `RhiPilotParityGpu` ctest (the auditor's GPU-03 signal): pilot pair on GL-via-Diligent and native
  Vulkan in one process, FLIP < calibrated per-pass threshold vs the raw-GL golden + frame-health
  nominal (proves 014 FR-A0.2).

### Exit criteria

- All five deliverables landed; `RhiPilotParityGpu` green on the RTX 5070 Ti.
- Raw GL retained as **comparison backend** only — a pass is raw-GL, GL-via-Diligent, or
  backend-native, never a hybrid (014 FR-A0.3 `spec.md:148-150`).
- `--smoke == 6f008a9f637c40b7` (DEBUG) after every commit of the phase; `--render-benchmark`
  within budget on the GL path (014 NFR-6).
- Go/no-go on Diligent recorded. **No-go path:** if Diligent's GL backend cannot hit FLIP threshold
  on the high-risk pilot pass (OQ-6), the fallback is per-pass tolerance bumps + frame-health-only
  validation for named passes — an owner/Codex sign-off decision, not silent threshold inflation.
  - **DONE 2026-07-04 — GO** (rank 66): OQ-6 resolved GO in `014/spec.md:366`; GL-via-Diligent
    bit-identical (`4fc41c27`) + native Vulkan byte-identical (`31fabfcc`) to raw-GL on the RTX
    5070 Ti. No-go fallback not exercised.
- Declared unblock: 015 C-1 → B → C-2 may now build on the proven seam.
  - **DECLARED 2026-07-04** (rank 66): the RHI-pilot half of the 015 B/C-2 gate is cleared — see
    `015/spec.md:283`. C-1 (never 014-gated) and B/C-2 may now build on the proven seam once their
    016 render-framework prerequisite is met.

### Risks

- **Diligent GL-backend fidelity (014 OQ-6)** — the keep-shipping-on-GL strategy assumes
  sub-threshold parity; discovered only here, which is why the pilot includes one high-risk pass.
- **CMake/vendor churn breaking `--smoke`** — Diligent + DXC touch shared build files; mitigation:
  smoke after every commit, vendoring commits isolated from code commits.
- **Seam churn under parallel 015/016 work** — `RenderContext.h`'s field union is shared; serialize
  (charter FR-F-003).
- **Windows/MSYS toolchain** — Diligent under the ucrt64 GCC toolchain (memory "Toolchain PATH
  contamination") is less-traveled than MSVC; budget bring-up time; a Vulkan SDK install on the dev
  box becomes a build prerequisite (document in the phase's first commit).

---

## 2. Phase 2 — VULKAN + DX12 BACKENDS (GL 4.5 stays the reference backend)

### Scope

014 Groups F (remaining shaders) + B/D (pass-by-pass Vulkan port) + E (DX12). GL-via-Diligent
remains the default ship backend at every intermediate step (014 FR-B.2, NFR-2 `spec.md:263-264`);
raw GL remains the FLIP comparison reference until each pass's GL-via-Diligent baseline is
registered. Nothing in this phase changes what the player sees — the contract is strict FLIP parity
per pass (014 FR-D.2/D.3 `spec.md:187-192`).

### Prerequisites

Phase 1 exit (pilot green). Reflection coverage 13/13 (GPU-05) — each ported shader's SPIRV-Cross
layout is validated against the pass's declared `ExpectedLayout`.

### Deliverables

| # | Deliverable | Plan item |
|---|---|---|
| 2.1 | **Single-source HLSL port of the remaining ~53 shaders** (55 GLSL files today, zero HLSL — `pillar-gpu.md:29`), batched in pass-risk order, each batch FLIP-validated **on GL-via-Diligent first** to isolate shader-port defects from backend defects (014 FR-F.5 `spec.md:216-218`); offline DXC compile step (DXIL + SPIR-V) as a CMake build step with cache, artifacts not checked in (OQ-7 lean `spec.md:351-352`); hot-reload preserved (FR-F.4, reusing the FR-D rollback at `Shader.cpp:125-127`) | GPU-P06 |
| 2.2 | **Pass-by-pass Vulkan port** in increasing-coupling order (014 FR-D.1 `spec.md:182-186`): blit, ssao, aerial, skybox → gbuffer, lighting → shadow → water, particle, foliage, plant-procgen → GroundDecalPass → TAAU last (it is replaced by DLSS in Phase 3; its Vulkan port is the fallback path). Each pass: GL-via-Diligent baseline registered (FR-B.1/B.3), then native-Vulkan in-process FLIP + frame-health. Full-Vulkan milestone: `LUMIN_RHI=vulkan` default-capable on the RTX target, GL the cross-vendor fallback (FR-D.4) | GPU-P07 |
| 2.3 | **DX12 backend**: device bring-up behind the same seam, per-pass FLIP validation reusing the DXIL compile — "mostly device-bring-up + per-pass FLIP validation, not new pass work" (014 FR-E.1 `spec.md:197-199`); `LUMIN_RHI=dx12` selectable, default-ship decision deferred to the owner (FR-E.3/OQ-2) | GPU-P08 |

### Single-source shader strategy (the migration path, per FR-E-001 and 014 Group F)

- **Author format:** HLSL (SM 6.x), one source per pass, under a new `res/shaders/hlsl/` tree;
  the GLSL originals are deleted per pass *only after* that pass's FLIP gate passes (no fork:
  a pass's shader is GLSL or HLSL, never both compiled into the ship binary — AC-F).
- **Compilers:** DXC → DXIL (DX12) and DXC → SPIR-V (Vulkan); **SPIRV-Cross** for reflection and,
  where Diligent's GL backend needs it, GLSL emission — "the GL baseline runs the same authored
  shader logic, keeping the FLIP baseline honest" (014 FR-F.3 `spec.md:210-212`).
- **Toolchain vendoring:** DXC as a prebuilt release binary fetched at configure time (a build
  *tool*, not linked); SPIRV-Cross via FetchContent as a library — note Diligent already bundles
  SPIRV-Cross/glslang internally, so evaluate reusing its copy before adding a second (decide in
  GPU-P04, record in the pilot commit).
- **Validation currency:** the reflected layout. Because `ReflectedLayout` is already what every GL
  link validates against (`ShaderReflection.h:21-23`: "the same ReflectedLayout will later come from
  SPIRV-Cross reflection"), each ported shader gets a mechanical gate: SPIRV-Cross-reflected layout
  == GL-introspected layout == the pass's declared `ExpectedLayout`.
- **Pilot scope (Phase 1, GPU-P04):** exactly two shaders — `debug_view.frag` (trivial) and the
  lighting (or ssao) shader (hard: G-Buffer reads, shadow array, LUTs). If the hard one ports and
  FLIP-matches, the remaining 53 are turning the crank.

### Per-phase proving signals

- Per ported shader: `flip_diff.py` candidate-vs-golden (ported-HLSL-on-GL vs original GLSL) +
  `PilotShaderReflectionParity`-style layout equality; WorldVisualSweep rerun at batch milestones
  (auditor GPU-08 signal).
- NEW `ShaderSingleSourceInventory` gate: counts GLSL vs HLSL per pass; fails if any pass has both
  live (fork detector); end state 0 GLSL pass shaders.
- NEW `VulkanPassParityGpu` ctest matrix: every ported pass, in-process FLIP vs its GL-via-Diligent
  baseline + frame-health nominal (GPU-P07).
- NEW `Dx12PassParityGpu`: same matrix, `LUMIN_RHI=dx12`, DXIL artifacts (GPU-P08).
- `--render-benchmark` within budget on GL and Vulkan at each milestone (014 NFR-6).

### Exit criteria

- All 13 passes (+GroundDecal/DebugView) Vulkan-parity (014 AC-D); no raw-GL escape hatch inside
  any ported pass (FR-A.5 end state); DX12 runs the full pipeline (`LUMIN_RHI=dx12`, AC-E);
  single-source HLSL everywhere (AC-F); `--smoke` unchanged throughout.

### Risks

- **Volume risk (55 shaders, XL):** mitigated by batch order + the mechanical layout gate; shaders
  are opus-inline work per the routing table (charter FR-F-002; memory "Dispatch shaders inline").
- **Stochastic passes (ssao, dithered particles) vs strict FLIP** (014 OQ-1): per-pass thresholds
  with a documented default; frame-health as backstop.
- **DX12 on this repo is untestable off-Windows** — fine (we are Windows-only), but DX12 gates are
  local-only, never CI-portable; mark them like the existing GPU-gated tests.

---

## 3. Phase 3 — DLSS VIA NVIDIA STREAMLINE (+ FSR2/3 and XeSS behind the same seam)

### Scope

014 Group G. Diligent ships no NGX, so DLSS integrates via **NVIDIA Streamline**, wired at the
existing TAAU integration point. The inputs mostly exist and are verified (`pillar-gpu.md`, items
re-verified 2026-07-02):

- **Motion vectors — present:** RG16F screen-space motion at `GL_COLOR_ATTACHMENT4`
  (`GBuffer.h:19-23`; format/binding in `GBufferPass.cpp:152-158`).
- **Jitter — present:** Halton[2,3] per-frame sub-pixel projection jitter, 16-sample cycle, zeroed
  when TAAU is off (`RenderPipeline.cpp:2105-2121`; `RenderContext.taau_jitter_ndc` at
  `RenderContext.h:141`).
- **Depth — present** (G-Buffer depth, `GBuffer.h:24`).
- **Exposure — NOT yet:** the `RenderContext.exposure` slot is a sentinel-0 placeholder until 015
  A-T05/A-T06 populate it (`RenderContext.h:150-155`) — a real (015-owned) prerequisite.
- **Internal render scale — NOT present at all:** TAAU is native-res TAA, default-OFF
  (`taau_resolve.frag:2,6-7`; `m_taau_enabled = false`, `RenderPipeline.h:1001`), and no
  render-scale plumbing exists anywhere in rendering/ (negative grep, `pillar-gpu.md:23`). DLSS
  quality modes render at reduced internal resolution — **without a render-scale seam, Streamline
  has nothing to plug into.** This is the phase's real work.

### Prerequisites

- Phase 1 exit (the pilot proves the seam Streamline slots behind).
- 016 TAAU extraction (Phase-1 GPU-04 slice) — DLSS **replaces the TAAU pass** (014 FR-G.3
  `spec.md:226-227`); only a contract pass can be swapped per-backend.
- **Phase 2 through the post/upscale chain on Vulkan or DX12**: Streamline supports D3D11/D3D12/
  Vulkan — **not OpenGL** — so DLSS is only reachable on the new backends; TAAU remains the GL-path
  and non-NVIDIA answer. (This is why Phase 3 follows Phase 2 in 014's own phasing,
  `spec.md:324-325`.)
- 015 A-T05/A-T06 exposure population (cross-pillar dependency, tracked as auditor GPU-07's `015`
  dep).

### Deliverables

| # | Deliverable | Plan item |
|---|---|---|
| 3.1 | **Internal-render-scale seam**: scaled allocation of the G-Buffer/lighting chain (registry-owned after GPU-12, so scale lives in ONE place), texture-LOD/mip bias, viewport/UV plumbing, and TAAU upgraded to actually upsample (its header already calls itself "the hook for upsampling", `taau_resolve.frag:2`); `scale=1.0` byte-identical | GPU-P09 |
| 3.2 | **Upscaler provider contract** (`IUpscaler`): inputs {color, depth, motion, jitter, exposure, internal→output extents}, output {upscaled color}; TAAU is the first provider (universal fallback); selection via SystemConfig `user.*` (014 FR-G.4); contract shaped so FSR2's extra inputs (reactive/transparency masks) are optional extension points | GPU-P10 |
| 3.3 | **Streamline + DLSS provider**: Streamline SDK vendored (see §6 for the link/ship split), `sl.dlss` wired at the TAAU point per 014 FR-G.1-G.4 (`spec.md:222-229`); runtime detection (NVIDIA driver + RTX) with automatic TAAU fallback; `LUMIN_DLSS=0` override; quality presets Quality/Balanced/Performance/Ultra-Perf through the settings system | GPU-P11 |

### FSR2/3 and XeSS — named fallbacks behind the SAME seam (charter FR-E-004; 014 OQ-4)

The provider contract (GPU-P10) is deliberately upscaler-agnostic so non-NVIDIA hardware gets a
real upscaler, not just TAAU:

- **AMD FidelityFX Super Resolution 2 / 3 (FSR2/FSR3)** — open source (MIT), C++ SDK, DX12 +
  Vulkan, runs on all vendors (shader-based, no ML hardware requirement). Same input set as DLSS
  (color/depth/motion/jitter/exposure) **plus** optional reactive + transparency-and-composition
  masks — the contract's optional inputs exist for exactly this. FSR3 frame generation is out of
  scope (interpolation is a separate seam); FSR2/3 *upscaling* is the target.
- **Intel XeSS** — DX12 + Vulkan; XMX-accelerated on Intel Arc with a **DP4a fallback path that
  runs on any modern vendor**, making it a second cross-vendor option. Same DLSS-shaped input set.
- **Sequencing:** this plan lands the *contract + an FSR2-shaped provider stub compiling against
  it* (the auditor's GPU-11 proving signal) in Phase 3; full FSR2/XeSS provider implementations are
  post-DLSS follow-ups (new spec items) — the seam guarantees they are additive, not architectural.
- 014 OQ-4 (`spec.md:344-345`) said "revisit once DLSS lands and the upscaler seam exists" — this
  phase *is* that seam; the revisit is now scheduled instead of open.

### Per-phase proving signals

- `UpscaleSeamParityGpu` ctest (auditor GPU-07 signal): fixed scene at 0.67x internal scale with
  TAAU-upscale; frame-health nominal + flip_diff vs native golden under the calibrated (~0.08)
  threshold; `scale=1.0` byte-identical.
- `UpscalerSelectSmoke` ctest (auditor GPU-11 signal): SystemConfig-selected `upscaler=off|taau`
  runtime swap; default-off byte-identical; FSR2-shaped stub compiles against the contract.
- `DlssSwapSmoke` gate (auditor GPU-13 signal; **RTX-hardware-gated, excluded from `-Mode All`**
  like the existing GPU gates): `LUMIN_DLSS=1` replaces the TAAU pass, frame-health nominal, within
  `--render-benchmark` budget; `LUMIN_DLSS=0` falls back to TAAU byte-identically.
- Owner-machine visual pass at each DLSS quality preset (RTX-only validation risk,
  `pillar-gpu.md:57`) — screenshots per memory "Show visual progress".

### Exit criteria

- 014 AC-G: DLSS replaces TAAU on the RTX target within frame-health + budget; TAAU fallback
  engages on `LUMIN_DLSS=0`/incapable hardware; presets apply via settings; the provider contract
  compiles an FSR2-shaped stub; `--smoke` unchanged.

### Risks

- **Streamline↔Diligent interop friction** — Streamline wants the native API objects (VkDevice/
  ID3D12Device etc.) which Diligent exposes via its native-handle accessors; if friction proves
  structural, 014 OQ-3 names NRI as the recorded fallback for a DLSS-only side path (avoid unless
  forced — second dependency).
- **Scaled-target churn**: internal render scale touches every intermediate target; doing it AFTER
  GPU-12 (registry ownership) confines the change to the registry; doing it before would smear it
  across passes — hence the dependency.
- **Exposure dependency on 015** — if A-T05/T06 slip, DLSS can run with its auto-exposure mode as
  an interim (Streamline supports omitting the exposure texture); note it as a divergence and
  re-validate when the metered value lands.

---

## 4. Phase 4 — HARDWARE RT (alongside SHIELD-RT, never replacing it)

### Scope

014 Groups H + I: BLAS from the near-field marching-cubes chunk mesh, TLAS over streamed chunks,
an RT-GI/AO pass, then RT reflections — all default-OFF, all on the RT-capable backends only.

**Coexistence contract (014 OQ-3 lean, FR-H.4):** the shipping software ray tracer —
`ShieldRtFarFieldPass`, the heightfield max-mip hierarchical-DDA raymarch that fills the far-field
G-buffer (`passes/ShieldRtFarFieldPass.h:3-22`) — **stays**. The SDF far-field is never added to
the BLAS/TLAS ("no far-field BLAS", 014 `spec.md:104`; FR-H.4 `spec.md:240-242`). HW-RT covers only
the near-field MC mesh (<256m) and *augments* deferred lighting; the existing
`ShieldRtFarFieldParityGpu`/`ShieldRtFarFieldGbufferGpu`/`ShieldRtFarFieldMaxMipGpu` ctests
(`ShieldRtFarFieldPass.h:19-22`) must stay green untouched through the whole phase — they are the
"never replaces" proof.

**The fidelity win beyond parity:** RT-GI/AO samples the actual near-field geometry (caves, runtime
edits), not the heightmap — closing the known coarse-LOD-ignores-SDF cave-lighting gap (014 FR-H.5
`spec.md:243-246`; memory "Coarse LOD ignores SDF"). This is the one place RT output legitimately
diverges from GL, validated by frame-health + visual review, not strict FLIP (NFR-4
`spec.md:267-269`).

### Prerequisites

- Phase 2 full-Vulkan milestone (RT pipelines need the Vulkan/DX12 backend; Diligent's
  cross-backend BLAS/TLAS/RT-pipeline API is the decided vehicle, 014 `spec.md:29-34`).
- Phase 2 shader toolchain (RT shaders are HLSL/DXC from day one — no GLSL RT ever exists).
- In-process parity harness (Phase 1) for the byte-identical-when-OFF assertions.

### Deliverables

| # | Deliverable | Plan item |
|---|---|---|
| 4.1 | **BLAS/TLAS infrastructure**: per-chunk BLAS from the MC mesh with refit-vs-rebuild heuristic + per-frame build budget so RT never stalls streaming (014 FR-H.1 `spec.md:233-236`, OQ-5 `spec.md:346-347`); TLAS over resident near-field chunks, updated on stream-in/out (FR-H.2 `spec.md:237-238`); no BLAS work when `LUMIN_RT=0` | GPU-P12 |
| 4.2 | **RT-GI/AO pass** (`LUMIN_RT=1`, default-OFF): Diligent RT-pipeline or inline RT sampling the TLAS, composited into deferred lighting (FR-H.3 `spec.md:239-240`); cave-gap validation on a caverns scene (AC-H `spec.md:298-301`) | GPU-P13 |
| 4.3 | **RT reflections** (`LUMIN_RT_REFLECTIONS=1`, default-OFF): TLAS-traced reflections for water/wet/reflective surfaces, screen-space path as fallback, water-pass integration (Group I `spec.md:250-256`) | GPU-P14 |

### Per-phase proving signals

- NEW `BlasTlasStreamGpu` ctest: scripted camera path streams chunks in/out (reuse the
  `--play-paths` scenario machinery); asserts TLAS instance count tracks chunk residency, the
  refit/rebuild budget is respected per frame (no frame over budget attributable to BLAS builds),
  and no job-watchdog stall (GPU-P12).
- `RtGiCaveGpu` ctest (auditor GPU-10 signal): `LUMIN_RT=1` caverns scene passes frame-health
  nominal and shows non-trivial RT-AO variance in cave pixels where heightmap lighting is blind;
  `LUMIN_RT=0` GL path byte-identical; `ShieldRtFarFieldParityGpu` stays green (GPU-P13).
- NEW `RtReflectionsWaterGpu` ctest: `LUMIN_RT_REFLECTIONS=1` water surface shows TLAS-traced
  reflection response absent in the fallback; OFF path byte-identical; frame-health nominal
  (GPU-P14).
- `--render-benchmark` with RT on/off recorded on the RTX target (RT must fit the budget when ON;
  budget unchanged when OFF, 014 NFR-6). Visual review screenshots to the owner (NFR-4 carve-out).

### Exit criteria

- 014 AC-H + AC-I: RT-GI/AO composites correctly under streaming, cave lighting is visibly correct
  where it was blind, far-field untouched, GL path byte-unaffected with RT off; RT reflections
  behind their flag with fallback intact; `--smoke` unchanged.

### Risks

- **BLAS rebuild cadence under heavy streaming (OQ-5)** — the known unbounded-wait hazards in
  streaming (memory "World-load hang: EnsureSurfaceReadyNear") say: budget BLAS builds like mesh
  uploads (amortized, time-budgeted), never block the frame on a build queue.
- **Memory footprint** — BLAS per chunk across the near-field at scale; needs a residency cap and
  eviction tied to chunk streaming from day one (engine scalability principle).
- **Vendor divergence** — RT is cross-backend via Diligent, but this phase validates on NVIDIA
  only (target hardware); AMD/Intel RT tuning is explicitly out of 014's scope (`spec.md:107-108`).

---

## 5. The dual-backend FLIP parity gate, defined (FR-E-003)

**Problem it solves:** per-run full-frame FLIP is NOT deterministic — two `--frame-scan` captures
of the same tree diff at ~0.057 (memory "Render FLIP gate run-to-run noise"), while per-pass port
verdicts need tight thresholds. `tools/flip_diff.py` is an offline candidate-vs-golden **file**
tool (`tools/flip_diff.py:2-15`); 014 FR-C.3 (`spec.md:173-175`) requires rendering both backends
**in the same process** and diffing the framebuffers. So:

**The gate (auditor GPU-09, `DualBackendFlipInProcessGpu`):**

1. **Same process, same frame:** one engine process builds one frame's inputs (camera, LUTs,
   G-Buffer state) ONCE, renders the pass-under-test twice — backend A (raw GL, later
   GL-via-Diligent) and backend B (GL-via-Diligent, later Vulkan/DX12) — into two offscreen
   targets, and FLIP-diffs the two images **in memory** (reuse `flip_diff.py`'s FLIP kernel ported
   in-process, or invoke it on two same-process dumps — the load-bearing property is that both
   images come from identical same-frame inputs, eliminating the cross-run noise term).
2. **Per-pass calibrated thresholds** (014 OQ-1 lean): a documented default (start at the
   calibrated ~0.08 full-frame heuristic, tighten per-pass since same-frame A/B removes the noise
   floor — expect near-zero for deterministic passes), with named exceptions for stochastic passes
   (ssao, dithered particles) and the two intentional-divergence features (RT-GI/AO, RT
   reflections → frame-health + visual review, 014 NFR-4).
3. **Zero cross-run variance asserted:** the ctest runs its A/B twice in the same invocation and
   asserts the two scores are equal — proving the harness itself is noise-free before it gates
   ports.
4. **Frame-health backstop** (014 FR-D.3): NaN/inf/black/over-bright/coverage checks from the
   existing harness on both images.
5. **Registered baselines** (014 FR-B.3): once a pass's GL-via-Diligent output passes vs raw GL,
   it becomes that pass's reference for all subsequent backend diffs.
6. **Placement:** gtest-discovered GPU ctest (hidden-context pattern like
   `async_readback_ring_test`, `test/CMakeLists.txt:316-323`), plus a frontier-gate mode wrapping
   the pass matrix; RTX-only entries excluded from `-Mode All` like existing GPU gates.

Built in Phase 1 *before* Diligent (calibrated raw-GL vs raw-GL — must score 0), so the harness is
proven before it judges a second backend.

## 6. Real SDK link paths — Nsight/RenderDoc capture + Streamline (FR-E-003)

**Capture hooks today are marker-only:** `CaptureBackend::Nsight` is an enum value
(`CaptureHooks.h:7-12`) and `BuildCaptureReadyMarker` always returns `capture_started = false`
("SDK trigger is not linked", `CaptureHooks.cpp:44-45`). Turn this into a linked capture backend
(auditor GPU-06):

- **RenderDoc (dev, GL + Vulkan):** the in-app API is a single header (`renderdoc_app.h`) resolved
  at **runtime** via `GetModuleHandle("renderdoc.dll")` + `RENDERDOC_GetAPI` — no link-time
  dependency, no ship-binary impact. Vendor the header via FetchContent; `CaptureHooks.cpp` gains a
  real trigger path (`StartFrameCapture`/`EndFrameCapture`), returning `capture_started=true` and
  the `.rdc` path when the module is present; MarkerOnly fallback preserved headless.
- **Nsight Graphics (dev, the Vulkan bring-up tool):** NVIDIA's NGFX Injection API
  (`NGFX_Injection.h` + DLL from the Nsight Graphics SDK) behind a CMake option
  `LUMINUMBRA_CAPTURE_SDK=ON` (dev presets only). Nsight is the practical debugger for Phases 1-4
  Vulkan work and the only one of the three that understands DLSS/NGX frames.
- **Ship-vs-dev split (capture):** `LUMINUMBRA_CAPTURE_SDK` is OFF in release/ship presets — ship
  binaries contain the enum + marker path only (today's behavior); dev builds link the trigger
  paths. The `RenderCapture.SdkTriggerLive` ctest (auditor GPU-06 signal) proves the live path;
  the existing marker tests keep the headless/CI path honest.
- **Streamline (ship path, Phase 3):** Streamline is a runtime-loaded plugin architecture —
  `sl.interposer.dll` + feature plugins (`sl.dlss`, `sl.common`) + the signed NGX DLL
  (`nvngx_dlss.dll`). CMake shape: FetchContent the Streamline SDK (headers + import lib +
  redistributable DLLs staged next to the client binary at build time). **Dev:** Streamline
  validation layer + logging ON, unsigned-plugin allowance for iteration. **Ship:** manual-hooking
  or interposer per Streamline's production guidance with **signature verification of the NGX/SL
  DLLs enforced**, validation OFF; non-NVIDIA machines simply fail `slInit` feature detection and
  fall back to the TAAU provider (GPU-P10) with zero DLL requirement — the FSR2/XeSS providers are
  plain linked libraries when they land, no signing story.

## 7. Determinism boundary (the note the whole plan carries)

**Everything in this plan is render-side. The sim hash `6f008a9f637c40b7` must be unaffected.**
Spec 014 is explicit: "the backend NEVER feeds into `world_hash`" (`spec.md:36-39`); NFR-1
(`spec.md:259-262`) requires the legacy `default` preset byte-identical if any shared file is
touched incidentally. The gates that prove it, named:

- **`--smoke == 6f008a9f637c40b7`** (DEBUG, run==replay) after every commit of every phase — the
  primary oracle (charter FR-F-004). The realistic exposure is not renderer code but **build/vendor
  churn** (Diligent/DXC/Streamline touching shared CMake) — hence smoke-per-commit, vendoring
  commits isolated.
- **FR-G-001 render-side readback ban** — `validate-engine-frontier.ps1 -Mode
  RenderReadbackAllowlist` (`:7306`): no new synchronous GPU→CPU readback may appear in render
  code; every RHI backend's readbacks go through the 017-A `AsyncReadbackRing`, whose API was
  designed for exactly this substitution ("spec 014's RHI backs it with timeline semaphores / DX12
  fences without callers changing", `AsyncReadbackRing.h:19-21,63`). The sim-side
  `Test-ReadbackDiscipline` (`:6983`) guards the other bank.
- **018-E/F render-vs-sim residency/audit gates, once landed** — the merged roadmap moves the
  determinism gates ahead of their consumers; when 018-E/F exist they become additional standing
  proof that no render-side state (including RT BLAS/TLAS residency and upscaler state) leaks into
  sim residency or hashing. Until then, `--smoke` + the readback bans are the boundary.
- Flag hygiene: every new capability is default-OFF (`LUMIN_RHI=gl`, `LUMIN_DLSS` off-by-detection,
  `LUMIN_RT=0`, `LUMIN_RT_REFLECTIONS=0`), so the all-off configuration is bit-for-bit today's
  renderer — and the parity gates assert exactly that.

## 8. Plan backlog (additional items, GPU-P01.., complementing the auditor's GPU-NN)

| id | phase | summary | effort | risk | deps | proving signal |
|----|-------|---------|--------|------|------|----------------|
| GPU-P01 | 1 | `PilotReadiness` frontier gate: machine-check of the four hard-gate legs (pilot surfaces on seam, reflection 13/13, registry ownership, in-process FLIP harness); fails listing unmet legs | S | low | GPU-04, GPU-05, GPU-12, GPU-09 | NEW: `PilotReadiness` gate mode green |
| GPU-P02 | 1 | Vendor Diligent (FetchContent) + `rhi/` device/swapchain bring-up + `LUMIN_RHI` flag (default gl); no pass ported | L | medium | GPU-06 (soft) | NEW: `RhiDeviceBringupGpu` ctest + `--smoke` unchanged |
| GPU-P03 | 1 | `Rhi*` type set beneath the 016 handles + `RhiNoReexport` gate (no Diligent header outside `rhi/`) | M | medium | GPU-P02, GPU-12 | NEW: `RhiNoReexport` gate + WorldVisualSweep byte-identical |
| GPU-P04 | 1 | Pilot-pair HLSL port (debug_view + lighting/ssao) via DXC→SPIR-V; SPIRV-Cross layout diffed vs GL-introspected `ReflectedLayout` | M | medium | GPU-P02, GPU-05 | NEW: `PilotShaderReflectionParity` ctest + flip_diff HLSL-on-GL vs GLSL golden |
| GPU-P05 | 1 | Pilot go/no-go (completes GPU-03): pilot pair on GL-via-Diligent AND native Vulkan, in-process FLIP; baselines registered; OQ-6 decision recorded; unblocks 015 C-1→B→C-2 | L | high | GPU-P01..P04 | Existing (GPU-03): `RhiPilotParityGpu` ctest |
| GPU-P06 | 2 | Group-F HLSL port of the remaining ~53 shaders, batched by risk, FLIP-gated on GL-via-Diligent first; offline DXC build step; hot-reload preserved | XL | medium | GPU-P05, GPU-05 | Per-shader flip_diff + NEW `ShaderSingleSourceInventory` fork-detector gate |
| GPU-P07 | 2 | Pass-by-pass Vulkan port in risk order; full-Vulkan milestone (`LUMIN_RHI=vulkan` default-capable on RTX, GL fallback) | XL | high | GPU-P05, GPU-P06 | NEW: `VulkanPassParityGpu` matrix + `--render-benchmark` in budget |
| GPU-P08 | 2 | DX12 backend bring-up + per-pass FLIP reusing DXIL; `LUMIN_RHI=dx12` selectable | L | medium | GPU-P06, GPU-P07 | NEW: `Dx12PassParityGpu` matrix |
| GPU-P09 | 3 | Internal-render-scale seam (registry-scaled targets, mip bias, UV plumbing) + TAAU-as-upscaler; scale=1.0 byte-identical | L | medium | GPU-04, GPU-12, 015 A-T05/T06 (exposure, soft) | Existing (GPU-07): `UpscaleSeamParityGpu` ctest |
| GPU-P10 | 3 | Upscaler provider contract (`IUpscaler`: color/depth/motion/jitter/exposure → upscaled color) with TAAU first provider; FSR2/3 + XeSS named behind it (charter FR-E-004) | M | low | GPU-P09 | Existing (GPU-11): `UpscalerSelectSmoke` ctest + FSR2-shaped stub compiles |
| GPU-P11 | 3 | Streamline SDK vendored + `sl.dlss` provider replacing TAAU on RTX; runtime detection + TAAU fallback; presets via SystemConfig; ship-vs-dev DLL split | L | high | GPU-P07, GPU-P09, GPU-P10 | Existing (GPU-13): `DlssSwapSmoke` (RTX-gated, out of -Mode All) |
| GPU-P12 | 4 | BLAS-from-MC-mesh + TLAS-over-streamed-chunks infrastructure with refit/rebuild budget; zero work when `LUMIN_RT=0` | L | high | GPU-P07 | NEW: `BlasTlasStreamGpu` ctest (residency tracking + budget held under scripted streaming) |
| GPU-P13 | 4 | RT-GI/AO pass (`LUMIN_RT=1`, default-OFF) composited into deferred lighting; cave-gap validation; SHIELD-RT far-field untouched | L | high | GPU-P12 | Existing (GPU-10): `RtGiCaveGpu` + `ShieldRtFarFieldParityGpu` stays green |
| GPU-P14 | 4 | RT reflections (`LUMIN_RT_REFLECTIONS=1`, default-OFF) for water/reflective surfaces with screen-space fallback; water-pass integration | M | medium | GPU-P13 | NEW: `RtReflectionsWaterGpu` ctest (on-path variance, off-path byte-identical) |

Wave alignment (merged roadmap): GPU-04/05/12/09/06 + GPU-P01 are Wave-2-compatible burn-down;
GPU-P02/P03 may overlap late Wave 2 (they touch no pass code — justification: the hard gate guards
the *pilot*, not vendoring); GPU-P04/P05 are Wave 3 (the 014 pilot); GPU-P06..P14 are post-Wave-3
in phase order. Routing per charter FR-F-002: shaders + the RHI seam + anything
determinism-adjacent are opus-inline; mechanical whole-file C++ conversions and spec→plan may go
to codex gpt-5.5 high; nothing here fans out against an uncommitted seam.
