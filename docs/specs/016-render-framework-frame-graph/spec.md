# Spec 016: Render Framework — Frame Graph, Pass Contracts & Resource Registry

> Status: SPEC (created 2026-06-26). Derived from the engine-infrastructure devil's-advocate
> critique (`.forge/critique-engine-infrastructure-framework-20260626-200421.md`, finding **F1**,
> and the shader-resource-reflection half of **F8**). This is the **keystone** of the critique's
> sequencing call: it is the render *framework* that specs 014 (RHI) and 015 (atmospheric lighting
> Pillars B/C-2) must be built on top of, not underneath. It is render-only — the sim is
> render-agnostic; nothing here feeds `world_hash` (legacy default stays `6f008a9f637c40b7`,
> `--smoke` run==replay). The gate is **FLIP image parity** (`tools/flip_diff.py`) per extracted
> pass, NOT a hash.

## The framing insight (why this spec exists)

The critique's highest-leverage finding: **the render-pass system is not a framework; it is a
god-object with pass-shaped helper classes.** The passes were *extracted* (T-I2-11) but not
*decoupled* — each pass still `#include`s `RenderPipeline.h` and executes against a
`RenderPipeline&`, while `RenderPipeline` retains all the real state, order, GL handles, timing,
debug behavior, and cross-pass side effects.

Concretely (verified against the current tree):
- `RenderPipeline.cpp` is **4,751 lines**; `RenderPipeline.h` is **1,464 lines**.
- `RenderPipeline.h:819-827` grants `friend class` access to nine extracted passes (ShadowPass,
  GBufferPass, SsaoPass, LightingPass, WaterPass, SkyboxPass, ParticlePass, FoliagePass,
  PlantProcgenPass) so they can reach the pipeline's privates.
- Passes include the pipeline and execute against it directly: `LightingPass.h:3`
  (`#include "../RenderPipeline.h"`), `LightingPass.h:33` (`void execute(RenderPipeline& pipeline,
  const Camera& camera)`), `GBufferPass.h` (same shape).
- The frame order is **manually scripted** as a long call sequence in `RenderPipeline.cpp` (~`:1758`
  onward: shadow → gbuffer → plant-procgen → ssao → lighting → water → aerial → god-rays → foliage
  → TAAU → particles → lightning → decals → final blit → debug override), with inline ordering
  exceptions noted in the comments.
- Render "metadata" is refreshed as **telemetry** (~`RenderPipeline.cpp:1176`), not consumed as a
  scheduling/resource contract.
- `update_time_of_day` alone spans `RenderPipeline.cpp:4375-4578`, mixing sky LUT, sun, moon,
  ambient, weather, exposure-adjacent color, and cloud phase — a single function holding many
  unrelated concerns.

This makes new effects **cheap to bolt on and expensive to move, test, parallelize, or port** —
which is exactly why the critique calls 015's froxel volumetrics (Pillar B) and OIT/refraction
(Pillar C-2) *mis-sequenced* if built before this layer exists, and why 014's Diligent migration
will "discover implicit GL state and hidden dependencies late, when the cost of changing them is
highest." This spec builds the missing framework so the expensive effects and the backend migration
ride on it instead of fighting it.

## Goals

- **G-1** — Give passes a real **contract**: declared inputs/outputs, a `RenderContext` + typed
  resource handles instead of `RenderPipeline&`, so a pass can be moved, tested, reordered, and
  ported without reaching into pipeline internals.
- **G-2** — Move render-target/buffer **lifetime, load/store, barriers, debug names, and history
  buffers** into a render **resource registry** the pipeline assembles, not into each pass + the
  god-object.
- **G-3** — Replace the manually-scripted frame order with a **declarative graph** (a real frame
  graph, or at minimum a strict pass-descriptor list) whose render metadata is an *enforceable
  scheduling/resource contract*, not telemetry.
- **G-4** — Provide **reflected shader-resource layout** validation (the render half of F8) so a
  pass's bindings are checked against its shader, setting up 014's single-source HLSL/SPIRV-Cross
  without per-backend binding drift.
- **G-5** — Own the **async GPU readback** seam (shared with spec 017) so render code never blocks
  on a synchronous readback.
- **G-6** — Decompose `RenderPipeline` so it **assembles the graph and owns policy**, not every
  implementation detail (start by splitting `update_time_of_day`).
- **G-7** — Be the **engine-owned abstraction above Diligent** (per F3): 014's Diligent backend
  implements *behind* this seam; this seam is the engine's render framework, Diligent is one backend.

## Non-Goals

- **NG-1** — No visual change. Every extraction is FLIP-parity-gated against the current output; this
  is a *structural* refactor, not a look change. (Contrast spec 015, which intentionally moves goldens.)
- **NG-2** — No sim/worldgen change; `world_hash` untouched (`--smoke == 6f008a9f637c40b7`).
- **NG-3** — Not the RHI/Vulkan/DX12 port itself (that is spec 014, which lands *behind* this seam).
- **NG-4** — Not the new effects (015 Pillars B/C-2); they become clients of this framework once it
  exists.
- **NG-5** — Not a full bespoke GPU memory allocator / transient aliasing engine unless OQ-1 chooses
  the full-frame-graph route; the minimum bar is a strict pass-contract + resource-registry layer.
- **NG-6** — Not the engine-wide concurrency rework (spec 017); this spec only owns the *render-side*
  async readback consumer of 017's ring.

## Functional Requirements

### Group A — Pass contract (decouple passes from the god-object)

- **FR-A-001 — `RenderContext` + typed handles.** Introduce a `RenderContext` (frame/camera/time,
  resource registry handle, command recorder, GPU-timer issue/collect) and typed resource handles.
  Passes receive `RenderContext` + their declared handles, **not** `RenderPipeline&`.
- **FR-A-002 — Declared inputs/outputs.** Each pass declares its read resources and its written
  resources (e.g. GBufferPass writes gbuffer targets; LightingPass reads gbuffer+shadow+ssao, writes
  the HDR lighting target). The declaration is data the graph can inspect.
- **FR-A-003 — No `RenderPipeline.h` include in passes.** Convert passes one by one so
  `passes/*.{h,cpp}` no longer `#include "../RenderPipeline.h"` and no longer take `RenderPipeline&`
  (retire `LightingPass.h:3`, `LightingPass.h:33`, and siblings).
- **FR-A-004 — Remove `friend` access.** When a pass no longer needs pipeline privates, delete its
  `friend class` line from `RenderPipeline.h:819-827`. The friend list shrinks to zero as the
  conversion completes.

### Group B — Render resource registry

- **FR-B-001 — Resource ownership.** A render resource registry owns FBOs/textures/buffers, with
  explicit **transient vs persistent** lifetime, load/store ops, debug names, and **history buffers**
  (for temporal passes like TAAU and the future froxel reproject).
- **FR-B-002 — Barrier/layout ownership.** Resource transitions/barriers are owned by the
  registry/graph, not hand-managed per pass — this is what makes the eventual Vulkan/DX12 layout
  model (spec 014) correct by construction rather than discovered late.
- **FR-B-003 — Targets move off the pipeline.** The G-Buffer and intermediate targets (shadow atlas,
  ssao, lighting accum, water, TAAU history) become registry resources referenced by handle, instead
  of pipeline-owned members reached via friend access.

### Group C — Declarative frame graph / ordering

- **FR-C-001 — Declarative pass list.** Replace the manually-scripted call sequence
  (`RenderPipeline.cpp:~1758`) with a declarative graph/descriptor list the pipeline assembles.
- **FR-C-002 — Metadata is a contract.** The render metadata currently refreshed as telemetry
  (`RenderPipeline.cpp:~1176`) becomes an enforceable scheduling/resource declaration (what each
  pass reads/writes), enabling validation and future automatic ordering/barriers.
- **FR-C-003 — Ordering exceptions become explicit edges.** The inline ordering hacks (plant-procgen,
  decals, god-rays, TAAU-before-transparent, debug override) are expressed as explicit graph
  dependencies, not comments.

### Group D — Shader-resource reflection (render half of F8)

- **FR-D-001 — Reflected resource layout.** Each pass's bound resources are validated against its
  shader's reflected resource layout (sampler/UBO/SSBO/attachment bindings), so a binding mismatch
  fails loudly at build/load, not as a silent wrong-texture bug.
- **FR-D-002 — Sets up single-source shaders.** The reflection contract is the seam spec 014's
  DXC/SPIRV-Cross single-source HLSL plugs into (014 `FR-F.3`); this spec proves it on the GL path
  first so "shader-port defects" are isolatable from "backend defects."
- **FR-D-003 — Hot-reload preserved + safe.** Shader hot-reload continues to work; a reload whose
  reflected layout no longer matches the pass declaration is rejected without tearing down the pass
  (rollback to the previous program). (The config/constant codegen half of F8 lives in spec 020.)

### Group E — Async GPU readback (render-side of spec 017)

- **FR-E-001 — Readback via the ring.** Render-side GPU→CPU readbacks go through the frame-delayed
  async readback ring defined in spec 017 (`FR-A-*`). No new synchronous readback is added in render
  code (this is what spec 015 Pillar A's auto-exposure metering must use — never a blocking readback).
- **FR-E-002 — Retire the blocking render readbacks.** The existing synchronous render readbacks
  (GPU-SDF `glClientWaitSync`/`glMapBuffer` in `RenderPipeline.cpp:~4321/~4328`; foliage
  `glGetBufferSubData` in `FoliagePass.cpp:~793`) are migrated to the ring or explicitly justified.

### Group F — RenderPipeline decomposition

- **FR-F-001 — Split `update_time_of_day`.** Decompose `RenderPipeline.cpp:4375-4578` into focused
  units (sky-LUT/atmosphere, sun, moon, ambient, exposure, cloud-phase) — this directly unblocks
  spec 015 Pillar A's intensity-coupling + exposure work landing on a clean surface.
- **FR-F-002 — Pipeline = assembler + policy.** `RenderPipeline` is reduced to assembling the graph,
  owning render policy/config, and collecting stats — not owning every pass's implementation state.

## Non-Functional Requirements

- **NFR-001 — FLIP parity per conversion (the gate).** Every pass extraction/conversion must
  FLIP-match (`tools/flip_diff.py`) the pre-conversion output within threshold. This is a structural
  refactor: the image must not move (contrast 015's intentional re-bless).
- **NFR-002 — Determinism untouched.** Render-only; `--smoke == 6f008a9f637c40b7`, run==replay, after
  every step. Nothing here feeds `world_hash`.
- **NFR-003 — Incremental, always-shippable.** Passes convert one at a time; the build stays green and
  the game renders correctly at every step (no big-bang refactor — the same discipline 014 demands).
- **NFR-004 — Perf-neutral.** `--render-benchmark` p50 frame/gpu within budget at each step; the
  framework must not add measurable overhead vs. the direct calls it replaces.
- **NFR-005 — Backend-agnostic seam.** The `RenderContext`/handle/command abstraction must be
  expressible over GL today and over Diligent (Vulkan/DX12) in spec 014 — engine-owned thin
  abstraction *above* Diligent (F3), not a re-export of Diligent's API.
- **NFR-006 — Hot-reload + debug views preserved.** Shader hot-reload and the `--debug-view`/F6
  G-buffer views keep working through the refactor.

## Acceptance Criteria

- [ ] **AC-001** — No `passes/*.h` `#include`s `RenderPipeline.h`, and no pass `execute(...)` takes a
      `RenderPipeline&` (grep is clean); the `friend class` list at `RenderPipeline.h:819-827` is
      empty.
- [ ] **AC-002** — A render resource registry owns the G-Buffer + intermediate targets; passes
      reference them by typed handle (no friend-accessed pipeline members).
- [ ] **AC-003** — The frame order is produced by a declarative graph/descriptor list; the inline
      ordering exceptions are explicit dependencies, not comments.
- [ ] **AC-004** — Each converted pass FLIP-matches its pre-conversion golden within threshold
      (`tools/flip_diff.py`), and `--smoke == 6f008a9f637c40b7` after every conversion.
- [ ] **AC-005** — A binding/reflected-layout mismatch between a pass and its shader fails at
      build/load (add a deliberately-wrong-binding test that must fail).
- [ ] **AC-006** — A shader hot-reload with a mismatched layout rolls back to the previous program
      instead of crashing/black-screening (hot-reload rollback test).
- [ ] **AC-007** — No synchronous GPU readback remains in render code except those explicitly
      justified; render readbacks use spec 017's ring (grep + the spec-017 readback-ban gate).
- [ ] **AC-008** — `update_time_of_day` is decomposed; `RenderPipeline.cpp` line count drops
      materially and `RenderPipeline.h`'s friend/privates surface shrinks (report before/after).
- [ ] **AC-009** — `--render-benchmark` p50 within budget at the milestone; no perf regression vs.
      baseline.

## Phasing (extraction order; cheapest/most-reversible first)

1. **Resource registry + `RenderContext` skeleton** (Group A scaffolding + Group B) — introduce the
   types and route ONE simple pass (e.g. SsaoPass or the final blit) through them, FLIP-parity-gated.
2. **Convert post/fullscreen passes** (ssao, aerial, skybox, blit) off `RenderPipeline&` — lowest
   coupling, each FLIP-gated; friend lines drop as they convert.
3. **Declarative graph + metadata-as-contract** (Group C) — replace the scripted order once enough
   passes declare I/O; encode the ordering exceptions as edges.
4. **Convert deferred core** (gbuffer, lighting, shadow) — the coupled middle; FLIP-gated.
5. **Convert dynamic/scatter passes** (water, particle, foliage, plant-procgen, decals).
6. **Shader-resource reflection** (Group D) on the GL path — validates bindings, sets up 014's HLSL.
7. **Async readback migration** (Group E) — once spec 017's ring exists; retire the blocking readbacks.
8. **RenderPipeline decomposition** (Group F) — split `update_time_of_day` (unblocks 015-A) and reduce
   the pipeline to assembler+policy.

> **Co-development with spec 014:** the revised 014 Phase A *pilots* this seam end-to-end on one
> low-risk + one high-risk pass (DebugView + lighting/SSAO) through GL-via-Diligent before mass
> porting. 016 and 014-Phase-A are intended to land together on the pilot passes, then 016 finishes
> the GL-side conversion while 014 ports backends behind the now-proven seam.

## Open Questions

- **OQ-1** — Full frame graph (automatic barrier insertion + transient resource aliasing) vs. a
  strict pass-descriptor/resource-registry layer (the critique's "or at minimum"). Lean: start with
  the strict layer (cheaper, covers the decoupling win), add automatic barriers/aliasing only if the
  Vulkan port (014) shows it's needed.
- **OQ-2** — How much to converge the `RenderContext`/handle model with Diligent's own resource model
  to avoid a double abstraction (F3 warns against giving Diligent too much architectural authority,
  but a needless parallel abstraction is also waste). Decide jointly with 014 Phase A.
- **OQ-3** — Does the metadata-as-contract layer subsume the existing telemetry path
  (`RenderPipeline.cpp:~1176`) or sit beside it during transition?
- **OQ-4** — Sequencing vs. 015 Pillar A: A only needs `update_time_of_day` split (FR-F-001) and the
  async-exposure seam (FR-E-001), not the whole framework. Can A land after just those two slices?
  (Lean: yes — that's the minimal 016 prerequisite for 015-A.)

## Key files

- `src/luminumbra_client/rendering/RenderPipeline.{h,cpp}` — the god-object (4,751 / 1,464 lines);
  friend access `:819-827`; scripted frame order `~:1758`; metadata-as-telemetry `~:1176`;
  `update_time_of_day` `:4375-4578`; blocking GPU-SDF readback `~:4321/~:4328`.
- `src/luminumbra_client/rendering/passes/*.{h,cpp}` — the extracted-but-coupled passes;
  representative: `LightingPass.h:3` (includes pipeline), `LightingPass.h:33` (`execute(RenderPipeline&)`),
  `GBufferPass.h`, `FoliagePass.cpp:~793` (blocking `glGetBufferSubData`).
- `tools/flip_diff.py`, `tools/golden_update.py` — the per-conversion parity gate.
- Cross-spec: **014** (Diligent backend implements behind this seam; revised Phase A pilots it),
  **015** (Pillars B/C-2 become clients of this framework; Pillar A needs FR-F-001 + FR-E-001),
  **017** (async readback ring this spec's Group E consumes), **020** (config/constant codegen half
  of F8; this spec owns the shader-resource-reflection half), **018** (render-vs-sim residency the
  parity contract upholds).

## Verification (end-to-end)

1. Build the canonical preset tree (`cmake --build --preset debug`, prepend
   `C:\msys64\ucrt64\bin`); client+server build clean at every conversion step.
2. `--smoke == 6f008a9f637c40b7`, run==replay, after every conversion (render-only).
3. FLIP-diff each converted pass against its pre-conversion golden (`tools/flip_diff.py`) — must stay
   within threshold (no intentional re-bless; the image must not move).
4. `--render-benchmark` within budget at the milestone.
5. Negative tests: a wrong shader binding fails at build/load (AC-005); a mismatched hot-reload rolls
   back (AC-006); a new synchronous render readback trips the spec-017 ban gate (AC-007).
6. Report `RenderPipeline.{cpp,h}` line counts and friend-list size before/after (AC-008).
