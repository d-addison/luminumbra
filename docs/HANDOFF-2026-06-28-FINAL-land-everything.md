# Handoff — 2026-06-28 FINAL: land everything remaining

Branch `feat/polyglot-audit-roadmap`. The merged engine-frontier roadmap (Codex GPT-5.5 High
audited, owner-approved) is in `docs/specs/014..020/` + memory `engine-frontier-merged-roadmap`.
This handoff is the single list of what's LEFT and exactly what each piece needs.

## DETERMINISM LAW (load-bearing — never break, never relearn)
`world_hash` is **BUILD-MODE-DEPENDENT**: **DEBUG `build/debug/bin/luminumbra_server_app --smoke
== 6f008a9f637c40b7`** (the canonical gate — the gates run build/debug); RELEASE deterministically
gives a different `ea9a0121d13bc3bd`. Both run==replay. **Always verify the DEBUG server smoke.**
Moving oracle: `--smoke-moving == cf501b6676d67249` (per-tick residency CONVERGES, not tick-identical).
Per-tick gate: `--avail-trace` (static MATCH; moving convergent). Build-mode axis: the FR-D matrix
(`validate-determinism-matrix.ps1`, per-build baselines) + the frontier gate
(`validate-engine-frontier.ps1 -Mode All`, which now includes ReadbackDiscipline + DeterminismAudit).

## LANDED this session (≈25 commits + 3 workflows; all DEBUG --smoke 6f008a9f)
- Wave 0.1 preview UAF fix + churn fix; load-hang stopgap (SDF guard + watchdog); 018-B residency
  contract test; per-tick `--avail-trace` gate; 017-D wait instrumentation (moving p99 239ms data).
- Determinism corrections: discovered the build-mode hash split; reverted a wrong re-pin; fixed the
  FR-D matrix per-build baseline.
- Workflow round 1 (4 items): 0.3 create-world UI polish; 0.4 020-B config codegen (byte-identical) +
  020-A manifest; 019-C1 net soak (ReplicationScale 4/4); 018 C/E/F determinism gates.
- Workflow round 2 (3 items): preview far-field anchored to diorama centre; 020-A END-TO-END (wired
  into MaterialVisual/RenderHealth); 016 FR-D shader reflection (9/9 tests).
- 017-B design: precise code-grounded decoupling plan in
  `docs/specs/017-engine-concurrency-async-execution/activation-queue-design.md`.

---

## REMAINING — prioritized, with what each NEEDS

### 1. 017-B activation queue (THE keystone — fixes the world-load hang + moving-lag; unblocks 015/016)
- **Needs:** a dedicated focused session, opus inline (NOT fan-out — determinism-critical, intricate).
- **The cut is fully specified** in `activation-queue-design.md` (§Step 1). Precise: the streaming
  LOD0-promotion backfills the full sim-truth SDF *inside the meshing job* (`SHIELD_WorldSystem.cpp:
  4154-4162`, staged to `pending_sdf_data` `:4190-4198`, published `:4262-4274`). Step 1 = move that
  SDF generation into a GENERATION job that publishes `sdf_data` on the MAIN thread (preserving the
  no-off-thread-write constraint, `:4156-4160`) BEFORE meshing; then the per-tick `wait_for_streaming_jobs`
  meshing wait leaves the sim critical path. Then build the deterministic tick-keyed activation queue.
- **Gate every increment:** static `--smoke == 6f008a9f` byte-identical + static `--avail-trace`
  identical + `--smoke-moving` converges `cf501b66` + the 017-D p99 wait drops. If the static hash
  moves, revert.
- Also folds in: the EnsureSurfaceReadyNear unbounded waits (the interactive load-hang root); the
  017-A ring (below); retiring the SDF readback (016 FR-E SDF part).

### 2. Visual waves — NEED OWNER GPU to re-bless (can't bless headless)
- **0.2 Pillar A finish** (memory `spec-015-pillar-a-plan`): the **dedicated moon radiance channel**
  (Codex C5 — give the moon its own ctx field/uniform, `lighting_pass.frag` refactor + NIGHT re-bless);
  **wire photo manual EV** (A-T07 — `main_client.cpp:7121` stores shutter/ISO/aperture as metadata only;
  add EV→`u_exposure` math + calibration); a **true-midnight (TOD 0.5) re-bless** to tune cool moonlight.
  Opus inline (all shaders). A-T06 GPU auto-exposure stays BLOCKED on 017-A.
- **015 C-1 colored shadow maps** (glass RGB-absorption + shadow color attachment + tinted light) —
  lands after the 016 pass/resource contract, BEFORE Pillar B. Opus inline shaders + GPU re-bless.
- **015 Pillars B (froxel volumetrics) + C-2 (OIT/refraction)** — gated on 016 + the 014 pilot. Shaders.
- Gate: deliberate re-bless (NOT byte-parity) across the scene set + `visual_critique.py --strict`;
  `--smoke` unaffected (render-only); 018 DeterminismAudit asserts exposure/froxel stay render-only.

### 3. 017-A async readback ring (unblocks 015 A-T06 + 016 FR-E foliage)
- N-buffered fenced PBO/SSBO ring; submit returns immediately; stale-safe consumers; RHI-shaped.
  GL-bound (build the ring + a mock-backed unit test headless; GL backend compiles). Additive,
  default-unused → low determinism risk. Do BEFORE/with 017-B (FR-E SDF retirement needs 017-B).

### 4. 016 framework remainder
- **FR-C declarative frame graph** — replace the scripted render call sequence with a declarative pass
  list; verify via in-process A/B parity (the 016 RenderContext seam supports it) — NOT the flaky
  `--frame-scan` (255 under load). Render-core, additive, byte-identical. Opus inline or careful agent.
- **FR-E async-readback migration** — retire the foliage blocking readback (`FoliagePass.cpp ~:793`)
  onto the 017-A ring NOW; the SDF readback (`RenderPipeline.cpp ~:4321/4328`) after 017-B.

### 5. 014 RHI/Diligent pilot (large, later)
- Multi-week; needs the GPU for dual-backend FLIP parity. Gate = the 016 RenderContext/registry seam +
  FR-D reflection (DONE) for the pilot pair (DebugView + lighting/SSAO), single-source HLSL. Defer.

### 6. Loose ends (small, mostly headless)
- **Preview far-field VISUAL confirm — RESOLVED this session** (`860230b1`): the headless capture hook
  `--ui-screenshot world_creation --preview-live --preview-weather rain` waits for `world_ready()` +
  settle, then captures the LIVE diorama. Verified non-black (3840×1581, mean luma 77, 100% non-black)
  showing UI + snow-capped terrain + centre-anchored far field + rain (control without the flag = black).
  CI can now visually-gate the create-world diorama. (A drag-orbit interactive pass is still nice-to-have
  for the overlap-band z-fight check, but the vista/UI/rain are confirmed.)
- **Near-field LOD render fidelity (#8) — LANDED this session** (`a21e088e`): decoupled
  `render_lod0_radius` from `collision_radius`; the preview now meshes all rings at full-SDF LOD0
  (caves/overhangs). Behind a default param → game LOD byte-identical (`--smoke 6f008a9f`). Side effect:
  ~80 preview-only colliders (harmless, bounded, not hashed; pass null physics to preview update() if ever a concern).
- **020-A FR-B-003** — the schema↔gameplay-constant `static_assert` (schema default vs
  CreatureBrainSystem.h / ThirstSystem.h etc.) was deferred; the schema↔registry freshness gate IS done.
- **019-C1 over-the-wire 32-client** — in-process ctest covers 32; wire validated at N=4 (single-PC).
  Full 32-over-wire needs a second box (not available).
- **0.3 UI** — headless ui_smoke passes; a visual eyeball (or the new capture hook) for layout/drag/rain.

## GOTCHAS + CONSTRAINTS (don't relearn)
- Prepend `C:\msys64\ucrt64\bin` to PATH for every build/gate. Build the tree you test (build/debug for
  the gate). No `libtsan`/`libasan` → sanitizers infeasible; `--smoke`/`--avail-trace` are the oracles.
- `--frame-scan` exits **255 under CPU load** at a deterministic point — a harness quirk, NOT a crash/
  regression. Render byte-identical is best argued by-construction + in-process A/B parity.
- NEVER `Stop-Process` clients by a `*build\debug\bin*` PATH filter — it also kills WORKTREE agents'
  gate clients. Kill by PID.
- Worktrees lack vendored deps → agents must `git submodule update --init --recursive` + robocopy
  vendor/* from the main checkout to configure. Never `git worktree remove --force` (vendor hazard);
  the workflow worktrees under `.claude/worktrees/wf_*` are left on disk — prune carefully later.
- A pre-existing OWNER edit to `validate-engine-frontier.ps1` (a `MovingHitch` mode + WeatherVisual/
  TimeOfDaySweep capture-time bumps) is in `git stash@{0}` — re-apply when ready (add `MovingHitch` to
  the current ValidateSet; the file was heavily rewritten by the 020-A/018 gate work).
- Workflow pattern that WORKED: file-disjoint additive domains → opus worktree agents with explicit
  build+gate+structured-output contracts + honest partial/caveat reporting; integrate by cherry-pick +
  re-verify on main yourself (don't trust agent claims); resolve shared-file conflicts by combining
  append-only additions. Do NOT fan out determinism-critical interdependent code (017-B) — it diverges.
