# Iteration 6 — Remaining blockers & TODOs (critique target, 2026-06-16)

Critique goal: decide the BEST approach + sequencing across all remaining work.
Branch `feat/polyglot-audit-roadmap`. Standing owner principles: most powerful /
composable / scalable-under-load engine; BF4/BF1 visual-fidelity floor; ~300 fps on
RTX 5070 Ti @ 3840x1600; do not sacrifice beauty for perf (within reason); commit
locally, never push. Autonomous mandate active.

## DONE this session (committed, dormant where experimental)
- Isolation/layer render mode v1 (backdrop + layer suppression + `-Mode IsolationLayer`
  gate). ctest 254/254, byte-stable.
- Terrain fidelity: pass-#2 unsharp contrast (detail 1.99->3.3); measured state recorded.
- SHIELD-RT far-field: step 1 (far-pixel early-out), step 3 (async region rebuild),
  + fixed two real bugs (async-attach wiring; teardown use-after-free). All DORMANT
  (`kEnableExperimentalFarFieldGpuRaymarching=false`). Default ctest green.

## BLOCKER 1 — SHIELD-RT far-field heightfield ASSEMBLY (~115 s) [task #15, biggest]
The far-field pass now RUNS clean (flag-on: 5/5 stations, regions 40/40, GL-clean,
exits 0) but cannot RENDER in-scenario because building its camera-centered heightfield
is ~115 s. Root cause (empirically isolated; the march was a RED HERRING — `u_maxSteps`
512->48 was identical): `assemble_field` calls `BuildPristineFarLodTile` for 49 tiles
(7x7 regions x 128^2 ~= 800K worldgen samples), and **A2 erosion (enabled this iteration)
makes every sample walk the eroded surface** -> ~2.3 s/tile x 49 ~= 115 s, currently in a
SINGLE job (one worker).
Candidate approaches (not mutually exclusive):
  1. **Incremental per-tile PARALLEL build** — dispatch 49 small jobs; the 16-worker
     JobSystem builds them concurrently (~115 s -> ~8 s wall-clock). Full fidelity, no
     seam change. Accumulate on CPU, upload+mip once when the center completes (prior
     field keeps rendering; eventual-consistency on camera move). Most code.
  2. **Reuse FarLodSystem tiles / LMR1 cache** — FarLodSystem streams these exact F1
     tiles for its meshes. It `load_tile`s from the LMR1 store but persistence of
     pristine tiles is unverified; cold in `--auto-create-world`. Avoids duplicate
     worldgen when warm.
  3. **Non-eroded far-field heightfield** — erosion is the per-sample cost and is
     largely invisible at 512-1536 m. BUT the far-LOD MESH uses eroded heights, so a
     non-eroded raymarch source risks a near<->far seam mismatch (raymarch only fills
     gaps beyond the mesh — Decision A augment-not-replace). Visual/consistency call.
  4. **Reduce region radius / far-LOD resolution** — fewer tiles (3x3=9 -> ~21 s, or
     coarser sampling), less coverage / lower far detail.
After assembly is real-time: the FarLodHorizon-style flag-on PARITY/SEAM gate (MAJOR #9)
can finally run; then the march cost at fullscreen 3840x1600 must be re-measured WITH a
real field (was never validated — only the stall was); then step 2 (half-res + depth-aware
upsample) if needed; then perf+visual gate; then enable-by-default. The early-out (step 1)
is committed but UNVALIDATED in-context.
Open meta-question: is the SHIELD-RT far-field worth continued investment now, or should
it stay dormant while higher-ROI visual work (Wave B) proceeds? (Sunk-cost check.)

## BLOCKER 2 — Terrain visual fidelity to BF4/BF1 floor [task #17]
12/48 cells flag `LOW_TEXTURE_DETAIL` (all summer-clear terrain). Near-ground
`ground_detail_energy` ~3.3 vs the provisional 8.0 floor. KEY finding: the metric
(mean-abs-Laplacian of ground-region luma) **conflates texture with LIGHT LEVEL** — the
same terrain scores 2.5-3x lower at dusk (gl~50) than noon (gl~100); dusk flags are a
brightness artifact, not texture. Levers:
  (a) push albedo/normal contrast further (aesthetic; haloing risk; over-sharpening can
      GAME the Laplacian while looking worse — needs owner's eye),
  (b) brightness-aware metric (`detail/max(luma,eps)`) + near-weighted ROI — honest
      recalibration of a provisional gate (additive companion metric is safe; threshold
      change is a gate decision),
  (c) higher-res/higher-contrast detail textures (asset work — the real floor lift),
  (d) macro material variation by slope/height (WORLDGEN -> deliberate world_hash bump),
  (e) GPU grass (Wave B) replaces billboard tufts.
Contention: (a) alone risks worse-looking higher-scoring images; recommend (b)+(c).

## TODO 3 — Phase 0.3 quiet-machine perf re-bless [task #3]
Carried i4->i5->i6 debt. `run-release-perf-lane.ps1 -Bless` on a quiet machine to capture
an honest post-iteration-5 RELEASE baseline before atmospheric/SDF perf work. Gives iter-6
budgets a real floor. Needs a quiet machine (perf measurement integrity).

## TODO 4 — Wave B visual-debt reimplementation (not started)
GPU grass (continuous scene-lit turf, compute density + indirect draw), volumetric clouds
tier-2 (Nubis raymarch), aurora curtains (volumetric), ocean/water waves (Gerstner/FFT).
Each discharged ONLY by a passing WorldVisualSweep re-run. Wave B was planned to consume
Wave-A froxel/raymarch substrate — but that substrate is the (stalled) far-field. Does
Wave B actually need the far-field substrate, or can GPU grass / ocean proceed independently?

## TODO 5 — Wave C multi-anchor streaming + server scale (not started)
Erosion (the worldgen half of Wave C) is DONE + enabled. Remaining: multi-anchor lockstep
streaming, per-anchor budgets, HeadlessServerTick multi-anchor mode. Scalability principle.

## TODO 6 — Wave D closeout
Full gate sweep + WorldVisualSweep dual-bias + Endurance300 (+storm variant) + forge verify
+ handoff with iteration-7 inputs. Verify iter-5 foliage/aurora BLOCKs discharged by passing
sweeps, not reclassified.

## Cross-cutting constraints
- Determinism: world_hash currently `f17726d44054d133` (after A1/A2 bumps). Render-only work
  must not touch it; worldgen changes (terrain lever d) = deliberate bump in own commit.
- Dormant experimental code is accumulating (far-field behind a compile flag). Risk of bit-rot.
- Composability: the half-res raymarch + upsample was meant to be the shared substrate for
  Wave B clouds/aurora/ocean (substrate API freeze = MAJOR #10) — but it's not real yet.
- Everything render-side must keep WorldVisualSweep + RenderHealth green at 3840x1600.

## Key files for reviewers
- `.forge/artifacts/engine-iteration-6/shieldrt-inc2b-plan.md` (far-field plan + all findings)
- `.forge/artifacts/engine-iteration-6/terrain-fidelity-plan.md` (terrain lever analysis)
- `src/luminumbra_client/rendering/passes/ShieldRtFarFieldPass.{h,cpp}` (the pass)
- `src/luminumbra_client/rendering/FarLodSystem.{h,cpp}` (incremental tile build + LMR1 cache pattern)
- `src/luminumbra_common/world/FarLodStore.h` (load_tile/save_tile)
- `tools/visual_critique.py` (detail_energy metric, FIDELITY_FLAGS)
- `.claude/plans/engine-frontier-handoff-enchanted-sunbeam.md` (the iteration-6 program/wave plan)
