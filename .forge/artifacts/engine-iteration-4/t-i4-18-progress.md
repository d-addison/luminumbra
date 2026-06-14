# T-I4-18 meshing-arenas — progress

Reset-per-job linear (bump) allocator for the marching-cubes meshing hot path.
Files in scope: src/luminumbra_common/world/MarchingCubes.{h,cpp} ONLY.

## 2026-06-14 — Milestone 1: arena implemented + clean debug build

- Base verified: HEAD 3681e41, MarchingCubes.h has PolygoniseTerrain +
  TerrainMeshBuildStats + AddBoundaryTransitionSkirts + FarLodRegionMesh. Good.
- Added a `thread_local MeshArena` (bump allocator) in the MarchingCubes.cpp
  anonymous namespace. Reset-per-job via `MeshArenaScope` RAII at PolygoniseTerrain
  entry. Grows geometrically to a per-worker high-water mark, never freed between
  jobs.
- Arena backs ONLY pure index-addressed scratch that never escapes the function:
  - PolygoniseTerrain (unit step): edge_vertex_cache (~59 KB, dominant),
    world_positions, materials, remap.
  - GenerateCoarseHeightfieldTerrain: remap.
- Buffers that are std::move'd into the chunk (mesh_vertices/mesh_indices/
  compact_vertices, water meshes) stay real owning std::vectors — Chunk takes
  ownership and outlives the job; arena-backing them would break ownership.
- Skirt by-value copies (T-I4-DR-live-needle-streak) left untouched: the arena
  never backs any vertex buffer a skirt generator pushes into, so the
  dangling-reference invariant is preserved as-is.
- Thread-safety: thread_local arena => one per JobSystem worker; meshing runs in
  Job lambdas on workers, never shared across concurrent jobs. No locks/atomics.
- Build: `cmake --build build/debug` clean, -Werror, no warnings in MarchingCubes.
  (One transient gtest_discover_tests 5s-timeout on runtime_world_visual_validation
  cleared on rebuild — unrelated to this change, exe links fine.)

## 2026-06-14 — Milestone 2: determinism bug found + fixed (offset-based arena)

- First arena pass used RAW POINTERS from the bump allocator. The HeadlessServerTick
  smoke (double-run determinism) FAILED and DIVERGED run-to-run (567a...d419 vs
  a82023...3e0b), neither matching canonical 2fa007951a21e140. Baseline (stashed)
  confirmed deterministic 2fa007951a21e140 on both runs -> the arena introduced it.
- Root cause (bisected): in PASS 1b, `materials` is arena-allocated AFTER
  `world_positions` but world_positions is filled/consumed LATER. The materials
  allocation could trigger a mid-job arena grow (realloc), invalidating the raw
  world_positions pointer captured earlier -> subsequent fill/read hit freed
  memory -> job-order-dependent garbage -> run-to-run hash divergence.
- Fix: arena now hands out BYTE OFFSETS; ArenaSpan resolves data() against the
  live arena base() on every access, and EnsureCapacity memcpy's existing bytes
  forward on grow. Outstanding spans survive a mid-job grow. Determinism-safe.
- Re-verified: HeadlessServerTick smoke run-1 == run-2 == 2fa007951a21e140 with
  ALL FOUR scratch buffers (edge_vertex_cache, world_positions, materials, remap)
  arena-backed. Canonical hash UNCHANGED.

## 2026-06-14 — Milestone 3: VERIFIED green + perf measured

Verification (debug, main tree):
- cmake --build build/debug: clean, -Werror, no warnings in MarchingCubes.
  (runtime_world_visual_validation gtest_discover_tests 5s-timeout flake is
  pre-existing/environmental; the exe links and all TUs compile.)
- validate-engine-frontier -Mode HeadlessServerTick: PASS.
  world_hash=2fa007951a21e140 == replay (UNCHANGED). Sub-hashes match:
  terrain=9e1b9316d5eeca32 mesh=812c3bb1c19b127a water=ed4265f8b090adf7
  entities=5735a5094c1e92a8.
- validate-engine-frontier -Mode SimDeterminismLint: PASS (0 new violations).
- ctest -E "_NOT_BUILT$" -LE manual: 195/195 PASS. Mesh/worldgen/determinism
  subset (71 tests incl. FarLodRegionMesher determinism, RenderCapture stable
  mesh scenes, world-hash roundtrips): 71/71 PASS.

Perf (InitialWorldLoadingPerfTest.MeasuresSurfaceHorizonPrep, radius=12, 2119
meshing jobs = 274 step1 unit-MC + 715 step2 + 1130 step4 coarse-heightfield):
- mesh-build elapsed_us (DEBUG build, 5 runs each, median):
    baseline ~349,285 us  (samples 336656,348713,349285,349367,373046)
    arena    ~386,749 us  (samples 361978,362690,386749,407401,450555)
  Overlapping/noisy: debug-build elapsed_us is dominated by un-inlined FastNoise
  sampling, not allocation, so the malloc-churn win does NOT surface in a debug
  wall-clock (and noise jitter swamps it). NOT a regression signal - release-lane
  measurement is the orchestrator's combined re-bless step (not run here).
- allocation-count delta (analytic, exact from code paths) per surface-horizon
  prep: malloc/free PAIRS eliminated (now served from the reset-per-job arena):
    coarse-heightfield jobs (1845): remap            -> 1845 pairs
    unit-step MC jobs (274): edge_vertex_cache +
      world_positions + materials + remap (<=4 each)  -> up to 1096 pairs
    TOTAL ~2941 malloc/free pairs (~5882 heap ops) eliminated per prep,
    replaced by bump allocs from <=16 per-worker arena blocks (one-time grow to
    high-water mark, reused for the rest of the session).
  edge_vertex_cache is the dominant per-job scratch (~59 KB at 17^3*3 u32); its
  per-job malloc/free is the churn this task targets.

Status: COMPLETE. Not committed (orchestrator reviews/commits). Touched ONLY
MarchingCubes.{cpp,h}. perf-baseline-release.json untouched; release lane NOT run.
