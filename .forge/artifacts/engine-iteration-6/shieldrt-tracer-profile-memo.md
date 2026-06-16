# SHIELD-RT far-field tracer — GPU micro-profile (T-I6-A3a)

**Decision: heightfield max-mip march of FarLodStore tiles is PINNED as the
SHIELD-RT far-field tracer for A3b.** Sparse SDF bricks stay reserved for genuine
3D far content (overhangs/arches/floating geometry), not the terrain field.

## What this measured

A3a is the gating decision for Wave A.2: which tracer does the productionized
far-field renderer use? The iteration-4 CPU spike
(`engine-iteration-4/shieldrt-spike-memo.md`) established the *representation-level*
facts (heightfield wins on per-ray step count, memory footprint, and the
conservative-mip correctness criterion), but explicitly deferred the *wall-clock*
question on real hardware to iteration 6.

This micro-profile (`test/performance/shieldrt_tracer_profile_gpu.cpp`, ctest
`ShieldRtTracerProfileGpu`, label `manual;perf;gpu`) answers it. It re-implements
the **same two tracers as GLSL compute shaders** over the **same**
FarLodStore-derived terrain (shared builders in `test/performance/shieldrt_far_field.h`,
also consumed by the CPU spike so the structures are byte-identical), fires the
**same** 320×180 ray grid from the **same** four representative views, and times
each kernel with `GL_TIME_ELAPSED` queries (median of 5 timed runs after 2 warmups).

Hardware: **NVIDIA GeForce RTX 5070 Ti**, GL 4.5 / driver 595.97.
Artifact: `shieldrt-tracer-profile.json` (this directory) ·
schema `luminumbra.shieldrt_tracer_profile.v1`.

## The non-obvious finding

Naively read, the raw GPU timings look like a *loss* for the heightfield: SDF
sphere-trace was faster (total 0.059 ms vs 0.111 ms across the four views). That
reading is **wrong**, and the hit-classification data shows why.

The heightfield march is the analytic ground truth for terrain hit/miss (it
samples the surface directly). Measuring the SDF tracer's agreement against it:

| view | heightfield hit-frac | SDF agreement | SDF false sky-hits |
|---|---|---|---|
| default_grazing   | 0.069 | **0.095** | 52,116 |
| default_elevated  | 0.114 | **0.114** | 51,026 |
| mountains_grazing | 0.520 | **0.513** | 27,113 |
| mountains_elevated| 0.114 | **0.114** | 51,026 |

**Mean SDF agreement with ground truth: 21%.** The naive conservative-mip sphere
trace false-hits the sky on ~79% of rays. Its "speed" is a degenerate early-out,
not real tracing: the conservative mip stores `sign·max(0, |closest child| −
coarse-cell-half-diagonal)`, so at coarse levels — where the half-diagonal spans
much of the volume — the stored distance **collapses to ~0** in every cell that
contains surface (i.e. almost all of them). The coarse-mip-first level schedule
selects those collapsed levels for far `t`, so beyond the near band the first
sampled distance is ~0, `d < surf_eps` fires, and the ray reports a spurious hit.
It terminates fast because it stops tracing, not because it found the surface.

This is the exact "conservative mips are a correctness burden" hazard the spike
flagged, now demonstrated on the GPU: a production-naive SDF terrain tracer over
these mips is **not viable** (`sdf_naive_mip_viable: false`).

## Why heightfield-primary

- **Correct by construction** — sensible terrain/sky hit mix on every view
  (7%–52% coverage), no false sky hits.
- **Within budget** — ~0.07 ms/view for a 1536 m / 320×180 probe on the 5070 Ti;
  total 0.111 ms across four views, far under the viability ceiling.
- **Cheap to feed** — reuses FarLodStore F1/F2 tiles that already exist; adds only
  a small max-mip pyramid (≈1.38 MB accel here), no 3D volume to build/stream.
- SDF bricks would additionally cost a per-region volume build (~1.9–2.6 MB here,
  scaling with relief) and the conservative-mip construction this profile just
  showed is the hard part — for content a heightfield already represents exactly.

## Gate semantics (so the artifact isn't a rubber stamp)

The ctest decision gate guards the path A3b builds on, and does NOT race raw ms:
- per view: heightfield hit-fraction ∈ (0.02, 0.995) — rejects degenerate
  all-hit / all-miss tracers;
- per view: heightfield `ms_median` < 2.0 ms viability ceiling;
- decision == `heightfield_primary`; total heightfield ms within ceiling×views.

The SDF agreement is recorded as the *evidence* for reserving SDF to 3D content;
it is intentionally not asserted to fail (brittle). Skips gracefully on a headless
context; on a software renderer it writes the artifact then skips the perf gate.

## Handoff to A3b

Build the production far-field render pass on the heightfield max-mip march:
heightfield tiles → max-mip pyramid (min-/max-filtered, NOT averaged) → quadtree
DDA march in the G-buffer slot after live chunks. Parity gate vs the F1/F2 mesh
path at 1536 m + near↔far seam gate (per WAVE-A-SPEC A3b). Do not pursue a generic
SDF sphere-trace for the terrain far-field.

## A3b increment 1 — ground-truth parity gate (DONE)

`ShieldRtFarFieldParityGpu` (`test/performance/shieldrt_far_field_parity_gpu.cpp`,
artifact `shieldrt-far-field-parity.json`) proves the pinned tracer hits the real
surface BEFORE any live-pipeline wiring. Two legs: (1) self-consistency — the hit
lies on the bilinear FarLodStore surface it marches; (2) ground truth — hit height
vs `GetTerrainHeightAtCoarse(x,z,4)` (the generator of the tile samples) within a
quantization-aware tolerance.

**Tracer-correctness fix surfaced here.** The A3a/spike march advanced by a *fixed
cell size from mid-cell*, which overshoots terrain rises for near-horizontal rays
(steep small-cell views were unaffected; low-relief/elevated views reported hits
~760 m median / ~1315 m off the surface). The parity gate caught it. The production
kernel was rewritten as an **overshoot-free hierarchical DDA**: step to the next
cell boundary at the current mip level, skip a cell only when the ray is above its
max height at both entry and exit t (py is linear → endpoints bound the segment),
descend levels where terrain may be present, climb for empty space, resolve the
final cross by bisection within the finest cell. Result: self-consistency ≤ 0.09 m,
ground-truth median ≤ 0.19 m / p99 ≤ 1.32 m / max ≤ 4.6 m (bilinear-grid error on
steep ridges). This corrected DDA is THE production far-field tracer for A3b
increment 2; the A3a profile kernel kept the simpler advance (its decision —
heightfield correct, SDF degenerate — is unaffected, the SDF false-hit rate was
catastrophic regardless).

### Remaining A3b increments (live wiring)
- **inc 2:** `ShieldRtFarFieldPass` in the G-buffer slot after live chunks
  (`GBufferPass.cpp:227`), behind a `--enable-shieldrt-far-field` flag mirroring
  the GPUSDF gating shape; `GpuTimerPass::ShieldRtFar`; near↔far dither blend;
  mesh-vs-raymarch parity leg in the `FarLodHorizon` scenario.
- **inc 3:** temporal-stability/ghosting gate (MAJOR #11) — needs a render-
  interpolated prev-view history (no TAA/history infra exists yet; build minimal).
- **substrate API freeze** (ray-setup/half-res/temporal/upsample taking
  output-target + field-sampler params) + Wave-B-consumer dry-run review (MAJOR #10).
