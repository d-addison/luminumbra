# Spec: GPU-locked landables from the 2026-06-22 perf/TAAU handoff

## Context

The `feat/polyglot-audit-roadmap` handoff (`.forge/artifacts/handover/HANDOVER-2026-06-22-perf-localize-taau-session.md`)
enumerates the remaining perf / gate-correctness / polish work. The owner's GPU is currently **locked at a
fixed clock** (2542/2550 MHz) from a prior perf session and cannot be reset without admin (a P0 owner-only
action). This spec lands the **maximal useful subset of that work now** and pre-stages the GPU-validated
pieces so the only remaining step when the clock is reset is "enable and bless".

Investigation (three Explore passes) established the decisive constraint: **almost every render item needs a
live GPU client to validate** — the visual gates (`SkyboxVisual`, `TimeOfDaySweep`, `WeatherVisual`,
`RenderBudget`) all launch the GPU executable, and even the "release-preset interim" still requires the GPU.
Only two things close **headlessly** (the 6 determinism gates run pure ECS sim, no rendering): the
streaming-residual collision pending-set and the FarLod gate-aggregation correctness fix.

Owner decision: be aggressive — also pre-write the depth pre-pass and sky-LUT GPU compute behind
**default-OFF** knobs, and **fix** (not merely instrument) the FarLod gate aggregation.

## Goals

- Cut the per-tick O(N) collision scan in `SHIELD_WorldSystem::update` to O(pending), validated headlessly
  (run==replay + host==peer across all 6 determinism gates), with `world_hash` re-pinned.
- Fix the FarLod mountains gate's last-station-wins residency read (the 96-vs-0 inconsistency) and add
  per-frame/per-station diagnostics so the eventual GPU mountains run is conclusive.
- Pre-write the depth pre-pass and sky-view LUT → GPU compute behind default-OFF render knobs.
- Keep every change determinism-neutral except the explicitly re-pinned collision change; render changes are
  default-OFF so baselines and visual gates do not move until deliberately re-blessed.

## Non-Goals

- NG-1: P0 GPU-clock reset (`nvidia-smi --reset-gpu-clocks`) — owner/admin action, documented only.
- NG-2: Tree impostors / aggressive far-tree LOD — large GPU feature, deferred to a dedicated spec.
- NG-3: Temporal cloud update — fidelity-sensitive, needs reference-scene A/B on GPU.
- NG-4: TAAU temporal upsampling (§14) — deliberately not built; frame is CPU-bound, upsampling won't raise
  fps and softens vs the BF4/BF1 floor.
- NG-5: TAAU prev-bone skinned motion vectors (§13 remainder) — low priority; optional stretch only.
- NG-6: Flipping any render knob ON or re-blessing any visual gate while the GPU is locked.

## Functional Requirements

### WS-1 — Streaming residual: collision pending-set
- FR-001: Collider creation (`SHIELD_WorldSystem::update` Step 4) MUST drain a deterministic O(pending)
  structure instead of scanning all streamed chunks O(N) every tick.
- FR-002: A chunk is enqueued exactly when it becomes eligible (reaches `Ready`, `lod==0`, non-empty mesh —
  the current predicate) and re-enqueued if a remesh resets `has_collision`. Enqueue MUST NOT depend on any
  job-activity/timing signal.
- FR-003: The drain MUST select the identical set of chunks in the identical chunk-id order as today's
  ordered-map scan (≤16/tick), so `has_collision` — which flows into `world_hash` — is bit-identical.
- FR-004: Double-enqueue MUST be prevented (already-pending or already-collided chunks are not re-added).

### WS-2 — FarLod gate aggregation + diagnostics
- FR-010: The FarLod horizon coverage gate MUST measure a converged/aggregate residency (minimum
  `regions_missing` across stations / settled eye-level state), not the last captured station.
- FR-011: `FarLodSystem::FrameStats` MUST expose per-frame `builds_dispatched`, `builds_integrated_ok`,
  `builds_integrated_failed`, `evictions_this_frame`, `pending_depth`, reset each `update()`.
- FR-012: The analysis JSON MUST include those per-frame counters per station and each station's resolved
  camera position (x, eye-height y, z, yaw, pitch).

### WS-3 — Depth pre-pass (default-OFF)
- FR-020: A `render.depth_prepass` knob (SystemConfig, default OFF) MUST gate a depth-only pre-pass that,
  when ON, renders depth then the gbuffer with `GL_EQUAL`.
- FR-021: The pre-pass MUST reuse the gbuffer vertex shaders (identical `windSway`, polygon offset, clip
  distances, draw order) so depth matches the gbuffer exactly.
- FR-022: With the knob OFF, the pipeline MUST be byte-for-byte identical to today.

### WS-4 — Sky-view LUT → GPU compute (default-OFF)
- FR-030: A `render.sky_lut_gpu` knob (default OFF → CPU path) MUST select a GPU-compute sky-view LUT path
  that writes the same `GL_RGB16F` textures as `build_sky_view_cpu`.
- FR-031: The existing CPU path MUST remain the default and unchanged when the knob is OFF.

## Non-Functional Requirements
- NFR-001 (Determinism): Only WS-1 may change `world_hash`, and only via an authorized re-pin; WS-2/3/4 are
  hash-neutral. All 6 determinism gates stay green.
- NFR-002 (Perf target): WS-1 removes the O(N) per-tick collision scan from the ~4 ms settled-CPU floor.
  Depth pre-pass targets ~0.4–0.8 ms off the 1.63 ms gbuffer (measured later on GPU).
- NFR-003 (Fidelity): No visual regression — render changes default OFF; no gate re-blessed while locked.
- NFR-004 (Locality): Branch `feat/polyglot-audit-roadmap` stays local-only; never pushed.

## Acceptance Criteria
- [ ] AC-001: O(N) per-tick collision scan replaced by an O(pending) drain.
- [ ] AC-002: All 6 determinism gates green (run==replay + host==peer); new canonical `world_hash` re-pinned
      and recorded.
- [ ] AC-003: FarLod coverage gate reads converged/aggregate residency; default + archipelago presets pass;
      per-frame/per-station diagnostics + station camera positions present in the JSON.
- [ ] AC-004: `render.depth_prepass` and `render.sky_lut_gpu` exist, default OFF; with both OFF every headless
      build + existing gate is unchanged (no new `WorldVisualSweep` flags vs baseline).
- [ ] AC-005: Depth pre-pass reuses the gbuffer vertex shaders; when ON it renders depth-only then gbuffer
      with `GL_EQUAL`. (Enable/measure/bless deferred to GPU-free.)
- [ ] AC-006: Sky-view LUT GPU compute path exists behind the knob and writes the same LUT textures; CPU path
      remains default. (Enable/validate deferred to GPU-free.)
- [ ] AC-007: No visual gate re-blessed while the GPU is locked; branch never pushed.

## Open Questions
- OQ-1: Exact converged-residency metric for FR-010 — min-across-stations vs settled eye-level station.
  Resolve during WS-2 implementation against the real per-station capture data.
- OQ-2: Whether WS-4 also ports transmittance/multiscatter to compute now or only sky-view (the per-frame
  refresh hot path). Default: sky-view first; transmittance/multiscatter if low-cost.

## Deferred (documented, not built)
- P0 GPU-clock reset; tree impostors; temporal cloud update; TAAU §14 upsampling; TAAU §13 prev-bone motion
  vectors. Validation that requires the GPU (RenderBudget, SkyboxVisual/TimeOfDaySweep/WeatherVisual,
  mountains FarLod verdict) runs once the clock is reset.
