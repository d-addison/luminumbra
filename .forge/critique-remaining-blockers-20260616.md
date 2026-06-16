# forge-critique — Iteration 6 remaining blockers & best approach (2026-06-16)

5-lens devil's-advocate pass (perf/scalability · visual-fidelity · correctness/
determinism · scope/sequencing/ROI · architecture/maintainability) on
`.forge/artifacts/engine-iteration-6/remaining-blockers.md`. Read-only analysis of
the live code. **Strong cross-lens consensus** on the headline call.

## HEADLINE (all 5 lenses agree): PARK the SHIELD-RT far-field; redirect to the near field
- **Sunk cost (scope lens):** ~17 of the last ~38 iteration-6 commits (~45%) are
  far-field, yet it is still DORMANT, still does NOT render in-scenario (115 s
  assembly), and its real-field fullscreen march cost has NEVER been measured. The
  cost estimate has been wrong ~1000x twice (0.07 ms bench → "7.5 ms" → observed
  ~15-17 s/frame → re-diagnosed as 115 s assembly). ~8-12 more comparable increments
  remain (assembly → re-measure march → half-res → temporal → parity gate → perf gate
  → visual gate → SHADING parity → enable). Near-zero estimation confidence.
- **Visual ROI (fidelity lens):** the far-field writes a single hardcoded olive
  `gAlbedoRoughness=vec4(0.4,0.45,0.3,0.9)` (ShieldRtFarFieldPass.cpp:179), NOT routed
  through the triplanar/material path, and "augments not replaces" — it fills only the
  gaps BEYOND the flat far-LOD mesh slabs and leaves the slabs untouched. It cannot
  look BF4-grade by construction. For a zen *photography* game the player composes
  near/mid terrain + foliage + sky + water; the 512-1536 m horizon is backdrop the
  FarLodSystem mesh already covers (non-void).
- **Perf (perf lens):** even with the early-out (step 1) the discard benefit is ~0 on
  the very horizon frames that matter (sky+far dominate), and `u_maxSteps=512` grazing
  rays survive; honest horizon-shot cost ~3-4 ms = the whole 300 fps budget for ONE
  pass. Enable-by-default at 300 fps needs half-res AND temporal AND a horizon cull AND
  a clipmap — none exist.
- **Decision:** keep it committed/dormant/green (it is clean). Write a resume-spec.
  Do NOT block higher-ROI visual work on it. (Architecture lens: put an explicit
  decision gate on the dormant flag so it stops accreting unvalidated commits.)

## If the far-field is EVER resumed (iter-7), the right architecture (consensus)
1. **Extract a shared cache-backed `FarLodHeightProvider`** (luminumbra_common/world)
   that BOTH FarLodSystem (meshes) and the raymarch pass consume — try
   `FarLodStore::load_tile` then `BuildPristineFarLodTile`, memoized. The pass today
   re-runs worldgen for 49 tiles and never touches the LMR1 cache (assemble_field,
   ShieldRtFarFieldPass.cpp:448-463) → duplicate erosion cost (the 115 s) AND a latent
   correctness bug: it can't see EDITED tiles, so terraformed far terrain disagrees
   with the mesh. One source of truth fixes duplication + stall + the edited-tile bug.
2. **Clipmap band-update, not wholesale 49-tile rebuild** (the inc2b glTexSubImage2D
   quadrant idea that was abandoned). A 512 m crossing re-exposes ≤13 tiles, not 49;
   stop the per-crossing full SSBO re-upload + 10-dispatch mip rebuild on the GL thread.
3. **If parallelizing the build:** ONE `dispatch_batch` of N tiles (one handle → drain
   stays correct), workers write DISJOINT slices, integrate ALL-OR-NOTHING (a partial
   base under the global conservative max-mip makes rays punch through mountains). Add
   FarLodSystem's `epoch` discard for camera-move restarts; honor F1/F2 tier-by-distance
   + the store, or the "no seam" claim is false. (correctness lens — these are BLOCKs.)
4. **Re-measure the march in RELEASE with a real field** before any enable-by-default.

## NEAR-FIELD work, sequenced by perceived-fidelity ROI (the redirect)
1. **Fix the terrain metric FIRST (cheap, safe, unblocks honest iteration).** The 8.0
   `LOW_TEXTURE_DETAIL` Laplacian floor conflates texture with LIGHT LEVEL (same terrain
   scores 2.5-3x lower at dusk than noon) and rewards high-freq noise (the opposite of
   the Frostbite look; the unsharp lever games it toward haloing). Add a brightness-
   normalized companion metric `detail/max(ground_luma, eps)` + restrict the absolute
   floor to the near-ROI (down35). ADDITIVE (new flag), gate-safe, reversible. Tune
   `eps` against black-frame + noise-frame fixtures (test_visual_critique.py).
2. **Render-side slope/height macro material blend in `g_buffer.frag`** — the SINGLE
   biggest visible deficit is the uniform-olive single-material field (no slope/height
   blend exists; grass dominates everywhere). Mix the already-loaded Stone/Soil/Grass/
   Sand triplanar layers by `WorldNormal.y` (slope) + `WorldPos.y` (height) with noise-
   jittered boundaries. **RENDER-ONLY — no new assets, no world_hash bump** (the plan
   wrongly classified this as a worldgen change). Improves every terrain cell at every
   time of day.
3. **GPU grass (Wave B)** — continuous scene-lit turf replacing billboard tufts;
   biggest near-field photographic win; closes the foliage debt; **independent of the
   far-field substrate** (false coupling). Compute density + indirect draw.
4. **Back off / soft-knee the unsharp lever** (`kDetailGain=2.2` hard-clamps overshoot
   → haloing; "higher score, worse image"). Owner's eye.
5. **Higher-contrast/higher-res detail textures** (asset work — the genuine near floor).
6. **Ocean/water waves (Wave B)** — Gerstner/FFT; prime photo subject; independent.
7. **Volumetric clouds/aurora (Wave B)** — sky is a photo subject; build STANDALONE,
   do NOT freeze a "substrate API" before consumers exist (YAGNI — only the depth-aware
   upsample is genuinely shareable, extract it after a 2nd consumer).

## TODOs needing the owner / a quiet machine
- **TODO 3 perf re-bless** (`run-release-perf-lane.ps1 -Bless`): no honest release
  baseline exists; every perf claim floats on it. Cheap but needs a quiet machine +
  owner. Schedule opportunistically, in parallel.
- **Wave C server-scale half** (multi-anchor streaming): real but invisible to the
  camera → defer toward iteration end / iter-7 if effort runs short. (Erosion half done.)
- **Wave D closeout**: last. Verify iter-5 foliage/aurora BLOCKs discharged by PASSING
  sweeps, not reclassified.

## Decisions for the owner
1. **Bless parking the far-field dormant** (consensus). It stays committed/green; I
   write a resume-spec. Y/N.
2. **Terrain metric recalibration** (additive companion + near-ROI floor) — a visual-
   gate change; recommend yes (fixes a provably-broken instrument).
3. **GPU grass vs slope/height-blend first** — both are top near-field wins; I'd do the
   metric fix + slope/height blend (render-only, no assets) immediately, GPU grass next.

## Recommended IMMEDIATE next actions (autonomous-safe, no aesthetic gamble)
A. Park far-field: write resume-spec, retask. (No code; already dormant.)
B. Terrain metric fix (lever b) with TDD fixtures — safe, unblocks honest iteration.
C. Then render-side slope/height material blend (render-only) — biggest visible win.
