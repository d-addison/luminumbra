## Subsystem State

World streaming is now instrumented enough to diagnose local readiness, upload drain, and LOD coverage, but it is still a batch-oriented CPU pipeline: one active generation batch, one active meshing batch, main-thread GPU uploads, and a simple FIFO job system underneath.

The current strong path is near-field stabilization. `lod_ground_smoke`/`LodGround` and `water_visual_smoke`/`WaterVisual` have already closed the large LOD-hole and visible-water blockers in short deterministic runs. The current weak path is endurance and churn: long-run drain, LOD boundary oscillation, transition seam stability, and job-lane observability are not yet enforced tightly enough for reliable `Endurance300`.

Wave 1 candidates should therefore bias toward deterministic runtime artifacts, not broad redesign first: sustained upload drain, bounded generation/meshing backlog, LOD hysteresis or no-hole replacement rules, and job/runtime telemetry that explains backlog growth without a debugger.

## Findings

1. Streaming is explicitly pressure-adaptive, but it is still coarse-grained. Runtime activation only runs every 4 frames, active chunks are capped at 8192, and the target radius shrinks to 20 while generation/meshing is active or pending chunks exceed `MAX_CHUNKS_TO_PROCESS_PER_FRAME * 8` (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:15`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:21`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:22`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:109`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:114`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:120`).

2. Generation scheduling is surface-first and distance-first. Candidate creation prioritizes the terrain surface column, then near and mid vertical stacks, and sorting orders by `surface`, ring distance, horizontal distance, vertical rank, and coordinates before applying the generation budget (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:503`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:534`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:537`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:547`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:580`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:596`).

3. Runtime meshing is priority-aware but single-lane. Chunks with no active mesh are sorted ahead of stale active meshes, then by surface distance, camera distance, and LOD, but `meshing_budget` becomes zero while the single meshing batch is active (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:389`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:392`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:394`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:396`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:399`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:402`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:423`).

4. CPU-side no-hole replacement mostly exists for runtime remeshes. A chunk with an active mesh is not moved to `Meshing`; new terrain and water payloads are staged in pending buffers and committed only after the job completes (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1203`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1206`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1215`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1279`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1330`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1332`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1342`).

5. LOD selection has thresholds but no hysteresis. Runtime LOD is recomputed directly from camera-to-chunk-center distance and any mismatch schedules terrain remeshing; configured thresholds are LOD0 192m, LOD1 384m, and LOD2 640m (`src/luminumbra_common/systems/SHIELD_WorldSystem.h:191`, `src/luminumbra_common/systems/SHIELD_WorldSystem.h:192`, `src/luminumbra_common/systems/SHIELD_WorldSystem.h:193`, `src/luminumbra_common/systems/SHIELD_WorldSystem.h:194`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:351`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:353`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:356`).

6. Boot/join horizon prep uses a different ring policy than runtime distance LOD: collision range is LOD0, the middle ring is LOD1, and the outer ring is LOD2 (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:88`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:93`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:669`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:670`).

7. Coarse LOD is now a primary-surface heightfield, not step-2/4 marching cubes. `PolygoniseTerrain` diverts `sample_step > 1` to `GenerateCoarseHeightfieldTerrain`, which emits heightfield cells only when the averaged per-cell local surface height is inside the chunk volume (`src/luminumbra_common/world/MarchingCubes.cpp:366`, `src/luminumbra_common/world/MarchingCubes.cpp:389`, `src/luminumbra_common/world/MarchingCubes.cpp:416`, `src/luminumbra_common/world/MarchingCubes.cpp:420`, `src/luminumbra_common/world/MarchingCubes.cpp:424`, `src/luminumbra_common/world/MarchingCubes.cpp:519`).

8. Transition skirts are opportunistic at dispatch time. Coarse chunks add skirts only for already-renderable finer horizontal neighbors discovered while dispatching the job; if a finer neighbor appears later, the coarse chunk may not automatically rebuild just to add that skirt (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1218`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1221`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1224`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1228`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1231`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1240`).

9. Render upload priority has been materially improved. The telemetry surface records candidates, selected/deferred new and stale uploads, nearest candidate/deferred distances, farthest selected distance, and near-deferred inversion counters for both terrain and water (`src/luminumbra_client/rendering/RenderPipeline.h:96`, `src/luminumbra_client/rendering/RenderPipeline.h:102`, `src/luminumbra_client/rendering/RenderPipeline.h:109`, `src/luminumbra_client/rendering/RenderPipeline.h:110`, `src/luminumbra_client/rendering/RenderPipeline.h:117`, `src/luminumbra_client/rendering/RenderPipeline.h:128`, `src/luminumbra_client/rendering/RenderPipeline.h:131`).

10. Terrain upload is distance-first and adaptive from 8 to 64 uploads/frame; water upload is distance-first but fixed at 8 uploads/frame. Terrain upload candidates sort by distance before new/stale status, then scale budget at candidate thresholds of 128/512/1024/2048; water uses the same distance-first ordering but a constant `MAX_UPLOADS_PER_FRAME = 8` (`src/luminumbra_client/rendering/RenderPipeline.cpp:1493`, `src/luminumbra_client/rendering/RenderPipeline.cpp:1494`, `src/luminumbra_client/rendering/RenderPipeline.cpp:1567`, `src/luminumbra_client/rendering/RenderPipeline.cpp:1577`, `src/luminumbra_client/rendering/RenderPipeline.cpp:1587`, `src/luminumbra_client/rendering/RenderPipeline.cpp:1631`, `src/luminumbra_client/rendering/RenderPipeline.cpp:1703`, `src/luminumbra_client/rendering/RenderPipeline.cpp:1713`).

11. Water remesh churn is coalesced, and water mesh freshness is split from terrain mesh freshness. Water invalidation waits for 60 dirty simulation ticks, and chunks carry separate `mesh_version` and `water_mesh_version` counters (`src/luminumbra_common/systems/WaterSystem.cpp:20`, `src/luminumbra_common/systems/WaterSystem.cpp:321`, `src/luminumbra_common/systems/WaterSystem.cpp:325`, `src/luminumbra_common/systems/WaterSystem.cpp:328`, `src/luminumbra_common/world/Chunk.h:58`, `src/luminumbra_common/world/Chunk.h:59`, `src/luminumbra_common/world/Chunk.h:76`).

12. The current meshing core is scalar and allocation-heavy for LOD0. It scans the full SDF for positive/negative early-out, visits every cell, uses an `unordered_map` vertex cache keyed by edge, and records only aggregate job/cell/triangle/elapsed counters (`src/luminumbra_common/world/MarchingCubes.h:14`, `src/luminumbra_common/world/MarchingCubes.h:19`, `src/luminumbra_common/world/MarchingCubes.h:24`, `src/luminumbra_common/world/MarchingCubes.cpp:528`, `src/luminumbra_common/world/MarchingCubes.cpp:575`, `src/luminumbra_common/world/MarchingCubes.cpp:612`, `src/luminumbra_common/world/MarchingCubes.cpp:630`, `src/luminumbra_common/world/MarchingCubes.cpp:702`).

13. The job system is a global FIFO queue, not a frontier scheduler. Runtime stats expose only worker count, queue depth, accepting-jobs, and stop-requested state; dispatch pushes all jobs into one `std::queue`, and workers pop the front (`src/luminumbra_common/core/JobSystem.h:25`, `src/luminumbra_common/core/JobSystem.h:27`, `src/luminumbra_common/core/JobSystem.h:44`, `src/luminumbra_common/core/JobSystem.h:45`, `src/luminumbra_common/core/JobSystem.cpp:95`, `src/luminumbra_common/core/JobSystem.cpp:122`, `src/luminumbra_common/core/JobSystem.cpp:165`, `src/luminumbra_common/core/JobSystem.cpp:188`).

14. Existing tests are useful but not yet endurance-grade. `initial_world_loading.json` and `meshing_throughput.json` assert initial coverage and throughput, `streaming_budget.json` asserts surface-first scheduling, and `streaming_walk.json` asserts final loading/meshing drain, but the performance framework still uses catastrophic budgets such as 120000ms p99 and an upload backlog cap of 8192 (`test/performance/initial_world_loading_perf_test.cpp:393`, `test/performance/initial_world_loading_perf_test.cpp:394`, `test/performance/initial_world_loading_perf_test.cpp:402`, `test/performance/initial_world_loading_perf_test.cpp:443`, `test/performance/initial_world_loading_perf_test.cpp:454`, `test/performance/initial_world_loading_perf_test.cpp:558`, `test/performance/initial_world_loading_perf_test.cpp:566`, `test/performance/initial_world_loading_perf_test.cpp:758`, `test/performance/initial_world_loading_perf_test.cpp:761`).

15. Recent artifacts prove short-run drain and visual LOD correctness, not long-run shipping readiness. The handoff records a 30s `LodGround` run with readiness true, zero upload candidates/deferred uploads, zero priority inversions, and queue depth zero; it also records that material heatmap and visible 300s endurance remain open (`.forge/artifacts/visual-performance-improvement/handoff.md:262`, `.forge/artifacts/visual-performance-improvement/handoff.md:265`, `.forge/artifacts/visual-performance-improvement/handoff.md:266`, `.forge/artifacts/visual-performance-improvement/handoff.md:268`, `.forge/artifacts/visual-performance-improvement/handoff.md:274`, `.forge/artifacts/visual-performance-improvement/handoff.md:276`).

16. GPU SDF exists, but it is not the async frontier yet. CPU generation calls an optional GPU SDF callback first, while the render pipeline compute path fences and then blocks with `glClientWaitSync(..., GL_TIMEOUT_IGNORED)` before mapping the SDF buffer (`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1058`, `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:1059`, `src/luminumbra_client/rendering/RenderPipeline.cpp:2316`, `src/luminumbra_client/rendering/RenderPipeline.cpp:2322`, `src/luminumbra_client/rendering/RenderPipeline.cpp:2328`, `src/luminumbra_client/rendering/RenderPipeline.cpp:2336`).

## Must-Fix

1. Close the endurance drain gap before claiming runtime stability. The 30s LOD path drains cleanly, but `Endurance300` is still open. Wave 1 should add an endurance-tail drain contract: final and rolling windows must have `ready=true`, zero generation/meshing active, zero job queue depth, zero terrain/water upload priority inversions, and bounded or zero deferred upload tail after the camera path stabilizes.

2. Gate LOD boundary oscillation before changing LOD constants again. Runtime LOD changes immediately at distance thresholds, with no enter/exit hysteresis. The system needs a deterministic boundary replay that hovers around LOD0/LOD1 and LOD1/LOD2 rings and proves mesh version churn, pending LOD count, upload backlog, screenshots, and near-field coverage stay bounded.

3. Gate transition-skirt/seam correctness under asynchronous neighbor arrival. Skirts are computed from the neighbor state visible at dispatch time, which is fragile when coarse and fine chunks arrive in different batches. Add a seam-risk scenario before deepening skirt geometry, and require the artifact to identify missing/late transition faces rather than relying only on whole-frame screenshot pixels.

4. Replace "queue high water by inference" with real runtime job telemetry. Current `JobSystem::RuntimeStats` cannot distinguish generation, meshing, water simulation, upload-copy pressure, job wait time, worker occupancy, or starvation. This blocks reliable Wave 1 diagnosis when `Endurance300` fails under load.

5. Make sustained generation/meshing backlog a first-class failure. Existing streaming tests assert final drain after explicit `EnsureSurfaceReadyNear`, but they do not fail on recurring deferred-generation/deferred-meshing waves during a long camera path unless final state is bad. Shipping needs rolling-window backlog budgets.

6. Finish the remaining visual prerequisite that can mask world-streaming conclusions: material/texture-layer validation. The roadmap says water and LOD holes are gated, but sand/grey fallback remains ungated; do not rerun visible 300s as a pass/fail authority until terrain material appearance has a deterministic heatmap or screenshot gate.

## Deepening Opportunities

1. Split runtime scheduling into priority lanes. Keep surface generation, collision-near generation, LOD replacement meshing, water-only remesh, and far-horizon fill as separate lanes with per-lane budgets and stats. This preserves the current surface-first behavior while making starvation visible.

2. Add LOD hysteresis with a no-hole upload rule. Keep current meshes until replacement CPU mesh and GPU upload are both complete, and use separate promote/demote distances to reduce boundary churn. The gate should be built first so hysteresis cannot hide seam defects.

3. Make upload budgets adaptive for water, not only terrain. Terrain already scales from 8 to 64 uploads/frame; water stays fixed at 8. Use the existing water candidate/deferred telemetry to scale within a bounded budget when water queues reappear after coalescing.

4. Introduce screen-space or velocity-aware upload priority as a tiebreaker after distance. Distance-first fixed the near-deferred inversion class, but projected size and camera velocity can better protect chunks likely to enter view in the next few frames.

5. Reduce LOD0 meshing cost with table-driven and allocation-light fast paths. Likely targets: reusable scratch buffers per worker, flat edge-cache arrays instead of `unordered_map`, SIMD sign masks for empty/full cell runs, and skipping material queries until vertices survive compaction.

6. Add incremental remeshing hooks. Water-only remesh is already split from terrain; terrain should eventually support dirty-region or column-window remeshes for localized edits, caves, or erosion without rebuilding whole chunk meshes.

7. Extend `TerrainMeshBuildStats` into percentiles and cohorts. Aggregate totals are helpful, but Wave 1/2 optimization needs p50/p95/p99 per LOD step, active-cell ratio, empty/full chunk counts, and payload bytes per completed job.

8. Treat `EnsureSurfaceReadyNear` as a boot/join builder, not a runtime performance model. It is useful for deterministic prep artifacts, but long-run streaming needs separate evidence because runtime uses throttled activation, water update, meshing, collision, and upload pipelines.

## Frontier Proposals

1. Gate-first GPU terrain meshing. Build `gpu_meshing_parity_smoke`, `gpu-meshing-parity.json`, and validator mode `GpuMeshingParity` before replacing CPU LOD0. The gate should compare CPU/GPU mesh topology tolerance, material IDs, normals, bounds, and triangle counts on fixed seeds and LOD rings.

2. Gate-first async SDF readback. Build `async_sdf_readback_smoke`, `async-sdf-readback.json`, and validator mode `AsyncSdfReadback` before changing the GPU SDF path. The gate should prove no frame blocks on `glClientWaitSync`, no stale buffer reuse, deterministic chunk completion order for a fixed seed, and parity with CPU fallback.

3. Gate-first predictive prefetch along camera velocity. Build `predictive_prefetch_replay`, `predictive-prefetch-analysis.json`, and validator mode `PredictivePrefetch`. Run the same deterministic camera replay with and without prediction; accept only if near-field misses and upload deferrals drop without exceeding active chunk and memory budgets.

4. Gate-first job-system frontier: priority lanes plus work stealing. Build `job_lane_contention_smoke`, `job-lane-contention.json`, and validator mode `JobLaneContention` before replacing FIFO. The synthetic load should mix generation, meshing, water, and tiny jobs and assert no starvation, bounded wait p95, and stable shutdown.

5. Gate-first meshlet/indirect terrain streaming. Build `terrain_meshlet_stream_smoke`, `terrain-meshlet-stream.json`, and validator mode `TerrainMeshletStream` before adopting GPU-driven culling or indirect draw submission. The gate should preserve chunk visibility, material IDs, and existing LOD visual pixel thresholds.

## Proposed Gates

1. `endurance_stream_drain`: deterministic 300s hidden and visible camera path; writes `build/debug/test-artifacts/runtime/endurance-stream-drain/stream-drain-analysis.json`; enforced by `.forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode EnduranceStreamDrain`. Must assert rolling and final drain of generation, meshing, job queue, terrain/water upload candidates, deferred uploads, and priority inversions.

2. `streaming_upload_stress_smoke`: deterministic fast pan plus chunk churn replay; writes `build/debug/test-artifacts/runtime/streaming-upload-stress/streaming-upload-drain.json`; enforced by `-Mode StreamingUploadDrain`. Must fail on near-deferred upload inversions, unbounded deferred upload tail, or active chunks over budget.

3. `lod_boundary_oscillation_smoke`: deterministic camera oscillation across LOD0/LOD1 and LOD1/LOD2 rings; writes `build/debug/test-artifacts/runtime/lod-boundary-oscillation/lod-boundary-hysteresis.json`; enforced by `-Mode LodBoundaryHysteresis`. Must include pending LOD counts, mesh-version churn, upload backlog, near-field coverage, and start/mid/end screenshots analyzed with the existing `lod-ground-visual-analysis.json` rules.

4. `lod_seam_arrival_smoke`: deterministic coarse/fine neighbor arrival order replay; writes `build/debug/test-artifacts/runtime/lod-seam-arrival/lod-seam-risk.json`; enforced by `-Mode LodSeamRisk`. Must record transition-face masks, neighbor LOD state at dispatch, late-finer-neighbor cases, and screenshot/pixel checks for seam holes.

5. `meshing_throughput_budget_smoke`: fixed-seed mixed LOD build with cold and warm scratch buffers; writes `build/debug/test-artifacts/performance/meshing-throughput-budget.json`; enforced by `.forge/scripts/validate-performance-framework-phase-5.ps1 -Mode MeshingThroughputBudget`. Must report p50/p95/p99 per LOD step, cells/second, active-cell ratio, payload bytes, and regression thresholds tighter than the current catastrophic 30ms unit average.

6. `job_contention_smoke`: deterministic synthetic generation/meshing/water workload; writes `build/debug/test-artifacts/performance/job-contention-runtime-stats.json`; enforced by `validate-performance-framework-phase-5.ps1 -Mode JobContention`. Must fail on worker starvation, unbounded queue depth, missing per-lane counters, and shutdown with queued jobs.

7. `material_terrain_heatmap_smoke`: deterministic terrain material capture around spawn and sand/water edges; writes `build/debug/test-artifacts/runtime/material-terrain/material-terrain-heatmap-analysis.json`; enforced by `.forge/scripts/validate-runtime-stability-phase-1.ps1 -Mode MaterialTerrainHeatmap`. Must prove sand/grass/soil/stone texture-layer agreement and fail on grey fallback or invalid material IDs.

8. `async_sdf_readback_smoke`: fixed-seed GPU SDF generation replay; writes `build/debug/test-artifacts/runtime/async-sdf-readback/async-sdf-readback.json`; enforced by `-Mode AsyncSdfReadback`. Must be added before any async GPU SDF or GPU meshing implementation and prove no blocking readback on the frame path.

## References

- `src/luminumbra_common/systems/SHIELD_WorldSystem.h`
- `src/luminumbra_common/systems/SHIELD_WorldSystem.cpp`
- `src/luminumbra_common/world/MarchingCubes.h`
- `src/luminumbra_common/world/MarchingCubes.cpp`
- `src/luminumbra_common/world/Chunk.h`
- `src/luminumbra_common/core/JobSystem.h`
- `src/luminumbra_common/core/JobSystem.cpp`
- `src/luminumbra_common/systems/WaterSystem.h`
- `src/luminumbra_common/systems/WaterSystem.cpp`
- `src/luminumbra_client/rendering/RenderPipeline.h`
- `src/luminumbra_client/rendering/RenderPipeline.cpp`
- `test/shield/test_world_generation.cpp`
- `test/performance/initial_world_loading_perf_test.cpp`
- `.forge/artifacts/visual-performance-improvement/handoff.md`
- `.forge/artifacts/visual-performance-improvement/next-plan.md`
- `.forge/artifacts/engine-framework-roadmap/ultimate-plan.md`
- `C:/Users/David/.codex/plugins/cache/personal/forge/0.17.0/skills/forge-cli/SKILL.md`
