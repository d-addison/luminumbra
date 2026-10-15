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

## Out of scope

- Queue and worker integration, and a charged `CompilePristineFarVolume` (needs the world).
- Render-mesh conversion buffers.
- Residency eviction policy.
- Approved runtime byte caps. Limits are supplied by the caller.

## Serving and target handoff

`FarVolumeHandoff` is an owner-thread companion to the fixture controller. Its
bounded manifests use complete per-tile build identities and committed frozen
terrain/water observations. `MakeFarHandoffTileInput` creates the water bridge
from that same observation and admits its shared allocation in the image pool.
Frozen capture must use the controller's tier pool; every image alias retains
its original allocation charge. Publication serials are process-local render
control state, separate from committed generation and content fingerprints.

One set of per-tier queues shares one memory domain across serving and staged
generations. Entry descriptors, retained key/span history, validation/order and
retirement scratch, queue controls, worker scratch, CPU completions, and both
GPU generations count against the existing limits. `entry_slots` bounds retained
key history and the union of serving/target keys, including removed keys, until
an explicit drain/rebind. History exhaustion refuses staging. Admission refuses
work when overlap cannot fit; serving fallback is never sold to finance a target.
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
