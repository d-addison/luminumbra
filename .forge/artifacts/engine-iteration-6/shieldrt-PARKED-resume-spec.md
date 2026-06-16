# SHIELD-RT far-field — PARKED (2026-06-16), resume-spec for iteration 7

Parked per the 5-lens forge-critique consensus
(`.forge/critique-remaining-blockers-20260616.md`): low perceived-fidelity ROI for a
photography game, ~45% of iter-6 commits already spent, still dormant/blocked, and the
fullscreen march cost is an unvalidated wall. Effort redirected to the near field
(terrain material variation + GPU grass). The code is **committed, dormant, green** —
nothing to clean up.

## Current state (commit 7772c80, flag `kEnableExperimentalFarFieldGpuRaymarching=false`)
- The pass RUNS clean flag-on: `farlod_horizon_smoke` → 5/5 stations, regions 40/40,
  GL-clean, exits 0. Validated end-to-end except the far-field's own rendered output
  (the field never builds in time — see below).
- Proven in isolation (manual;gpu ctest): `ShieldRtFarFieldParityGpu` (tracer == ground
  truth), `ShieldRtFarFieldGbufferGpu` (G-buffer encoding), `ShieldRtFarFieldMaxMipGpu`
  (GPU max-reduction == CPU ref).
- Landed: step 1 far-pixel early-out (depth-copy + discard), step 3 async region rebuild,
  two bug fixes (async-attach wiring; teardown use-after-free drain).

## Why it's blocked (root causes, empirically isolated)
1. **Assembly ~115 s.** `assemble_field` runs 49 `BuildPristineFarLodTile` (7×7 regions
   × 128² ≈ 800K erosion-heavy worldgen samples) in ONE job on ONE worker. (The march
   was a RED HERRING — `u_maxSteps` 512→48 was identical because the field never built.)
2. **Even fixed, the fullscreen march is unvalidated and likely over budget** at 300 fps /
   3840×1600 — the early-out helps least on horizon/sky frames (the ones that matter).
3. By construction it **augments, not replaces** the flat far-LOD mesh slabs and writes a
   single hardcoded olive albedo — cannot look BF4-grade without shading-parity work.

## Resume architecture (consensus across perf/correctness/architecture lenses)
1. **Extract a shared cache-backed `FarLodHeightProvider`** (`luminumbra_common/world`)
   that BOTH FarLodSystem (meshes) and the raymarch pass consume: try
   `FarLodStore::load_tile` then `BuildPristineFarLodTile`, memoized. Fixes the duplicate
   erosion cost (the 115 s), the cold-cache, AND a latent correctness bug — the pass today
   never consults the store so it can't see EDITED tiles → terraformed far terrain
   disagrees with the mesh. ONE source of truth. (Delete `assemble_field`'s direct loop.)
2. **Clipmap band-update, not wholesale rebuild.** A 512 m crossing re-exposes ≤13 tiles;
   update only the new band (`glBufferSubData` / the inc2b `glTexSubImage2D` quadrant idea)
   and recompute only affected max-mip cells. Kills the per-crossing full SSBO re-upload +
   10-dispatch mip rebuild on the GL thread.
3. **If parallelizing:** ONE `dispatch_batch` of N tiles (one handle → drain stays
   correct), workers write DISJOINT slices, integrate ALL-OR-NOTHING (a partial base under
   the global conservative max-mip makes rays punch through mountains). Add FarLodSystem's
   `epoch` discard for camera-move restarts; honor F1/F2 tier-by-distance + the store, or
   the near↔far seam guarantee is false. Reset `m_have_cache_key` in `drain()`; cover the
   3rd teardown (`main_client.cpp:1758` runtime-boot `clear_world`).
4. **Re-measure the march in a RELEASE build with a real field** before any enable-by-
   default. Make `u_maxSteps` measured/justified (hardcoded 512). Add half-res +
   depth-aware nearest-of-4 upsample + a horizon-line cull. Then perf gate + the
   FarLodHorizon parity/seam gate (MAJOR #9), then enable.
5. **Shading parity:** route the far-field through the same triplanar/material/slope path
   as the near terrain (and consider REPLACING the slabs, not just augmenting) so distance
   reads as continuous realistic terrain.

## Do NOT freeze a "Wave-B substrate API" on this
GPU grass + ocean are independent of the far-field (false coupling). Only the depth-aware
upsample is genuinely shareable — extract it after a 2nd real consumer exists (YAGNI).

## Files
`src/luminumbra_client/rendering/passes/ShieldRtFarFieldPass.{h,cpp}`,
`src/luminumbra_client/rendering/FarLodSystem.{h,cpp}` (the proven incremental+cache+epoch
pattern to mirror), `src/luminumbra_common/world/FarLodStore.h`,
`.forge/artifacts/engine-iteration-6/shieldrt-inc2b-plan.md` (full history).
