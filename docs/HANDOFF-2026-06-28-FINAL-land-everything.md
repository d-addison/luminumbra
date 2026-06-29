# Handoff — 2026-06-28 FINAL (v2): all remaining work

Branch `feat/polyglot-audit-roadmap`. The merged engine-frontier roadmap (Codex GPT-5.5 High
audited, owner-approved) is in `docs/specs/014..020/` + memory `engine-frontier-merged-roadmap`.
THE single list of what's LEFT and exactly what each piece needs. ~30 commits + 4 workflows this
session; 11 TODOs closed; determinism held `6f008a9f` throughout.

## DETERMINISM LAW (load-bearing — never break, never relearn)
`world_hash` is **BUILD-MODE-DEPENDENT**: **DEBUG `build/debug/bin/luminumbra_server_app --smoke
== 6f008a9f637c40b7`** (the canonical gate — gates run build/debug); RELEASE is a different
`ea9a0121d13bc3bd`. Both run==replay. **Always verify the DEBUG server smoke.** Moving oracle:
`--smoke-moving == cf501b6676d67249` (per-tick residency CONVERGES). Per-tick: `--avail-trace`
(static MATCH; moving convergent). Frontier gate `validate-engine-frontier.ps1 -Mode All` now
includes ReadbackDiscipline + DeterminismAudit; FR-D matrix `validate-determinism-matrix.ps1` is
per-build-baseline aware (run via `& script.ps1`, NOT `powershell -File` which mangles `@()` args).

## LANDED this session (all DEBUG --smoke 6f008a9f)
0.1 preview UAF + churn fix; load-hang stopgap (SDF guard + watchdog); 018-B residency contract test;
`--avail-trace` per-tick gate; 017-D wait instrumentation (moving p99 239ms); build-mode hash discovery
+ matrix per-build fix. Workflow rounds: 0.3 create-world UI polish; 0.4 020-B config codegen
(byte-identical) + 020-A manifest END-TO-END; 019-C1 net soak (ReplicationScale 4/4); 018 C/E/F gates;
016 FR-D shader reflection (9/9); preview far-field anchored to diorama centre + headless capture hook
(`--preview-live`); near-field full-SDF preview LOD; 015 Pillar A dedicated moon radiance channel
(`3aa9740d`, byte-identical, C++-tunable). 017-B design fully specified
(`docs/specs/017.../activation-queue-design.md`).

---

## ⚠ THE BLOCKER that gates ALL visual work — fix this FIRST for any re-bless
**Headless IN_GAME-render capture HANGS** (memory `headless-ingame-render-capture-stall`). The IN_GAME
game-render capture path (`--scene-config`, `--frame-scan`) loads the world fine, renders ONE ~2.4s
frame, starts frame 2, logs the doline scan (`main_client.cpp:4349`), then BLOCKS DEAD (5+ min, no
frames). Confirmed noon AND night, 4 runs. NOT a harness param / not the CPU sky-LUT alone. The
`--ui-screenshot --preview-live` path works (different render path = preview backbuffer). **This blocks
every headless scene/shader re-bless** (Pillar A moon tuning, 015 shaders, visual gates). FIX = a
focused engine debug: instrument the first/second IN_GAME frame to find the main-thread block (candidates
after the doline scan); tools = `LUMINUMBRA_JOB_WATCHDOG=1`, the crash/hang breadcrumbs, TOD bisect,
or route scene capture through an offscreen FBO like the working preview path. Until fixed, visual
re-bless must be done INTERACTIVELY (owner runs the client).

---

## REMAINING — prioritized, with what each NEEDS

### 1. 017-B activation queue (THE determinism keystone — headless-verifiable, no render needed)
Fixes the world-load hang + moving-lag (p99 239ms); unblocks 015 A-T06 + 016 FR-E. **Fully specified**
in `activation-queue-design.md` §Step 1: the streaming LOD0-promotion backfills the sim-truth SDF
*inside the meshing job* (`SHIELD_WorldSystem.cpp:4154-4162`, published `:4262-4274`); move that into a
GENERATION job that publishes `sdf_data` on the MAIN thread (preserve the no-off-thread-write constraint
`:4156-4160`) BEFORE meshing, then build the deterministic tick-keyed activation queue. Gate EACH step:
static `--smoke 6f008a9f` byte-identical + static `--avail-trace` identical + `--smoke-moving` cf501b66
+ 017-D p99 drops. Opus inline (NOT fan-out — diverges). Dedicated focused session.

### 2. Pillar A finish (moon channel DONE; rest gated on the render hang OR interactive)
- **Moon CALIBRATION/re-bless** — taste call on `m_moonRadiance` (default 0.40,0.52,0.92). Needs a night
  render (blocked by §the-blocker) or the owner's interactive eye.
- **Photo manual EV (A-T07)** — controls live in `data/ui/photo_mode.rml` (RML, not yet wired). Read
  shutter/ISO/aperture in C++ (via Rml_UIManager), EV = log2(N²/t) − log2(ISO/100) → exposure mult →
  override `m_pillarA_exposure` (the A-T05b exposure seam, RenderContext.exposure → u_exposure) when
  photo manual mode is active. Verifiable at noon (exposure affects all TOD) once §the-blocker is fixed.
- **True-midnight (TOD 0.5) re-bless.** A-T06 GPU auto-exposure stays BLOCKED on 017-A ring.

### 3. 017-A async readback ring (unblocks 015 A-T06 + 016 FR-E foliage)
N-buffered fenced PBO/SSBO ring; submit returns immediately; stale-safe; RHI-shaped. GL-bound (build +
mock-backed unit test headless). Additive, default-unused → low risk. Before/with 017-B.

### 4. 016 framework remainder
- **FR-C declarative frame graph** — replace the scripted render call sequence; verify via in-process
  A/B parity (the RenderContext seam supports it — NOT the flaky `--frame-scan`). Additive, byte-identical.
- **FR-E async-readback** — retire foliage blocking readback (`FoliagePass.cpp ~:793`) onto 017-A NOW;
  the SDF readback (`RenderPipeline.cpp ~:4321/4328`) after 017-B.

### 5. 015 Pillars C-1 / B / C-2 (shaders — gated on §the-blocker for re-bless)
C-1 colored shadow maps (after the 016 pass contract, before B); B froxel volumetrics; C-2 OIT/refraction
(gated on 016 + 014 pilot). Opus inline shaders; visual re-bless.

### 6. 014 RHI/Diligent pilot (large, later) — multi-week, needs GPU dual-backend FLIP. Gate = 016 seam +
FR-D reflection (DONE). Defer.

### 7. Small loose ends
- **020-A FR-B-003** schema↔gameplay-constant static_assert (schema↔registry freshness IS done).
- **019-C1 over-the-wire 32-client** (in-process ctest covers 32; wire at N=4 — single-PC limit).
- **Preview overlap-band z-fight** interactive check (48–90m), the far-vista is otherwise confirmed.
- Re-apply the owner's stashed `validate-engine-frontier.ps1` edit (`stash@{0}`: a `MovingHitch` mode +
  WeatherVisual/TimeOfDaySweep time bumps) — add `MovingHitch` to the current ValidateSet.

## GOTCHAS + CONSTRAINTS (current)
- Prepend `C:\msys64\ucrt64\bin` to PATH; build the tree you test (build/debug for the gate). No
  libtsan/libasan (sanitizers infeasible) → `--smoke`/`--avail-trace` are the oracles.
- §the-blocker: headless IN_GAME render capture hangs frame 2; `--ui-screenshot --preview-live` works.
- `--frame-scan` 255s = the same IN_GAME hang, not a regression. Render byte-identical is argued
  by-construction + in-process A/B parity.
- NEVER `Stop-Process` clients by a `*build\debug\bin*` PATH filter — it kills WORKTREE agents' gate
  clients too. Kill by PID. Night `--scene-config` client also hangs on exit (kill before relink).
- Worktrees lack vendored deps → agents `git submodule update --init --recursive` + robocopy vendor/*
  from the main checkout. Never `git worktree remove --force` (vendor hazard); workflow worktrees under
  `.claude/worktrees/wf_*` are left on disk (prune carefully later).
- WORKFLOW PATTERN that worked: file-disjoint additive domains → opus worktree agents with explicit
  build+gate+structured-output contracts + honest partial reporting; integrate by cherry-pick + re-verify
  on main yourself; resolve shared-file conflicts by combining append-only additions. Do NOT fan out
  determinism-critical interdependent code (017-B) or shaders — they diverge; do those inline.

## TASK BOARD
#1 preview UAF ✅ · #2 Pillar A (moon channel done; tuning/EV/midnight remain, gated) · #3 UI ✅ ·
#4 config ✅ · #5 Wave 1 (stopgap/018-B/trace/017-D done; 017-B cut remains) · #6 Wave 2 (018 C/E/F +
016 FR-D done; 015 A-T06 / 016 FR-C/FR-E / 015 C-1 remain) · #7 Wave 3 (019-C1 done; 014 + 015 B/C-2
remain) · #8 preview far-field + near-field ✅
