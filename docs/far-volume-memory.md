# Far-volume memory accounting

Far-volume generation, meshing and facade compilation can charge their allocations
to a byte budget. The budget types live in `src/luminumbra_common/world/FarVolumeMemory.h`.

## Model

- `FarVolumeMemoryPool` is a thread-safe byte budget with a limit, `used`, `peak`,
  `attempts` and `refusals` counters. `RefuseAttemptsFrom(n)` is a diagnostic fault
  injection that refuses attempt `n` and every later one.
- `FarVolumeMemoryLease` is a move-only holder of bytes taken from one pool.
- `FarVolumeMemory{pool, retained, upstream}` binds an operation to a pool. A null
  `pool` selects the unaccounted legacy path, which is the behaviour before this change.

## Charged and not charged

Charged (admitted before the system allocation):

- tile brick buffers, mesh vertex and index buffers;
- mesher sample, gradient and edge caches, and brick ordering scratch;
- the facade height cache and the checksum ordering scratch.

Not charged:

- memory owned by caller callbacks, including copies of their `std::function` targets;
- the operation's own `std::function` adapters;
- exception objects and allocator bookkeeping below the requested size.

Vector outputs are charged at their exact capacity. Growth charges the new buffer
while the old one is still held, so the peak is old plus new.

## Entry points

- `BuildFarVolumeTileCharged`, `MeshFarVolumeTileCharged`
- `ResolveFarVolumeCoverageCharged`, `CompileFarVolumeCharged`
- `FarVolumeLegacyChecksumCharged`

The existing `BuildFarVolumeTile`, `MeshFarVolumeTile`, `ResolveFarVolumeCoverage`,
`CompileFarVolume` and `FarVolumeLegacyChecksum` forward with an empty `FarVolumeMemory`.
`FarVolumeCompilationRetainedBytes` and the `*RetainedBytes` helpers report what a
retained lease should hold.

## Transactional guarantees

- A refused admission allocates nothing and fails with `AllocationFailure`. The one exception
  is a standard library whose `reserve(n)` returns more than `n` elements of capacity: the
  surplus is charged after the allocation, and a refusal then frees the buffer first. The
  supported libraries reserve exactly, and a test pins that.
- On success, `retained` is replaced with the new output's bytes and the output is assigned.
- On any failure, including cancellation by a callback, the caller's output, its
  `retained` lease and the pool's `used` value are unchanged.
- Previous output stays charged while its replacement is built.
- `CompileFarVolumeCharged` promotes a nested generation or mesh `AllocationFailure`
  to a facade `AllocationFailure`, and a nested `InvalidMemory` to a facade `InvalidMemory`.
  The nested codes are still recorded in the failure struct.
- The same promotion applies on the unaccounted path: a `std::bad_alloc` during generation or
  meshing in `CompileFarVolume` now reports a facade `AllocationFailure`, where it previously
  reported the generation or mesh refusal code.

## InvalidMemory rule

`InvalidMemory` is reported before the first callback runs when a pool is set but:

- the required `retained` lease is null (for operations that publish a retained output), or
- the `retained` lease is bound to a different pool.

`ResolveFarVolumeCoverageCharged` and `FarVolumeLegacyChecksumCharged` return no
retained output, so only the pool is required for them.

## Threading

- A pool may be shared by concurrent operations.
- A lease, a scope and an output object have a single owner.
- Each concurrent operation needs its own output and its own `retained` lease.
  A retained lease must stay paired with the one output it describes.
- A caller-supplied `upstream` must be thread-safe if operations share it.

## Dormant page planning

Issue #239 adds `FarVolumePagePlan` and `FarVolumePageCompleteness` in
`FarVolumeRuntime.h`. Only tests call these APIs. Planning does not activate
streaming or change build queues, span generations, world identity or save formats.

A plan stores one canonical `FarVolumeRequest` and a signed grid descriptor.
Reset, indexed lookup, admission and acknowledgement take constant time and allocate
no storage. Strides are positive brick multiples up to 4096 metres, with an
explicit caller page cap from 1 to 1024. Invalid tiers, coordinates, unaligned or
empty spans, unsupported strides and excessive page counts are refused before
sampling or allocation. The complete one-sample normal halo must remain strictly
inside the facade's exact-coordinate domain. Refusals preserve the previous plan
or output.

Windows are anchored at world zero using signed floor division, including negative
and zero crossings. Only outer windows are clipped to the request. Existing full
interior windows remain identical as either end extends. The completeness fold
copies its plan and accepts each exact request in ascending order, including its
key and field identity. Skipped, duplicate, reordered, overlapping, shortened or
otherwise changed acknowledgements leave progress unchanged. An empty plan never
reports completion.

Admission describes the worst single page for sequential compilation with every
candidate mixed. With `A = tile_edge / brick_edge`, `L = page_extent / brick_edge`,
`H = 4A` and `V = 4L`, generation visits `A*A*L` candidates and makes 125 calls per
candidate. Mesh work admits 64 cells per candidate and at most 15 indices per cell.
The mesher sample cache and its normal halo fit within `(H+3)^2*(V+3)` entries,
gradients within `(H+1)^2*(V+1)`, and vertices and edge-cache entries within
`2H*(H+1)*(V+1) + V*(H+1)^2`. The sample rectangle conservatively includes unused
halo corners. Density calls include generation and mesher samples, while height
calls fit within `(H+3)^2` cached columns.

`FarVolumePageBudget` independently bounds candidates, combined height/density
calls, contiguous buffer bytes and total mesher cache entries. The contiguous
byte bound includes twice the worst tile and mesh buffers for simultaneous growth,
the height cache and two brick-order pointer buffers for meshing and legacy
checksum work. Use the reported candidate, cell, sample, vertex and index bounds
as the canonical compilation limits so charged vector growth remains capped.
Map-node allocation sizes depend on the library ABI and are bounded here by entry
counts, separately from contiguous bytes. Actual node and buffer allocations must
still use the canonical charged APIs and their byte pool. This admission is not an
aggregate byte reservation for concurrent or retained pages.

Explicit-span charged facade compilation, full-span/page geometry equivalence,
seam qualification and production page ownership remain separate work. Registered
planner regressions await native execution.

## Out of scope

- Queue and worker integration, and a charged `CompilePristineFarVolume` (needs the world).
- Render-mesh conversion buffers.
- Residency eviction policy.
- Approved runtime byte caps. Limits are supplied by the caller.

## Serving and target handoff

`FarVolumeHandoff` is an owner-thread companion to the fixture controller. Its
bounded manifests use complete per-tile build identities and Committed or
SessionRender frozen terrain/water observations. Preview-use captures are refused even when their
durable identity is committed. `MakeFarHandoffTileInput` creates the water bridge
from that same observation and admits its shared allocation in the image pool.
Frozen capture must use the controller's tier pool; every image alias retains
its original allocation charge. Publication serials are process-local render
control state, separate from committed generation and content fingerprints.

SessionRender is an immutable presentation snapshot of a committed durable
baseline plus published session edits. Terrain resolves live, parked, committed,
then pristine; water uses the equivalent live, parked, then committed/base truth.
Expected unavailable or partial authority refuses capture rather than falling
through to a lower layer. The caller resolves ambiguous water ownership as
unavailable. Prepared SessionRender captures are refused. Unsaved edits change
resolved observations and their render fingerprint without incrementing durable
generation. SessionRender cannot publish an edited bake, even when its durable
identity matches. Committed and Preview capture and fingerprint rules are unchanged.
Normal play does not select SessionRender yet. Native qualification and a separately
named, measured production limits profile remain outstanding;
`FarWorldDevelopmentLimits()` is not a production profile.

`FarVolumeHandoff::AdmissionRefusal()` is a const diagnostic of the last staging
attempt. It returns existing selection reasons and resets to `None` on successful
staging or drain. Refusal preserves the target, serving resources and retained
charges. Validation retains the header, bounded manifest/key, per-input and
serving/target union order. Capacity and coverage scratch memory failures report
`AdmissionFailed`; they do not report missing authority.

| Staging failure | Selection reason |
|---|---|
| Invalid cave mode | `InvalidCavePolicy` |
| Missing/ineligible image, content or generation mismatch | `ImmutableAuthorityUnavailable` |
| Bound cave mode mismatch | `CavePolicyMismatch` |
| Missing/inconsistent water bridge or bounds | `WaterUnavailable` |
| Wrong capture pool, capacity or memory admission | `AdmissionFailed` |
| Invalid manifest identity, duplicate keys or publication ordering | `InvalidRequests` |
| Invalid/out-of-range coverage, insufficient support or shrinking retained span | `SpanRejected` |
| Wet water outside resolved coverage | `WaterSpanRefused` |

One set of per-tier queues shares one memory domain across serving and staged
generations. Entry descriptors, retained key/span history, validation/order and
retirement scratch, queue controls, worker scratch, CPU completions, and both
GPU generations count against the existing limits. `entry_slots` bounds retained
key history and the union of serving/target keys. Removed keys retain their
identity and span history while their descriptors are idle. When a validated
manifest needs space, only keys absent from both serving and the incoming target
are reclaimed, in stable slot order and only as many as needed. An evicted key
starts a new residency if revisited; epoch, publication serial and committed
generation checks remain global until explicit drain/rebind. Retained keys keep
all identity and span checks, including keys returning in the incoming manifest.
Admission refuses work when the serving/target union cannot fit; serving fallback
is never sold to finance a target.
No second result allowance or budget is introduced.

Replacement uploads write staged pairs. Every wanted V5 prerequisite must cover
before the complete serving manifest and serial publish together. Changed finer
pairs without a target replacement retire with that publication. Later finer
arrivals publish only into the already serving target. Successful air/solid Empty
pairs cover without a GPU allocation. Serving draws, child identities and child
spans always come from the serving manifest; target readiness is independent.

Upload attempt bytes debit the frame allowance once before backend entry. Failed
backend attempts release temporary GPU admission, retain serving allocations,
report zero successful bytes and latch until a fresh publication. Superseded
workers retain inputs and charges until queue cleanup; complete identity checks
prevent their results from integrating into a newer target.

`FarWorldHandoffRenderer` exposes explicit backend installation, binding, owning
pool aliases for capture, manifest staging, frame advancement, same-frame live
ownership, submission, diagnostics and drain/shutdown. Hosts publish immutable
inputs at their owner-thread boundary; persistent-cache producers feed those
inputs through the same seam. Neither producer has a mutable callback on a
worker. Rendering selection is independent of simulation configuration.

Draw resources are borrowed only during visitation. Before mutations that retire
GPU pairs, the renderer unbinds ownership, vertex array and vertex buffer state.
Submission restores caller bindings on success, refusal and exception. A world
swap drains every queue before releasing staged pairs, serving pairs, observations
and controller/ownership charges. Owning capture aliases retain the memory domain
and its control charge even after renderer teardown.

The separate `luminumbra.far_handoff.v1` diagnostic artifact reports serving and
target serials, identities, requested/built spans and coverage, target refusal,
fallback waits and promotions, pool peaks, retained CPU/GPU bytes, attempted and
successful uploads, and serving draw/child metadata with submission flags.
`FarWorldHandoffTargetReady` requires latest-target terrain/water coverage; older
serving continuity never qualifies it. The existing fixture renderer and its
artifact keep their original behavior.
