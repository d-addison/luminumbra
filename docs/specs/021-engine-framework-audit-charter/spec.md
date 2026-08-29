# Spec 021: Engine & Framework Audit Charter + Orchestration Mandate

> Status: SPEC (created 2026-07-02). Authored on owner request as the mission brief for a
> **Fable Claude Code session** acting first as **auditor** and then as **orchestrator** of all
> resulting work. This spec is a **charter**, not a feature implementation: its subject is the audit
> + orchestration mission itself. It **reconciles and extends** the owner-approved merged execution
> roadmap (`~/.claude/plans/handoff-2026-06-28-agile-puppy.md`, Codex GPT-5.5 High
> APPROVE-WITH-CHANGES) and **deepens** spec 014 (RHI/Vulkan/DX12/DLSS/HW-RT) into an executable
> GPU-modernization track. It does **not** author a competing roadmap and does **not** change the
> determinism baseline. The audit phase is **read-only**: no tracked source changes, `--smoke`
> run==replay stays `6f008a9f637c40b7` (DEBUG). The proving discipline is "TDD of an audit" — the
> gate is that the produced **audit artifacts** exist, validate against schema, and that **every
> backlog item carries a proving-signal** per `test/features/TDD-LOCK.md`.

## The framing insight (why this spec exists)

The engine already has 20 specs (`docs/specs/001-020/`, 010 skipped), a governing 2026-06-26
engine-infrastructure critique that spawned the coherent 014–020 block, and an owner-approved
merged execution roadmap with a Codex-reconciled dependency spine and Waves 0–3. What it does **not**
have is a **single, verified-against-current-tree audit that ranks every core pillar on one axis** —
the canonical Four Pillars *and* the render/SHIELD-RT layer *and* the feature systems
(ecology, foliage, water, networking, audio, UI, build/test-ops) *and* a first-class
GPU-modernization pillar — where every item carries a TDD proving-signal and a **named owner**
(the Fable session) empowered to drive it to completion.

Concretely (verified against the current tree):
- The renderer is a large deferred GL 4.5 pipeline: `src/luminumbra_client/rendering/RenderPipeline.cpp`
  is the god-object spec 016 is decomposing; the pass-contract seam (`RenderContext.h`, typed
  `RenderResourceHandles.h`) already exists but the Vulkan/DX12 backend behind it does not.
- **Spec 014 already decides** Diligent-thin-seam + DLSS-via-NVIDIA-Streamline + HW-RT
  (BLAS/TLAS/RT-GI/AO) — but ships **zero** GPU-vendor code: `CaptureHooks.h` has an `Nsight` enum
  value that is marker-only, there is no `rhi/` directory, no `VK_`/`vulkan.h`/`nvapi`/Streamline
  include anywhere in source. The owner's "get all the DLSS/RT support" directive is a request to
  turn that decision into an executable, dependency-honoring plan.
- Software ray tracing **is** shipping: `passes/ShieldRtFarFieldPass.*` (heightfield max-mip
  hierarchical-DDA raymarch, GPU-tested) — distinct from hardware RT, and worth preserving.
- The upscaling foundation exists: `res/shaders/taau_resolve.frag` (TAA + the upscale hook), motion
  vectors in `GBuffer.h` (RG16F @ COLOR_ATTACHMENT4), Halton jitter in the pipeline — the exact
  integration point DLSS/FSR/XeSS plug into.
- Two live blockers gate visual verification and load stability: the headless IN_GAME render-capture
  hang (`src/luminumbra_client/main_client.cpp:4349`, frame-2 main-thread block after the doline
  scan) and the world-load `EnsureSurfaceReadyNear` unbounded-wait hang
  (`SHIELD_WorldSystem.cpp:2825/2827/2925`).
- The merged roadmap's assumptions have drifted: 017-A async readback ring **landed**
  (`AsyncReadbackRing.*`, commits `ca2616d8`/`3bba2a52`), the dedicated moon radiance channel
  **landed** (`3aa9740d`). An audit must re-verify state, not restate 2026-06-28.

This charter creates the missing single-axis audit, attaches a proving-signal to every finding, and
hands the Fable session the mandate to orchestrate delivery — reusing the existing forge lifecycle,
routing table, and determinism gates rather than inventing new ones.

## Goals

- **G-1** — A **verified full-engine audit**: every pillar's current state re-checked against the
  tree with `file:line` evidence; no claim without a citation.
- **G-2** — A **per-pillar critique** document for each core pillar and feature sub-pillar (state,
  evidence, gaps/debt, risks, opportunities).
- **G-3** — A **TDD-backed itemized backlog** (`backlog.json`) where **every item carries a
  proving-signal** drawn from `test/features/TDD-LOCK.md`.
- **G-4** — A **single reconciled priority ranking** across all items that **extends, not
  supersedes**, the 2026-06-28 merged roadmap and honors its dependency spine.
- **G-5** — A **deep GPU-modernization track** (`gpu-modernization-plan.md`) that deepens spec 014
  into phased, dependency-honoring execution: RHI seam → Vulkan/DX12 → DLSS(Streamline) → HW-RT.
- **G-6** — A live **orchestration mandate**: the Fable session drives the forge
  spec→plan→execute→verify lifecycle autonomously wave-by-wave, committing (not pushing) after each
  green gate, pausing only at wave boundaries and on true external blockers.

## Non-Goals

- **NG-1** — Not re-authoring specs 014–020. This charter **reconciles** them; each stays canonical.
- **NG-2** — Not shipping the GPU backend inside this spec. It charters the plan and the first-pilot
  ordering; the backend lands under 014's execution, gated by 016.
- **NG-3** — Not changing the determinism baseline. The audit is read-only; `--smoke` stays
  `6f008a9f637c40b7` (DEBUG) / `ea9a0121d13bc3bd` (RELEASE), run==replay.
- **NG-4** — Not a from-scratch competing roadmap. Any re-ordering past a merged-roadmap hard
  dependency must carry an explicit justification.
- **NG-5** — Not a new taxonomy. Pillars map to `README.md` §2/§4.1; feature systems fold under the
  relevant canonical pillar.

## Functional Requirements

### Group A — Exploration & Evidence (read-only)

- **FR-A-001** — Re-verify each pillar's current state against the working tree, citing `file:line`
  for every load-bearing claim. Machine baseline `.forge/reports/polyglot-audit.md` (2026-06-07 tool
  scan) may be cited for LOC/risk counts only, never as the roadmap.
- **FR-A-002** — Diff current state against the merged roadmap's assumptions and record what has
  shipped since 2026-06-28 (e.g. 017-A ring, moon radiance channel) so the ranking reflects reality,
  not the stale snapshot.
- **FR-A-003** — Record the two live blockers as first-class findings: the headless IN_GAME
  render-capture hang (`main_client.cpp:4349`) and the world-load `EnsureSurfaceReadyNear` hang
  (`SHIELD_WorldSystem.cpp:2825/2827/2925`), each with its current stopgap state.

### Group B — Per-Pillar Critique

- **FR-B-001** — Produce one critique doc per pillar under `docs/audit/021/pillar-<name>.md`, each
  containing: **Current state + evidence** (`file:line`), **Gaps / debt**, **Risks**,
  **Opportunities**. The pillar set (canonical + render + GPU + feature sub-pillars):
  - **FR-B-001a** `pillar-shield.md` — SHIELD world: worldgen, streaming/LOD, SDF contract,
    Marching Cubes, persistence (`systems/SHIELD_WorldSystem.*`, `world/MarchingCubes.*`,
    `docs/shield/sdf-contract.md`).
  - **FR-B-001b** `pillar-instinct.md` — Instinct/AI: GOAP, needs, perception, ecology
    (`ai/UtilityAI.h`, `ai/CreatureBrain.h`, specs 005/011).
  - **FR-B-001c** `pillar-atmospheric.md` — weather, seasons, wind, time-of-day
    (`systems/WeatherSystem.*`, Hillaire atmosphere; note the color-not-intensity insight).
  - **FR-B-001d** `pillar-aetheric.md` — light/energy field, diffusion, emissive/GI.
  - **FR-B-001e** `pillar-render.md` — SHIELD-RT, deferred pipeline, the 016 frame-graph seam,
    atmosphere/lighting spec 015 (Pillars A/B/C-1/C-2), the two live blockers.
  - **FR-B-001f** `pillar-gpu.md` — RHI/Vulkan/DX12/DLSS/HW-RT (deepens 014); feeds Group E.
  - **FR-B-001g..l** feature sub-pillars: `pillar-foliage.md` (006), `pillar-water.md` (009/010
    hydrology), `pillar-networking.md` (019), `pillar-audio.md`, `pillar-ui.md` (001/002),
    `pillar-buildtestops.md` (020, the determinism/visual gate suite).

### Group C — Itemized Backlog

- **FR-C-001** — Every finding becomes an item in `docs/audit/021/backlog.json`.
- **FR-C-002** — **Each item MUST carry a non-empty `proving_signal`** selected per
  `test/features/TDD-LOCK.md`: determinism/worldgen → heavy oracle + `LREC1` replay + lockstep;
  visual/render → `WorldVisualSweep` / `tools/flip_diff.py`; replication → `ReplicationScale` /
  `NetworkedReplication`; general → a named ctest or `validate-engine-frontier.ps1 -Mode <gate>`.
  An item with no proving-signal is **invalid** and fails AC-002.
- **FR-C-003** — Each item records: `id`, `pillar`, `spec` (existing `0NN` or `"new"`), `summary`,
  `evidence` (`file:line`), `effort`, `risk`, `dependencies` (item ids or spec ids), `status`
  (`todo`/`in-progress`/`done`), `proving_signal`.

### Group D — Prioritization & Reconciliation

- **FR-D-001** — Produce one ranked order across ALL items in `docs/audit/021/priority-ranking.md`.
- **FR-D-002** — Reconcile against the merged-roadmap dependency spine: `018-B → 017-B`;
  `016-seam + FR-D reflection + registry → 014 pilot`; `018-E/F readback+audit gates → readback
  consumers (015 A-T06, 016 FR-E)`. Do not order past a hard dependency without an explicit
  justification note.
- **FR-D-003** — Re-rank **including** shipped items (mark `done`, do not drop) so the ranking is a
  complete picture, not just remaining work.
- **FR-D-004** — State the ranking criteria explicitly: blocker-unblock value, dependency depth,
  the owner-stated GPU emphasis, effort, and risk.

### Group E — GPU-Modernization Deep Track

- **FR-E-001** — Deepen spec 014 into `docs/audit/021/gpu-modernization-plan.md` as phased,
  dependency-honoring execution:
  1. **RHI seam** — the engine-owned thin seam *above* Diligent, built on 016's `RenderContext.h` +
     `RenderResourceHandles.h` (no re-export; per 014 F3).
  2. **Vulkan + DX12 backends** behind the seam; GL stays the reference backend.
  3. **DLSS via NVIDIA Streamline** (Diligent ships no NGX) wired at the existing TAAU integration
     point (`res/shaders/taau_resolve.frag`; motion vectors already in `GBuffer.h`; jitter already
     present).
  4. **Hardware ray tracing** — BLAS from the Marching-Cubes near-field mesh, TLAS, an RT-GI/AO pass
     — deployed **alongside** the existing software SHIELD-RT SDF far-field raymarch
     (`passes/ShieldRtFarFieldPass.*`), not replacing it.
- **FR-E-002** — Honor the hard gate: the 014 pilot requires the **016 seam + FR-D shader reflection
  + resource registry** (per the merged roadmap's Codex change #8). The plan states this prerequisite
  and does not schedule the pilot ahead of it.
- **FR-E-003** — Define the dual-backend **GL↔Vulkan FLIP parity** gate (`tools/flip_diff.py`) and
  the real SDK link path for Nsight/Streamline (turn the `CaptureHooks.h` `Nsight` marker-only enum
  into a linked capture backend).
- **FR-E-004** — Survey and name **FSR** and **XeSS** as alternative/fallback upscalers behind the
  same TAAU seam, so the DLSS path is not the sole option on non-NVIDIA hardware.

### Group F — Orchestration Mandate

- **FR-F-001** — The Fable session drives the forge lifecycle (brainstorm → spec → plan → execute →
  verify) per ranked item/wave, producing `docs/audit/021/orchestration-plan.md`.
- **FR-F-002** — Apply the routing table: **opus inline** — all shaders, determinism-sensitive code,
  the activation queue/readback ring, the RHI seam; **codex gpt-5.5 high** — spec→plan, mechanical
  whole-file C++, read-only architectural critique/sign-off (`model_reasoning_effort=high`,
  emit-as-final-message); **sonnet fan-out** — atomic UI/RML items against an already-committed
  interface.
- **FR-F-003** — Serialize shared-seam / shared-ABI edits (the parallel-pass-divergence hazard); fan
  out only file-disjoint work; run `git status` after every generation workflow.
- **FR-F-004** — Commit (not push) after each green gate; `--smoke == 6f008a9f637c40b7` (DEBUG) must
  hold after every wave; local `world_hash` bumps are acceptable only when deliberate, re-blessed,
  and ≤1 per change.
- **FR-F-005** — Escalate architectural sign-off to Codex GPT-5.5 High (read-only) rather than
  blocking on the owner; pause for owner review only at wave boundaries and on true external blockers.

## Non-Functional Requirements

- **NFR-001** — Every audit claim cites `file:line`; unsourced assertions are defects.
- **NFR-002** — `backlog.json` is machine-readable and regenerable; the ranking is reproducible from it.
- **NFR-003** — The audit phase changes no tracked source (`git status` clean except
  `docs/audit/021/` and this spec); determinism baseline untouched.
- **NFR-004** — Toolchain rule: prepend `C:\msys64\ucrt64\bin` to PATH for every build/ctest/
  validator call.
- **NFR-005** — Build-the-tree-you-test: gates read `build/debug` (`cmake --build --preset debug`),
  not the legacy `build/` root tree.

## Acceptance Criteria

- [ ] **AC-001** — All 12 pillar critique docs (FR-B-001a…l) exist under `docs/audit/021/` with the
  four required sections each (state+evidence / gaps / risks / opportunities).
- [ ] **AC-002** — `backlog.json` validates against the FR-C-003 item schema and **every item has a
  non-empty `proving_signal`**. Negative test: an item with an empty/missing `proving_signal` FAILS
  the validation script.
- [ ] **AC-003** — `priority-ranking.md` contains no ordering that violates a merged-roadmap hard
  dependency (FR-D-002) without an explicit justification note.
- [ ] **AC-004** — Shipped items are present and marked `done` (re-ranked, not dropped).
- [ ] **AC-005** — `gpu-modernization-plan.md` covers all four phases (RHI → Vulkan/DX12 → DLSS →
  HW-RT), cites spec 014, states the 016-seam+reflection prerequisite, and names FSR/XeSS fallbacks.
- [ ] **AC-006** — `orchestration-plan.md` defines waves, the routing table, the shared-seam
  serialization rule, and the commit-per-green-gate cadence.
- [ ] **AC-007** — The two live blockers (FR-A-003) appear as ranked backlog findings with
  proving-signals.
- [ ] **AC-008** — The audit run changed no tracked source (`git status` clean except
  `docs/audit/021/`) and `luminumbra_server_app --smoke == 6f008a9f637c40b7` is unchanged.

## Phasing

1. **Explore + evidence-gather** (read-only) — FR-A group.
2. **Per-pillar critique** — FR-B group; 12 docs.
3. **Itemize + attach proving-signals** — FR-C group; `backlog.json`.
4. **Reconcile + rank** — FR-D group; `priority-ranking.md`.
5. **GPU deep track** — FR-E group; `gpu-modernization-plan.md`.
6. **Orchestration plan** — FR-F group; `orchestration-plan.md`.
7. **Begin autonomous wave execution** — drive the ranked backlog through the forge lifecycle.

## Open Questions

- **OQ-1** — Diligent thin-seam vs hand-rolled Vulkan for the RHI. **Lean:** Diligent, per 014 (only
  surveyed lib shipping Vulkan+DX12+GL); engine owns a thin seam above it, not a re-export.
- **OQ-2** — DLSS via NVIDIA Streamline vs NRI-builtin. **Lean:** Streamline (Diligent ships no NGX).
- **OQ-3** — How much of the software SHIELD-RT survives once HW-RT lands. **Lean:** keep it for
  far-field and GPU-free/non-RTX paths; HW-RT augments near-field GI/AO.
- **OQ-4** — Update the stale `execution-model-tiering` memory ("Fable no longer available"). **Lean:**
  yes — the current roster lists Fable 5 (`claude-fable-5`) as available; this charter targets it.

## Key files

- Canonical pillars: `README.md` §2 / §4.1; echoed `docs/CLAUDE.md`, `docs/TDD.md` §3.
- Existing GPU/RHI decision: `docs/specs/014-rhi-vulkan-dx12-migration/spec.md`.
- Render-seam keystone: `docs/specs/016-render-framework-frame-graph/spec.md`.
- Reconciliation baseline: `~/.claude/plans/handoff-2026-06-28-agile-puppy.md`.
- TDD discipline: `docs/TDD.md`, `test/features/TDD-LOCK.md`, `docs/STANDARDS.md` §8–10.
- Gate scripts: `tools/gates/validate-engine-frontier.ps1`,
  `tools/gates/validate-determinism-matrix.ps1`.
- Render code: `src/luminumbra_client/rendering/{RenderContext.h, RenderResourceHandles.h,
  RenderResourceRegistry.h, GBuffer.h, AsyncReadbackRing.h, CaptureHooks.h,
  passes/ShieldRtFarFieldPass.h}`; `res/shaders/taau_resolve.frag`.
- Live blockers: `src/luminumbra_client/main_client.cpp:4349`;
  `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:2825/2827/2925`.
- Machine baseline (counts only): `.forge/reports/polyglot-audit.md`.

## Verification (end-to-end)

1. **Charter well-formed** — the spec follows the house section order (compare structurally to
   `docs/specs/016-render-framework-frame-graph/spec.md`); every FR/AC references a real,
   verified `file:line` anchor.
2. **Audit artifacts exist** — after Fable runs phases 1–6, all files under `docs/audit/021/`
   (12 pillar docs + `backlog.json` + `priority-ranking.md` + `gpu-modernization-plan.md` +
   `orchestration-plan.md`) are present (AC-001, AC-005, AC-006).
3. **Proving-signal gate** — a validation script asserts every `backlog.json` item has a non-empty
   `proving_signal`; seed a deliberately-broken item to prove the script FAILS it (AC-002).
4. **Dependency-safe ranking** — cross-check `priority-ranking.md` against the merged-roadmap spine;
   any inversion carries a justification (AC-003).
5. **Read-only audit** — `git status` is clean except `docs/audit/021/`; run
   `build/debug/bin/luminumbra_server_app --smoke` and confirm `world_hash == 6f008a9f637c40b7`,
   run==replay (AC-008).
6. **Per executed wave (orchestration)** — `--smoke` unchanged (or one deliberate re-blessed bump);
   `validate-engine-frontier.ps1 -Mode <gate>` green for touched domains; `flip_diff.py` /
   `WorldVisualSweep` for visual waves; `validate-determinism-matrix.ps1` for Wave-1 concurrency;
   commit after each green gate.
