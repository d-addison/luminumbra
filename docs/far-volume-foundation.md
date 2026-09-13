# Pristine far-volume foundation

This in-memory generator and mesher implement the sparse-brick representation
in [distant-world.md](distant-world.md). They use the existing five-tier
`FarTierTable` without changing any runtime consumer or draw distance. Runtime
streaming, residency, transitions, overlays, uploads, normals and shading remain
separate work. This is not a 16 km rendering qualification.

## Representation and bounded discovery

`FarVolumeRequest` selects a tier, signed X/Z tile coordinates, an optional
additional world-Y interval, and an experimental cave mode. Every tile has 32
bricks per horizontal axis. Each brick owns 4×4×4 cells and stores 5×5×5 samples,
with X varying fastest, then Y, then Z. Brick keys are signed global (X,Y,Z)
indices, sorted lexicographically. Adjacent bricks repeat their shared samples.

Density uses the existing `FarLodStore` quantizer: i16 at 256 units/metre,
saturation at ±32767, with -32768 reserved for invalid input. Negative means
solid; zero and positive mean air. Materials are per-sample u8 IDs. CRCs fold
explicit little-endian fields, never struct padding: brick coordinates, density
and material; the tile CRC also covers its tier, coordinates, cave mode, complete
scan interval/count, brick count and every brick including its CRC. These are
integrity checks for an in-memory result, not authentication or proof that an
untrusted producer sampled the field honestly.

The generator first samples 129×129 surface heights. It scans the interval from
the lowest sampled surface minus 256 m through the highest sampled surface,
unioned with the optional additional interval. Bounds are rounded outward to
whole bricks, including an air sample above an exactly aligned surface. Extra
requests extend the interval rather than replace it. A caller can union several
anchor/depth requests before calling; no anchor scheduler is added here.

Discovery evaluates a single shared quantized lattice over that bounded span
and retains exactly the bricks whose samples contain both signs. An absent
pristine brick means no sampled crossing. Homogeneous authoritative tombstones
are an overlay concern and are not introduced here. Subspacing surfaces may be
missed at some phases; a sphere of radius 0.35 sample spacings is an explicit
control demonstrating this limit.

Default limits are eight million density samples, 65,536 retained bricks and
128 MiB for height/lattice/reserved-brick buffers. Coordinates are restricted to
the exact integer float sampling range, with additional span/filter margins.
Invalid tiers, modes, coordinates, spans, non-finite samples, or budget overruns
throw. A failed generation never returns a shortened tile. The limits bound the
owned buffers, not process RSS, world-height caches, caller callbacks or a
per-frame latency budget.

## Meshing and validation

`PolygoniseFarVolume` validates metadata, both CRC levels, brick ordering,
identities, density sentinels and all repeated face/edge/corner samples before
emitting geometry. It visits each present brick's owned cells once, using the
existing Marching Cubes tables. It requires neither a heightfield background
nor absent neighbor/halo records, and adds no skirts. Absolute world lattice
coordinates and lexicographically ordered interpolation endpoints make shared
vertices bit-identical even at negative coordinates. The existing triangle table
and this corner order define winding from negative solid toward air. Degenerate
triangles are omitted. The material comes from the solid edge endpoint.

Geometry is returned as positions/materials and indices. Vertices are reused
within each cell, not welded across cells. Defaults allow two million vertices,
six million indices and 64 MiB of reserved output buffers; exhaustion throws
without exposing a partial mesh. Conservative reservations may refuse a dense
tile even when a more compact implementation could fit it.

The mesher preserves table winding for each component. A single cell-wide
finite-difference gradient cannot safely choose a triangle's orientation: in
case 65, corner values `[-2,1,1,1,1,1,-1,1]` make it favor one of two disconnected
solid components and reverse the other inward. The regression generates this
valid brick, exchanges component amplitudes, and negates the field to make
cavities. It checks every triangle against an independent piecewise-trilinear
field sampled on both sides of its normal and against the component's local
outward direction. The original gradient override fails this regression in both
Debug and Release; preserving table winding passes. Material-only corruption on
an isolated shared edge or corner is also refused with recomputed CRCs.

Plane tests check boundary vertex bits and winding at all tiers. Alternating
sign fields check actual ambiguous-face segment connectivity on adjacent X/Z
tiles. The existing table also agrees for all 4,096 compatible adjacent sign
configurations on each of the three axes. A torus/ground analytical arch checks
walls, upper surfaces and undersides. These controls do not constitute a proof
of the topology of every continuous field, nor do they prove runtime LOD
ownership or transition behavior.

## Cave alternatives and the signed-opening difference

V1/V2 directly call the unchanged live density router. Both modes retain its
float operation order at common coordinates. Neither live cave composition nor
existing goldens are modified.

At V3 both alternatives filter the noise-only nonnegative carve contribution
with a cell-width box approximated by eight equally weighted midpoint taps at
±spacing/4 along each axis. Tap order is fixed Z/Y/X and accumulation uses
double. Each tap samples the existing noise router with its existing local cap
and terrain height; terrain and analytic opening silhouettes are composed at
the lattice position. World-coordinate taps require no brick-local filtering
state or tile halo.

- `BandLimited` uses that kernel at V3 and omits noise caves at V4/V5, retaining
  analytic surface openings. Its name identifies the contracted cutoff
  alternative; the finite V3 quadrature is not an ideal spectral low-pass.
- `BoxFiltered` retains the same eight-tap noise kernel at V3, V4 and V5.

For the analytical field `2 + cos(2π f x + phase)`, the one-axis transfer is
`cos(π f spacing / 2)`. The tests cancel `f=1/spacing` but retain full-amplitude
aliasing at `f=2/spacing`. The finite box estimate therefore reduces some
frequencies and does not eliminate aliasing. Sampling-phase and dense-cave
classification results are measured rather than presented as visual quality
or a final design choice.

The new coarse analytic branch deliberately preserves a signed feature SDF.
With negative-solid terrain `T` and a negative-inside cone/capsule `S`, it uses
`max(T,-S)`. The historical live surface-break path first clamps its feature
contribution to `max(0,-S)` and then composes its negative with terrain. With
noise suppressed and zero smoothing, that historical composition remains
negative inside an intended opening. The coarse branch uses hard signed CSG
and thus can produce positive air there; the historical live field remains
unchanged. This is an explicit coarse representation difference, not a live
cave repair.

The opening control locates a deterministic mouth more than 8 km from the
origin, tests both V4/V5 modes inside it, and verifies negative solid below it.
A separate retained executable runs the original composition against the
positive-air criterion and exits 1, then runs the signed coarse composition
and exits 0. This proves the sign difference independently of noisy cavities.
Small analytic openings can still disappear between coarse sample positions;
retaining their field is not a guarantee of visibility at every phase.

## Recorded Linux comparison

After the winding correction, one Release run on an AMD Ryzen 7 9800X3D, GCC 13.3.0 (`-O3 -DNDEBUG`),
produced the following full-tile results. The same source also ran under an
actual Debug configuration. Host activity was not isolated, so these are
observations rather than latency guarantees. Each row starts a cold world;
V5 covers a larger horizontal area and pays more terrain-height discovery cost.

| Tier | Mode | Generation ms | Validation/meshing ms | Retained bricks | Indices |
|---|---|---:|---:|---:|---:|
| V3 | band | 2461 | 36.0 | 5120 | 1042167 |
| V3 | box | 2459 | 38.8 | 5120 | 1042167 |
| V4 | band | 171 | 7.3 | 1052 | 125142 |
| V4 | box | 1645 | 19.3 | 3072 | 823617 |
| V5 | band | 471 | 7.2 | 1059 | 99036 |
| V5 | box | 1545 | 12.7 | 2048 | 522255 |

The V3 modes use the same field and produce equal geometry counts. At V4/V5,
retaining filtered noise costs more and produces more interior geometry. All
six tile CRCs match between the actual Linux Debug and Release builds; mode
metadata intentionally gives the two V3 tiles different CRCs.

Across the 512 phase pairs, the box field changes solid/air classification
163 times at V3, 155 at V4 and 208 at V5. The band field gives the same V3 result
and zero changes at the deeper V4/V5 probes, where noise caves are intentionally
absent. That zero count is removal of the interior feature, not evidence of
superior reconstruction. The independent distant-mouth control verifies that
signed analytic openings still survive in the field after noise is removed.

## Qualification and reproduction

Build and run the new tests and existing far-storage/meshing regressions:

```sh
cmake --build --preset debug --target common_tests -j2
build/debug/bin/common_tests --gtest_filter='FarVolume.*:FarTierTable.*:FarLod*:MarchingCubesAuthoritativeSdf.*:TerrainPresetLoaderTest.*' --gtest_output=xml:far-volume-debug.xml
```

The same tests run from an actual Release build, with `release` substituted in
both commands. Test properties report the alternative measurements in the
GoogleTest XML. Full-tile measurements construct separate cold world instances
from `worlds/atlas/presets/caverns.json`, seed 1337, tile (-1,-1), with the default
surface/depth interval. Generation and validation/meshing are timed separately.
The sample comparison uses 512 positions and corresponding half-spacing X/Z
shifts at depths 32, 56, 80 and 104 m, with equal cache warm-up and only the
selected sampler timed. Classification differences from live are not an alias
oracle; the analytical transfer and phase controls provide the independent
sampling evidence.

Measurements and exact build/source provenance are retained in the campaign's
ignored `evidence/volumes` directory. Native Windows qualification is owned by
the coordinator. The evidence does not select the final cave representation:
that review remains pending, with the band-cutoff alternative retained as the
recorded fallback. Persistent formats, durable authority, deep request
scheduling, asynchronous admission and complete-runtime performance remain
outside this slice.
