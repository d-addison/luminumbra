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
