# GPU Upload Architecture

## Problem

Terrain and water chunks currently reach the renderer as CPU-owned mesh vectors keyed by `ChunkID`.
`RenderPipeline::manage_chunk_gpu_resources()` scans renderable chunks, detects stale mesh versions, and uploads up to a fixed number of chunks per frame. The current resource model is simple and correct, but the hot path still has two costs that show up during world boot and fast streaming:

- Fresh or stale terrain uploads can allocate and delete VAO/VBO/EBO objects instead of reusing capacity.
- Upload priority is independent of camera distance, visibility, and backlog age, so visible near chunks can wait behind far chunks.
- Water and terrain use separate resource maps, but expose only aggregate upload counters, making backlog health hard to gate.

The next implementation should preserve the `mesh_version` contract while removing avoidable driver churn.

## Options Considered

### Per-Chunk Reusable Buffers

Each `ChunkRenderData` owns a VAO/VBO/EBO and records vertex/index capacity. When a chunk gets a new mesh:

- If existing capacity is enough, update the buffers in place with `glBufferSubData` or orphan-and-upload.
- If capacity is too small, grow the buffers and keep the new capacity for future versions.
- On chunk eviction, keep the GL objects in a free-list slot pool instead of deleting immediately.

This is the first implementation target because it is low risk and matches the current one-draw-per-chunk renderer.

### Page/Arena Buffer Pool

Terrain meshes are packed into larger VBO/EBO pages. A chunk owns offsets into a page, and upload allocates or relocates a range. This can reduce bind churn and prepares for multi-draw, but needs free-range management, fragmentation handling, and more invasive draw code.

This is a phase-two target after the per-chunk pool proves stable.

### Persistent Mapped Staging

A persistently mapped upload ring writes CPU mesh data into staging memory, then copies into device-local buffers. This reduces map/unmap overhead and gives better async behavior, but requires explicit sync fences and a backend-specific policy.

This is a later optimization. The current OpenGL path should first reduce allocation churn and add backlog gates.

### Multi-Draw Readiness

Multi-draw indirect becomes useful once many chunks share larger buffers. It should not be mixed into the first pool change because it changes culling, sorting, and draw submission together.

## Chosen Plan

Wave 3 should implement a per-chunk reusable slot pool:

1. Extend terrain and water render data with vertex/index capacity, uploaded byte counters, and a pool slot state.
2. Replace eager delete/recreate on stale terrain meshes with capacity-aware reuse.
3. Keep eviction safe by moving GL objects to a free list and clearing the `ChunkID` association.
4. Track created slots, reused slots, grown slots, failed uploads, bytes uploaded, and per-frame backlog.
5. Keep draw behavior unchanged: one VAO bind and one indexed draw per visible chunk.

## Lifetime Rules

- `Chunk::mesh_version` remains the source of truth for terrain and water staleness.
- A render slot is valid only when its stored `chunk_id` and `mesh_version` match the source chunk.
- A stale chunk can reuse its old slot if capacity is sufficient. Otherwise the slot grows or is replaced.
- An evicted chunk must clear all chunk identity and version fields before the slot enters the free list.
- A failed upload must leave the previous valid slot drawable if one exists.
- Terrain and water pools remain separate because they use different source vectors and may get different budgets.

## Upload Priority

Wave 4 should sort upload candidates before applying the frame budget:

1. Currently visible near-field chunks with no GPU resource.
2. Stale visible chunks whose old mesh can remain drawable.
3. Near-field water.
4. Far terrain and water, ordered by distance and backlog age.

The upload gate should fail only on persistent end-of-window backlog growth, not on a single deferred frame during boot.

## Metrics And Gates

Required counters:

- `terrain_slots_created`
- `terrain_slots_reused`
- `terrain_slots_grown`
- `terrain_upload_failures`
- `terrain_upload_candidates`
- `terrain_uploads`
- `terrain_uploads_deferred`
- `terrain_payload_bytes`
- Equivalent water counters where water uploads are enabled.

Runtime gates should use a final-N-frame window. A healthy boot is allowed to defer early work, but the final window must show backlog draining or bounded deferrals.

## Test Strategy

- Keep `RuntimeBootMetricsHarness.ShortRun` as the debug smoke gate.
- Add a render resource test that uploads the same chunk twice and verifies slot reuse/growth counters.
- Add a backlog test that feeds visible near/far chunks and verifies upload ordering.
- Add a release benchmark later with a longer frame window and stricter final-window backlog limits.
