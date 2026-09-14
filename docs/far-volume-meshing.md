# Bounded far-volume meshing

`MeshFarVolumeTile` in `src/luminumbra_common/world/FarVolumeMesher.h` and
`FarVolumeMesher.cpp` is the CPU production entry point for the first meshing
part of DW03. It consumes the sparse 5×5×5 bricks produced by
[bounded volume generation](far-volume-generation.md), reads all five accepted
spacings from `FarTierTable`, and is compiled into `luminumbra_common`.

This is a source kernel. The [distant-world contract](distant-world.md) still
requires terrain/cave authority integration, requested-span discovery, runtime
scheduling, arrival ownership, upload/draw integration, water, persistence and
measured budgets. The client retains its existing F1/F2 path and formats. This
addition does not qualify the 16,384 m horizon or any native performance target.
There is also a [representation integration blocker](far-volume-reconciliation.md)
with the separately developed volume foundation and its prepared asynchronous
consumer; the duplicate APIs require one reviewed composition before runtime use.

## Geometry and identity

Each resident brick owns its 4×4×4 cells. Cells use the existing production
Marching Cubes topology tables, corner/edge layout, negative-solid sign test,
solid-endpoint material and table-defined orientation. Material bytes
remain opaque, including 255; there is no legacy background-material lookup.
The existing tables' ambiguous-cell topology is unchanged. This kernel supplies
neither transition stitching nor a new topology guarantee for ambiguous fields.
The legacy cell-wide gradient winding override is deliberately omitted: it can
reverse one of two disconnected components in case 65 and its cavity complement.
An independent continuous trilinear oracle checks both components' directions.

Input bricks may arrive in any order. The mesher sorts them by `(z,x,y)` origin,
visits cells in `(z,y,x)` order, and keys vertices by the lower integer world
endpoint and axis of each lattice edge. Opposite classifications interpolate
in double precision, without the older mesher's small-density epsilon snap.
Exact-zero endpoints and subnormal finite densities therefore keep their sample
phase. Exactly collapsed triangles emit no vertices or indices.

Output stores the integer world origin and double positions in metres relative
to that origin. There is no conversion to float world coordinates or heightfield
ceiling. Optional geometric bounds come from the vertices actually referenced
by triangles. Empty geometry has no geometric bounds; request, sampled span,
content and work counts remain valid. This includes homogeneous air/solid and
mixed bricks whose zero-measure surface collapses to a point.

This arithmetic representation does not extend packed chunk addressing or the
accepted 32 km rendering qualification extent. A renderer must separately select
its coordinate conversion. Double local coordinates also have finite precision;
arbitrarily large vertical spans do not imply arbitrarily precise geometry.

## Authority and border normals

The caller supplies the immutable field identity and the same bounded, pure
density/material sampler used for generation. The identity must match the tile,
and every resident sample is compared against the sampler: density bits
(including signed zero) and material must match exactly. Duplicate coordinates
share a cache entry, so disagreements between resident bricks are also refused.
Metadata, aligned bounds, generation counts, crossing bricks and duplicate brick
origins are validated. The kernel cannot prove that an omitted brick should have
been present; the caller still owns generation provenance and field purity.

For each used edge endpoint, central differences sample one tier spacing in
both directions along each axis. These are same-authority halo samples, including
outside the tile/span. Endpoint gradients interpolate along the canonical edge
and are normalized; exact endpoint crossings use that endpoint's gradient.
Adjacent tiles therefore use the same coordinates and samples for both positions
and normals, including curved boundaries, without accumulating unrelated faces.
All unique resident and halo sample calls are cached and counted once.

A required halo coordinate outside signed 64-bit range refuses the operation.
A zero interpolated gradient also refuses it; substituting a local face normal
would not satisfy the shared-normal contract. These are concrete kernel limits,
not an adopted physical cave filter or new runtime world boundary.

## Budgets and refusal

The caller supplies maximum cells, sample evaluations, vertices and indices.
Zero-initialized limits authorize no work or output. The cell count is checked
before any sampler call and accounts for 64 cells plus the 125 resident-sample
validations per supplied brick. Sample limits bound unique callback calls and
sample-cache entries. Edge and gradient caches are bounded by emitted vertices
and their endpoints. Vertex output also respects the 32-bit index representation.
These explicit work/output counts are not measured runtime byte or frame caps.

The complete result accumulates privately and replaces the previous mesh only on
success. Invalid metadata, wrong authority, conflicting samples, non-finite
densities, exhausted budgets, failed/cancelled samplers, coordinate overflow,
zero gradients and allocation failures return typed errors. A sampler can return
`nullopt` to cancel or refuse; exceptions also refuse the operation. The kernel
cannot preempt a callback that blocks or prove it is pure after caching a value.

## Verification

`test/common/FarVolumeMesher_test.cpp` registers fifteen default-CTest cases.
Independent analytic fixtures cover walls and downward-facing ceilings at all
five spacings, negative origins, exact rectangular coverage and winding,
curved shared X/Z borders with exactly equal normals/materials, vertical borders,
crossings at deep and tall origins, independent cold builds and reordered bricks,
homogeneous and collapsed empty results, exact-zero and subnormal crossings,
unique-sample accounting, exact/insufficient budgets and transactional refusals.
Malformed metadata, resident disagreement, signed zero, failed/non-finite halo
samples, integer-limit halo overflow and zero-gradient controls are explicit.
Unequal disconnected components/cavities and mixed shared-face connectivity
across all axes and tiers cover the reproduced winding failure and adjacent cells.

```sh
cmake --build build/release --target far_volume_mesher_test --parallel 2
ctest --test-dir build/release -R '^FarVolumeMesher\.' --no-tests=error --output-on-failure
```

The owned Linux Release and ASan/UBSan runs each pass all fifteen mesher cases
and the ten generation cases. The production common-library translation unit
also compiles with warnings treated as errors. Strict Doxygen generation and
the generated-link check pass. These are CPU correctness results.
They do not exercise live cave filtering,
runtime scheduling, a GPU, transition ownership, water, or native acceptance.
The world-profile requirement remains p99 ≤16.67 ms with the separate 8.33 ms
reported target; no performance result is claimed by this kernel.
