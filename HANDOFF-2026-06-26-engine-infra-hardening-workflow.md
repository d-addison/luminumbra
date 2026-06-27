# Handoff — 2026-06-26: Engine-Infrastructure Hardening → author a Forge workflow

**Owner:** David. **Your job next session:** *pull latest `develop`, build, then author a single
**Forge workflow** that executes the engine-infrastructure-hardening spec set below* (specs 016–020 +
the 014/015 revisions), in the dependency order given. This doc is the **consolidated, start-here**
entry point — it folds the critique, the dispositions, and the sequencing into one execution plan.

> This work is **spec/planning only so far** — no engine code changed. It is the output of a
> Codex (gpt-5.5, high reasoning) devil's-advocate critique of the engine *infrastructure/framework*,
> which the owner approved. The next step is to turn it into an executable Forge workflow.

---

## 0. Approved decisions (locked — don't re-litigate)

The critique proposed two course-corrections that reverse prior direction; **the owner approved
both**, plus the spec structure:

1. **Freeze spec 015 Pillars B (froxel volumetrics) & C-2 (OIT/refraction)** until the render seam
   (spec 016) + the 014 RHI pilot exist. **Pillar A (lighting intensity + exposure) still proceeds**
   (it only needs two narrow 016 slices). Reason: build the expensive effects once on the framework,
   not twice (GL now, re-port under 014 later).
2. **Demote lockstep** from a scale path to a determinism-oracle / replay / small-co-op tool;
   **server-authoritative delta replication is THE 20–32 player architecture** (spec 019).
3. **Five domain specs (016–020) + two revisions (014, 015)**, consolidated under this one roadmap.

---

## 1. The spec set (what the workflow must execute)

Source critique: `.forge/critique-engine-infrastructure-framework-20260626-200421.md`.
Finding→spec map (detail): `.forge/reports/critique-dispositions-engine-infra-20260626.md`.
All specs match the canonical Forge format (Status / Goals / Non-Goals / FR / NFR / AC / Open
Questions / Phasing / Key files / Verification) and are anchored to verified `file:line`.

| Spec | Title | Addresses | FR groups |
|---|---|---|---|
| **016** `docs/specs/016-render-framework-frame-graph/` | Render Framework — frame graph, pass contracts, resource registry (**keystone**) | F1 + F8-shader half | A pass-contract · B resource-registry · C frame-graph · D shader-reflection · E async-readback · F pipeline-decomp |
| **017** `docs/specs/017-engine-concurrency-async-execution/` | Concurrency / async — phase ownership + async readback rings | F4 | A readback-ring · B deterministic activation · C continuations · D tail-latency gates + readback-ban |
| **018** `docs/specs/018-determinism-hardening/` | Two-worlds residency + determinism test matrix + audit gate | F5 | A two-worlds · B availability-set · C streamed-pattern · D test-matrix · E readback-discipline · F audit-gate |
| **019** `docs/specs/019-networking-scale-out/` | Server-authoritative delta replication as the 20–32 player framework | F6 | A scale-path · B lockstep-demote · C transport-matrix · D backpressure · E metrics · F GNS-before-Steam |
| **020** `docs/specs/020-build-test-operability-config-schema/` | Build/test operability + config schema | F7 + F8-config half | A build-operability · B config-schema/codegen |
| **014 (revised)** `docs/specs/014-rhi-vulkan-dx12-migration/` | RHI migration — now a **pilot-pair** phase under the 016 seam | F3 | new FR-A0.* pilot; FR-A.2/A.5/B.1 amended; Phase A→A0 |
| **015 (revised)** `docs/specs/015-atmospheric-lighting-colored-glass/` | Atmospheric lighting — B/C-2 **gated** behind the seam; A async exposure | F2 | Dependencies + re-sequenced Phasing; FR-A-004 async readback |

**One-abstraction rule (016 ↔ 014):** spec 016 defines the engine/pass-facing seam (`RenderContext`
+ typed resource handles + registry); spec 014's `Rhi*` types are the **backend-facing implementation**
of those handles (GL/Vulkan/DX12). Passes see the 016 handles; Diligent lives beneath. Not two
competing layers (014 FR-A.2 ⟦F3 clarified⟧; 016 OQ-2).

---

## 2. Execution DAG + recommended order (the sequencing call)

```
016 Render Framework (KEYSTONE)
   ├── needs only FR-F-001 (split update_time_of_day) + FR-E-001 (async-readback seam) → unblocks 015-A
   ├── gates 015 Pillars B/C-2  (built as graph/RHI clients)
   └── is the engine seam 014 Diligent implements behind

017 Concurrency → async readback ring (shared w/ 016) + deterministic activation queue
018 Determinism → render-vs-sim residency contract that 017 consumes + audit gate guarding 015/016
019 Networking  → replication = scale path; lockstep = oracle (consumes 018's determinism)
020 Build/Config → single build tree + typed config codegen w/ schema-declared hash residency
```

**Recommended phase order for the workflow:**
1. **Framework-first:** 016 (resource registry + pass contracts) ‖ 017 (readback ring + activation
   queue) ‖ 020-A (build-tree cleanup) ‖ 018-A/B (residency contract + test matrix). These four are
   the foundation; 017's ring and 018's availability-set contract are co-owned with 016.
2. **015 Pillar A** (lighting authority + async exposure) once 016 FR-F-001 + 017 ring land.
3. **014 Phase A → A0 pilot** (engine seam over Diligent; 2-pass dual-backend FLIP proof).
4. **015 C-1** (colored shadows) through the 016 pass contract.
5. **014 pass-by-pass port → 015 B & C-2** as first-class graph/RHI clients.
6. **DLSS / RT / DX12** (014 later phases) last.
- **019 (networking)** and **020-B (config schema)** run as parallel domain tracks; **018** lands its
  audit gate (Group F) last, after A–E are the things it checks.

---

## 3. THE NEXT-SESSION TASK — author a Forge workflow

### Step 1 — pull latest `develop` and build (do this first)
```powershell
# prepend the toolchain every time (KiCad/mingw64 contaminate PATH)
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
git fetch origin
git switch develop ; git pull --ff-only origin develop      # "devel" = the develop branch
# Ensure THIS spec set is on your working branch — see §5 (git state) before building the workflow.
cmake --build build/debug --target luminumbra_server_app     # engine-frontier gate uses build/debug
luminumbra_server_app --smoke                                # sanity: expect 6f008a9f637c40b7, run==replay
```

### Step 2 — author the Forge workflow
Build a **multi-phase Forge workflow** (`.forge/workflows/engine-infra-hardening.yaml` + a
`.forge/tasks/engine-infra-hardening/dispatch.json`) that drives the spec set through forge's
**spec → plan → execute → verify** lifecycle per domain (dogfood-forge-defer-nothing). Shape it on the
existing `engine-framework-roadmap` package (`.forge/workflows/engine-framework-roadmap.yaml`,
`.forge/scripts/run-codex-engine-framework-roadmap.ps1`, `.forge/scripts/validate-*-roadmap.ps1`) as
the template — it is the proven Codex-only pattern in this repo.

Workflow requirements:
- **Phases mirror §2's order** (framework-first → 015-A → 014 pilot → C-1 → port → effects), with the
  DAG dependencies as gating edges. 019 + 020-B as parallel tracks.
- **Per-spec lifecycle:** each spec gets plan → execute → verify; the per-spec Acceptance Criteria
  are the verify gates (they're already written and testable).
- **Universal gates** (every phase): determinism `--smoke == 6f008a9f637c40b7` run==replay; FLIP
  visual parity (`tools/flip_diff.py`) for render-touching work; `--render-benchmark` perf budget;
  **plus the new gates these specs introduce** — 017's p95/p99 main-thread-wait + sync-readback-ban,
  018's determinism test matrix + audit checklist, 020's two-build-tree preflight + artifact manifest.
- **Codex-only routing** (`.forge/config.yaml`: `allowed_spawners:[codex]`, `model: gpt-5.5`).
  **Use `model_reasoning_effort="high"`, NOT xhigh** — xhigh stalls/hangs here (proven this session:
  the xhigh and the file-writing runs wedged at 0 CPU; the read-only `high` run with
  "emit report as final message" completed cleanly). For Codex `exec`: `--sandbox read-only` (or
  `workspace-write` only when it must edit), `-c 'approval_policy="never"'`, `-c
  'model_reasoning_effort="high"'`, and have it **emit deliverables as the final message captured by
  `-o`** rather than writing files via apply_patch (the apply_patch path hung).
- **Validate the workflow before running:** `forge tasks validate <dispatch.json>` and
  `forge workflow validate <workflow.yaml>` (see the roadmap script's verification block).

> Whether to *run* the workflow or just author + validate it is the owner's call — this handoff scopes
> authoring it. Confirm before kicking off a long multi-spec execution.

---

## 4. Build / test / verify quick reference
- **Toolchain:** prepend `C:\msys64\ucrt64\bin` to PATH every build/run. Run GPU client + builds via
  the **PowerShell** tool (Bash tool is sandboxed — exit 127 / silent compile fails).
- **Two build trees (see spec 020 — this is the F7 cleanup):** `cmake --build build` → `build/bin`;
  the engine-frontier gate uses **`build/debug`** via `cmake --build --preset debug`. Build the tree
  you test. Spec 020 FR-A-007 will correct the stale "build both trees" text in 014/015.
- **Determinism:** `luminumbra_server_app --smoke` must stay `6f008a9f637c40b7`, run==replay.
  `--smoke-moving` is the streaming-arrival oracle.
- **Visual gate:** `python tools/flip_diff.py --selftest`; bless with `tools/golden_update.py`
  (refuses overwrite w/o `--force`); goldens under `build/debug/showcase/`.
- **Perf:** `--render-benchmark` (p50 frame/gpu). Spec 017 adds p95/p99 main-thread-wait fields.
- **Engine-frontier gate:** `.forge/scripts/validate-engine-frontier.ps1 -Mode Build` then test/gate
  modes (needs `-Mode Build` first; doesn't auto-build).

## 5. Git state (important — so the specs aren't lost)
- The entire spec set (016–020 + 014/015 revisions + disposition + critique + this handoff) is
  **committed on `feat/polyglot-audit-roadmap`** (this session, no push). HEAD before this work:
  `86058583`.
- **The new session pulls `develop`.** These specs are NOT on `develop` yet. Before authoring the
  workflow, get them onto your working branch — e.g. merge/rebase `feat/polyglot-audit-roadmap` onto
  `develop`, or cherry-pick this commit. Don't author a workflow that references specs absent from the
  checked-out tree.
- The working tree has lots of **pre-existing** untracked junk (build artifacts, `.forge` logs,
  `vendor/`, `crop_*.png`, etc.) — NOT this work. Don't `git add -A`; stage precisely.

## 6. Forge gotchas (memory-backed)
- `forge spec` state-sync is blocked by a forward-only phase guard once past `spec` phase (filed:
  internal-org/forge#1978). The specs stand as docs regardless; don't fight the guard.
- Codex spawners under-deliver on intricate shaders (do those inline); they're fine for spec/plan
  drafting and mechanical work — which is what this workflow's planning phases are.
- Single-PC: no two-box LAN; Steam SDR transport can't be locally validated (spec 019 NG-3 / OQ-3) —
  019's scale validation is local multiprocess soak over TCP + GNS.

## 7. Pointers
- Critique: `.forge/critique-engine-infrastructure-framework-20260626-200421.md`
- Dispositions (finding→spec, anchor corrections): `.forge/reports/critique-dispositions-engine-infra-20260626.md`
- Specs: `docs/specs/{014,015,016,017,018,019,020}-*/spec.md`
- Workflow template to copy: `.forge/workflows/engine-framework-roadmap.yaml` +
  `.forge/scripts/run-codex-engine-framework-roadmap.ps1`
