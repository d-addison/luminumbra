# R0 asynchronous pristine-volume consumer

R0 is a compiled client-library consumer with controlled pristine fixture callers.
It connects the reviewed [volume foundation](far-volume-foundation.md) to the
existing Normal JobSystem lane and converts actual Marching Cubes output to
renderer-ready `VoxelVertex` buffers. No gameplay, save, GL upload, preset, or
render-pipeline caller selects it. The existing heightfield renderer and its
3,200 m far plane remain unchanged. This is not 16 km rendering, deep streaming,
edited-world acceptance, or a final cave-filter decision.

`rendering/FarVolumeBuildQueue.{h,cpp}` owns request bookkeeping and asynchronous
result lifetimes. `rendering/FarVolumeRenderMesh.{h,cpp}` adapts absolute geometry
without terrain sampling. Both are compiled into `luminumbra_client`; the
`far_volume_runtime_test` executable exercises those actual objects without a GL
context. The accepted [distant-world contract](distant-world.md) still controls
later runtime integration.

## Fixture boundary and lifetime

A caller constructs a queue around an existing JobSystem, explicitly binds a
fresh controlled world with `bind_fixture_world`, submits `FarVolumeRequest`
values, pumps dispatch, and takes move-only result leases. The JobSystem must
outlive the queue, including its destructor, even after an earlier drain or
world swap. A bound world must remain alive until `prepare_world_swap` returns
or the queue is destroyed. `drain` waits for jobs but does not unbind the world.

Call `prepare_world_swap` before destroying/replacing the world; it cancels queued
work, drains actual jobs, invalidates the epoch, and discards unleased results.
Rebinding the same address still advances the epoch. The destructor also cancels
and drains workers.

Every queue method belongs to its constructing thread and rejects other callers.
Lease release may occur on another thread. A lease contains geometry/metadata
only and may outlive the queue, world, and JobSystem. Its result is immutable and
cannot be moved out of the lease through the API. Caller-made copies are outside
the queue's accounting. Call `is_current(identity)` before using a retained
lease after any world/request change: lease lifetime does not imply continuing
world currency. Holding a reference after lease release is invalid.

The initial binding rejects nonzero authority revision, and a subsequent change
to edit authority refuses more pristine work. This is an additional fixture
guard, not a proof that a world has no durable edits. Const access and a zero
revision cannot qualify an arbitrary saved world. R0 has no production caller
or unchecked `authority_complete` flag that pretends otherwise.

## Exact request identity

The result envelope retains binding epoch, monotonically increasing request
generation, seed, terrain/content parameter hash, captured authority revision,
the submitted request, and the effective request. Tier, signed tile XZ, cave
mode and optional requested Y span are validated before queue allocation.
Generation and identity reads use SHIELD's existing worldgen sampling scope;
the generator itself does not acquire a nested per-sample lock. Workers never
borrow mutable Chunk arrays.

For one tier/tile, new span requests coalesce by union. The submitted span stays
in the receipt, while effective span includes the previous requests. A mode
change creates a new generation. Older arrivals cannot discharge an extended
request, and retained older leases cease to be current. Seed/parameter changes
invalidate work and require an explicit new binding. Edited-authority changes
cannot be handled by rebinding that edited fixture and regenerating pristine.

An unchanged request remains a duplicate after terminal failure or cancellation;
the queue does not retry an impossible tile every frame. `forget(generation)`
explicitly cancels/removes a descriptor so a caller may retry. Identity sequence
exhaustion is reported without wraparound. Binding exhaustion refuses a new
binding while still allowing safe teardown.

## Bounded ownership

| Resource | Fixture ceiling |
|---|---:|
| Logical request descriptors, including terminal duplicate suppression | 64 |
| Running, completed, or leased result slots | 2 |
| New dispatches per owner pump | 1 |
| Generation density samples per job | 8,000,000 |
| Crossing bricks per job | 65,536 |
| Generation-owned discovery/sample/brick buffers | 128 MiB |
| Geometry buffers per job | 64 MiB |
| Converted vertex/index buffers per job | 64 MiB |
| Conservative payload reservation per occupied payload slot | 256 MiB |
| Combined payload reservations | 512 MiB |

Limits may be reduced for controlled refusal tests, but the queue rejects limits
above these ceilings. Its descriptor and result arrays are fixed. Their actual
object size is exposed as `control_storage_bytes`, alongside descriptor, result,
vertex, index, and brick strides; a compile-time check keeps those fixed objects below 64 KiB. The two
bounded worker captures, single-job dispatch vector, shared ownership control
blocks, and JobSystem's own bookkeeping add implementation-dependent small
control allocations outside that reported fixed-object size. They do not grow
with an unconsumed completion backlog. This is not a total allocator/RSS limit;
world/hydro caches and arbitrary caller copies are outside it.

Each running job reserves for simultaneous retained sparse bricks, geometry,
and converted output. Completed successful results keep the full conservative
reservation, including valid empty successes; taking a lease does not release
it. Full results backpressure further dispatch. Terminal failure drops all mesh
payloads and releases the payload reservation while keeping a bounded result
slot for its error receipt. Consuming/releasing that receipt allows another job.
Actual tile/geometry capacities and retained converted bytes are recorded.

When a lease is released immediately after worker publication, its JobHandle
survives until JobSystem's completion epilogue returns. A slot cannot be reused
while that handle remains unfinished. This prevents a fast consumer from losing
the handle needed for world teardown.

Submission performs bounded validation/bookkeeping and identity reads; it does
not sample terrain or synchronously prefetch hydro regions. Cold hydro work
incurred by generation runs on the worker. The sampling API has no inner-loop
cancellation hook, so cancellation is checked before/between phases and at
publication. A drain can wait for one bounded full tile; R0 promises no shutdown
wall-clock deadline, per-frame latency, or production performance budget.

## Geometry and status

The adapter validates all finite positions and triangle indices before output
allocation. Nondegenerate triangles expand to three vertices, each carrying the
normalized outward face normal and the original solid-side material widened
from u8 to u32. Double-precision normal arithmetic avoids intermediate overflow
for finite input coordinates. Positions and winding are unchanged. There is no
legacy region translation, global-up normal, Y displacement, reclassification,
skirt, heightfield underlay, water fabrication, or absent-brick halo sampling.

Output AABBs use actual indexed geometry, including walls and undersides. Empty
or degenerate geometry has no invented bounds. Triangle expansion is included
in the converted budget; failure returns no partial mesh. Flat normals avoid a
new gradient/halo contract but do not qualify smooth shading or cross-LOD seams.

| Result | Meaning |
|---|---|
| `Ready` | Valid complete requested span and nonempty converted geometry |
| `ReadyEmpty` | Valid complete requested span, with no emitted geometry |
| `BudgetRefused` | Generation, meshing, conversion, or allocation exceeded limits |
| `Invalid` | Invalid/corrupt stream or geometry |
| `Cancelled` | Explicit cancellation of dispatched work |
| `Stale` | A superseded request or changed world identity |
| `Failed` | Other worker failure or rejected JobSystem dispatch |

Errors use a fixed 256-byte, terminated diagnostic field. Failure/cancellation/
staleness never reports `span_complete`, and never retains a partial output mesh.
Submitted work that is cancelled while still pending is acknowledged directly by
`cancel`; it never consumes a worker/result slot. Queue overflow and invalid or
unbound submissions return explicit submission statuses without dispatch.

Receipts retain encoded first/last brick Y, sampled brick/density counts, tile
CRC, and mesh bounds. CRCs are content integrity, not world identity. An empty
span is not a claim that a whole column is empty at every Y, nor an authoritative
air/solid tombstone.

## Vertical and cave limits

The foundation still unions the surface band plus 256 m below the lowest
surface with every extra span and scans the intervening full Y interval. A deep
request that exceeds the bound returns refusal; R0 never truncates it to the
default band. A separately reviewed aligned Y-window/page API is needed before
continuous descent and long deep sightlines can stream bounded work. Raising
all limits or resubmitting a narrow far-away extra span does not implement it.

Both existing cave modes retain their exact identity. V1/V2 use the live field;
V3 uses the finite eight-midpoint noise filter; BandLimited V4/V5 keep analytic
openings without noise, while BoxFiltered retains filtered noise. The corrected
signed coarse analytic opening composition and foundation winding controls are
unchanged. Final analytical/phase/native visual/cost acceptance remains pending;
BandLimited remains the recorded fallback.

## Checks and remaining runtime work

The focused target checks actual five-tier generation and adaptation, separate
repeat receipts, absolute positions/materials, walls/ceilings, case 65 and torus
normals, fixed descriptor/worker/result bounds, transfer backpressure, exception
and corruption release, full-span refusal, stale identity, cancellation, drain,
and same-address re-entry. Deterministic barriers hold real workers; private
friend-only test seams inject phase faults and synthetic valid empty data.
These fault/empty fixtures are distinct from real pristine-generator evidence.
A private post-publication barrier also holds real workers before JobSystem's
completion decrement: tests consume and release visible leases while both jobs
remain unfinished and verify that retired slots cannot yet be reused. A separate
queue-owner thread cannot finish a world swap until its held worker is released.
The brief future wait checks synchronization, not a performance deadline. Isolated
premature-reuse and omitted-drain variants fail these assertions; assertion cleanup
opens the worker gates before joining the owner thread.

Build and run the focused checks in each configuration:

```sh
cmake --build --preset debug --target luminumbra_client_app far_volume_runtime_test -j2
cmake --build --preset release --target luminumbra_client_app far_volume_runtime_test -j2
ctest --test-dir build/debug --output-on-failure -R FarVolumeRuntime
ctest --test-dir build/release --output-on-failure -R FarVolumeRuntime
```

Qualification also retains the relevant foundation/FarTierTable/mesher/far-worker
and existing deterministic checks in both actual configurations, plus explicit
failing normal/origin and generation-guard controls. Exact source, configuration,
binary and check receipts live in the campaign evidence; this document does not
relabel older binaries or state an unexecuted test passed. Windows/native work
belongs to the coordinator.

Production integration still requires bounded vertical pages, five-tier wanted
sets and arrival ownership, actual uploaded 3-D live coverage, byte-bounded GL
upload/cache accounting, table-driven legacy range retirement and the 17,408 m
far plane, water metadata, composited fog and far shadow policy, all-tier FSV1
edit authority, C3 dirty parking/durable re-entry, final cave choice, and native
horizon/deep/edit/seam/performance acceptance. R0 does not enable a path that
silently omits any of those obligations.
