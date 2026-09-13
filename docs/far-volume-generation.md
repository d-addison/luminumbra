# Bounded far-volume tile generation

`BuildFarVolumeTile` in
[FarVolumeTile.h](https://github.com/d-addison/luminumbra/blob/3f5076e5974805d00e53200a51c58d66d7dd23d2/src/luminumbra_common/world/FarVolumeTile.h) and
[FarVolumeTile.cpp](https://github.com/d-addison/luminumbra/blob/3f5076e5974805d00e53200a51c58d66d7dd23d2/src/luminumbra_common/world/FarVolumeTile.cpp) is the CPU
entry point for the first generation part of DW01. The production
`luminumbra_common` target includes it. It reads the accepted
[FarTierTable](https://github.com/d-addison/luminumbra/blob/3f5076e5974805d00e53200a51c58d66d7dd23d2/src/luminumbra_common/world/FarTierTable.h) for all five sample,
brick and tile dimensions; there is no second range table.

The [implementation map](distant-world-implementation-status.md) remains a
snapshot of the preceding source revision. This slice supplies a buildable
sampler-driven primitive. Pristine terrain/cave sampler integration, vertical
request discovery, meshing, scheduling, arrival fallback, rendering and durable
edit baking remain unfinished. The client continues to use its existing F1/F2
path, camera plane and formats.

## Request and result

A `FarVolumeRequest` contains a one-based tier, signed integer XZ tile coordinates,
a vertical span in world metres, and an opaque caller-owned field identity.
The span must be nonempty and aligned to that tier's brick edge. Brick origins
occupy `[min_y,max_y)`; samples include `max_y`, sharing the upper boundary with
the next request. Each horizontal tile is aligned to the world origin.
The caller will later derive requested spans from the accepted surface/depth
policy; generation does not guess a surface height or cave cutoff.

Coordinates stay signed 64-bit integers through multiplication and sampling.
Unrepresentable tile endpoints are refused before sampling. This arithmetic
range is an implementation property, not an extension of packed chunk addressing
or the accepted 32 km rendering qualification extent.

Each candidate brick has exactly 5×5×5 density/material samples. Samples use
`x + 5*(y + 5*z)` indexing; retained bricks are ordered by world `(z,x,y)` origin.
The density is the caller's finite float, with no quantization or filtering;
material is an opaque byte. Following the existing Marching Cubes sign test,
negative is solid and zero/nonnegative is outside. A brick is retained when it
contains both signs under that convention. Uniform-air and uniform-solid tiles
succeed with no bricks and distinct `FarVolumeContent` values; an all-zero field
is outside. These are sampled pristine-volume observations, not durable edit
tombstones or proof about unsampled geometry.

The result retains its request, inclusive sampled bounds, candidate count and
actual sample-call count. Shared samples are deliberately evaluated again at
exactly the same world coordinates across brick/tile/span borders. A pure
sampler with the same immutable field input therefore produces bit-identical
border values without a tile-local noise phase. A sub-spacing feature can be
missed or retained depending on its alignment; the generation tests record both.

## Caller obligations and refusal

The caller supplies both `max_candidate_bricks` and `max_surface_bricks`.
Zero-initialized limits authorize no candidate work. The complete candidate
count and multiplication by 125 are checked before the first sample. The
surface limit permits homogeneous requests with zero output allowance and
refuses the first additional crossing brick. These are explicit work and
output-count bounds, not measured byte, frame-time or per-tier residency caps;
DW09 still measures and reviews those caps.

The sampler must be bounded and pure for the request's field identity. It
receives an integer world position and returns an optional density/material
sample. A missing value can signal cancellation or sampler refusal; an exception
also refuses the build. The API cannot preempt a sampler that blocks. Field
identity is carried unchanged and cannot by itself prove a caller supplied the
correct density function.

Invalid/future tiers, empty/reversed/unaligned spans, coordinate overflow,
excess work/output, absent or failed samplers, non-finite densities and allocation
failures return a typed error. A temporary tile accumulates all work; the caller's
previous output is replaced only after success. No partial tile is published.

There is no FSV1 writer/reader, new saved-world record, cache hash, physical cave
filter or renderer connection in this slice. The accepted V1/V2 live cave
sampler and measured V3–V5 filter choice must be integrated separately. Existing
FSD2/LMR1 payloads and runtime defaults are unchanged.

## Verification

[FarVolumeTile_test.cpp](https://github.com/d-addison/luminumbra/blob/3f5076e5974805d00e53200a51c58d66d7dd23d2/test/common/FarVolumeTile_test.cpp) provides ten
registered default-CTest cases. Independent literal dimensions and analytic
fields check all five tiers, negative origins, exact integer endpoints, both
horizontal and vertical shared borders, cold rebuild density bits/materials,
sparse layer selection, homogeneous/zero results, subnormal density retention,
sampling-phase controls, invalid bounds and transactional refusal after partial
work. The same implementation source is compiled into the focused test and
production common target.

```sh
cmake --build build/release --target far_volume_tile_test --parallel 2
ctest --test-dir build/release -R '^FarVolumeTile\.' --no-tests=error --output-on-failure
```

The owned Linux Release and ASan/UBSan runs each pass all ten cases with warnings
treated as errors; the production common-library translation unit also compiles.
These are CPU correctness results. Native Windows, runtime cave/meshing integration, the 16 km
horizon and performance/visual acceptance remain pending.
