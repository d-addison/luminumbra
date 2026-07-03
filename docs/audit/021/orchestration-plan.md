# Spec 021 — Orchestration Plan (2026-07-02)

> How the ranked backlog (`priority-ranking.md`) gets executed: waves, model routing,
> the shared-seam serialization rule, gate/commit cadence, and escalation (charter Group F,
> AC-006). The Fable session is the named owner and drives the forge
> brainstorm → spec → plan → execute → verify lifecycle per item/wave (FR-F-001).

## Waves

Waves map 1:1 onto the ranking tiers. A wave is DONE when every item's proving-signal is
green, the wave gate passes, and the work is committed. **Owner review points are wave
boundaries only** (FR-F-005); within a wave the session runs autonomously.

| Wave | Ranks | Theme | Wave gate (on top of per-item signals) |
| --- | --- | --- | --- |
| **A** | 34–49 | Live defects, red/latent-red gates, hygiene | `--smoke == 6f008a9f637c40b7`; FoliageInstancing + EngineGameSplitLint + the new HeadlessInGameCapture gate all green; `git status` clean post-OPS-10 |
| **B** | 50–56 | 017-B concurrency keystone (HIGH hash risk) | static `--avail-trace` per-tick MATCH + `validate-determinism-matrix.ps1` + `--smoke-moving` 0-flake + p99 main-wait reduction recorded; `--smoke` byte-identical unless ONE deliberate re-blessed bump |
| **C** | 57–66 | 014 pilot-gate closure → RHI pilot | PilotReadiness gate green BEFORE the pilot starts; RhiPilotParityGpu (in-process GL-via-Diligent vs native-Vulkan FLIP) green; `--smoke` unchanged (render-only) |
| **D** | 67–75 | 015 finish + 016 FR-C/F payoff | TimeOfDaySweep + WorldVisualSweep deliberate re-bless via flip_diff heatmap review; DeterminismAudit green (exposure stays render-only) |
| **E** | 76–112 | Living-world payoff (weather/aether/ecology/water/audio/UI) | PopulatedWorldReplay re-pins BATCHED (one per sub-batch, not per item); heavy oracle + LREC1 for any sim-visible change; audio/UI gate modes green |
| **F** | 113–121 | GPU deep track (upscale seam → HLSL → Vulkan → DLSS/FSR/XeSS → DX12 → HW-RT) | per-pass in-process FLIP vs registered baseline + frame-health `nominal` + `--render-benchmark` within budget; OFF-paths byte-identical |
| **G** | 122–146 | Scale, structural, docs tail | ReplicationScale/NetSoak for net items; per-item signals otherwise |

Within-wave parallel lanes (file-disjoint only — see serialization rule):

- **Wave A:** RENDER-01 is an inline investigation lane; the hygiene items (OPS-10/-04/-12,
  AUDIO-04, UI-07/-12, FOLIAGE-06/-08/-11, SHIELD-04/-05, RENDER-10, AETHER-09) are
  fan-out-safe.
- **Wave B:** strictly serialized, one increment at a time, avail-trace verified per
  increment. No fan-out, ever.
- **Wave C:** the seam chain 57→58→59→60→61 is serialized (shared pass/registry headers);
  GPU-P02 (new `rhi/` + CMake) and GPU-06 (`CaptureHooks`) are disjoint parallel lanes.
- **Wave E:** batch re-pins — INSTINCT-04/06/07/08 land together under ONE
  PopulatedWorldReplay re-pin; INSTINCT-10/11 under a second.

## Routing table (FR-F-002)

| Route | What | Items (examples) |
| --- | --- | --- |
| **Fable/opus inline** (the driving session) | All shaders, determinism-sensitive code, the activation queue, readback/ring consumers, the RHI seam, hash re-pins, calibration/re-bless | RENDER-01, SHIELD-02/03/01, RENDER-12+GPU-12, GPU-P03/P04, RENDER-15/17/18, WATER-08, all Wave-B/C seam work |
| **Codex GPT-5.5 High** (`model_reasoning_effort="high"`, emit-as-final-message, read-only sandbox for critique) | spec→plan for each wave, mechanical whole-file C++, architectural sign-off | OPS-05/-07/-08 codegen follow-ons, NET-13 POSIX transport, GPU-P06 batch ports (mechanical after the pilot proves the pattern), wave-plan critiques |
| **Sonnet fan-out** | Atomic UI/RML/doc items against already-committed interfaces | UI-06/-09/-10/-11, docs tail (ranks 141–146), AUDIO asset-wiring items |

Standing constraints: Codex `xhigh` is banned (wedges); shaders are never delegated
(dispatch-shaders-inline rule); forge spawners are codex-only, so Sonnet fan-out runs
through the harness, not forge.

## The serialization rule (FR-F-003)

Fan out ONLY file-disjoint work. Any two items touching the same seam/ABI/header
(`RenderContext.h`, the registry headers, `SHIELD_WorldSystem.cpp`, `main_client.cpp`,
config constants, any shader shared by two passes) are serialized in rank order — this is
the known parallel-pass-divergence hazard. After EVERY generation workflow: `git status`,
and reconcile anything unexpected before proceeding. Uncommitted visual state must never
ride the tree (that discipline is itself Wave-A items FOLIAGE-08 / WATER-06).

## Gate & commit cadence (FR-F-004)

1. Build the tree you test: `cmake --build --preset debug`; gates read `build/debug`.
   Prepend `C:\msys64\ucrt64\bin` to PATH on every build/ctest/validator call.
2. Per item: lock the proving-signal FIRST (TDD-LOCK — failing test/gate before production
   code), implement to green, run the touched domains' `validate-engine-frontier.ps1 -Mode`
   gates.
3. Per green gate: **commit** (never push). One logical change per commit;
   `world_hash` bumps only deliberate, re-blessed (heavy oracle + LREC1 + lockstep), ≤1 per
   change, and batched where the plan says batched.
4. Per wave: the wave gate above + `--smoke == 6f008a9f637c40b7` (or the single documented
   re-blessed successor) + a wave-summary note in the commit message; then pause for owner
   review.
5. Visual changes: deliberate re-bless via `tools/flip_diff.py` heatmap review — never
   byte-parity assertions on noisy cross-run captures (in-process same-frame only).

## Escalation (FR-F-005)

Architectural forks (e.g. 017-B increment shape, registry ownership design, OIT variant,
FSR-vs-XeSS ordering) go to **Codex GPT-5.5 High read-only critique** with the decision
folded back into the plan — never block on the owner mid-wave. Owner review happens at wave
boundaries and on true external blockers only (second-machine Steam validation NET-12,
RTX-hardware-gated DLSS smoke, any SDK licensing question).

## Charter OQ-4 position

The current model roster lists **Fable 5 (`claude-fable-5`) as available**; this
orchestration is owned by a Fable session. The stale `execution-model-tiering` memory
("Fable no longer available; all tasks on Opus 4.8") is corrected as part of this audit.

## Standing telemetry

- After every wave: refresh `docs/audit/021/backlog.json` statuses (todo → done) and re-run
  `validate_backlog.py`; each item carries its `rank`, so the ranked order is mechanically
  derivable from the JSON (NFR-002).
- The two FR-A-003 blockers (RENDER-01, SHIELD-01) carry wave-A/B exit criteria — neither
  wave closes while its blocker is open.
