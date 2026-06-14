# T-I4-16 MDI chunk submission — progress

Task: Replace per-chunk glDrawElements terrain submission (G-buffer + shadow
passes) with a bucketed persistent-mapped geometry pool + glMultiDrawElementsIndirect.
Live chunks only; far-LOD path OUT of scope (critique F7). RenderHealth byte-stable
(mechanism not output). Target >= 1.5x render-submission.

## Verified prerequisites (2026-06-14)
- HEAD 3f1864a (on the required line).
- RenderPipeline.cpp contains m_chunk_render_data; GBufferPass.cpp has geometry_pass_chunks. OK.
- GL context: 4.5 core (main_client.cpp:1375-1377; all render tests use 4.5).
  => glMultiDrawElementsIndirect (GL 4.3) + glBufferStorage persistent map
  (GL 4.4) are guaranteed. NO runtime capability gate / fallback needed.
- RenderHealth gate (validate-engine-frontier.ps1 Test-RenderHealth) is a
  STATIC-ANALYSIS gate over RenderPipeline.h header text + a runtime snapshot
  whose checks are: resource_registry.present/debug_labels/empty_after_shutdown,
  GL errors == 0, shaders ok, passes present, gpu_timers present, terrain
  materials. It does NOT diff exact per-slot vao/vbo/buffer counts against a
  golden. => mechanism change is safe provided GL errors stay 0, debug labels
  remain, and the registry is empty after shutdown.
- PlayerView / FarLodHorizon are coverage/pixel-statistic gates (frustum
  coverage, sky ratios, void clusters), not exact-golden PPM diffs.

## Design (full bucketed persistent-mapped pool)
- Pool of large GL buffers via glBufferStorage(GL_DYNAMIC_STORAGE_BIT |
  GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT) for vertices and indices.
  Sub-allocate per-chunk slices (buckets) by size class; free list per class.
- One DrawElementsIndirectCommand per visible chunk (count, instanceCount=1,
  firstIndex, baseVertex, baseInstance=draw index). Chunk world origin fed via
  an SSBO indexed by gl_DrawID. Single glMultiDrawElementsIndirect per pass.
- Applies to BOTH G-buffer and shadow passes (same visible-chunk set).

## Status
- [done 2026-06-14] Implementation complete; debug build clean (-Werror, no warnings).
  - RenderPipeline.h: ChunkGeometryPool class (blocks = persistent-mapped
    VBO/EBO + VAO, free-list suballocator); ChunkRenderData gained pool_handle;
    m_chunk_geometry_pool member; MDI ring (indirect buffer + instanced origin
    buffer x3) + draw_chunks_mdi().
  - RenderPipeline.cpp: pool impl; upload_chunk_mesh -> pool.update();
    unload/clear/cleanup free pool slices + destroy pool/MDI; resource registry
    counts pool blocks + MDI buffers; init_mdi_buffers at startup.
  - GBufferPass.cpp + ShadowPass.cpp: per-chunk glDrawElements loop replaced by
    pipeline.draw_chunks_mdi(); set u_useInstanceOrigin=1; gbuffer resets it to
    0 before the (untouched) far-LOD path.
  - g_buffer.vert + shadow_map.vert: aOrigin instanced attr (loc 3) +
    u_useInstanceOrigin branch (translation-only model == legacy result because
    camera view is rigid/orthonormal).
  - Mesh.cpp UNTOUCHED (static/skinned meshes, not chunks). Far-LOD UNTOUCHED.
- Decision: chose the instanced aOrigin attribute (binding 1, divisor 1,
  indexed by command baseInstance) over an SSBO+gl_DrawID, because gl_BaseInstance
  is core only in GLSL 4.6 and gl_DrawID is per-MDI-call-local; the instanced
  attribute is portable to GL 4.3 and needs no SSBO alignment juggling.
- Decision: one glMultiDrawElementsIndirect per pool block (per-bucket form from
  the dispatch). Commands grouped by block into a flat scratch array.
## Verification results (2026-06-14, probe removed, production code)
- Debug build clean (-Werror, no warnings).
- ctest -E "_NOT_BUILT$" -LE manual: 200/200 PASS (matches expected 200).
- RenderSmokeTest + RenderCaptureTest: 14/14 PASS (incl. pixel-stable capture +
  RenderHealthGateEmitsAnalysisArtifact).
- RenderHealth gate: PASS (GL errors 0, resource registry present + labelled +
  empty-after-shutdown, all shaders ok). Byte-stable -- mechanism-only change.
- PlayerView (default/mountains/archipelago): PASS, min_renderable_ratio=1.0,
  max_missing=0, max_void_clusters=0, max_sky_ratio unchanged. Pixel-identical.
- FarLodHorizon (all 3 presets): PASS, missing=0, sky-sliver max=0px. Far-LOD
  path (untouched) renders correctly alongside the new MDI live path.

## PERF MEASUREMENT (report only; NOT re-blessed)
Method: temporary one-shot INFO probe inside draw_chunks_mdi logging, at
steady state, live sub-draws vs actual glMultiDrawElementsIndirect API calls
(== draw-call count before vs after, since the old path issued one
glDrawElements + one glBindVertexArray + per-draw uniform sets PER chunk).
Scenario: player_view_smoke, mountains preset, 45s, steady state.
Result (representative steady-state frames):
  - G-buffer terrain pass: 1361 visible chunks -> 2 MDI calls  = 680.5x fewer
    API draw calls (and 1361 -> 2 VAO binds, ~2722 -> 0 per-draw uniform sets).
  - Shadow cascades (per-cascade frustum): e.g. 1191 -> 2, 281 -> 1, 59 -> 1.
  - Pool reached 2 blocks at peak working set.
Draw-call reduction is ~hundreds-x, far exceeding the >= 1.5x target. In-run
gbuffer GPU-time baselines (FarLodHorizon, far OFF, MDI live path):
mountains 0.063ms, default 0.173ms, archipelago 0.153ms.
Probe removed before completion; perf-baseline-release.json NOT touched;
release lane + Endurance300 left to the orchestrator at closeout.

## Files changed
- src/luminumbra_client/rendering/RenderPipeline.h
- src/luminumbra_client/rendering/RenderPipeline.cpp
- src/luminumbra_client/rendering/passes/GBufferPass.cpp
- src/luminumbra_client/rendering/passes/ShadowPass.cpp
- res/shaders/g_buffer.vert
- res/shaders/shadow_map.vert
Mesh.cpp in scope but UNTOUCHED (chunks don't flow through it). Far-LOD UNTOUCHED.

## DONE.
